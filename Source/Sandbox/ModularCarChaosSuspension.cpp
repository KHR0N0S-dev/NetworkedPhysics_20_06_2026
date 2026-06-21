#include "ModularCarChaosSuspension.h"

#include "Engine/World.h"
#include "Engine/HitResult.h"

namespace ModularCarChaosSuspension
{
	bool TraceGround(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		AActor* TraceIgnoreActor,
		FHitResult& OutHit)
	{
		if (!World)
		{
			return false;
		}

		FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(ModularCarSuspension), false, TraceIgnoreActor);
		TraceParams.bTraceComplex = true;

		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);

		if (World->LineTraceSingleByObjectType(OutHit, Start, End, ObjectParams, TraceParams))
		{
			return true;
		}

		// Fallback: channel trace (matches Chaos Vehicles overlap path for landscape proxies).
		return World->LineTraceSingleByChannel(OutHit, Start, End, ECC_WorldStatic, TraceParams)
			|| World->LineTraceSingleByChannel(OutHit, Start, End, ECC_WorldDynamic, TraceParams);
	}

	static FVector GetSuspensionForceDirection(const FHitResult& Hit, const FVector& VehicleUpAxis)
	{
		if (!Hit.bBlockingHit)
		{
			return VehicleUpAxis;
		}

		FVector Normal = Hit.ImpactNormal.GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			return VehicleUpAxis;
		}

		if (FVector::DotProduct(Normal, VehicleUpAxis) < 0.0f)
		{
			Normal = -Normal;
		}

		return Normal;
	}

	void UpdateSuspensionHitCache(
		UWorld* World,
		AActor* TraceIgnoreActor,
		const FTransform& BodyWorldTransform,
		const TArray<FWheelSuspensionRuntime>& Wheels,
		TArray<FSuspensionHitCache>& OutHits)
	{
		OutHits.SetNum(Wheels.Num());

		if (!World)
		{
			return;
		}

		for (int32 WheelIdx = 0; WheelIdx < Wheels.Num(); ++WheelIdx)
		{
			const FWheelSuspensionRuntime& Wheel = Wheels[WheelIdx];
			FSuspensionHitCache& Cache = OutHits[WheelIdx];

			Chaos::FSuspensionTrace Trace;
			BuildSuspensionRaycastTrace(Wheel.Config, Wheel.LocalRestPosition, BodyWorldTransform, Wheel.Radius, Trace);

			FHitResult HitResult;
			if (TraceGround(World, Trace.Start, Trace.End, TraceIgnoreActor, HitResult) && HitResult.bBlockingHit)
			{
				Cache.bBlockingHit = true;
				Cache.Distance = HitResult.Distance;
				Cache.ImpactNormal = HitResult.ImpactNormal.GetSafeNormal();
				Cache.ImpactPoint = HitResult.ImpactPoint;
			}
			else
			{
				Cache = FSuspensionHitCache();
			}
		}
	}

	void ApplySuspensionForces(
		UWorld* World,
		AActor* TraceIgnoreActor,
		Chaos::FPBDRigidParticleHandle* ParticleHandle,
		const FTransform& BodyWorldTransform,
		const FVector& VehicleUpAxis,
		float DeltaTime,
		TArray<FWheelSuspensionRuntime>& Wheels,
		float RollbarScaling,
		const TArray<FSuspensionHitCache>* CachedHits)
	{
		if (!ParticleHandle || Wheels.IsEmpty() || DeltaTime <= SMALL_NUMBER)
		{
			return;
		}

		TArray<float> SusForces;
		SusForces.Init(0.f, Wheels.Num());

		TArray<FVector> ContactNormals;
		ContactNormals.Init(VehicleUpAxis, Wheels.Num());

		for (int32 WheelIdx = 0; WheelIdx < Wheels.Num(); ++WheelIdx)
		{
			FWheelSuspensionRuntime& Wheel = Wheels[WheelIdx];
			Chaos::FSimpleSuspensionSim& PSuspension = Wheel.Sim;

			Chaos::FSuspensionTrace Trace;
			PSuspension.UpdateWorldRaycastLocation(BodyWorldTransform, Wheel.Radius, Trace);

			FHitResult HitResult;
			bool bHit = false;

			if (CachedHits && CachedHits->IsValidIndex(WheelIdx) && (*CachedHits)[WheelIdx].bBlockingHit)
			{
				const FSuspensionHitCache& Cache = (*CachedHits)[WheelIdx];
				HitResult.bBlockingHit = true;
				HitResult.Distance = Cache.Distance;
				HitResult.ImpactNormal = Cache.ImpactNormal;
				HitResult.ImpactPoint = Cache.ImpactPoint;
				bHit = true;
			}
			else if (World)
			{
				bHit = TraceGround(World, Trace.Start, Trace.End, TraceIgnoreActor, HitResult);
			}

			const FVector WheelWorldLocation = BodyWorldTransform.TransformPosition(Wheel.LocalRestPosition);
			const FVector LocalWheelVelocity = BodyWorldTransform.InverseTransformVector(
				GetVelocityAtPoint(ParticleHandle, WheelWorldLocation));

			if (bHit && HitResult.bBlockingHit)
			{
				const FVector ForceDir = GetSuspensionForceDirection(HitResult, VehicleUpAxis);
				ContactNormals[WheelIdx] = ForceDir;

				PSuspension.SetSuspensionLength(HitResult.Distance, Wheel.Radius);
				PSuspension.SetLocalVelocity(LocalWheelVelocity);
				PSuspension.Simulate(DeltaTime);

				const float ForceMagnitude = PSuspension.GetSuspensionForce();
				const FVector SuspensionForceVector = ForceDir * ForceMagnitude;
				const FVector SusApplicationPoint = WheelWorldLocation + PSuspension.Setup().SuspensionForceOffset;

				AddForceAtPosition(ParticleHandle, SuspensionForceVector, SusApplicationPoint);

				const float WheelLoad = PSuspension.Setup().WheelLoadRatio * ForceMagnitude
					+ (1.f - PSuspension.Setup().WheelLoadRatio) * PSuspension.Setup().RestingForce;
				SusForces[WheelIdx] = WheelLoad;
			}
			else
			{
				PSuspension.SetSuspensionLength(PSuspension.GetTraceLength(Wheel.Radius), Wheel.Radius);
			}
		}

		if (RollbarScaling > SMALL_NUMBER)
		{
			for (int32 PairIdx = 0; PairIdx + 1 < Wheels.Num(); PairIdx += 2)
			{
				const float ForceDiff = SusForces[PairIdx] - SusForces[PairIdx + 1];
				const FVector Force0 = ContactNormals[PairIdx] * ForceDiff * RollbarScaling;
				const FVector Force1 = ContactNormals[PairIdx + 1] * -ForceDiff * RollbarScaling;

				const FVector Point0 = BodyWorldTransform.TransformPosition(Wheels[PairIdx].LocalRestPosition)
					+ Wheels[PairIdx].Sim.Setup().SuspensionForceOffset;
				const FVector Point1 = BodyWorldTransform.TransformPosition(Wheels[PairIdx + 1].LocalRestPosition)
					+ Wheels[PairIdx + 1].Sim.Setup().SuspensionForceOffset;

				AddForceAtPosition(ParticleHandle, Force0, Point0);
				AddForceAtPosition(ParticleHandle, Force1, Point1);
			}
		}
	}
}