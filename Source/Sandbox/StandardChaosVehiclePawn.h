// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "StandardChaosVehiclePawn.generated.h"

class USkeletalMeshComponent;
class UChaosWheeledVehicleMovementComponent;

/**
 * Standard Chaos Vehicles pawn using the default Epic approach (ChaosWheeledVehicleMovementComponent).
 * This is for testing how vehicles work "out of the box" with Chaos from Epic's template/system,
 * as opposed to the custom low-level async + NetworkPhysicsComponent setup.
 */
UCLASS()
class SANDBOX_API AStandardChaosVehiclePawn : public APawn
{
	GENERATED_BODY()

public:
	AStandardChaosVehiclePawn();

	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TObjectPtr<USkeletalMeshComponent> VehicleMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TObjectPtr<UChaosWheeledVehicleMovementComponent> VehicleMovement;

	// Basic camera for testing (similar to the custom car)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<class USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<class UCameraComponent> Camera;

	// Input setup (similar to custom pawn for consistency)
	UPROPERTY()
	TObjectPtr<class UInputMappingContext> InputMapping;

	UPROPERTY()
	TObjectPtr<class UInputAction> ThrottleAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> SteerAction;

	UPROPERTY()
	TObjectPtr<class UInputAction> HandbrakeAction;

	float ThrottleInput = 0.0f;
	float SteerInput = 0.0f;
	bool bHandbrakeInput = false;

	// Self test for drive
	bool bSelfTestStarted = false;
	float SelfTestTime = 0.0f;
	float SelfTestMaxSpeed = 0.0f;

	void EnsureInputAssets();
	void PawnClientRestart() override;

	void PollKeyboard();
	void ApplyVehicleInput();

	void Input_Throttle(const struct FInputActionValue& Value);
	void Input_Steer(const struct FInputActionValue& Value);
	void Input_Handbrake(const struct FInputActionValue& Value);

	void RunSelfTest(float DeltaTime);
};