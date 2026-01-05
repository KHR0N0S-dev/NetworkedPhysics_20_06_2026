// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ChaosModularVehicle/VehicleSimBaseComponent.h"
#include "VehicleSimArmComponent.generated.h"

UCLASS(ClassGroup = (ModularVehicle), meta = (BlueprintSpawnableComponent), hidecategories = (Object, Replication, Cooking, Activation, LOD, Physics, Collision, AssetUserData, Event))
class SANDBOX_API UVehicleSimArmComponent : public UVehicleSimBaseComponent
{
	GENERATED_BODY()

public:
	// Removed UE_API from individual members to fix C2487
	UVehicleSimArmComponent();
	virtual ~UVehicleSimArmComponent() = default;

	/** Torque strength applied to the arm */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float ArmTorqueStrength;

	/** The name of the input axis to listen for (e.g., "RaiseArm") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	FName ArmInputName;

	/** Axis of rotation for the torque (Local Space) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	FVector RotationAxis;

	virtual ESimModuleType GetModuleType() const override { return ESimModuleType::Rudder; }
	
	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;
};
