#include "VehicleSimArmComponent.h"
#include "SimModule/SimulationModuleBase.h"
#include "ChaosModularVehicle/ModularVehicleSimulationCU.h"
#include "PhysicsProxy/ClusterUnionPhysicsProxy.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VehicleSimArmComponent)

namespace Chaos
{
	void FFollowerSimModule::Simulate(IPhysicsProxyBase* Proxy, Chaos::FPBDRigidParticleHandle* ParticleHandle, float DeltaTime, const FAllInputs& Inputs, FSimModuleTree& VehicleModuleSystem)
	{
		// Manual tilt input
		if (Setup().MoveSpeed > 0.01f && !Setup().InputName.IsNone())
		{
			const float InputValue = static_cast<float>(Inputs.GetControls().GetFloat(Setup().InputName));
			if (FMath::Abs(InputValue) > 0.01f)
			{
				CurrentAngle = FMath::Clamp(CurrentAngle + InputValue * Setup().MoveSpeed * DeltaTime, Setup().MinAngle, Setup().MaxAngle);
			}
		}

		// Ensure our particle remains part of the cluster and doesn't get simulated independently
		if (ParticleHandle)
		{
			// We don't want the follower to apply its own physics forces, 
			// it should strictly follow the Arm's kinematic update.
		}
	}

	void FArmSimModule::Simulate(IPhysicsProxyBase* Proxy, Chaos::FPBDRigidParticleHandle* ParticleHandle, float DeltaTime, const FAllInputs& Inputs, FSimModuleTree& VehicleModuleSystem)
	{
		if (!bInitialized)
		{
			TargetAngle = 0.0f;
			CurrentAngle = 0.0f;
			CurrentAngularVel = 0.0f;
			bInitialized = true;
		}

		const float InputValue = static_cast<float>(Inputs.GetControls().GetFloat(Setup().InputName));
		if (FMath::Abs(InputValue) > 0.01f)
		{
			TargetAngle = FMath::Clamp(TargetAngle + InputValue * Setup().MoveSpeed * DeltaTime, Setup().MinAngle, Setup().MaxAngle);
		}

		// PD Controller for the virtual state
		const float AngleError = TargetAngle - CurrentAngle;
		float TorqueMagnitude = (AngleError * Setup().Stiffness) - (CurrentAngularVel * Setup().Damping);

		// Limit torque to avoid explosions
		const float MaxTorque = 1.0e8f;
		TorqueMagnitude = FMath::Clamp(TorqueMagnitude, -MaxTorque, MaxTorque);

		// inertia
		const float ArmInertia = 1000.0f; 
		CurrentAngularVel += (TorqueMagnitude / ArmInertia) * DeltaTime;
		CurrentAngle += CurrentAngularVel * DeltaTime;

		// Clamp physical limits
		if (CurrentAngle > Setup().MaxAngle || CurrentAngle < Setup().MinAngle)
		{
			CurrentAngle = FMath::Clamp(CurrentAngle, Setup().MinAngle, Setup().MaxAngle);
			CurrentAngularVel = 0.0f;
		}

		static int32 ExecCount = 0;
		if (ExecCount++ % 100 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("Component: ARM SIM | Target: %.2f | Current: %.2f | Torque: %.2e | Input: %.2f"), 
				TargetAngle, CurrentAngle, TorqueMagnitude, InputValue);
		}

		// Reaction torque to chassis
		if (ParticleHandle)
		{
			if (ISimulationModuleBase* ChassisModule = VehicleModuleSystem.AccessSimModule(0))
			{
				ChassisModule->AddLocalTorque(ParticleHandle->GetR().RotateVector(Setup().Axis) * -TorqueMagnitude, true, false);
			}
		}

		// Propagate movement to children
		if (Proxy && Proxy->GetType() == EPhysicsProxyType::ClusterUnionProxy)
		{
			FClusterUnionPhysicsProxy* CUProxy = static_cast<FClusterUnionPhysicsProxy*>(Proxy);
			
			TArray<int32> ChildrenToProcess;
			const TSet<int32>& TreeChildren = VehicleModuleSystem.GetChildren(SimTreeIndex);
			for (int32 ChildIdx : TreeChildren) ChildrenToProcess.Add(ChildIdx);

			// Fallback for orphaned followers
			if (ChildrenToProcess.Num() == 0)
			{
				for (int32 i = 0; i < 32; ++i)
				{
					if (i == SimTreeIndex) continue;
					if (ISimulationModuleBase* Module = VehicleModuleSystem.AccessSimModule(i))
					{
						if (Module->GetDebugName() == TEXT("FollowerSimModule")) ChildrenToProcess.Add(i);
					}
				}
			}
			
			// The Arm's current rotation relative to its own start
			const FQuat ArmLocalRotation = FQuat(Setup().Axis, FMath::DegreesToRadians(CurrentAngle));
			
			for (int32 ChildIdx : ChildrenToProcess)
			{
				if (ISimulationModuleBase* ChildModule = VehicleModuleSystem.AccessSimModule(ChildIdx))
				{
					if (FPBDRigidClusteredParticleHandle* ChildParticle = ChildModule->GetClusterParticle(CUProxy))
					{
						const FTransform& ParentInitial = GetInitialParticleTransform();
						const FTransform& ChildInitial = ChildModule->GetInitialParticleTransform();
						
			
						// The Bucket needs this same 
						
						FQuat ChildTilt = FQuat::Identity;
						if (FFollowerSimModule* Follower = static_cast<FFollowerSimModule*>(ChildModule))
						{
							ChildTilt = Follower->GetLocalRotation();
						}

						const FVector RelativeInitialPos = ChildInitial.GetLocation() - ParentInitial.GetLocation();
						const FVector RotatedPos = ArmLocalRotation.RotateVector(RelativeInitialPos);
						const FVector NewWorldPos = ParentInitial.GetLocation() + RotatedPos;
						

						const FQuat SwizzledInheritedRotation = FQuat(FVector(0, 1, 0), FMath::DegreesToRadians(CurrentAngle));
						
						FTransform FinalTransform = ChildInitial;
						FinalTransform.SetLocation(NewWorldPos);
						
						// Final Rotation 
						FinalTransform.SetRotation(SwizzledInheritedRotation * ChildInitial.GetRotation() * ChildTilt);
						
						// FORCE the update
						ChildParticle->ChildToParent() = FinalTransform;

						if (ExecCount % 100 == 0)
						{
							const FRotator Euler = FinalTransform.GetRotation().Rotator();
							UE_LOG(LogTemp, Warning, TEXT("Component: ARM SIM | SWIZZLING Z TO Y | P=%.2f Y=%.2f R=%.2f"), 
								Euler.Pitch, Euler.Yaw, Euler.Roll);
						}
					}
				}
			}
		}
	}
}

UVehicleSimArmComponent::UVehicleSimArmComponent()
{
	Stiffness = 500000.0f;
	Damping = 10000.0f;
	MaxAngle = 90.0f;
	MinAngle = -10.0f;
	MoveSpeed = 45.0f;
	ArmInputName = TEXT("RaiseArm");
	RotationAxis = FVector(0.f, 1.f, 0.f); 
	bAnimationEnabled = true;
}

Chaos::ISimulationModuleBase* UVehicleSimArmComponent::CreateNewCoreModule() const
{
	Chaos::FArmSettings Settings;
	Settings.Stiffness = Stiffness;
	Settings.Damping = Damping;
	Settings.MaxAngle = MaxAngle;
	Settings.MinAngle = MinAngle;
	Settings.MoveSpeed = MoveSpeed;
	Settings.InputName = ArmInputName;
	Settings.Axis = RotationAxis;

	Chaos::FArmSimModule* NewModule = new Chaos::FArmSimModule(Settings);
	NewModule->SetAnimationEnabled(bAnimationEnabled);

	return NewModule;
}

UVehicleSimFollowerComponent::UVehicleSimFollowerComponent()
{
	MoveSpeed = 0.0f;
	FollowerInputName = NAME_None;
	RotationAxis = FVector(0.f, 1.f, 0.f);
	FollowAxis = FVector(0.f, 1.f, 0.f);
	MaxAngle = 45.0f;
	MinAngle = -45.0f;
	bInvertRotation = false;
}

Chaos::ISimulationModuleBase* UVehicleSimFollowerComponent::CreateNewCoreModule() const
{
	Chaos::FFollowerSettings Settings;
	Settings.MoveSpeed = MoveSpeed;
	Settings.InputName = FollowerInputName;
	Settings.Axis = RotationAxis;
	Settings.FollowAxis = FollowAxis;
	Settings.MaxAngle = MaxAngle;
	Settings.MinAngle = MinAngle;
	Settings.bInvert = bInvertRotation;

	UE_LOG(LogTemp, Warning, TEXT("Component: FOLLOWER | Creating Module | FollowAxis: %s | RotationAxis: %s"), 
		*Settings.FollowAxis.ToString(), *Settings.Axis.ToString());

	return new Chaos::FFollowerSimModule(Settings);
}