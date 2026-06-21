// Fill out your copyright notice in the Description page of Project Settings.
// Compiled outside unity builds: ChaosVehicles + ChaosModularVehicle headers collide in merged TUs.

#include "StandardChaosVehiclePawn.h"
#include "Components/SkeletalMeshComponent.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "ChaosVehicleWheel.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "UObject/ConstructorHelpers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputActionValue.h"

static TAutoConsoleVariable<int32> CVarVehicleSelfTest(
	TEXT("vehicle.SelfTest"),
	0,
	TEXT("1=drive forward test (apply throttle, measure speed after time); 0=off"));

AStandardChaosVehiclePawn::AStandardChaosVehiclePawn()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	AutoPossessPlayer = EAutoReceiveInput::Player0;

	// Skeletal mesh for the car body + wheels (visual + some collision)
	VehicleMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("VehicleMesh"));
	RootComponent = VehicleMesh;
	VehicleMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	VehicleMesh->SetCollisionObjectType(ECC_Vehicle);
	VehicleMesh->SetSimulatePhysics(false); // Let the movement component handle physics

	// Load the SportsCar mesh used elsewhere in the project for consistency
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> SportsCarMesh(TEXT("/ChaosModularVehicleExamples/Models/SportsCar/SKM_SportsCar.SKM_SportsCar"));
	if (SportsCarMesh.Succeeded())
	{
		VehicleMesh->SetSkeletalMesh(SportsCarMesh.Object);
	}

	// Standard Chaos Wheeled Vehicle Movement Component (this is the "default Epic way")
	VehicleMovement = CreateDefaultSubobject<UChaosWheeledVehicleMovementComponent>(TEXT("VehicleMovement"));
	VehicleMovement->SetIsReplicated(true);

	// Basic 4-wheel setup using common bone names.
	// If the mesh uses different bone names, the visual wheels won't spin but physics should still work.
	// Adjust in Blueprint or here for your specific mesh.
	VehicleMovement->WheelSetups.SetNum(4);
	// Use the bone names from the SportsCar template (Phys_Wheel_*)
	VehicleMovement->WheelSetups[0].BoneName = TEXT("Phys_Wheel_FL");
	VehicleMovement->WheelSetups[1].BoneName = TEXT("Phys_Wheel_FR");
	VehicleMovement->WheelSetups[2].BoneName = TEXT("Phys_Wheel_RL");
	VehicleMovement->WheelSetups[3].BoneName = TEXT("Phys_Wheel_RR");

	// Set WheelClass for each (using default Chaos wheel)
	// Note: For full functionality, you may need to create custom UChaosVehicleWheel subclasses in BP or C++
	TSubclassOf<class UChaosVehicleWheel> DefaultWheelClass = UChaosVehicleWheel::StaticClass();
	for (auto& Setup : VehicleMovement->WheelSetups)
	{
		Setup.WheelClass = DefaultWheelClass;
	}

	// Simple chase camera (for fair comparison with the custom pawn)
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(VehicleMesh);
	SpringArm->TargetArmLength = 2700.0f;
	SpringArm->SocketOffset = FVector(0.0f, 0.0f, 750.0f);
	SpringArm->bDoCollisionTest = false;
	SpringArm->bEnableCameraLag = false;
	SpringArm->bInheritPitch = false;
	SpringArm->bInheritRoll = false;
	SpringArm->bInheritYaw = true;
	SpringArm->SetRelativeRotation(FRotator(-20.0f, 0.0f, 0.0f));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
}

void AStandardChaosVehiclePawn::BeginPlay()
{
	Super::BeginPlay();

	// Wake up the vehicle and configure for driving (standard Chaos setup)
	if (VehicleMovement)
	{
		VehicleMovement->SetUpdatedComponent(VehicleMesh);

		// Basic engine/transmission to make it drive
		VehicleMovement->EngineSetup.MaxTorque = 800.0f;
		VehicleMovement->EngineSetup.MaxRPM = 6000.0f;
		VehicleMovement->TransmissionSetup.bUseAutomaticGears = true;
		// VehicleMovement->bUseAutomaticGears = true;  // may cause compile error in this UE version, using TransmissionSetup instead
	}

	EnsureInputAssets();
}

void AStandardChaosVehiclePawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	PollKeyboard();
	ApplyVehicleInput();

	RunSelfTest(DeltaTime);
}

void AStandardChaosVehiclePawn::RunSelfTest(float DeltaTime)
{
	const int32 Mode = CVarVehicleSelfTest.GetValueOnGameThread();
	if (Mode == 0 || !VehicleMovement) return;

	if (!bSelfTestStarted)
	{
		bSelfTestStarted = true;
		SelfTestTime = 0.0f;
		SelfTestMaxSpeed = 0.0f;
		UE_LOG(LogTemp, Warning, TEXT("VEHICLE SELFTEST START | Mode=%d (DRIVE)"), Mode);
	}

	// Force drive input for test (overrides poll)
	ThrottleInput = 1.0f;
	SteerInput = 0.0f;
	bHandbrakeInput = false;

	SelfTestTime += DeltaTime;
	SelfTestMaxSpeed = FMath::Max(SelfTestMaxSpeed, VehicleMovement->GetForwardSpeed());

	if (SelfTestTime >= 5.0f)
	{
		const bool bMoved = SelfTestMaxSpeed > 100.0f; // cm/s threshold
		UE_LOG(LogTemp, Warning, TEXT("VEHICLE SELFTEST | MaxSpeed=%.1f (>100?) => %s"), SelfTestMaxSpeed, bMoved ? TEXT("PASS (drives)") : TEXT("FAIL (no drive)"));
		UE_LOG(LogTemp, Warning, TEXT("VEHICLE SELFTEST DONE"));
		FPlatformMisc::RequestExit(false);
	}
}

void AStandardChaosVehiclePawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EIC->BindAction(ThrottleAction, ETriggerEvent::Triggered, this, &AStandardChaosVehiclePawn::Input_Throttle);
		EIC->BindAction(ThrottleAction, ETriggerEvent::Completed, this, &AStandardChaosVehiclePawn::Input_Throttle);
		EIC->BindAction(SteerAction, ETriggerEvent::Triggered, this, &AStandardChaosVehiclePawn::Input_Steer);
		EIC->BindAction(SteerAction, ETriggerEvent::Completed, this, &AStandardChaosVehiclePawn::Input_Steer);
		EIC->BindAction(HandbrakeAction, ETriggerEvent::Triggered, this, &AStandardChaosVehiclePawn::Input_Handbrake);
		EIC->BindAction(HandbrakeAction, ETriggerEvent::Completed, this, &AStandardChaosVehiclePawn::Input_Handbrake);
	}
}

void AStandardChaosVehiclePawn::PollKeyboard()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		ThrottleInput = 0.0f;
		SteerInput = 0.0f;
		bHandbrakeInput = false;
		return;
	}

	ThrottleInput = 0.0f;
	SteerInput = 0.0f;

	if (PC->IsInputKeyDown(EKeys::W)) { ThrottleInput += 1.0f; }
	if (PC->IsInputKeyDown(EKeys::S)) { ThrottleInput -= 1.0f; }
	if (PC->IsInputKeyDown(EKeys::D)) { SteerInput += 1.0f; }
	if (PC->IsInputKeyDown(EKeys::A)) { SteerInput -= 1.0f; }

	bHandbrakeInput = PC->IsInputKeyDown(EKeys::SpaceBar);
}

void AStandardChaosVehiclePawn::ApplyVehicleInput()
{
	if (!VehicleMovement) return;

	VehicleMovement->SetThrottleInput(ThrottleInput);
	VehicleMovement->SetSteeringInput(SteerInput);
	VehicleMovement->SetHandbrakeInput(bHandbrakeInput);
}

void AStandardChaosVehiclePawn::EnsureInputAssets()
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
	InputMapping->MapKey(ThrottleAction, EKeys::S); // negative handled in value?
	{
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(ThrottleAction, EKeys::S);
		Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputMapping));
	}

	InputMapping->MapKey(SteerAction, EKeys::D);
	{
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(SteerAction, EKeys::A);
		Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputMapping));
	}

	InputMapping->MapKey(HandbrakeAction, EKeys::SpaceBar);
}

void AStandardChaosVehiclePawn::PawnClientRestart()
{
	Super::PawnClientRestart();

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC && InputMapping)
	{
		if (auto* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(InputMapping, 0);
		}
	}
}

void AStandardChaosVehiclePawn::Input_Throttle(const FInputActionValue& Value)
{
	ThrottleInput = Value.Get<float>();
}

void AStandardChaosVehiclePawn::Input_Steer(const FInputActionValue& Value)
{
	SteerInput = Value.Get<float>();
}

void AStandardChaosVehiclePawn::Input_Handbrake(const FInputActionValue& Value)
{
	bHandbrakeInput = Value.Get<bool>();
}