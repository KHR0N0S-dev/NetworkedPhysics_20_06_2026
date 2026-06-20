// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Physics/NetworkPhysicsComponent.h"
#include "ModularCarPawn.generated.h"

class UStaticMeshComponent;
class UStaticMesh;
class UInputComponent;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;
class USpringArmComponent;
class UCameraComponent;
class UPhysicalMaterial;
class AModularCarPawn;

//----------------------------------------------------
//------------------ Networked physics ----------------
//----------------------------------------------------
// The car is simulated on the physics thread through a Chaos async sim-callback, with client-side
// prediction + server reconciliation driven by UNetworkPhysicsComponent. This mirrors the project's
// reference AMyPhysicsPawn so the modular car behaves identically across server and clients.

// GameThread -> PhysicsThread input.
struct FAsyncInputModularCar : public Chaos::FSimCallbackInput
{
	float Throttle = 0.0f;
	float Steer = 0.0f;
	float Handbrake = 0.0f;

	void Reset()
	{
		Throttle = 0.0f;
		Steer = 0.0f;
		Handbrake = 0.0f;
	}
};

// Replicated input (predicted on clients, corrected by the server).
USTRUCT()
struct FNetInputModularCar : public FNetworkPhysicsPayload
{
	GENERATED_BODY()

	FNetInputModularCar() : Throttle(0.0f), Steer(0.0f), Handbrake(0.0f) {}

	UPROPERTY()
	float Throttle;
	UPROPERTY()
	float Steer;
	UPROPERTY()
	float Handbrake;

	void InterpolateData(const FNetworkPhysicsPayload& MinData, const FNetworkPhysicsPayload& MaxData, float LerpAlpha) override;
	void MergeData(const FNetworkPhysicsPayload& FromData) override;
	void DecayData(float DecayAmount) override;
	bool CompareData(const FNetworkPhysicsPayload& PredictedData) const override;

	const FString DebugData() const override { return FString::Printf(TEXT("Throttle:%f Steer:%f Hb:%f"), Throttle, Steer, Handbrake); }
};

// PhysicsThread -> GameThread output (unused, kept for the callback template).
struct FAsyncOutputModularCar : public Chaos::FSimCallbackOutput
{
	void Reset() {}
};

// Replicated state. The transform/velocity are already synced by the physics network layer, so this
// stays empty (the car has no extra gameplay state to reconcile).
USTRUCT()
struct FNetStateModularCar : public FNetworkPhysicsPayload
{
	GENERATED_BODY()
	FNetStateModularCar() {}
	void InterpolateData(const FNetworkPhysicsPayload& MinData, const FNetworkPhysicsPayload& MaxData, float LerpAlpha) override {}
	bool CompareData(const FNetworkPhysicsPayload& PredictedData) const override { return true; }
};

// Physics-thread callback: applies all the car forces to the chassis particle every physics step.
class FModularCarAsync : public Chaos::TSimCallbackObject<FAsyncInputModularCar, FAsyncOutputModularCar,
	(Chaos::ESimCallbackOptions::Presimulate | Chaos::ESimCallbackOptions::PhysicsObjectUnregister)>
	, TNetworkPhysicsInputState_Internal<FNetInputModularCar, FNetStateModularCar>
{
	friend class AModularCarPawn;
	~FModularCarAsync() {}

	// TSimCallbackObject
	virtual void OnPostInitialize_Internal() override;
	virtual void ProcessInputs_Internal(int32 PhysicsStep) override;
	virtual void OnPreSimulate_Internal() override;
	virtual void OnPhysicsObjectUnregistered_Internal(Chaos::FConstPhysicsObjectHandle InPhysicsObject) override;

	// TNetworkPhysicsInputState_Internal
	virtual void BuildInput_Internal(FNetInputModularCar& Input) const override;
	virtual void ValidateInput_Internal(FNetInputModularCar& Input) const override;
	virtual void ApplyInput_Internal(const FNetInputModularCar& Input) override;
	virtual void BuildState_Internal(FNetStateModularCar& State) const override;
	virtual void ApplyState_Internal(const FNetStateModularCar& State) override;

public:
	Chaos::FConstPhysicsObjectHandle PhysicsObject = nullptr;

	// Tuning copied from the pawn before the sim starts (constant + identical on server/client, so
	// prediction stays deterministic).
	float EngineForceTotal = 1000000.0f;
	float LateralGripRate = 12.0f;
	float LongitudinalGripRate = 4.0f;
	float MaxYawAccelRad = 0.0f;   // rad/s^2 at full steer & full speed
	float YawInertia = 1.0f;       // estimated yaw inertia, to turn the accel into a torque
	float SteerRefSpeed = 350.0f;
	float RestSpeedDeadzone = 3.0f;

private:
	float Throttle_Internal = 0.0f;
	float Steer_Internal = 0.0f;
	float Handbrake_Internal = 0.0f;
};

//----------------------------------------------------
//--------------------- Wheel ------------------------
//----------------------------------------------------

USTRUCT(BlueprintType)
struct FCarWheel
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wheel")
	TObjectPtr<UStaticMeshComponent> Mesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	FVector RelativeLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	bool bDriven = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	bool bSteering = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float Radius = 35.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wheel")
	float Width = 20.0f;
};

//----------------------------------------------------
//--------------------- Pawn -------------------------
//----------------------------------------------------

/**
 * A modular car built from primitives (cube chassis + cylinder wheels). The chassis is one rigid
 * body resting on its four (welded) wheels; drive / steering / grip forces are applied on the
 * physics thread with Chaos networked-physics prediction, so it is fully multiplayer.
 */
UCLASS()
class SANDBOX_API AModularCarPawn : public APawn
{
	GENERATED_BODY()

public:
	AModularCarPawn();

	virtual void Tick(float DeltaTime) override;
	virtual void PostInitializeComponents() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	//-------------------- Runtime build API --------------------

	UFUNCTION(BlueprintCallable, Category = "Modular Car")
	int32 AddWheel(FVector InRelativeLocation, bool bInDriven = true, bool bInSteering = false, float InRadius = 35.0f, float InWidth = 20.0f);

	UFUNCTION(BlueprintCallable, Category = "Modular Car")
	bool RemoveWheel(int32 WheelIndex);

	UFUNCTION(BlueprintCallable, Category = "Modular Car")
	bool RemoveLastWheel();

	UFUNCTION(BlueprintCallable, Category = "Modular Car")
	int32 GetNumWheels() const { return Wheels.Num(); }

	//-------------------- Input (feed the predicted sim) --------------------

	UFUNCTION(BlueprintCallable, Category = "Modular Car")
	void SetThrottleInput(float Value) { ThrottleInput_External = FMath::Clamp(Value, -1.0f, 1.0f); }

	UFUNCTION(BlueprintCallable, Category = "Modular Car")
	void SetSteerInput(float Value) { SteerInput_External = FMath::Clamp(Value, -1.0f, 1.0f); }

	UFUNCTION(BlueprintCallable, Category = "Modular Car")
	void SetHandbrake(bool bEnabled) { HandbrakeInput_External = bEnabled ? 1.0f : 0.0f; }

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Modular Car")
	TObjectPtr<UStaticMeshComponent> Body;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Modular Car")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Modular Car")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Modular Car")
	TArray<FCarWheel> Wheels;

	//-------------------- Tuning --------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Body")
	FVector BodyExtent = FVector(440.0f, 220.0f, 70.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Body")
	float BodyMassKg = 1200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Body")
	bool bSpawnDefaultWheels = true;

	// Angular damping tames chassis tilt under acceleration; a little linear damping settles creep.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Body")
	float ChassisAngularDamping = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Body")
	float ChassisLinearDamping = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Drive")
	float EngineForcePerWheel = 250000.0f;

	// Steering = speed-scaled yaw acceleration (deg/s^2); the car only turns while rolling and
	// reaches full authority at SteerRefSpeed (cm/s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Drive")
	float MaxYawAccel = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Drive")
	float SteerRefSpeed = 350.0f;

	// Grip = frame-rate-independent slip damping rate (1/s): force ~ -slipVel * mass * rate.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Drive", meta = (ClampMin = "0.0"))
	float LateralGripRate = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Drive", meta = (ClampMin = "0.0"))
	float LongitudinalGripRate = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular Car|Drive")
	float RestSpeedDeadzone = 3.0f;

	UPROPERTY(EditAnywhere, Category = "Modular Car|Debug")
	bool bDebugDrive = true;

private:
	void SettleOntoGround();
	void PollKeyboard();
	void RunSelfTest(float DeltaTime);

	void EnsureInputAssets();
	void Input_Throttle(const FInputActionValue& Value);
	void Input_Steer(const FInputActionValue& Value);
	void Input_Handbrake(const FInputActionValue& Value);

	UStaticMeshComponent* CreateWheelMesh(const FVector& RelLoc, float Radius, float Width);

	// Networked physics
	FModularCarAsync* CarAsync = nullptr;

	UPROPERTY()
	TObjectPtr<UNetworkPhysicsComponent> NetworkPhysicsComponent = nullptr;

	UPROPERTY()
	TObjectPtr<UInputMappingContext> InputMapping;
	UPROPERTY()
	TObjectPtr<UInputAction> ThrottleAction;
	UPROPERTY()
	TObjectPtr<UInputAction> SteerAction;
	UPROPERTY()
	TObjectPtr<UInputAction> HandbrakeAction;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;
	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh;
	UPROPERTY()
	TObjectPtr<UPhysicalMaterial> ChassisPhysMaterial;

	// Local input, fed to the predicted sim each frame.
	float ThrottleInput_External = 0.0f;
	float SteerInput_External = 0.0f;
	float HandbrakeInput_External = 0.0f;

	// Distance from chassis centre down to the lowest wheel contact (for spawning on the wheels).
	float RestRideHeight = 35.0f;

	// Self-test (headless), driven by the "car.SelfTest" CVar.
	bool bSelfTestStarted = false;
	float SelfTestTime = 0.0f;
	float SelfTestLogAccum = 0.0f;
	float SelfTestMinZ = 0.0f;
	float SelfTestMaxZ = 0.0f;
	FVector SelfTestStartPos = FVector::ZeroVector;
	float RestPeakSpeed = 0.0f;
	float RestMinZ = 0.0f;
	float RestMaxZ = 0.0f;
};
