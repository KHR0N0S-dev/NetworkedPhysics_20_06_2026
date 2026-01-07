// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ChaosModularVehicle/VehicleSimBaseComponent.h"
#include "SimModule/SimulationModuleBase.h"
#include "VehicleSimArmComponent.generated.h"

namespace Chaos
{
	struct FArmSettings
	{
		float Stiffness = 500000.0f;
		float Damping = 10000.0f;
		float MaxAngle = 90.0f;
		float MinAngle = -10.0f;
		float MoveSpeed = 45.0f;
		FName InputName = TEXT("RaiseArm");
		FVector Axis = FVector(0.f, 1.f, 0.f);
	};

	class FArmSimModule : public ISimulationModuleBase, public TSimModuleSettings<FArmSettings>
	{
	public:
		FArmSimModule(const FArmSettings& InSettings) 
			: ISimulationModuleBase()
			, TSimModuleSettings<FArmSettings>(InSettings)
			, TargetAngle(0.0f)
			, CurrentAngle(0.0f)
			, CurrentAngularVel(0.0f)
			, bInitialized(false)
		{
		}

		virtual void Simulate(IPhysicsProxyBase* Proxy, Chaos::FPBDRigidParticleHandle* ParticleHandle, float DeltaTime, const FAllInputs& Inputs, FSimModuleTree& VehicleModuleSystem) override;

		virtual void Animate() override
		{
			AnimationData.AnimFlags = Chaos::EAnimationFlags::AnimateRotation;
			AnimationData.CombinedRotation = FQuat(Setup().Axis, FMath::DegreesToRadians(CurrentAngle));
		}

		virtual const FString GetDebugName() const override { return TEXT("ArmSimModule"); }
		virtual bool IsBehaviourType(eSimModuleTypeFlags InType) const override { return (InType == eSimModuleTypeFlags::TorqueBased); }
		virtual TSharedPtr<FModuleNetData> GenerateNetData(const int32 NodeArrayIndex) const override { return nullptr; }

		DEFINE_CHAOSSIMTYPENAME(FArmSimModule);

	private:
		float TargetAngle;
		float CurrentAngle;
		float CurrentAngularVel;
		bool bInitialized;
	};

	struct FFollowerSettings
	{
		float MoveSpeed = 0.0f;
		FName InputName = NAME_None;
		FVector Axis = FVector(0.f, 1.f, 0.f);
		FVector FollowAxis = FVector(0.f, 1.f, 0.f);
		float MaxAngle = 45.0f;
		float MinAngle = -45.0f;
		bool bInvert = false;
	};

	class FFollowerSimModule : public ISimulationModuleBase, public TSimModuleSettings<FFollowerSettings>
	{
	public:
		FFollowerSimModule(const FFollowerSettings& InSettings) 
			: ISimulationModuleBase()
			, TSimModuleSettings<FFollowerSettings>(InSettings)
			, CurrentAngle(0.0f)
		{
		}

		virtual void Simulate(IPhysicsProxyBase* Proxy, Chaos::FPBDRigidParticleHandle* ParticleHandle, float DeltaTime, const FAllInputs& Inputs, FSimModuleTree& VehicleModuleSystem) override;

		virtual void Animate() override 
		{
			AnimationData.AnimFlags = Chaos::EAnimationFlags::AnimateRotation;
			AnimationData.CombinedRotation = FQuat(Setup().Axis, FMath::DegreesToRadians(CurrentAngle));
		}

		FQuat GetLocalRotation() const
		{
			return FQuat(Setup().Axis, FMath::DegreesToRadians(CurrentAngle));
		}

		FVector GetAxis() const { return Setup().Axis; }
		FVector GetFollowAxis() const { return Setup().FollowAxis; }
		bool IsInverted() const { return Setup().bInvert; }

		virtual const FString GetDebugName() const override { return TEXT("FollowerSimModule"); }
		virtual bool IsBehaviourType(eSimModuleTypeFlags InType) const override { return false; }
		virtual TSharedPtr<FModuleNetData> GenerateNetData(const int32 NodeArrayIndex) const override { return nullptr; }
		
		static FName GetStaticSimTypeName() { return FName("FFollowerSimModule"); }

		DEFINE_CHAOSSIMTYPENAME(FFollowerSimModule);

	private:
		float CurrentAngle;
	};
}

UCLASS(ClassGroup = (ModularVehicle), meta = (BlueprintSpawnableComponent), hidecategories = (Object, Replication, Cooking, Activation, LOD, Physics, Collision, AssetUserData, Event))
class SANDBOX_API UVehicleSimArmComponent : public UVehicleSimBaseComponent
{
	GENERATED_BODY()

public:
	// Removed UE_API from individual members to fix C2487
	UVehicleSimArmComponent();
	virtual ~UVehicleSimArmComponent() = default;

	/** P-gain for the PD controller */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float Stiffness;

	/** D-gain for the PD controller */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float Damping;

	/** Maximum angle (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float MaxAngle;

	/** Minimum angle (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float MinAngle;

	/** Speed at which the target angle changes with input (degrees per second) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float MoveSpeed;

	/** The name of the input axis to listen for (e.g., "RaiseArm") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	FName ArmInputName;

	/** Axis of rotation for the joint (Local Space) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	FVector RotationAxis;

	virtual ESimModuleType GetModuleType() const override { return ESimModuleType::Rudder; }
	
	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;
};

UCLASS(ClassGroup = (ModularVehicle), meta = (BlueprintSpawnableComponent))
class SANDBOX_API UVehicleSimFollowerComponent : public UVehicleSimBaseComponent
{
	GENERATED_BODY()
public:
	UVehicleSimFollowerComponent();

	/** Speed at which the follower can rotate (degrees per second). Set to 0 to only follow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float MoveSpeed;

	/** The name of the input axis to listen for (e.g., "RaiseBucket") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	FName FollowerInputName;

	/** Axis of rotation for the follower relative to parent (Local Space) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	FVector RotationAxis;

	/** Axis used to map the arm's rotation onto the follower. Adjust this if following arm moves in wrong direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	FVector FollowAxis;

	/** Maximum angle (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float MaxAngle;

	/** Minimum angle (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	float MinAngle;

	/** If true, the rotation inherited from the arm will be inverted. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Attributes)
	bool bInvertRotation;

	virtual ESimModuleType GetModuleType() const override { return ESimModuleType::Thruster; }
	virtual Chaos::ISimulationModuleBase* CreateNewCoreModule() const override;
};
