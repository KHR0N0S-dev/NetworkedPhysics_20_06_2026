// Fill out your copyright notice in the Description page of Project Settings.

#include "VehicleSimArmComponent.h"
#include "SimModule/SimulationModuleBase.h"
#include "ChaosModularVehicle/ModularVehicleSimulationCU.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VehicleSimArmComponent)

namespace Chaos
{
	class FArmSimModule : public ISimulationModuleBase
	{
	public:
		FArmSimModule(float InStrength, FName InInputName, const FVector& InAxis) 
			: ISimulationModuleBase(), Strength(InStrength), InputName(InInputName), Axis(InAxis), DebugCounter(0) 
		{
			// Tell the system this part is NOT welded to the chassis
			SetClustered(false);
		}

		virtual void Simulate(float DeltaTime, const FAllInputs& Inputs, FSimModuleTree& VehicleModuleSystem) override
		{
			// Ensure we stay unclustered to move independently
			if (IsClustered())
			{
				SetClustered(false);
			}

			float InputValue = static_cast<float>(Inputs.GetControls().GetFloat(InputName));
			FPBDRigidParticleHandle* RigidParticle = CachedParticle ? CachedParticle->CastToRigidParticle() : nullptr;

			if (RigidParticle)
			{
				// Use world space to align the rotation axis with the current arm orientation
				const FVec3 WorldRotationAxis = RigidParticle->GetR() * Axis;

				// Ensure the particle is dynamic
				if (RigidParticle->ObjectState() != Chaos::EObjectStateType::Dynamic)
				{
					RigidParticle->SetObjectStateLowLevel(Chaos::EObjectStateType::Dynamic);
				}

				if (FMath::Abs(InputValue) > 0.01f)
				{
					// Apply local torque to move relative to the tractor parent
					AddLocalTorque(Axis * InputValue * Strength);

					if (++DebugCounter >= 10)
					{
						DebugCounter = 0;
						UE_LOG(LogTemp, Warning, TEXT("ARM MOVING | Input: %f | Axis: %s | Particle: %d"),
							InputValue, *WorldRotationAxis.ToString(), ParticleIdx.Idx);
					}
				}
			}
		}

		/** Returns a debug identifier for this module */
		virtual const FString GetDebugName() const override { return TEXT("ArmSimModule"); }

		/** Defines the module's behavior type */
		virtual bool IsBehaviourType(eSimModuleTypeFlags InType) const override { return (InType == eSimModuleTypeFlags::TorqueBased); }

		/** Generates any required network replication data */
		virtual TSharedPtr<FModuleNetData> GenerateNetData(const int32 NodeArrayIndex) const override { return nullptr; }

		DEFINE_CHAOSSIMTYPENAME(FArmSimModule);

	private:
		float Strength;
		FName InputName;
		FVector Axis;
		int32 DebugCounter;
	};
}

UVehicleSimArmComponent::UVehicleSimArmComponent()
{
	// Default torque strength and input settings
	ArmTorqueStrength = 500.0f; 
	ArmInputName = TEXT("RaiseArm");
	RotationAxis = FVector(0.f, 1.f, 0.f); 
	bAnimationEnabled = true;
}

Chaos::ISimulationModuleBase* UVehicleSimArmComponent::CreateNewCoreModule() const
{
	// Create the arm simulation module with specified settings
	Chaos::FArmSimModule* NewModule = new Chaos::FArmSimModule(ArmTorqueStrength, ArmInputName, RotationAxis);

	// Enable animation for debugging
	NewModule->SetAnimationEnabled(bAnimationEnabled);

	// Ensure the module is detached from other clusters
	NewModule->SetClustered(false);

	return NewModule;
}