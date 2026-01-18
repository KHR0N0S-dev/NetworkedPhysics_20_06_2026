#include "VehicleSimSpringJointComponent.h"
#include "SimModule/SimulationModuleBase.h"
#include "ChaosModularVehicle/ModularVehicleSimulationCU.h"
#include "PhysicsProxy/ClusterUnionPhysicsProxy.h"
#include "PBDRigidsSolver.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/PBDRigidClustering.h"
#include "Chaos/ClusterUnionManager.h"
#include "SimModule/SimModuleTree.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VehicleSimSpringJointComponent)

namespace Chaos
{
	void FSpringJointSimModule::Simulate(IPhysicsProxyBase* Proxy, Chaos::FPBDRigidParticleHandle* ParticleHandle, float DeltaTime, const FAllInputs& Inputs, FSimModuleTree& VehicleModuleSystem)
	{
		if (!bInitialized)
		{
			CurrentLinearOffset = FVector::ZeroVector;
			CurrentLinearVel = FVector::ZeroVector;
			bInitialized = true;
		}
		
		const FVector LocalVel = GetLocalLinearVelocity();

		static int32 LogCounter = 0;
		if (LogCounter++ % 100 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("SpringJointSimModule: LocalVel=%s, Offset=%s"), *LocalVel.ToString(), *CurrentLinearOffset.ToString());
		} 

		// Lag force proportional to velocity
		FVector DisplacementForce = -LocalVel * Setup().SimulatedMass;
		// Spring back to origin
		FVector RestoringForce = -Setup().LinearStiffness * CurrentLinearOffset;
		// Damping
		FVector DampingForce = -Setup().LinearDamping * CurrentLinearVel;

		FVector TotalLinearForce = DisplacementForce + RestoringForce + DampingForce;

		// Integrate
		float Mass = FMath::Max(Setup().SimulatedMass, 1.0f);
		CurrentLinearVel += (TotalLinearForce / Mass) * DeltaTime;
		CurrentLinearOffset += CurrentLinearVel * DeltaTime;

		// Clamp Offset
		CurrentLinearOffset.X = FMath::Clamp(CurrentLinearOffset.X, -Setup().LinearMaxOffset.X, Setup().LinearMaxOffset.X);
		CurrentLinearOffset.Y = FMath::Clamp(CurrentLinearOffset.Y, -Setup().LinearMaxOffset.Y, Setup().LinearMaxOffset.Y);
		CurrentLinearOffset.Z = FMath::Clamp(CurrentLinearOffset.Z, -Setup().LinearMaxOffset.Z, Setup().LinearMaxOffset.Z);
	}

	void FSpringJointSimModule::Animate()
	{
		// pass the displacement to the Game Thread
		AnimationData.AnimFlags = Chaos::EAnimationFlags::AnimatePosition;
		AnimationData.AnimationLocOffset = CurrentLinearOffset;
	}
}

UVehicleSimSpringJointComponent::UVehicleSimSpringJointComponent()
{
	LinearStiffness = 50000.0f;
	LinearDamping = 5000.0f;
	LinearMaxOffset = FVector(50.0f, 50.0f, 50.0f);
	SimulatedMass = 100.0f;
	bUpdateComponentTransform = true;
	bAnimationEnabled = true;
}

Chaos::ISimulationModuleBase* UVehicleSimSpringJointComponent::CreateNewCoreModule() const
{
	Chaos::FSpringJointSettings Settings;
	Settings.LinearStiffness = LinearStiffness;
	Settings.LinearDamping = LinearDamping;
	Settings.LinearMaxOffset = LinearMaxOffset;
	Settings.SimulatedMass = SimulatedMass;

	Chaos::FSpringJointSimModule* NewModule = new Chaos::FSpringJointSimModule(Settings);
	NewModule->SetAnimationEnabled(bAnimationEnabled);
	return NewModule;
}

void UVehicleSimSpringJointComponent::OnOutputReady(const Chaos::FSimOutputData* OutputData)
{
	if (bUpdateComponentTransform && OutputData && (OutputData->AnimationData.AnimFlags & Chaos::EAnimationFlags::AnimatePosition))
	{
		SetWorldLocation(OutputData->AnimationData.AnimationLocOffset);
	}
}
