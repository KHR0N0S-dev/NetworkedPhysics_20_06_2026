#include "VehicleSimRotatorComponent.h"
#include "ChaosModularVehicle/ModularVehicleBaseComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VehicleSimRotatorComponent)

namespace Chaos
{
	void FRotatorSimModule::Simulate(IPhysicsProxyBase* Proxy, Chaos::FPBDRigidParticleHandle* ParticleHandle, float DeltaTime, const FAllInputs& Inputs, FSimModuleTree& VehicleModuleSystem)
	{
		// 1. Handle User Input (Directly from Controls like the Arm Module)
		bool bHasInput = false;
		if (!Setup().InputName.IsNone())
		{
			const float InputValue = static_cast<float>(Inputs.GetControls().GetFloat(Setup().InputName));
			if (FMath::Abs(InputValue) > 0.01f)
			{
				TargetAngle += FMath::DegreesToRadians(InputValue * Setup().MoveSpeed * DeltaTime);
				bHasInput = true;
			}
		}

		// Return to zero logic
		if (!bHasInput && Setup().bReturnToZero)
		{
			float TargetRad = 0.0f;
			float Step = FMath::DegreesToRadians(Setup().MoveSpeed * DeltaTime);
			
			if (TargetAngle > TargetRad)
			{
				TargetAngle = FMath::Max(TargetRad, TargetAngle - Step);
			}
			else if (TargetAngle < TargetRad)
			{
				TargetAngle = FMath::Min(TargetRad, TargetAngle + Step);
			}
		}
		// Fallback for SetRotation from Blueprints (only if no active return-to-zero or input)
		else if (!bHasInput && Inputs.StateInputs)
		{
			TargetAngle = FMath::DegreesToRadians(static_cast<float>(Inputs.GetState().GetFloat(TEXT("RotatorTargetAngle"))));
		}

		// Clamp and Update Current
		float MinRad = FMath::DegreesToRadians(Setup().MinAngle);
		float MaxRad = FMath::DegreesToRadians(Setup().MaxAngle);
		TargetAngle = FMath::Clamp(TargetAngle, MinRad, MaxRad);
		CurrentAngle = TargetAngle;
	}

	void FRotatorSimModule::Animate()
	{
		AnimationData.AnimFlags = Chaos::EAnimationFlags::AnimateRotation;
		
		FVector NormalizedRotationAxis = Setup().RotationAxis;
		if (!NormalizedRotationAxis.IsNormalized())
		{
			NormalizedRotationAxis.Normalize();
		}

		AnimationData.CombinedRotation = FQuat(NormalizedRotationAxis, CurrentAngle);
		AnimationData.CombinedRotation.Normalize();
	}
}

UVehicleSimRotatorComponent::UVehicleSimRotatorComponent()
{
	MaxAngle = 10.0f;
	MinAngle = -10.0f;
	MoveSpeed = 45.0f;
	InputName = NAME_None;
	bReturnToZero = false;
	RotationAxis = FVector(0.0f, 0.0f, 1.0f);
	bAnimationEnabled = true;
	TargetAngle = 0.0f;
}

void UVehicleSimRotatorComponent::SetRotation(float AngleDegrees)
{
	TargetAngle = FMath::Clamp(AngleDegrees, MinAngle, MaxAngle);

	if (UModularVehicleBaseComponent* Vehicle = Cast<UModularVehicleBaseComponent>(GetAttachParent()))
	{
		Vehicle->SetState(TEXT("RotatorTargetAngle"), (double)TargetAngle);
	}
}

TArray<FModuleInputSetup> UVehicleSimRotatorComponent::GetInputConfig() const
{
	TArray<FModuleInputSetup> Config = Super::GetInputConfig();
	
	// Add both for flexibility
	Config.Add(FModuleInputSetup(TEXT("RotatorTargetAngle"), EModuleInputValueType::MAxis1D));
	
	if (!InputName.IsNone())
	{
		Config.Add(FModuleInputSetup(InputName, EModuleInputValueType::MAxis1D));
	}
	
	return Config;
}

void UVehicleSimRotatorComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	if (OutputData && (OutputData->AnimationData.AnimFlags & Chaos::EAnimationFlags::AnimateRotation))
	{
		// Get the component this rotator is attached to
		if (USceneComponent* Parent = GetAttachParent())
		{
			// Apply the rotation to the parent component cause we built like that
			Parent->SetRelativeRotation(OutputData->AnimationData.AnimationRotOffset);
			// do yourself too interesting but works somehow it rotates the bone directly need to poke around more
			SetRelativeRotation(OutputData->AnimationData.AnimationRotOffset);

		}
	}
}

Chaos::ISimulationModuleBase* UVehicleSimRotatorComponent::CreateNewCoreModule() const
{
	Chaos::FRotatorSettings Settings;
	Settings.MaxAngle = MaxAngle;
	Settings.MinAngle = MinAngle;
	Settings.MoveSpeed = MoveSpeed;
	Settings.InputName = InputName;
	Settings.bReturnToZero = bReturnToZero;
	Settings.RotationAxis = RotationAxis;

	Chaos::FRotatorSimModule* NewModule = new Chaos::FRotatorSimModule(Settings);
	NewModule->SetTargetAngle(FMath::DegreesToRadians(TargetAngle));
	NewModule->SetAnimationEnabled(bAnimationEnabled);
	NewModule->SetTreeIndex(GetTreeIndex());
	
	NewModule->SetAnimationData(GetBoneName(), GetAnimationOffset(), GetAnimationSetupIndex());

	return NewModule;
}
