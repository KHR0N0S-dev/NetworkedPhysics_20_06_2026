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
#include "HAL/PlatformMisc.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "UObject/ConstructorHelpers.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsSettings.h"

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

static TAutoConsoleVariable<int32> CVarCarSelfTest(
	TEXT("car.SelfTest"),
	0,
	TEXT("Headless self-test: 1 = drive in a circle, 2 = rest-stability check, 3 = cosmetic-mesh check; logs telemetry then quits."),
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

void FModularCarAsync::OnPostInitialize_Internal()
{
	if (PhysicsObject)
	{
		Chaos::FWritePhysicsObjectInterface_Internal Interface = Chaos::FPhysicsObjectInternalInterface::GetWrite();
		if (Chaos::FPBDRigidParticleHandle* ParticleHandle = Interface.GetRigidParticle(PhysicsObject))
		{
			// Never sleep: the prediction layer needs a live particle to resimulate.
			ParticleHandle->SetSleepType(Chaos::ESleepType::NeverSleep);
		}
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

	if (FMath::Abs(RightVel) > RestSpeedDeadzone)
	{
		Force += -Right * (RightVel * Mass * LateralGripRate);
	}

	if ((bHandbrake || FMath::Abs(Throttle) < 0.05f) && FMath::Abs(FwdVel) > RestSpeedDeadzone)
	{
		Force += -Fwd * (FwdVel * Mass * LongitudinalGripRate);
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
	// Movement/state sync is handled by the network physics component (prediction + reconciliation),
	// not by ordinary movement replication. The game mode possesses each player's pawn.
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

	// Networked physics component (only when physics prediction is enabled in project settings).
	if (UPhysicsSettings::Get()->PhysicsPrediction.bEnablePhysicsPrediction)
	{
		static const FName NetworkPhysicsComponentName(TEXT("NetworkPhysicsComponent"));
		NetworkPhysicsComponent = CreateDefaultSubobject<UNetworkPhysicsComponent>(NetworkPhysicsComponentName);
		NetworkPhysicsComponent->SetNetAddressable();
		NetworkPhysicsComponent->SetIsReplicated(true);

		// Drive the body through resimulation-based physics replication. Without this the actor
		// stays on EPhysicsReplicationMode::Default, where replicated targets are snapped onto the
		// body each update instead of being reconciled by prediction - that snapping is the network
		// jitter. Resimulation rewinds & replays from the corrected state, so motion stays smooth.
		SetPhysicsReplicationMode(EPhysicsReplicationMode::Resimulation);
	}
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

	Body->SetRelativeScale3D(BodyExtent / 100.0f);
	Body->SetMassOverrideInKg(NAME_None, BodyMassKg, true);
	Body->SetSimulatePhysics(true);
	Body->SetEnableGravity(true);
	Body->SetLinearDamping(ChassisLinearDamping);
	Body->SetAngularDamping(ChassisAngularDamping);

	// Low-friction chassis so OUR drive/grip forces govern motion. Friction combine = Min so the
	// low value wins over the ground's default (otherwise it averages back up and glues the car).
	if (!ChassisPhysMaterial)
	{
		ChassisPhysMaterial = NewObject<UPhysicalMaterial>(this, TEXT("ModularCarChassisPM"));
		ChassisPhysMaterial->Friction = 0.2f;
		ChassisPhysMaterial->Restitution = 0.0f;
		ChassisPhysMaterial->bOverrideFrictionCombineMode = true;
		ChassisPhysMaterial->FrictionCombineMode = EFrictionCombineMode::Min;
	}
	Body->SetPhysMaterialOverride(ChassisPhysMaterial);

	Body->WakeAllRigidBodies();

	if (bSpawnDefaultWheels)
	{
		const float HX = BodyExtent.X * 0.5f - 70.0f;
		const float HY = BodyExtent.Y * 0.5f + 15.0f;
		const float ZZ = -BodyExtent.Z * 0.5f - 10.0f;

		AddWheel(FVector( HX, -HY, ZZ), true, true);
		AddWheel(FVector( HX,  HY, ZZ), true, true);
		AddWheel(FVector(-HX, -HY, ZZ), true, false);
		AddWheel(FVector(-HX,  HY, ZZ), true, false);
	}

	SettleOntoGround();

	// Hide the collision cube once the car body art is in place (collision/simulation unaffected).
	if (bUseVisualMeshes && CarBodyMesh)
	{
		Body->SetVisibility(false);
	}
	ApplyBodyVisual();

	UE_LOG(LogTemp, Warning, TEXT("CARSETUP | Sim=%d Grav=%d Mass=%.0f Wheels=%d Net=%d"),
		Body->IsSimulatingPhysics() ? 1 : 0, Body->IsGravityEnabled() ? 1 : 0,
		Body->GetMass(), Wheels.Num(), NetworkPhysicsComponent ? 1 : 0);

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

int32 AModularCarPawn::AddWheel(FVector InRelativeLocation, bool bInDriven, bool bInSteering, float InRadius, float InWidth)
{
	if (!Body || !CylinderMesh)
	{
		return INDEX_NONE;
	}

	FCarWheel Wheel;
	Wheel.RelativeLocation = InRelativeLocation;
	Wheel.bDriven = bInDriven;
	Wheel.bSteering = bInSteering;
	Wheel.Radius = InRadius;
	Wheel.Width = InWidth;
	Wheel.Mesh = CreateWheelMesh(InRelativeLocation, InRadius, InWidth);

	return Wheels.Add(Wheel);
}

bool AModularCarPawn::RemoveWheel(int32 WheelIndex)
{
	if (!Wheels.IsValidIndex(WheelIndex))
	{
		return false;
	}
	if (UStaticMeshComponent* Comp = Wheels[WheelIndex].Mesh)
	{
		Comp->DestroyComponent();
	}
	Wheels.RemoveAt(WheelIndex);
	return true;
}

bool AModularCarPawn::RemoveLastWheel()
{
	return RemoveWheel(Wheels.Num() - 1);
}

void AModularCarPawn::PostInitializeComponents()
{
	Super::PostInitializeComponents();

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

						// Copy tuning into the async object (constant + identical server/client).
						const int32 WheelCount = FMath::Max(1, bSpawnDefaultWheels ? 4 : Wheels.Num());
						CarAsync->EngineForceTotal = EngineForcePerWheel * WheelCount;
						CarAsync->LateralGripRate = LateralGripRate;
						CarAsync->LongitudinalGripRate = LongitudinalGripRate;
						CarAsync->MaxYawRateRad = FMath::DegreesToRadians(MaxYawRate);
						CarAsync->SteerRefSpeed = SteerRefSpeed;
						CarAsync->RestSpeedDeadzone = RestSpeedDeadzone;

						if (NetworkPhysicsComponent)
						{
							NetworkPhysicsComponent->CreateDataHistory<FNetInputModularCar, FNetStateModularCar>(CarAsync);
						}
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

void AModularCarPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

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

		// The cosmetic car (skeletal mesh) must be loaded & applied, the cube hidden, and every
		// collision cylinder hidden (the car model covers them).
		const USkeletalMesh* BV = BodyVisual ? BodyVisual->GetSkeletalMeshAsset() : nullptr;
		const bool bBodyOk = (CarBodyMesh != nullptr) && (BV == CarBodyMesh) && Body && !Body->IsVisible();

		int32 CylHidden = 0;
		for (const FCarWheel& W : Wheels)
		{
			if (W.Mesh && !W.Mesh->IsVisible())
			{
				++CylHidden;
			}
		}
		const bool bProxiesHidden = (Wheels.Num() > 0) && (CylHidden == Wheels.Num());
		const bool bPass = bBodyOk && bProxiesHidden;

		UE_LOG(LogTemp, Warning, TEXT("SELFTEST VISUAL | CarMesh=%s BodyArtApplied=%d hiddenCube=%d hiddenCylinders=%d/%d => %s"),
			CarBodyMesh ? *CarBodyMesh->GetName() : TEXT("NULL"),
			(CarBodyMesh != nullptr && BV == CarBodyMesh) ? 1 : 0,
			(Body && !Body->IsVisible()) ? 1 : 0,
			CylHidden, Wheels.Num(),
			bPass ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Warning, TEXT("SELFTEST DONE"));
		FPlatformMisc::RequestExit(false);
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
			const bool bPass = (RestPeakSpeed <= SpeedTol) && (RestBand <= BandTol);
			UE_LOG(LogTemp, Warning, TEXT("SELFTEST REST | PeakSpeed=%.1f (<= %.0f) ZBand=%.1f (<= %.0f) => %s"),
				RestPeakSpeed, SpeedTol, RestBand, BandTol, bPass ? TEXT("PASS") : TEXT("FAIL"));
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
