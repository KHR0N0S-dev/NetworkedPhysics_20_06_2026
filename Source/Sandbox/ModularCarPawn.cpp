// Fill out your copyright notice in the Description page of Project Settings.

#include "ModularCarPawn.h"

#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimationAsset.h"
#include "Components/InputComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "UObject/ConstructorHelpers.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Chaos/ChaosEngineInterface.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputActionValue.h"

#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PBDRigidsSolver.h"
#include "Chaos/PhysicsObjectInterface.h"
#include "Chaos/PhysicsObjectInternalInterface.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

static TAutoConsoleVariable<int32> CVarCarSelfTest(
	TEXT("car.SelfTest"),
	0,
	TEXT("Headless self-test: 1=drive, 2=rest, 3=cosmetic-mesh, 4=collision, 5=adversarial collision, 6=networked collision (server+client), 7=dual-client head-on, 8=add-wheel sleep regression, 9=remote drive (server throttle, client sim-proxy); logs telemetry then quits."),
	ECVF_Default);

//----------------------------------------------------
//------------------ Networked physics ----------------
//----------------------------------------------------

void FModularCarAsync::OnPhysicsObjectUnregistered_Internal(Chaos::FConstPhysicsObjectHandle InPhysicsObject)
{
	if (PhysicsObject == InPhysicsObject)
	{
		PhysicsObject = nullptr;
	}
}

/** Autonomous-proxy client / controlling server: snapshot the local input to be sent + predicted. */
void FModularCarAsync::BuildInput_Internal(FNetInputModularCar& Input) const
{
	Input.Throttle = Throttle_Internal;
	Input.Steer = Steer_Internal;
	Input.Handbrake = Handbrake_Internal;
}

/** Server: clamp incoming input to valid ranges. */
void FModularCarAsync::ValidateInput_Internal(FNetInputModularCar& Input) const
{
	Input.Throttle = FMath::Clamp(Input.Throttle, -1.0f, 1.0f);
	Input.Steer = FMath::Clamp(Input.Steer, -1.0f, 1.0f);
	Input.Handbrake = FMath::Clamp(Input.Handbrake, 0.0f, 1.0f);
}

/** Server & sim-proxy clients each frame, and on clients during resimulation. */
void FModularCarAsync::ApplyInput_Internal(const FNetInputModularCar& Input)
{
	Throttle_Internal = Input.Throttle;
	Steer_Internal = Input.Steer;
	Handbrake_Internal = Input.Handbrake;
}

void FModularCarAsync::BuildState_Internal(FNetStateModularCar& State) const {}
void FModularCarAsync::ApplyState_Internal(const FNetStateModularCar& State) {}

static void ApplyNeverSleepToParticle(Chaos::FConstPhysicsObjectHandle PhysicsObject)
{
	if (!PhysicsObject)
	{
		return;
	}
	Chaos::FWritePhysicsObjectInterface_Internal Interface = Chaos::FPhysicsObjectInternalInterface::GetWrite();
	if (Chaos::FPBDRigidParticleHandle* ParticleHandle = Interface.GetRigidParticle(PhysicsObject))
	{
		ParticleHandle->SetSleepType(Chaos::ESleepType::NeverSleep);
	}
}

void FModularCarAsync::OnPostInitialize_Internal()
{
	if (bApplyDriveForces)
	{
		ApplyNeverSleepToParticle(PhysicsObject);
	}
}

void FModularCarAsync::ProcessInputs_Internal(int32 PhysicsStep)
{
	// Capture the local input into the internal state so the network component records it into the
	// input history (via BuildInput_Internal) - this is what gets sent to the server and replayed
	// during resimulation. Skip while resimulating (the framework supplies historical input then).
	const FAsyncInputModularCar* AsyncInput = GetConsumerInput_Internal();
	if (AsyncInput == nullptr)
	{
		return;
	}
	Chaos::FPhysicsSolverBase* BaseSolver = GetSolver();
	if (!BaseSolver || BaseSolver->IsResimming())
	{
		return;
	}

	Throttle_Internal = AsyncInput->Throttle;
	Steer_Internal = AsyncInput->Steer;
	Handbrake_Internal = AsyncInput->Handbrake;
}

void FModularCarAsync::OnPreSimulate_Internal()
{
	if (!PhysicsObject)
	{
		return;
	}

	Chaos::FWritePhysicsObjectInterface_Internal Interface = Chaos::FPhysicsObjectInternalInterface::GetWrite();
	Chaos::FPBDRigidParticleHandle* ParticleHandle = Interface.GetRigidParticle(PhysicsObject);
	if (ParticleHandle == nullptr)
	{
		return;
	}

	// Predicted/authority only — sim-proxies must follow replicated state, not fight it with NeverSleep.
	if (bApplyDriveForces)
	{
		ParticleHandle->SetSleepType(Chaos::ESleepType::NeverSleep);
	}

	// Outside resimulation use the freshest local input (and keep *_Internal in sync for the
	// standalone case, where the rewind machinery doesn't drive ProcessInputs_Internal). During a
	// resimulation, KEEP whatever the network component already applied from the input history
	// (ApplyInput_Internal) - reading live input here would make the replay diverge from the
	// original step, producing the constant corrections that show up as jitter.
	if (!GetSolver()->IsResimming())
	{
		if (const FAsyncInputModularCar* AsyncInput = GetConsumerInput_Internal())
		{
			Throttle_Internal = AsyncInput->Throttle;
			Steer_Internal = AsyncInput->Steer;
			Handbrake_Internal = AsyncInput->Handbrake;
		}
	}

	// Client sim-proxies must not apply grip/drive locally — the server's physics is authoritative and
	// local grip forces fight predicted-body contacts (the multiplayer "sticking" bug).
	if (!bApplyDriveForces)
	{
		return;
	}

	const float Throttle = FMath::Clamp(Throttle_Internal, -1.0f, 1.0f);
	const float Steer = FMath::Clamp(Steer_Internal, -1.0f, 1.0f);
	const bool bHandbrake = Handbrake_Internal > 0.5f;

	const Chaos::FRotation3 R = ParticleHandle->GetR();
	const Chaos::FVec3 Fwd = R * Chaos::FVec3(1.0, 0.0, 0.0);
	const Chaos::FVec3 Right = R * Chaos::FVec3(0.0, 1.0, 0.0);
	const Chaos::FVec3 Up = R * Chaos::FVec3(0.0, 0.0, 1.0);
	const Chaos::FVec3 Vel = ParticleHandle->GetV();
	const Chaos::FReal Mass = FMath::Max(ParticleHandle->M(), (Chaos::FReal)1.0);

	const Chaos::FReal FwdVel = Chaos::FVec3::DotProduct(Vel, Fwd);
	const Chaos::FReal RightVel = Chaos::FVec3::DotProduct(Vel, Right);

	// All horizontal forces at the centre of mass (no spurious roll/yaw torque); steering is the
	// only deliberate rotation. This is the same model proven calm-at-rest single-player, now run
	// on the physics thread so it predicts/reconciles across the network.
	Chaos::FVec3 Force(0.0, 0.0, 0.0);

	if (!bHandbrake && FMath::Abs(Throttle) > 0.01f)
	{
		Force += Fwd * (Throttle * EngineForceTotal);
	}

	// Grip forces are friction-circle LIMITED (capped at MaxGripAccel * mass). Without a cap the
	// "-vel * mass * rate" term is an infinite anchor: when another car rams this one, the grip
	// resists the push with unbounded force, so the car won't budge ("sticks") and the contact
	// solver blows up. Capping makes grip behave like a real tyre - a hard enough shove overcomes it
	// and the car slides instead of breaking the physics.
	const Chaos::FReal MaxGripForce = MaxGripAccel * Mass;

	if (FMath::Abs(RightVel) > RestSpeedDeadzone)
	{
		const Chaos::FReal Lat = FMath::Clamp(RightVel * Mass * LateralGripRate, -MaxGripForce, MaxGripForce);
		Force += -Right * Lat;
	}

	// Longitudinal grip only under handbrake — coasting with throttle=0 must stay pushable in collisions.
	if (bHandbrake && FMath::Abs(FwdVel) > RestSpeedDeadzone)
	{
		const Chaos::FReal Lon = FMath::Clamp(FwdVel * Mass * LongitudinalGripRate, -MaxGripForce, MaxGripForce);
		Force += -Fwd * Lon;
	}

	if (Force.SizeSquared() > UE_SMALL_NUMBER)
	{
		ParticleHandle->AddForce(Force, true);
	}

	// Steering = command a target yaw rate directly (arcade model). Applying a torque through the
	// welded body's (large, hard-to-estimate) moment of inertia barely turned the car; driving the
	// yaw angular velocity toward a target gives a predictable, tunable "how sharp it turns" knob
	// (MaxYawRate, deg/s) and stays deterministic for prediction/resimulation. Scaled by speed so
	// the car only turns while rolling and reverses naturally in reverse.
	if (FMath::Abs(Steer) > 0.01f && MaxYawRateRad > 0.0f)
	{
		const Chaos::FReal SpeedFrac = FMath::Clamp(FwdVel / FMath::Max((Chaos::FReal)SteerRefSpeed, (Chaos::FReal)1.0), (Chaos::FReal)-1.0, (Chaos::FReal)1.0);
		const Chaos::FReal TargetYaw = Steer * SpeedFrac * MaxYawRateRad;
		Chaos::FVec3 W = ParticleHandle->GetW();
		// Converge quickly toward the target yaw rate; leave pitch/roll (tilt) to the solver.
		W.Z = FMath::Lerp(W.Z, TargetYaw, (Chaos::FReal)0.5);
		ParticleHandle->SetW(W);
	}
}

//----------------------------------------------------
//--------------------- Pawn -------------------------
//----------------------------------------------------

AModularCarPawn::AModularCarPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	// ReplicateMovement must stay enabled: server GatherCurrentMovement() publishes physics state
	// into ReplicatedMovement for sim-proxies (PostNetReceivePhysicState -> PI). Input prediction
	// and resimulation remain on UNetworkPhysicsComponent. Disabling movement replication freezes
	// remote cars on clients because they never receive SetRigidBodyReplicatedTarget updates.
	SpawnCollisionHandlingMethod = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	CubeMesh = CubeFinder.Succeeded() ? CubeFinder.Object : nullptr;
	CylinderMesh = CylinderFinder.Succeeded() ? CylinderFinder.Object : nullptr;
	// The cosmetic car (skeletal SKM_SportsCar) is loaded at runtime in BeginPlay, not here:
	// the project's own SportsCar_SM static meshes are saved with a too-new package version and
	// will not load in this engine build, so we use the engine plugin's native skeletal car instead.

	// Chassis = the simulating root rigid body.
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	SetRootComponent(Body);
	if (CubeMesh)
	{
		Body->SetStaticMesh(CubeMesh);
	}
	Body->SetMobility(EComponentMobility::Movable);
	Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Body->SetCollisionObjectType(ECC_PhysicsBody);
	Body->SetCollisionResponseToAllChannels(ECR_Block);
	Body->SetSimulatePhysics(true);
	Body->SetEnableGravity(true);
	Body->BodyInstance.SleepFamily = ESleepFamily::Sensitive;
	Body->BodyInstance.bStartAwake = true;

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(Body);
	SpringArm->TargetArmLength = 900.0f;
	SpringArm->SocketOffset = FVector(0.0f, 0.0f, 250.0f);
	SpringArm->bDoCollisionTest = false;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 5.0f;
	SpringArm->bInheritPitch = false;
	SpringArm->bInheritRoll = false;
	SpringArm->bInheritYaw = true;
	SpringArm->SetRelativeRotation(FRotator(-20.0f, 0.0f, 0.0f));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);

	// Cosmetic car (skeletal SKM_SportsCar - body + wheels in one mesh, rendered in ref pose),
	// overlaid on the collision cube. Absolute scale so the chassis' non-uniform scale (sized from
	// BodyExtent) never squashes the art; no collision (the cube + cylinders handle that).
	BodyVisual = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("BodyVisual"));
	BodyVisual->SetupAttachment(Body);
	BodyVisual->SetUsingAbsoluteScale(true);
	BodyVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyVisual->SetCollisionProfileName(TEXT("NoCollision"));
	BodyVisual->CanCharacterStepUpOn = ECB_No;
	BodyVisual->SetGenerateOverlapEvents(false);
	BodyVisual->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	BodyVisual->PrimaryComponentTick.bCanEverTick = false;
	// Purely cosmetic: no simulation, no gravity. The physics asset is cleared in ApplyBodyVisual
	// (after the mesh is set) so the skeletal mesh creates NO kinematic physics bodies - those bodies,
	// updated at game-frame rate while the chassis steps on the async physics thread, lag the chassis
	// and show up as the car visibly jittering.
	BodyVisual->SetSimulatePhysics(false);
	BodyVisual->SetEnableGravity(false);

	// Networked physics component (only when physics prediction is enabled in project settings).
	if (UPhysicsSettings::Get()->PhysicsPrediction.bEnablePhysicsPrediction)
	{
		static const FName NetworkPhysicsComponentName(TEXT("NetworkPhysicsComponent"));
		NetworkPhysicsComponent = CreateDefaultSubobject<UNetworkPhysicsComponent>(NetworkPhysicsComponentName);
		NetworkPhysicsComponent->SetNetAddressable();
		NetworkPhysicsComponent->SetIsReplicated(true);

		// Replication mode is applied per-role in ApplyNetworkPhysicsReplicationMode() — must be set
		// before NetworkPhysicsComponent::BeginPlay marshals mode to the physics thread.

		static const FName NetworkPhysicsSettingsComponentName(TEXT("NetworkPhysicsSettingsComponent"));
		NetworkPhysicsSettingsComponent = CreateDefaultSubobject<UNetworkPhysicsSettingsComponent>(NetworkPhysicsSettingsComponentName);
	}
}

void AModularCarPawn::InitializeChassisPhysics()
{
	if (!Body || bChassisPhysicsInitialized)
	{
		return;
	}
	bChassisPhysicsInitialized = true;

	Body->SetRelativeScale3D(BodyExtent / 100.0f);
	Body->SetMassOverrideInKg(NAME_None, BodyMassKg, true);
	Body->SetSimulatePhysics(true);
	Body->SetEnableGravity(true);
	Body->SetLinearDamping(ChassisLinearDamping);
	Body->SetAngularDamping(ChassisAngularDamping);

	if (!ChassisPhysMaterial)
	{
		ChassisPhysMaterial = NewObject<UPhysicalMaterial>(this, TEXT("ModularCarChassisPM"));
		ChassisPhysMaterial->Friction = 0.2f;
		ChassisPhysMaterial->Restitution = 0.0f;
		ChassisPhysMaterial->bOverrideFrictionCombineMode = true;
		ChassisPhysMaterial->FrictionCombineMode = EFrictionCombineMode::Min;
	}
	Body->SetPhysMaterialOverride(ChassisPhysMaterial);
}

void AModularCarPawn::BeginPlay()
{
	Super::BeginPlay();

	if (!Body)
	{
		return;
	}

	// Load the cosmetic car at runtime (content is mounted by now; FObjectFinder at CDO time is
	// unreliable for plugin/game content). SKM_SportsCar is the engine's native 5.7 skeletal car
	// (body + wheels); the project's own SportsCar_SM is unusable here (too-new package version).
	if (bUseVisualMeshes && !CarBodyMesh)
	{
		CarBodyMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/ChaosModularVehicleExamples/Models/SportsCar/SKM_SportsCar.SKM_SportsCar"));
		UE_LOG(LogTemp, Warning, TEXT("VISUALLOAD | SKM_SportsCar=%s"), CarBodyMesh ? TEXT("OK") : TEXT("FAIL"));
	}

	InitializeChassisPhysics();

	// Specs can replicate after PostInitializeComponents on clients — build wheels once they arrive.
	if (HasAuthority() && bSpawnDefaultWheels && ReplicatedWheelSpecs.Num() == 0)
	{
		InitializeDefaultSymmetricWheelSpecs();
	}
	if (ReplicatedWheelSpecs.Num() > 0 && Wheels.Num() == 0)
	{
		ApplyReplicatedWheelLayout();
	}

	// Only the server may teleport/settle — clients (especially sim-proxies) must follow replication.
	if (HasAuthority())
	{
		Body->WakeAllRigidBodies();
		SettleOntoGround();
	}

	// Hide the collision cube once the car body art is in place (collision/simulation unaffected).
	if (bUseVisualMeshes && CarBodyMesh)
	{
		Body->SetVisibility(false);
	}
	ApplyBodyVisual();
	ApplyNetworkPhysicsReplicationMode();
	EnsureNetworkPhysicsHistory();
	ScheduleRefreshPhysicsBindings();

	RefreshDriveTuningFromWheels();

	UE_LOG(LogTemp, Warning, TEXT("CARSETUP | Role=%d LC=%d RepMode=%d Sim=%d Grav=%d Mass=%.0f Wheels=%d Net=%d Hist=%d EngineForce=%.0f"),
		(int32)GetLocalRole(), IsLocallyControlled() ? 1 : 0, (int32)GetPhysicsReplicationMode(),
		Body->IsSimulatingPhysics() ? 1 : 0, Body->IsGravityEnabled() ? 1 : 0,
		Body->GetMass(), Wheels.Num(), NetworkPhysicsComponent ? 1 : 0, bNetworkPhysicsHistoryCreated ? 1 : 0,
		CarAsync ? CarAsync->EngineForceTotal : 0.0f);

	// Diagnostics: did the cosmetic car load and get applied, and are the proxies hidden?
	const USkeletalMesh* BV = BodyVisual ? BodyVisual->GetSkeletalMeshAsset() : nullptr;
	int32 CylVisible = 0;
	for (const FCarWheel& W : Wheels)
	{
		if (W.Mesh && W.Mesh->IsVisible()) { ++CylVisible; }
	}
	UE_LOG(LogTemp, Warning, TEXT("VISUALCHECK | UseVis=%d CarBodyMesh=%s BodyVisualMesh=%s BodyHidden=%d CylindersVisible=%d/%d"),
		bUseVisualMeshes ? 1 : 0,
		CarBodyMesh ? *CarBodyMesh->GetName() : TEXT("NULL"),
		BV ? *BV->GetName() : TEXT("NULL"),
		(Body && !Body->IsVisible()) ? 1 : 0,
		CylVisible, Wheels.Num());
}

void AModularCarPawn::SettleOntoGround()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Ride height = how far the lowest wheel reaches below the chassis centre.
	RestRideHeight = BodyExtent.Z * 0.5f;
	for (const FCarWheel& Wheel : Wheels)
	{
		RestRideHeight = FMath::Max(RestRideHeight, -(Wheel.RelativeLocation.Z - Wheel.Radius));
	}

	Body->SetMassOverrideInKg(NAME_None, BodyMassKg, true);

	const FVector ActorLoc = GetActorLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ModularCarSettle), false, this);
	FHitResult Hit;
	const FVector Start = ActorLoc + FVector(0, 0, 2000.0f);
	const FVector End = ActorLoc - FVector(0, 0, 5000.0f);
	if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params))
	{
		return;
	}

	// Spawn slightly above the wheel rest height so the car gently settles onto its wheels.
	FVector NewLoc = ActorLoc;
	NewLoc.Z = Hit.ImpactPoint.Z + RestRideHeight + 45.0f;
	SetActorLocation(NewLoc, false, nullptr, ETeleportType::TeleportPhysics);
	Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Body->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
}

UStaticMeshComponent* AModularCarPawn::CreateWheelMesh(const FVector& RelLoc, float Radius, float Width)
{
	UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(this);
	if (!Comp)
	{
		return nullptr;
	}
	Comp->SetStaticMesh(CylinderMesh);
	Comp->SetMobility(EComponentMobility::Movable);
	Comp->SetupAttachment(Body);
	Comp->RegisterComponent();
	// Weld into the chassis rigid body: the car rests on its four wheels (one body, four contact
	// points) instead of on the cube's belly.
	Comp->AttachToComponent(Body, FAttachmentTransformRules(EAttachmentRule::KeepRelative, true));

	// Cylinder basic shape: radius ~50 in local XY, height 100 along local Z.
	// Roll 90 deg so the axle (local Z) points along the car's Y (sideways).
	const FVector Scale((2.0f * Radius) / 100.0f, (2.0f * Radius) / 100.0f, Width / 100.0f);
	Comp->SetRelativeLocationAndRotation(RelLoc, FRotator(0.0f, 0.0f, 90.0f));
	Comp->SetRelativeScale3D(Scale);

	Comp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Comp->SetCollisionObjectType(ECC_PhysicsBody);
	Comp->SetCollisionResponseToAllChannels(ECR_Block);
	if (ChassisPhysMaterial)
	{
		Comp->SetPhysMaterialOverride(ChassisPhysMaterial);
	}
	// The cylinder is the support/collision; hide it once the cosmetic car covers everything.
	if (bUseVisualMeshes && CarBodyMesh)
	{
		Comp->SetVisibility(false);
	}
	return Comp;
}

void AModularCarPawn::ApplyBodyVisual()
{
	if (!BodyVisual)
	{
		return;
	}
	const bool bShow = bUseVisualMeshes && (CarBodyMesh != nullptr);
	BodyVisual->SetVisibility(bShow);
	if (!bShow)
	{
		return;
	}
	BodyVisual->SetSkeletalMeshAsset(CarBodyMesh);
	// Drop the mesh's physics asset so no kinematic bodies are created (cosmetic follower only).
	// Setting the mesh can re-instantiate the asset's physics state, so clear it here, afterwards.
	BodyVisual->SetSimulatePhysics(false);
	BodyVisual->SetPhysicsAsset(nullptr);
	BodyVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyVisual->SetUsingAbsoluteScale(true);
	BodyVisual->SetWorldScale3D(BodyVisualScale);

	// Relative offset is scaled by the chassis' non-uniform scale, so pre-divide to land where asked.
	const FVector S = Body ? Body->GetRelativeScale3D() : FVector::OneVector;
	const FVector SafeS(
		FMath::Abs(S.X) > KINDA_SMALL_NUMBER ? S.X : 1.0f,
		FMath::Abs(S.Y) > KINDA_SMALL_NUMBER ? S.Y : 1.0f,
		FMath::Abs(S.Z) > KINDA_SMALL_NUMBER ? S.Z : 1.0f);
	BodyVisual->SetRelativeLocation(BodyVisualOffset / SafeS);
	BodyVisual->SetRelativeRotation(BodyVisualRotation);
}

FVector AModularCarPawn::GetSymmetricPairWheelLocation(int32 PairIndex, bool bLeftSide) const
{
	const float HX = BodyExtent.X * 0.5f - 70.0f;
	const float HY = BodyExtent.Y * 0.5f + 15.0f;
	const float ZZ = -BodyExtent.Z * 0.5f - 10.0f;

	float X = HX;
	if (PairIndex == 1)
	{
		X = -HX;
	}
	else if (PairIndex > 1)
	{
		X = -HX - static_cast<float>(PairIndex - 1) * 140.0f;
	}

	const float Y = bLeftSide ? -HY : HY;
	return FVector(X, Y, ZZ);
}

void AModularCarPawn::AppendSymmetricPairSpecs(int32 PairIndex)
{
	const bool bFrontAxle = (PairIndex == 0);
	const float DefaultRadius = 35.0f;
	const float DefaultWidth = 20.0f;

	FModularCarWheelSpec Left;
	Left.RelativeLocation = GetSymmetricPairWheelLocation(PairIndex, true);
	Left.bDriven = true;
	Left.bSteering = bFrontAxle;
	Left.Radius = DefaultRadius;
	Left.Width = DefaultWidth;

	FModularCarWheelSpec Right = Left;
	Right.RelativeLocation = GetSymmetricPairWheelLocation(PairIndex, false);
	Right.bSteering = bFrontAxle;

	ReplicatedWheelSpecs.Add(Left);
	ReplicatedWheelSpecs.Add(Right);
}

void AModularCarPawn::InitializeDefaultSymmetricWheelSpecs()
{
	ReplicatedWheelSpecs.Reset();
	AppendSymmetricPairSpecs(0);
	AppendSymmetricPairSpecs(1);
}

void AModularCarPawn::ApplyReplicatedWheelLayout()
{
	if (!Body || !CylinderMesh)
	{
		return;
	}

	for (FCarWheel& Wheel : Wheels)
	{
		if (Wheel.Mesh)
		{
			Wheel.Mesh->DestroyComponent();
		}
	}
	Wheels.Reset();

	for (const FModularCarWheelSpec& Spec : ReplicatedWheelSpecs)
	{
		FCarWheel Wheel;
		Wheel.RelativeLocation = Spec.RelativeLocation;
		Wheel.bDriven = Spec.bDriven;
		Wheel.bSteering = Spec.bSteering;
		Wheel.Radius = Spec.Radius;
		Wheel.Width = Spec.Width;
		Wheel.Mesh = CreateWheelMesh(Spec.RelativeLocation, Spec.Radius, Spec.Width);
		Wheels.Add(Wheel);
	}

	ScheduleRefreshPhysicsBindings();
	RefreshDriveTuningFromWheels();
}

void AModularCarPawn::ApplyNetworkPhysicsReplicationMode()
{
	if (!UPhysicsSettings::Get()->PhysicsPrediction.bEnablePhysicsPrediction)
	{
		return;
	}

	const ENetRole LocalRole = GetLocalRole();
	if (LocalRole == ROLE_SimulatedProxy)
	{
		// Remote cars on clients follow server targets via predictive interpolation, not resim.
		SetPhysicsReplicationMode(EPhysicsReplicationMode::PredictiveInterpolation);
	}
	else if (LocalRole == ROLE_Authority || LocalRole == ROLE_AutonomousProxy)
	{
		SetPhysicsReplicationMode(EPhysicsReplicationMode::Resimulation);
	}
}

void AModularCarPawn::EnsureNetworkPhysicsHistory()
{
	// History must be created from RefreshPhysicsBindings on the next tick after wheel weld —
	// same-frame CreateDataHistory after ApplyReplicatedWheelLayout binds a stale handle and
	// freezes the default 4-wheel car until a later rebind (e.g. pressing P to add wheels).
	ScheduleRefreshPhysicsBindings();
}

void AModularCarPawn::PostNetReceiveRole()
{
	Super::PostNetReceiveRole();
	ApplyNetworkPhysicsReplicationMode();
	EnsureNetworkPhysicsHistory();
	ScheduleRefreshPhysicsBindings();
}

void AModularCarPawn::ScheduleRefreshPhysicsBindings()
{
	if (!GetWorld() || bPhysicsRefreshScheduled)
	{
		return;
	}
	bPhysicsRefreshScheduled = true;
	GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		bPhysicsRefreshScheduled = false;
		RefreshPhysicsBindings();
	}));
}

void AModularCarPawn::RefreshPhysicsBindings()
{
	if (!Body)
	{
		return;
	}

	const Chaos::FConstPhysicsObjectHandle BodyHandle = Body->GetPhysicsObjectByName(NAME_None);
	if (!BodyHandle)
	{
		return;
	}

	// Deferred init: compound body + valid handle only after wheel weld settles (next tick).
	if (!bNetworkPhysicsHistoryCreated && CarAsync && NetworkPhysicsComponent
		&& ReplicatedWheelSpecs.Num() > 0 && Wheels.Num() > 0)
	{
		ApplyNetworkPhysicsReplicationMode();
		NetworkPhysicsComponent->CreateDataHistory<FNetInputModularCar, FNetStateModularCar>(CarAsync);
		bNetworkPhysicsHistoryCreated = true;
		RefreshDriveTuningFromWheels();
	}

	if (!bNetworkPhysicsHistoryCreated)
	{
		return;
	}

	const bool bDriveRole = ShouldApplyDriveForces();

	if (CarAsync)
	{
		CarAsync->PhysicsObject = BodyHandle;
		if (bDriveRole)
		{
			ApplyNeverSleepToParticle(BodyHandle);
		}
	}

	// Welding wheels rebuilds the compound body; NetworkPhysicsComponent must track the new handle
	// or sim-proxies stop receiving replicated motion (frozen remote cars).
	if (NetworkPhysicsComponent)
	{
		NetworkPhysicsComponent->SetPhysicsObject(BodyHandle);
		if (bNetworkPhysicsHistoryCreated)
		{
			NetworkPhysicsComponent->AddDataHistory();
		}
	}

	// Server must cache physics state so GatherCurrentMovement can replicate to sim-proxies.
	if (HasAuthority())
	{
		if (UWorld* World = GetWorld())
		{
			if (FPhysScene_Chaos* Scene = static_cast<FPhysScene_Chaos*>(World->GetPhysicsScene()))
			{
				Scene->RegisterForReplicationCache(Body);
			}
		}
	}

	if (bDriveRole || HasAuthority())
	{
		Body->WakeAllRigidBodies();
	}
}

void AModularCarPawn::OnRep_ReplicatedWheelSpecs()
{
	InitializeChassisPhysics();
	ApplyReplicatedWheelLayout();
}

bool AModularCarPawn::AddSymmetricWheelPairAuthority()
{
	if (!HasAuthority())
	{
		return false;
	}

	const int32 PairCount = ReplicatedWheelSpecs.Num() / 2;
	if (PairCount >= MaxSymmetricWheelPairs)
	{
		UE_LOG(LogTemp, Warning, TEXT("AddSymmetricWheelPair: max pairs %d reached"), MaxSymmetricWheelPairs);
		return false;
	}

	AppendSymmetricPairSpecs(PairCount);
	ApplyReplicatedWheelLayout();
	UE_LOG(LogTemp, Warning, TEXT("AddSymmetricWheelPair | Pairs=%d Wheels=%d"), PairCount + 1, Wheels.Num());
	return true;
}

bool AModularCarPawn::RemoveSymmetricWheelPairAuthority()
{
	if (!HasAuthority())
	{
		return false;
	}

	if (ReplicatedWheelSpecs.Num() < 2)
	{
		return false;
	}

	ReplicatedWheelSpecs.RemoveAt(ReplicatedWheelSpecs.Num() - 1);
	ReplicatedWheelSpecs.RemoveAt(ReplicatedWheelSpecs.Num() - 1);
	ApplyReplicatedWheelLayout();
	UE_LOG(LogTemp, Warning, TEXT("RemoveSymmetricWheelPair | Pairs=%d Wheels=%d"), ReplicatedWheelSpecs.Num() / 2, Wheels.Num());
	return true;
}

void AModularCarPawn::ServerRequestAddSymmetricWheelPair_Implementation()
{
	AddSymmetricWheelPairAuthority();
}

void AModularCarPawn::ServerRequestRemoveSymmetricWheelPair_Implementation()
{
	RemoveSymmetricWheelPairAuthority();
}

void AModularCarPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AModularCarPawn, ReplicatedWheelSpecs);
}

void AModularCarPawn::RefreshDriveTuningFromWheels()
{
	int32 DrivenCount = 0;
	for (const FCarWheel& W : Wheels)
	{
		if (W.bDriven)
		{
			++DrivenCount;
		}
	}
	const int32 EffectiveWheels = FMath::Max(1, DrivenCount > 0 ? DrivenCount : Wheels.Num());
	if (CarAsync)
	{
		CarAsync->EngineForceTotal = EngineForcePerWheel * EffectiveWheels;
	}

	RestRideHeight = BodyExtent.Z * 0.5f;
	for (const FCarWheel& W : Wheels)
	{
		RestRideHeight = FMath::Max(RestRideHeight, -(W.RelativeLocation.Z - W.Radius));
	}
}

int32 AModularCarPawn::AddWheel(FVector InRelativeLocation, bool bInDriven, bool bInSteering, float InRadius, float InWidth)
{
	if (!HasAuthority())
	{
		return INDEX_NONE;
	}

	FModularCarWheelSpec Spec;
	Spec.RelativeLocation = InRelativeLocation;
	Spec.bDriven = bInDriven;
	Spec.bSteering = bInSteering;
	Spec.Radius = InRadius;
	Spec.Width = InWidth;
	ReplicatedWheelSpecs.Add(Spec);
	ApplyReplicatedWheelLayout();
	return Wheels.Num() - 1;
}

bool AModularCarPawn::RemoveWheel(int32 WheelIndex)
{
	if (!HasAuthority() || !ReplicatedWheelSpecs.IsValidIndex(WheelIndex))
	{
		return false;
	}

	ReplicatedWheelSpecs.RemoveAt(WheelIndex);
	ApplyReplicatedWheelLayout();
	return true;
}

bool AModularCarPawn::RemoveLastWheel()
{
	return RemoveWheel(Wheels.Num() - 1);
}

void AModularCarPawn::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	InitializeChassisPhysics();

	// Server: weld wheels before registering network physics so the initial handle is the compound body.
	if (HasAuthority() && bSpawnDefaultWheels && ReplicatedWheelSpecs.Num() == 0)
	{
		InitializeDefaultSymmetricWheelSpecs();
	}
	if (ReplicatedWheelSpecs.Num() > 0)
	{
		ApplyReplicatedWheelLayout();
	}

	if (UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(GetRootComponent()))
	{
		if (UWorld* World = GetWorld())
		{
			if (FPhysScene* PhysScene = World->GetPhysicsScene())
			{
				if (Chaos::FPhysicsSolver* Solver = PhysScene->GetSolver())
				{
					CarAsync = Solver->CreateAndRegisterSimCallbackObject_External<FModularCarAsync>();
					if (ensure(CarAsync))
					{
						CarAsync->PhysicsObject = Root->GetPhysicsObjectByName(NAME_None);

						CarAsync->LateralGripRate = LateralGripRate;
						CarAsync->LongitudinalGripRate = LongitudinalGripRate;
						CarAsync->MaxGripAccel = MaxGripAccel;
						CarAsync->MaxYawRateRad = FMath::DegreesToRadians(MaxYawRate);
						CarAsync->SteerRefSpeed = SteerRefSpeed;
						CarAsync->RestSpeedDeadzone = RestSpeedDeadzone;
						CarAsync->bApplyDriveForces = ShouldApplyDriveForces();

						ScheduleRefreshPhysicsBindings();
					}
				}
			}
		}
	}
}

void AModularCarPawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (CarAsync)
	{
		if (UWorld* World = GetWorld())
		{
			if (FPhysScene* PhysScene = World->GetPhysicsScene())
			{
				if (Chaos::FPhysicsSolver* Solver = PhysScene->GetSolver())
				{
					Solver->UnregisterAndFreeSimCallbackObject_External(CarAsync);
				}
			}
		}
		CarAsync = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

bool AModularCarPawn::ShouldApplyDriveForces() const
{
	const ENetRole LocalRole = GetLocalRole();
	if (LocalRole == ROLE_Authority || LocalRole == ROLE_AutonomousProxy)
	{
		return true;
	}
	// Simulated proxy on a client: remote car — physics replication drives the body, no local grip.
	return false;
}

void AModularCarPawn::UpdateAsyncDriveForcesFlag()
{
	if (CarAsync)
	{
		CarAsync->bApplyDriveForces = ShouldApplyDriveForces();
	}
}

void AModularCarPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateAsyncDriveForcesFlag();

	if (Body && Body->IsSimulatingPhysics() && IsLocallyControlled()
		&& FMath::Abs(ThrottleInput_External) > 0.01f
		&& Body->GetPhysicsLinearVelocity().SizeSquared() < FMath::Square(RestSpeedDeadzone))
	{
		Body->WakeAllRigidBodies();
	}

	PollKeyboard();
	RunSelfTest(DeltaTime);

	// Feed local input into the predicted physics sim.
	if (CarAsync && IsLocallyControlled())
	{
		if (FAsyncInputModularCar* AsyncInput = CarAsync->GetProducerInputData_External())
		{
			AsyncInput->Throttle = ThrottleInput_External;
			AsyncInput->Steer = SteerInput_External;
			AsyncInput->Handbrake = HandbrakeInput_External;
		}
	}

	if (bDebugDrive && Body)
	{
		const FString Msg = FString::Printf(TEXT("ModularCar | Thr=%.2f Str=%.2f Hb=%.0f | Z=%.0f Spd=%.0f | %s"),
			ThrottleInput_External, SteerInput_External, HandbrakeInput_External,
			Body->GetComponentLocation().Z, Body->GetComponentVelocity().Size(),
			HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage((uint64)((PTRINT)this), 0.0f, FColor::Green, Msg);
		}
		UE_LOG(LogTemp, Warning, TEXT("%s"), *Msg);
	}
}

void AModularCarPawn::PollKeyboard()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}

	float Throttle = 0.0f;
	float Steer = 0.0f;
	if (PC->IsInputKeyDown(EKeys::W)) { Throttle += 1.0f; }
	if (PC->IsInputKeyDown(EKeys::S)) { Throttle -= 1.0f; }
	if (PC->IsInputKeyDown(EKeys::D)) { Steer += 1.0f; }
	if (PC->IsInputKeyDown(EKeys::A)) { Steer -= 1.0f; }

	SetThrottleInput(Throttle);
	SetSteerInput(Steer);
	SetHandbrake(PC->IsInputKeyDown(EKeys::SpaceBar));

	// Symmetric wheel pairs (replicated): P = add axle (L+R), L = remove last axle.
	if (IsLocallyControlled())
	{
		if (PC->WasInputKeyJustPressed(EKeys::P))
		{
			ServerRequestAddSymmetricWheelPair();
		}
		if (PC->WasInputKeyJustPressed(EKeys::L))
		{
			ServerRequestRemoveSymmetricWheelPair();
		}
	}
}

void AModularCarPawn::RunSelfTest(float DeltaTime)
{
	const int32 Mode = CVarCarSelfTest.GetValueOnGameThread();
	if (Mode == 0 || !Body)
	{
		return;
	}

	// Mode 3: cosmetic-mesh check. Verify the car art loaded and replaced the cube/cylinder proxies,
	// then quit. Catches the "still a cube" failure that the physics tests (1/2) cannot see.
	if (Mode == 3)
	{
		if (!bSelfTestStarted)
		{
			bSelfTestStarted = true;
			SelfTestTime = 0.0f;
		}
		SelfTestTime += DeltaTime;
		if (SelfTestTime < 0.5f)
		{
			return;
		}

		const USkeletalMesh* BV = BodyVisual ? BodyVisual->GetSkeletalMeshAsset() : nullptr;
		const bool bCubeVisible = (Body && Body->IsVisible());
		int32 CylVisible = 0;
		for (const FCarWheel& W : Wheels)
		{
			if (W.Mesh && W.Mesh->IsVisible())
			{
				++CylVisible;
			}
		}

		bool bPass;
		if (bUseVisualMeshes)
		{
			// Cosmetic mode: skeletal car applied, cube + all cylinders hidden.
			bPass = (CarBodyMesh != nullptr) && (BV == CarBodyMesh) && !bCubeVisible
				&& (Wheels.Num() > 0) && (CylVisible == 0);
		}
		else
		{
			// Rolled-back cube mode: plain cube + cylinders visible, no skeletal car.
			bPass = bCubeVisible && (Wheels.Num() > 0) && (CylVisible == Wheels.Num()) && (BV == nullptr);
		}

		UE_LOG(LogTemp, Warning, TEXT("SELFTEST VISUAL | UseVis=%d CarMesh=%s cubeVisible=%d cylindersVisible=%d/%d => %s"),
			bUseVisualMeshes ? 1 : 0,
			CarBodyMesh ? *CarBodyMesh->GetName() : TEXT("NULL"),
			bCubeVisible ? 1 : 0,
			CylVisible, Wheels.Num(),
			bPass ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
		FPlatformMisc::RequestExit(false);
		return;
	}

	// Mode 4: collision test. Spawn a second (passive) car ahead, ram it at full throttle, and verify
	// the physics stays sane - no explosion (huge speed), no launch into the air, no tunnelling
	// through each other, and the target actually gets shoved (not stuck/immovable).
	if (Mode == 4)
	{
		if (!IsLocallyControlled())
		{
			return; // only the driven (player) car orchestrates; the spawned target stays passive
		}

		if (!bSelfTestStarted)
		{
			bSelfTestStarted = true;
			SelfTestTime = 0.0f;
			SelfTestLogAccum = 1.0f;
			CollPeakSpeed = 0.0f;
			CollMaxZ = -BIG_NUMBER;
			CollMinSep = BIG_NUMBER;
			SelfTestStartPos = Body->GetComponentLocation();

			if (UWorld* World = GetWorld())
			{
				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				CollTargetStart = GetActorLocation() + GetActorForwardVector() * 700.0f;
				CollTarget = World->SpawnActor<AModularCarPawn>(GetClass(), CollTargetStart, GetActorRotation(), Params);
			}
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST START | Mode=4 (COLLISION) target=%s"),
				CollTarget ? *CollTarget->GetName() : TEXT("NULL"));
		}

		SelfTestTime += DeltaTime;
		SelfTestLogAccum += DeltaTime;

		const bool bSettled = SelfTestTime > 1.5f;
		ThrottleInput_External = bSettled ? 1.0f : 0.0f; // ram forward after both cars settle
		SteerInput_External = 0.0f;
		HandbrakeInput_External = 0.0f;

		if (bSettled && CollTarget)
		{
			const FVector A = Body->GetComponentLocation();
			const FVector B = CollTarget->GetActorLocation();
			CollPeakSpeed = FMath::Max3(CollPeakSpeed, (float)Body->GetComponentVelocity().Size(), (float)CollTarget->GetVelocity().Size());
			CollMaxZ = FMath::Max3(CollMaxZ, (float)A.Z, (float)B.Z);
			CollMinSep = FMath::Min(CollMinSep, (float)FVector::Dist(A, B));
		}

		if (SelfTestLogAccum >= 0.5f)
		{
			SelfTestLogAccum = 0.0f;
			const FVector A = Body->GetComponentLocation();
			const FVector B = CollTarget ? CollTarget->GetActorLocation() : FVector::ZeroVector;
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST t=%.1f | Self=(%.0f,%.0f,%.0f) Target=(%.0f,%.0f,%.0f) Sep=%.0f"),
				SelfTestTime, A.X, A.Y, A.Z, B.X, B.Y, B.Z, (float)FVector::Dist(A, B));
		}

		if (SelfTestTime >= 7.0f)
		{
			const FVector EndB = CollTarget ? CollTarget->GetActorLocation() : FVector::ZeroVector;
			const float TargetPushed = CollTarget ? (float)FVector::Dist(FVector(EndB.X, EndB.Y, 0), FVector(CollTargetStart.X, CollTargetStart.Y, 0)) : 0.0f;
			const bool bHasTarget = (CollTarget != nullptr);
			const bool bFinite = FMath::IsFinite(CollPeakSpeed) && FMath::IsFinite(CollMaxZ) && FMath::IsFinite(CollMinSep);
			const float SpeedTol = 2500.0f;          // explosion if either car flies off this fast
			const float ZTol = SelfTestStartPos.Z + 300.0f; // launched into the air
			const float SepTol = 150.0f;             // tunnelled through each other if closer than this
			const float PushTol = 50.0f;             // target must actually be shoved (not stuck)
			const bool bPass = bHasTarget && bFinite
				&& (CollPeakSpeed <= SpeedTol)
				&& (CollMaxZ <= ZTol)
				&& (CollMinSep >= SepTol)
				&& (TargetPushed >= PushTol);
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST COLLISION | PeakSpeed=%.0f (<=%.0f) MaxZ=%.0f (<=%.0f) MinSep=%.0f (>=%.0f) TargetPushed=%.0f (>=%.0f) => %s"),
				CollPeakSpeed, SpeedTol, CollMaxZ, ZTol, CollMinSep, SepTol, TargetPushed, PushTol, bPass ? TEXT("PASS") : TEXT("FAIL"));
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
			FPlatformMisc::RequestExit(false);
		}
		return;
	}

	// Mode 5: adversarial collision. Target spawned ahead AND offset sideways (corner/side contact),
	// rammed at full throttle, then the rammer REVERSES away. Catches lateral sticking and "clinging"
	// (the rammed car following when you back off) that the clean rear-end test (mode 4) misses.
	if (Mode == 5)
	{
		if (!IsLocallyControlled())
		{
			return;
		}

		if (!bSelfTestStarted)
		{
			bSelfTestStarted = true;
			SelfTestTime = 0.0f;
			SelfTestLogAccum = 1.0f;
			CollPeakSpeed = 0.0f;
			CollMaxZ = -BIG_NUMBER;
			CollMinSep = BIG_NUMBER;
			CollSepRamEnd = -1.0f;
			SelfTestStartPos = Body->GetComponentLocation();

			if (UWorld* World = GetWorld())
			{
				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				// 550 ahead + 180 to the side so the rammer hits a front corner, not dead-centre.
				CollTargetStart = GetActorLocation() + GetActorForwardVector() * 550.0f + GetActorRightVector() * 180.0f;
				CollTarget = World->SpawnActor<AModularCarPawn>(GetClass(), CollTargetStart, GetActorRotation(), Params);
			}
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST START | Mode=5 (COLLISION-ADV) target=%s"),
				CollTarget ? *CollTarget->GetName() : TEXT("NULL"));
		}

		SelfTestTime += DeltaTime;
		SelfTestLogAccum += DeltaTime;

		// Phases: settle (0-1.5s), ram forward (1.5-4.5s), reverse away (4.5-7.5s).
		const bool bRam = (SelfTestTime > 1.5f && SelfTestTime <= 4.5f);
		const bool bRetreat = (SelfTestTime > 4.5f);
		ThrottleInput_External = bRetreat ? -1.0f : (bRam ? 1.0f : 0.0f);
		SteerInput_External = 0.0f;
		HandbrakeInput_External = 0.0f;

		if (CollTarget && (bRam || bRetreat))
		{
			const FVector A = Body->GetComponentLocation();
			const FVector B = CollTarget->GetActorLocation();
			CollPeakSpeed = FMath::Max3(CollPeakSpeed, (float)Body->GetComponentVelocity().Size(), (float)CollTarget->GetVelocity().Size());
			CollMaxZ = FMath::Max3(CollMaxZ, (float)A.Z, (float)B.Z);
			CollMinSep = FMath::Min(CollMinSep, (float)FVector::Dist(A, B));
		}
		// Capture separation right when the ram ends / retreat begins.
		if (CollSepRamEnd < 0.0f && SelfTestTime > 4.5f && CollTarget)
		{
			CollSepRamEnd = (float)FVector::Dist(Body->GetComponentLocation(), CollTarget->GetActorLocation());
		}

		if (SelfTestLogAccum >= 0.5f)
		{
			SelfTestLogAccum = 0.0f;
			const FVector A = Body->GetComponentLocation();
			const FVector B = CollTarget ? CollTarget->GetActorLocation() : FVector::ZeroVector;
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST t=%.1f (%s) | Self=(%.0f,%.0f,%.0f) Target=(%.0f,%.0f,%.0f) Sep=%.0f"),
				SelfTestTime, bRetreat ? TEXT("retreat") : (bRam ? TEXT("ram") : TEXT("settle")),
				A.X, A.Y, A.Z, B.X, B.Y, B.Z, (float)FVector::Dist(A, B));
		}

		if (SelfTestTime >= 7.5f)
		{
			const float EndSep = CollTarget ? (float)FVector::Dist(Body->GetComponentLocation(), CollTarget->GetActorLocation()) : 0.0f;
			// Clinging: after retreating, the gap must GROW vs when the ram ended (target not dragged).
			const float SepGain = (CollSepRamEnd > 0.0f) ? (EndSep - CollSepRamEnd) : 0.0f;
			const bool bHasTarget = (CollTarget != nullptr);
			const bool bFinite = FMath::IsFinite(CollPeakSpeed) && FMath::IsFinite(CollMaxZ) && FMath::IsFinite(EndSep);
			const float SpeedTol = 2500.0f;
			const float ZTol = SelfTestStartPos.Z + 300.0f;
			const float SepTol = 150.0f;
			const float SepGainTol = 100.0f; // must pull apart by at least this on retreat (no clinging)
			const bool bPass = bHasTarget && bFinite
				&& (CollPeakSpeed <= SpeedTol)
				&& (CollMaxZ <= ZTol)
				&& (CollMinSep >= SepTol)
				&& (SepGain >= SepGainTol);
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST COLLISION-ADV | PeakSpeed=%.0f (<=%.0f) MaxZ=%.0f (<=%.0f) MinSep=%.0f (>=%.0f) SepGainOnRetreat=%.0f (>=%.0f) => %s"),
				CollPeakSpeed, SpeedTol, CollMaxZ, ZTol, CollMinSep, SepTol, SepGain, SepGainTol, bPass ? TEXT("PASS") : TEXT("FAIL"));
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
			FPlatformMisc::RequestExit(false);
		}
		return;
	}

	// Mode 6: NETWORKED collision test. Run across a dedicated server + client. The server spawns a
	// stationary target car ahead of the (client-controlled) player car; on the client that target is
	// a sim-proxy. The client rams it then retreats - this is the predicted-body-vs-sim-proxy contact
	// that sticks in multiplayer, which standalone modes 4/5 cannot exercise.
	if (Mode == 6)
	{
		const UWorld* World = GetWorld();
		const bool bNetworked = World && (World->GetNetMode() != NM_Standalone);

		// --- Server side: spawn one stationary target ahead of the real player's car. ---
		if (HasAuthority())
		{
			if (bIsNetTarget || GetController() == nullptr)
			{
				return; // only the real player's (controller-owned) pawn spawns; targets stay passive
			}
			NetTime += DeltaTime;
			if (!bNetServerSpawned && NetTime > 1.5f) // let the player car settle first
			{
				bNetServerSpawned = true;
				if (UWorld* W = GetWorld())
				{
					FActorSpawnParameters Params;
					Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					const FVector Ahead = GetActorLocation() + GetActorForwardVector() * 700.0f;
					if (AModularCarPawn* T = W->SpawnActor<AModularCarPawn>(GetClass(), Ahead, GetActorRotation(), Params))
					{
						T->bIsNetTarget = true;
						UE_LOG(LogTemp, Warning, TEXT("SELFTEST NET | SERVER spawned target %s at %s"), *T->GetName(), *Ahead.ToString());
					}
				}
			}
			return; // server never drives; the client's replicated input moves the player car
		}

		// --- Client side: only the locally controlled player car drives & measures. ---
		if (!IsLocallyControlled())
		{
			return;
		}

		// Find the sim-proxy target (the other modular car).
		if (!CollTarget)
		{
			for (TActorIterator<AModularCarPawn> It(GetWorld()); It; ++It)
			{
				if (*It != this) { CollTarget = *It; break; }
			}
		}

		if (!bSelfTestStarted)
		{
			bSelfTestStarted = true;
			SelfTestTime = 0.0f;
			SelfTestLogAccum = 1.0f;
			CollPeakSpeed = 0.0f;
			CollMaxZ = -BIG_NUMBER;
			CollMinSep = BIG_NUMBER;
			CollSepRamEnd = -1.0f;
			CollTargetJitter = 0.0f;
			CollTargetMaxMove = 0.0f;
			bCollTargetPosInit = false;
			CollTargetInitialPos = FVector::ZeroVector;
			bCollTargetInitialPosSet = false;
			SelfTestStartPos = Body->GetComponentLocation();
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST START | Mode=6 (NETCOLLISION) netmode=%d target=%s"),
				(int32)(World ? World->GetNetMode() : NM_Standalone), CollTarget ? *CollTarget->GetName() : TEXT("WAITING"));
		}

		SelfTestTime += DeltaTime;
		SelfTestLogAccum += DeltaTime;

		// Wait for the target to replicate in, then: ram (full throttle), then retreat (reverse).
		const bool bHaveTarget = (CollTarget != nullptr);
		const bool bRam = bHaveTarget && (SelfTestTime > 2.0f && SelfTestTime <= 5.0f);
		const bool bRetreat = bHaveTarget && (SelfTestTime > 5.0f);
		ThrottleInput_External = bRetreat ? -1.0f : (bRam ? 1.0f : 0.0f);
		SteerInput_External = 0.0f;
		HandbrakeInput_External = 0.0f;

		if (bHaveTarget)
		{
			const FVector B = CollTarget->GetActorLocation();
			if (!bCollTargetInitialPosSet)
			{
				CollTargetInitialPos = B;
				bCollTargetInitialPosSet = true;
			}
			CollTargetMaxMove = FMath::Max(CollTargetMaxMove, (float)FVector::Dist(B, CollTargetInitialPos));
		}

		if (bHaveTarget && (bRam || bRetreat))
		{
			const FVector A = Body->GetComponentLocation();
			const FVector B = CollTarget->GetActorLocation();
			CollPeakSpeed = FMath::Max3(CollPeakSpeed, (float)Body->GetComponentVelocity().Size(), (float)CollTarget->GetVelocity().Size());
			CollMaxZ = FMath::Max3(CollMaxZ, (float)A.Z, (float)B.Z);
			CollMinSep = FMath::Min(CollMinSep, (float)FVector::Dist(A, B));
			// Sim-proxy jitter: frame-to-frame movement of the target while in/around contact.
			if (bCollTargetPosInit)
			{
				CollTargetJitter = FMath::Max(CollTargetJitter, (float)FVector::Dist(B, CollPrevTargetPos));
			}
			CollPrevTargetPos = B;
			bCollTargetPosInit = true;
		}
		if (CollSepRamEnd < 0.0f && SelfTestTime > 5.0f && bHaveTarget)
		{
			CollSepRamEnd = (float)FVector::Dist(Body->GetComponentLocation(), CollTarget->GetActorLocation());
		}

		if (SelfTestLogAccum >= 0.5f)
		{
			SelfTestLogAccum = 0.0f;
			const FVector A = Body->GetComponentLocation();
			const FVector B = bHaveTarget ? CollTarget->GetActorLocation() : FVector::ZeroVector;
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST t=%.1f (%s) | Self=(%.0f,%.0f,%.0f) Target=(%.0f,%.0f,%.0f) Sep=%.0f"),
				SelfTestTime, bRetreat ? TEXT("retreat") : (bRam ? TEXT("ram") : TEXT("wait")),
				A.X, A.Y, A.Z, B.X, B.Y, B.Z, bHaveTarget ? (float)FVector::Dist(A, B) : -1.0f);
		}

		if (SelfTestTime >= 9.0f)
		{
			const float EndSep = bHaveTarget ? (float)FVector::Dist(Body->GetComponentLocation(), CollTarget->GetActorLocation()) : 0.0f;
			const float SepGain = (CollSepRamEnd > 0.0f) ? (EndSep - CollSepRamEnd) : 0.0f;
			const bool bFinite = FMath::IsFinite(CollPeakSpeed) && FMath::IsFinite(CollMinSep) && FMath::IsFinite(EndSep);
			const float SpeedTol = 2500.0f;
			const float SepTol = 150.0f;       // interpenetration if closer than this
			const float JitterTol = 60.0f;     // sim-proxy frame jitter (sticking/fighting) cap
			const float SepGainTol = 100.0f;   // must pull apart on retreat (no clinging)
			const float TargetMoveTol = 15.0f; // sim-proxy must follow server push (frozen if ~0)
			const bool bPass = bHaveTarget && bFinite
				&& (CollPeakSpeed <= SpeedTol)
				&& (CollMinSep >= SepTol)
				&& (CollTargetJitter <= JitterTol)
				&& (CollTargetMaxMove >= TargetMoveTol)
				&& (SepGain >= SepGainTol);
			const int32 TargetRole = CollTarget ? (int32)CollTarget->GetLocalRole() : -1;
			const int32 TargetRepMode = CollTarget ? (int32)CollTarget->GetPhysicsReplicationMode() : -1;
			const bool bSimProxyOk = CollTarget && (CollTarget->GetLocalRole() == ROLE_SimulatedProxy)
				&& (CollTarget->GetPhysicsReplicationMode() == EPhysicsReplicationMode::PredictiveInterpolation);
			const bool bPassWithProxy = bPass && bSimProxyOk;
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST NETCOLLISION | HaveTarget=%d TargetRole=%d TargetRepMode=%d PeakSpeed=%.0f (<=%.0f) MinSep=%.0f (>=%.0f) TargetJitter=%.1f (<=%.0f) TargetMove=%.0f (>=%.0f) SepGainOnRetreat=%.0f (>=%.0f) => %s"),
				bHaveTarget ? 1 : 0, TargetRole, TargetRepMode, CollPeakSpeed, SpeedTol, CollMinSep, SepTol, CollTargetJitter, JitterTol, CollTargetMaxMove, TargetMoveTol, SepGain, SepGainTol, bPassWithProxy ? TEXT("PASS") : TEXT("FAIL"));
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
			FPlatformMisc::RequestExit(false);
		}
		return;
	}

	// Mode 9: server drives a spawned target (authority); client watches sim-proxy follow without contact.
	if (Mode == 9)
	{
		const UWorld* World = GetWorld();
		const bool bNetworked = World && (World->GetNetMode() != NM_Standalone);
		if (!bNetworked)
		{
			return;
		}

		if (HasAuthority() && bIsNetTarget)
		{
			ThrottleInput_External = 1.0f;
			SteerInput_External = 0.0f;
			HandbrakeInput_External = 0.0f;
			if (CarAsync)
			{
				if (FAsyncInputModularCar* AsyncInput = CarAsync->GetProducerInputData_External())
				{
					AsyncInput->Throttle = 1.0f;
					AsyncInput->Steer = 0.0f;
					AsyncInput->Handbrake = 0.0f;
				}
			}
			return;
		}

		if (HasAuthority())
		{
			if (bIsNetTarget || GetController() == nullptr)
			{
				return;
			}
			NetTime += DeltaTime;
			if (!bNetServerSpawned && NetTime > 1.5f)
			{
				bNetServerSpawned = true;
				if (UWorld* W = GetWorld())
				{
					FActorSpawnParameters Params;
					Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					const FVector Ahead = GetActorLocation() + GetActorForwardVector() * 1200.0f;
					if (AModularCarPawn* T = W->SpawnActor<AModularCarPawn>(GetClass(), Ahead, GetActorRotation(), Params))
					{
						T->bIsNetTarget = true;
						UE_LOG(LogTemp, Warning, TEXT("SELFTEST NET | SERVER spawned drive-target %s at %s"), *T->GetName(), *Ahead.ToString());
					}
				}
			}
			return;
		}

		if (!IsLocallyControlled())
		{
			return;
		}

		if (!CollTarget)
		{
			for (TActorIterator<AModularCarPawn> It(GetWorld()); It; ++It)
			{
				if (*It != this && !It->IsLocallyControlled()) { CollTarget = *It; break; }
			}
		}

		if (!bSelfTestStarted)
		{
			bSelfTestStarted = true;
			SelfTestTime = 0.0f;
			SelfTestLogAccum = 1.0f;
			CollTargetMaxMove = 0.0f;
			bCollTargetInitialPosSet = false;
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST START | Mode=9 (REMOTE-DRIVE) target=%s"),
				CollTarget ? *CollTarget->GetName() : TEXT("WAITING"));
		}

		SelfTestTime += DeltaTime;
		SelfTestLogAccum += DeltaTime;
		ThrottleInput_External = 0.0f;
		SteerInput_External = 0.0f;
		HandbrakeInput_External = 0.0f;

		if (CollTarget)
		{
			const FVector B = CollTarget->GetActorLocation();
			if (!bCollTargetInitialPosSet)
			{
				CollTargetInitialPos = B;
				bCollTargetInitialPosSet = true;
			}
			CollTargetMaxMove = FMath::Max(CollTargetMaxMove, (float)FVector::Dist(B, CollTargetInitialPos));
		}

		if (SelfTestLogAccum >= 0.5f)
		{
			SelfTestLogAccum = 0.0f;
			const FVector B = CollTarget ? CollTarget->GetActorLocation() : FVector::ZeroVector;
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST t=%.1f (observe) | Target=(%.0f,%.0f,%.0f) Move=%.0f Role=%d RepMode=%d"),
				SelfTestTime, B.X, B.Y, B.Z, CollTargetMaxMove,
				CollTarget ? (int32)CollTarget->GetLocalRole() : -1,
				CollTarget ? (int32)CollTarget->GetPhysicsReplicationMode() : -1);
		}

		if (SelfTestTime >= 8.0f)
		{
			const float TargetMoveTol = 120.0f;
			const int32 TargetRole = CollTarget ? (int32)CollTarget->GetLocalRole() : -1;
			const int32 TargetRepMode = CollTarget ? (int32)CollTarget->GetPhysicsReplicationMode() : -1;
			const bool bSimProxyOk = CollTarget && (CollTarget->GetLocalRole() == ROLE_SimulatedProxy)
				&& (CollTarget->GetPhysicsReplicationMode() == EPhysicsReplicationMode::PredictiveInterpolation);
			const bool bPass = CollTarget && bSimProxyOk && (CollTargetMaxMove >= TargetMoveTol);
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST REMOTEDRIVE | HaveTarget=%d TargetRole=%d TargetRepMode=%d TargetMove=%.0f (>=%.0f) => %s"),
				CollTarget ? 1 : 0, TargetRole, TargetRepMode, CollTargetMaxMove, TargetMoveTol, bPass ? TEXT("PASS") : TEXT("FAIL"));
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
			FPlatformMisc::RequestExit(false);
		}
		return;
	}

	// Mode 8: add symmetric wheel pair mid-session, settle, then drive — catches sleep-after-weld bug.
	if (Mode == 8)
	{
		if (!bSelfTestStarted)
		{
			bSelfTestStarted = true;
			SelfTestTime = 0.0f;
			SelfTestLogAccum = 1.0f;
			bSelfTestWheelPairAdded = false;
			bSelfTestDrivePhase = false;
			RestPeakSpeed = 0.0f;
			SelfTestStartPos = Body->GetComponentLocation();
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST START | Mode=8 (ADDWHEEL-SLEEP) Wheels=%d"), Wheels.Num());
		}

		SelfTestTime += DeltaTime;
		SelfTestLogAccum += DeltaTime;

		if (SelfTestTime >= 2.0f && !bSelfTestWheelPairAdded && HasAuthority())
		{
			const int32 WheelsBefore = Wheels.Num();
			bSelfTestWheelPairAdded = AddSymmetricWheelPairAuthority();
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST ADDWHEEL | ok=%d wheels %d -> %d"),
				bSelfTestWheelPairAdded ? 1 : 0, WheelsBefore, Wheels.Num());
		}

		if (SelfTestTime >= 3.5f && bSelfTestWheelPairAdded)
		{
			if (!bSelfTestDrivePhase)
			{
				bSelfTestDrivePhase = true;
				SelfTestDriveStartPos = Body->GetComponentLocation();
			}
			ThrottleInput_External = 1.0f;
			SteerInput_External = 0.0f;
			HandbrakeInput_External = 0.0f;
			RestPeakSpeed = FMath::Max(RestPeakSpeed, (float)Body->GetComponentVelocity().Size());
		}
		else
		{
			ThrottleInput_External = 0.0f;
			SteerInput_External = 0.0f;
			HandbrakeInput_External = 0.0f;
		}

		if (SelfTestLogAccum >= 0.5f)
		{
			SelfTestLogAccum = 0.0f;
			const FVector Pos = Body->GetComponentLocation();
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST t=%.1f | Wheels=%d Pos=(%.0f,%.0f,%.0f) Spd=%.0f"),
				SelfTestTime, Wheels.Num(), Pos.X, Pos.Y, Pos.Z, Body->GetComponentVelocity().Size());
		}

		if (SelfTestTime >= 8.0f)
		{
			const FVector EndPos = Body->GetComponentLocation();
			const float MovedXY = bSelfTestDrivePhase
				? (float)FVector::Dist2D(EndPos, SelfTestDriveStartPos) : 0.0f;
			const float SpeedTol = 40.0f;
			const float MoveTol = 120.0f;
			const bool bPass = bSelfTestWheelPairAdded && bSelfTestDrivePhase
				&& (Wheels.Num() == 6)
				&& (RestPeakSpeed >= SpeedTol)
				&& (MovedXY >= MoveTol);
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST ADDWHEEL | Wheels=%d PeakSpeed=%.0f (>=%.0f) MovedXY=%.0f (>=%.0f) => %s"),
				Wheels.Num(), RestPeakSpeed, SpeedTol, MovedXY, MoveTol, bPass ? TEXT("PASS") : TEXT("FAIL"));
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
			FPlatformMisc::RequestExit(false);
		}
		return;
	}

	// Mode 7: DUAL-CLIENT head-on (both player cars are predicted on their owning client). Run with
	// dedicated server + 2 clients; each client drives forward into the other, then reverses.
	if (Mode == 7)
	{
		const UWorld* World = GetWorld();
		if (!World || World->GetNetMode() == NM_Standalone)
		{
			return;
		}
		if (!IsLocallyControlled() || bIsNetTarget)
		{
			return;
		}

		if (!CollTarget)
		{
			// Remote player pawns are sim-proxies on this client — they have no local Controller.
			for (TActorIterator<AModularCarPawn> It(GetWorld()); It; ++It)
			{
				if (*It != this && !It->IsLocallyControlled() && !It->bIsNetTarget)
				{
					CollTarget = *It;
					break;
				}
			}
		}

		if (!bSelfTestStarted)
		{
			bSelfTestStarted = true;
			SelfTestTime = 0.0f;
			SelfTestLogAccum = 1.0f;
			CollPeakSpeed = 0.0f;
			CollMinSep = BIG_NUMBER;
			CollSepRamEnd = -1.0f;
			CollTargetJitter = 0.0f;
			bCollTargetPosInit = false;
			NetDualSepAtContact = 0.0f;
			SelfTestStartPos = Body->GetComponentLocation();
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST START | Mode=7 (NETDUAL) netmode=%d target=%s"),
				(int32)World->GetNetMode(), CollTarget ? *CollTarget->GetName() : TEXT("WAITING"));
		}

		SelfTestTime += DeltaTime;
		SelfTestLogAccum += DeltaTime;

		const bool bHaveTarget = (CollTarget != nullptr);
		const bool bRam = bHaveTarget && (SelfTestTime > 2.0f && SelfTestTime <= 5.0f);
		const bool bRetreat = bHaveTarget && (SelfTestTime > 5.0f);
		ThrottleInput_External = 0.0f;
		SteerInput_External = 0.0f;
		HandbrakeInput_External = 0.0f;
		if (bHaveTarget && (bRam || bRetreat))
		{
			FVector ToOther = CollTarget->GetActorLocation() - Body->GetComponentLocation();
			ToOther.Z = 0.0f;
			if (!ToOther.IsNearlyZero())
			{
				const FVector Fwd = Body->GetForwardVector().GetSafeNormal2D();
				const FVector Dir = ToOther.GetSafeNormal();
				const float SignedSteer = FMath::Clamp(FVector::CrossProduct(Fwd, Dir).Z, -1.0f, 1.0f);
				const float Align = FVector::DotProduct(Fwd, Dir);
				SteerInput_External = SignedSteer;
				ThrottleInput_External = bRetreat ? -1.0f : FMath::Clamp(Align, 0.25f, 1.0f);
			}
			else if (bRetreat)
			{
				ThrottleInput_External = -1.0f;
			}
			else if (bRam)
			{
				ThrottleInput_External = 1.0f;
			}
		}

		if (bHaveTarget && (bRam || bRetreat))
		{
			const FVector A = Body->GetComponentLocation();
			const FVector B = CollTarget->GetActorLocation();
			CollPeakSpeed = FMath::Max3(CollPeakSpeed, (float)Body->GetComponentVelocity().Size(), (float)CollTarget->GetVelocity().Size());
			CollMinSep = FMath::Min(CollMinSep, (float)FVector::Dist(A, B));
			if (bCollTargetPosInit)
			{
				CollTargetJitter = FMath::Max(CollTargetJitter, (float)FVector::Dist(B, CollPrevTargetPos));
			}
			CollPrevTargetPos = B;
			bCollTargetPosInit = true;
		}
		if (NetDualSepAtContact <= 0.0f && bRam && bHaveTarget && CollMinSep < 500.0f)
		{
			NetDualSepAtContact = CollMinSep;
		}
		if (CollSepRamEnd < 0.0f && SelfTestTime > 5.0f && bHaveTarget)
		{
			CollSepRamEnd = (float)FVector::Dist(Body->GetComponentLocation(), CollTarget->GetActorLocation());
		}

		if (SelfTestLogAccum >= 0.5f)
		{
			SelfTestLogAccum = 0.0f;
			const FVector A = Body->GetComponentLocation();
			const FVector B = bHaveTarget ? CollTarget->GetActorLocation() : FVector::ZeroVector;
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST t=%.1f (%s) | Self=(%.0f,%.0f,%.0f) Other=(%.0f,%.0f,%.0f) Sep=%.0f"),
				SelfTestTime, bRetreat ? TEXT("retreat") : (bRam ? TEXT("ram") : TEXT("wait")),
				A.X, A.Y, A.Z, B.X, B.Y, B.Z, bHaveTarget ? (float)FVector::Dist(A, B) : -1.0f);
		}

		if (SelfTestTime >= 9.0f)
		{
			const float EndSep = bHaveTarget ? (float)FVector::Dist(Body->GetComponentLocation(), CollTarget->GetActorLocation()) : 0.0f;
			const float SepGain = (CollSepRamEnd > 0.0f) ? (EndSep - CollSepRamEnd) : 0.0f;
			const bool bFinite = FMath::IsFinite(CollPeakSpeed) && FMath::IsFinite(CollMinSep) && FMath::IsFinite(EndSep);
			const float SpeedTol = 2500.0f;
			const float SepTol = 150.0f;
			const float JitterTol = 80.0f;
			const float SepGainTol = 80.0f;
			const bool bPass = bHaveTarget && bFinite
				&& (CollPeakSpeed <= SpeedTol)
				&& (CollMinSep >= SepTol)
				&& (CollTargetJitter <= JitterTol)
				&& (SepGain >= SepGainTol);
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST NETDUAL | HaveOther=%d PeakSpeed=%.0f MinSep=%.0f OtherJitter=%.1f SepGainOnRetreat=%.0f => %s"),
				bHaveTarget ? 1 : 0, CollPeakSpeed, CollMinSep, CollTargetJitter, SepGain, bPass ? TEXT("PASS") : TEXT("FAIL"));
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
			FPlatformMisc::RequestExit(false);
		}
		return;
	}

	const bool bRestTest = (Mode == 2);

	if (!bSelfTestStarted)
	{
		bSelfTestStarted = true;
		SelfTestTime = 0.0f;
		SelfTestLogAccum = 1.0f;
		SelfTestStartPos = Body->GetComponentLocation();
		SelfTestMinZ = BIG_NUMBER;
		SelfTestMaxZ = -BIG_NUMBER;
		RestPeakSpeed = 0.0f;
		RestMinZ = BIG_NUMBER;
		RestMaxZ = -BIG_NUMBER;
		RestPeakAngSpeed = 0.0f;
		RestMaxTilt = 0.0f;
		SelfTestPeakYawRate = 0.0f;
		UE_LOG(LogTemp, Warning, TEXT("SELFTEST START | Mode=%d (%s) StartPos=%s Wheels=%d"),
			Mode, bRestTest ? TEXT("REST") : TEXT("DRIVE"), *SelfTestStartPos.ToString(), Wheels.Num());
	}

	const float SettleTime = bRestTest ? 4.0f : 1.0f;
	const bool bSettled = SelfTestTime > SettleTime;

	if (bRestTest)
	{
		ThrottleInput_External = 0.0f;
		SteerInput_External = 0.0f;
		HandbrakeInput_External = 0.0f;
	}
	else
	{
		ThrottleInput_External = bSettled ? 0.6f : 0.0f;
		SteerInput_External = bSettled ? 0.6f : 0.0f;
		HandbrakeInput_External = 0.0f;
	}

	SelfTestTime += DeltaTime;
	SelfTestLogAccum += DeltaTime;

	if (bSettled)
	{
		const float ZNow = Body->GetComponentLocation().Z;
		const float SpeedNow = Body->GetComponentVelocity().Size();
		SelfTestMinZ = FMath::Min(SelfTestMinZ, ZNow);
		SelfTestMaxZ = FMath::Max(SelfTestMaxZ, ZNow);
		if (bRestTest)
		{
			RestPeakSpeed = FMath::Max(RestPeakSpeed, SpeedNow);
			RestMinZ = FMath::Min(RestMinZ, ZNow);
			RestMaxZ = FMath::Max(RestMaxZ, ZNow);
			// Rotational jitter: angular speed and lean off vertical (cube/cylinders alone are dead
			// stable, so any wobble here points at the cosmetic mesh's physics state interfering).
			RestPeakAngSpeed = FMath::Max(RestPeakAngSpeed, (float)Body->GetPhysicsAngularVelocityInDegrees().Size());
			const float UpZ = FMath::Clamp((float)GetActorUpVector().Z, -1.0f, 1.0f);
			RestMaxTilt = FMath::Max(RestMaxTilt, FMath::RadiansToDegrees(FMath::Acos(UpZ)));
		}
		else
		{
			// How fast the car yaws under full steer = the felt "steering angle".
			const float YawRate = FMath::Abs(Body->GetPhysicsAngularVelocityInDegrees().Z);
			SelfTestPeakYawRate = FMath::Max(SelfTestPeakYawRate, YawRate);
		}
	}

	if (SelfTestLogAccum >= 0.5f)
	{
		SelfTestLogAccum = 0.0f;
		const FVector Pos = Body->GetComponentLocation();
		UE_LOG(LogTemp, Warning, TEXT("SELFTEST t=%.1f | Pos=(%.0f,%.0f,%.0f) Speed=%.0f"),
			SelfTestTime, Pos.X, Pos.Y, Pos.Z, Body->GetComponentVelocity().Size());
	}

	const float Duration = bRestTest ? 9.0f : 8.0f;
	if (SelfTestTime >= Duration)
	{
		const FVector EndPos = Body->GetComponentLocation();
		const FVector Delta = EndPos - SelfTestStartPos;
		UE_LOG(LogTemp, Warning, TEXT("SELFTEST RESULT | Start=%s End=%s MovedXY=%.1f MovedZ=%.1f"),
			*SelfTestStartPos.ToString(), *EndPos.ToString(), FVector(Delta.X, Delta.Y, 0).Size(), Delta.Z);

		if (bRestTest)
		{
			const float RestBand = RestMaxZ - RestMinZ;
			const float SpeedTol = 5.0f;
			const float BandTol = 3.0f;
			const float AngSpeedTol = 5.0f; // deg/s
			const float TiltTol = 2.0f;      // deg
			const bool bPass = (RestPeakSpeed <= SpeedTol) && (RestBand <= BandTol)
				&& (RestPeakAngSpeed <= AngSpeedTol) && (RestMaxTilt <= TiltTol);
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST REST | PeakSpeed=%.1f (<= %.0f) ZBand=%.1f (<= %.0f) PeakAngSpeed=%.1f (<= %.0f) MaxTilt=%.1f (<= %.0f) => %s"),
				RestPeakSpeed, SpeedTol, RestBand, BandTol, RestPeakAngSpeed, AngSpeedTol, RestMaxTilt, TiltTol, bPass ? TEXT("PASS") : TEXT("FAIL"));
		}
		else
		{
			const float RideBand = SelfTestMaxZ - SelfTestMinZ;
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST RIDE | MinZ=%.0f MaxZ=%.0f Band=%.0f PeakYawRate=%.1f deg/s (MaxYawRate=%.0f SteerRefSpeed=%.0f)"),
				SelfTestMinZ, SelfTestMaxZ, RideBand, SelfTestPeakYawRate, MaxYawRate, SteerRefSpeed);
		}

		UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
		FPlatformMisc::RequestExit(false);
	}
}

void AModularCarPawn::EnsureInputAssets()
{
	if (InputMapping)
	{
		return;
	}

	ThrottleAction = NewObject<UInputAction>(this, TEXT("IA_Car_Throttle"));
	ThrottleAction->ValueType = EInputActionValueType::Axis1D;
	SteerAction = NewObject<UInputAction>(this, TEXT("IA_Car_Steer"));
	SteerAction->ValueType = EInputActionValueType::Axis1D;
	HandbrakeAction = NewObject<UInputAction>(this, TEXT("IA_Car_Handbrake"));
	HandbrakeAction->ValueType = EInputActionValueType::Boolean;

	InputMapping = NewObject<UInputMappingContext>(this, TEXT("IMC_Car"));
	InputMapping->MapKey(ThrottleAction, EKeys::W);
	{
		FEnhancedActionKeyMapping& M = InputMapping->MapKey(ThrottleAction, EKeys::S);
		M.Modifiers.Add(NewObject<UInputModifierNegate>(this));
	}
	InputMapping->MapKey(SteerAction, EKeys::D);
	{
		FEnhancedActionKeyMapping& M = InputMapping->MapKey(SteerAction, EKeys::A);
		M.Modifiers.Add(NewObject<UInputModifierNegate>(this));
	}
	InputMapping->MapKey(HandbrakeAction, EKeys::SpaceBar);
}

void AModularCarPawn::PawnClientRestart()
{
	Super::PawnClientRestart();
	EnsureInputAssets();

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(InputMapping, 0);
		}
	}
}

void AModularCarPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	EnsureInputAssets();

	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EIC->BindAction(ThrottleAction, ETriggerEvent::Triggered, this, &AModularCarPawn::Input_Throttle);
		EIC->BindAction(ThrottleAction, ETriggerEvent::Completed, this, &AModularCarPawn::Input_Throttle);
		EIC->BindAction(SteerAction, ETriggerEvent::Triggered, this, &AModularCarPawn::Input_Steer);
		EIC->BindAction(SteerAction, ETriggerEvent::Completed, this, &AModularCarPawn::Input_Steer);
		EIC->BindAction(HandbrakeAction, ETriggerEvent::Triggered, this, &AModularCarPawn::Input_Handbrake);
		EIC->BindAction(HandbrakeAction, ETriggerEvent::Completed, this, &AModularCarPawn::Input_Handbrake);
	}
}

void AModularCarPawn::Input_Throttle(const FInputActionValue& Value)
{
	SetThrottleInput(Value.Get<float>());
}

void AModularCarPawn::Input_Steer(const FInputActionValue& Value)
{
	SetSteerInput(Value.Get<float>());
}

void AModularCarPawn::Input_Handbrake(const FInputActionValue& Value)
{
	SetHandbrake(Value.Get<bool>());
}

//----------------------------------------------------
//------------ Networked input payload ----------------
//----------------------------------------------------

void FNetInputModularCar::InterpolateData(const FNetworkPhysicsPayload& MinData, const FNetworkPhysicsPayload& MaxData, float LerpAlpha)
{
	const FNetInputModularCar& Min = static_cast<const FNetInputModularCar&>(MinData);
	const FNetInputModularCar& Max = static_cast<const FNetInputModularCar&>(MaxData);
	Throttle = FMath::Lerp(Min.Throttle, Max.Throttle, LerpAlpha);
	Steer = FMath::Lerp(Min.Steer, Max.Steer, LerpAlpha);
	Handbrake = FMath::Lerp(Min.Handbrake, Max.Handbrake, LerpAlpha);
}

void FNetInputModularCar::MergeData(const FNetworkPhysicsPayload& FromData)
{
	const FNetInputModularCar& From = static_cast<const FNetInputModularCar&>(FromData);
	Throttle = FMath::Abs(From.Throttle) > FMath::Abs(Throttle) ? From.Throttle : Throttle;
	Steer = FMath::Abs(From.Steer) > FMath::Abs(Steer) ? From.Steer : Steer;
	Handbrake = FMath::Max(Handbrake, From.Handbrake);
}

void FNetInputModularCar::DecayData(float DecayAmount)
{
	Throttle *= (1.0f - DecayAmount);
	Steer *= (1.0f - DecayAmount);
	Handbrake *= (1.0f - DecayAmount);
}

bool FNetInputModularCar::CompareData(const FNetworkPhysicsPayload& PredictedData) const
{
	const FNetInputModularCar& P = static_cast<const FNetInputModularCar&>(PredictedData);
	bool bHasDiff = false;
	bHasDiff |= Throttle != P.Throttle;
	bHasDiff |= Steer != P.Steer;
	bHasDiff |= Handbrake != P.Handbrake;
	return (bHasDiff == false);
}
