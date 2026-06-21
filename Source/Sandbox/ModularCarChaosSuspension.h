// Chaos Vehicles suspension helpers for ModularCarPawn (FSimpleSuspensionSim + raycast).

#pragma once

#include "CoreMinimal.h"
#include "SuspensionSystem.h"
#include "SuspensionUtility.h"
#include "Chaos/Particle/ParticleUtilities.h"

class UWorld;
class AActor;
struct FHitResult;

namespace ModularCarChaosSuspension
{
	/** Game-thread suspension raycast result consumed by the physics callback. */
	struct FSuspensionHitCache
	{
		bool bBlockingHit = false;
		float Distance = 0.0f;
		FVector ImpactNormal = FVector::UpVector;
		FVector ImpactPoint = FVector::ZeroVector;
	};

	/** One wheel's Chaos-style spring sim; config storage must outlive the sim pointer. */
	struct FWheelSuspensionRuntime
	{
		FVector LocalRestPosition = FVector::ZeroVector;
		float Radius = 35.0f;
		Chaos::FSimpleSuspensionConfig Config;
		Chaos::FSimpleSuspensionSim Sim;

		FWheelSuspensionRuntime()
			: Sim(&Config)
		{
		}
	};

	/** Build editor N/m spring rate into Chaos physics config (cm-based sim). */
	inline void FillSuspensionConfig(
		Chaos::FSimpleSuspensionConfig& OutConfig,
		float SpringRateNm,
		float SpringPreloadN,
		float SuspensionMaxRaiseCm,
		float SuspensionMaxDropCm,
		float DampingRatio,
		float WheelLoadRatio,
		int32 SuspensionSmoothing)
	{
		OutConfig = Chaos::FSimpleSuspensionConfig();
		OutConfig.SuspensionAxis = FVector(0.f, 0.f, -1.f);
		OutConfig.SuspensionForceOffset = FVector::ZeroVector;
		OutConfig.SetSuspensionMaxRaise(SuspensionMaxRaiseCm);
		OutConfig.SetSuspensionMaxDrop(SuspensionMaxDropCm);
		OutConfig.SpringRate = Chaos::MToCm(SpringRateNm);
		OutConfig.SpringPreload = Chaos::MToCm(SpringPreloadN);
		OutConfig.DampingRatio = DampingRatio;
		OutConfig.WheelLoadRatio = WheelLoadRatio;
		OutConfig.SuspensionSmoothing = SuspensionSmoothing;
	}

	/** Match UChaosWheeledVehicleMovementComponent::SetupSuspension sprung-mass + damping setup. */
	inline void FinalizeSprungMassAndDamping(
		TArray<FWheelSuspensionRuntime>& Wheels,
		float TotalMassKg,
		float GravityZ)
	{
		if (Wheels.IsEmpty())
		{
			return;
		}

		TArray<FVector> LocalSpringPositions;
		LocalSpringPositions.Reserve(Wheels.Num());
		for (FWheelSuspensionRuntime& Wheel : Wheels)
		{
			Wheel.Sim.SetLocalRestingPosition(Wheel.LocalRestPosition);
			LocalSpringPositions.Add(Wheel.LocalRestPosition);
		}

		TArray<float> SprungMasses;
		if (!FSuspensionUtility::ComputeSprungMasses(LocalSpringPositions, TotalMassKg, SprungMasses))
		{
			SprungMasses.Init(TotalMassKg / Wheels.Num(), Wheels.Num());
		}

		for (int32 Idx = 0; Idx < Wheels.Num(); ++Idx)
		{
			FWheelSuspensionRuntime& Wheel = Wheels[Idx];
			const float SprungMass = SprungMasses.IsValidIndex(Idx) ? SprungMasses[Idx] : (TotalMassKg / Wheels.Num());
			const float Damping = FSuspensionUtility::ComputeDamping(
				Wheel.Config.SpringRate, SprungMass, Wheel.Config.DampingRatio);
			Wheel.Config.CompressionDamping = Damping;
			Wheel.Config.ReboundDamping = Damping;
			Wheel.Config.RestingForce = SprungMass * GravityZ;
			Wheel.Sim.SetSpringIndex(static_cast<uint32>(Idx));
		}
	}

	inline FVector GetVelocityAtPoint(const Chaos::FPBDRigidParticleHandle* Particle, const FVector& WorldPoint)
	{
		if (!Particle)
		{
			return FVector::ZeroVector;
		}
		const Chaos::FVec3 COM = Chaos::FParticleUtilitiesGT::GetCoMWorldPosition(Particle);
		const Chaos::FVec3 Diff = WorldPoint - COM;
		return Particle->GetV() - Chaos::FVec3::CrossProduct(Diff, Particle->GetW());
	}

	inline void AddForceAtPosition(
		Chaos::FPBDRigidParticleHandle* Particle,
		const FVector& Force,
		const FVector& Position)
	{
		if (!Particle)
		{
			return;
		}
		const Chaos::FVec3 COM = Chaos::FParticleUtilitiesGT::GetCoMWorldPosition(Particle);
		const Chaos::FVec3 Torque = Chaos::FVec3::CrossProduct(Position - COM, Force);
		Particle->AddForce(Force, true);
		Particle->AddTorque(Torque, true);
	}

	/** Ray segment for suspension traces (const-safe; mirrors FSimpleSuspensionSim::UpdateWorldRaycastLocation). */
	inline void BuildSuspensionRaycastTrace(
		const Chaos::FSimpleSuspensionConfig& Config,
		const FVector& LocalRestPosition,
		const FTransform& BodyTransform,
		float WheelRadius,
		Chaos::FSuspensionTrace& OutTrace)
	{
		const FVector WorldLocation = BodyTransform.TransformPosition(LocalRestPosition);
		const FVector WorldDirection = BodyTransform.TransformVector(Config.SuspensionAxis);

		OutTrace.Start = WorldLocation - WorldDirection * (Config.SuspensionMaxRaise + Config.RaycastSafetyMargin);
		OutTrace.End = WorldLocation + WorldDirection * (Config.SuspensionMaxDrop + WheelRadius);
	}

	/** Line trace against WorldStatic + WorldDynamic (Map1 meshes may use either). */
	bool TraceGround(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		AActor* TraceIgnoreActor,
		FHitResult& OutHit);

	/**
	 * Raycast + FSimpleSuspensionSim::Simulate + AddForceAtPosition, mirroring
	 * UChaosWheeledVehicleSimulation::ApplySuspensionForces (constraint path disabled).
	 * Suspension forces align with hit surface normal so partial wheel layouts stay supported.
	 */
	void UpdateSuspensionHitCache(
		UWorld* World,
		AActor* TraceIgnoreActor,
		const FTransform& BodyWorldTransform,
		const TArray<FWheelSuspensionRuntime>& Wheels,
		TArray<FSuspensionHitCache>& OutHits);

	void ApplySuspensionForces(
		UWorld* World,
		AActor* TraceIgnoreActor,
		Chaos::FPBDRigidParticleHandle* ParticleHandle,
		const FTransform& BodyWorldTransform,
		const FVector& VehicleUpAxis,
		float DeltaTime,
		TArray<FWheelSuspensionRuntime>& Wheels,
		float RollbarScaling,
		const TArray<FSuspensionHitCache>* CachedHits = nullptr);
}