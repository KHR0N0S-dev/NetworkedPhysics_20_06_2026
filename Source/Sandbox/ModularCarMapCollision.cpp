#include "ModularCarMapCollision.h"

#include "ModularCarChaosSuspension.h"
#include "ModularCarMapBootstrap.h"
#include "ModularCarPawn.h"

#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeProxy.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionSubsystem.h"

namespace ModularCarMapCollision
{
	static bool IsVehicleActor(const AActor* Actor)
	{
		return Actor && (Actor->IsA<AModularCarPawn>() || Actor->IsA<AModularCarMapBootstrap>());
	}

	static void EnsurePrimitiveBlocksGround(UPrimitiveComponent* Prim)
	{
		if (!Prim)
		{
			return;
		}

		const ECollisionEnabled::Type Collision = Prim->GetCollisionEnabled();
		if (Collision == ECollisionEnabled::NoCollision || Collision == ECollisionEnabled::PhysicsOnly)
		{
			Prim->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		}

		Prim->SetCollisionObjectType(ECC_WorldStatic);
		Prim->SetCollisionResponseToAllChannels(ECR_Block);
	}

	static void EnsureMeshBlocksGround(UStaticMeshComponent* MeshComp)
	{
		if (!MeshComp || !MeshComp->GetStaticMesh())
		{
			return;
		}

		EnsurePrimitiveBlocksGround(MeshComp);
	}

	bool ProbeGroundAtLocation(UWorld* World, const FVector& Location, float TraceUpCm, float TraceDownCm)
	{
		if (!World)
		{
			return false;
		}

		FHitResult Hit;
		const FVector Start = Location + FVector(0.0f, 0.0f, TraceUpCm);
		const FVector End = Location - FVector(0.0f, 0.0f, TraceDownCm);
		return ModularCarChaosSuspension::TraceGround(World, Start, End, nullptr, Hit) && Hit.bBlockingHit;
	}

	void EnsureGroundReadyAtLocation(UWorld* World, const FVector& Location, float MaxWaitSeconds)
	{
		if (!World)
		{
			return;
		}

		UWorldPartitionSubsystem* WorldPartition = World->GetSubsystem<UWorldPartitionSubsystem>();
		TArray<FWorldPartitionStreamingQuerySource> QuerySources;
		FWorldPartitionStreamingQuerySource& Query = QuerySources.Add_GetRef(FWorldPartitionStreamingQuerySource(Location));
		Query.bUseGridLoadingRange = true;

		const double Deadline = FPlatformTime::Seconds() + static_cast<double>(MaxWaitSeconds);
		while (FPlatformTime::Seconds() < Deadline)
		{
			World->FlushLevelStreaming();

			if (GEngine)
			{
				GEngine->BlockTillLevelStreamingCompleted(World);
			}

			EnsureLevelGroundCollision(World);

			if (ProbeGroundAtLocation(World, Location))
			{
				return;
			}

			if (WorldPartition
				&& WorldPartition->IsStreamingCompleted(
					EWorldPartitionRuntimeCellState::Activated, QuerySources, /*bExactState*/ false))
			{
				EnsureLevelGroundCollision(World);
				if (ProbeGroundAtLocation(World, Location))
				{
					return;
				}
			}

			FPlatformProcess::Sleep(0.02f);
		}

		UE_LOG(LogTemp, Warning, TEXT("ModularCarMapCollision | Ground probe timed out at %s"), *Location.ToString());
	}

	void EnsureLevelGroundCollision(UWorld* World)
	{
		if (!World)
		{
			return;
		}

		int32 FixedComponents = 0;

		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			AActor* Actor = *ActorIt;
			if (!Actor || IsVehicleActor(Actor))
			{
				continue;
			}

			if (AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(Actor))
			{
				if (UStaticMeshComponent* MeshComp = StaticMeshActor->GetStaticMeshComponent())
				{
					const ECollisionEnabled::Type Before = MeshComp->GetCollisionEnabled();
					EnsureMeshBlocksGround(MeshComp);
					if (Before != MeshComp->GetCollisionEnabled())
					{
						++FixedComponents;
					}
				}
			}

			TArray<UPrimitiveComponent*> PrimitiveComponents;
			Actor->GetComponents(PrimitiveComponents);
			for (UPrimitiveComponent* Prim : PrimitiveComponents)
			{
				if (!Prim)
				{
					continue;
				}

				if (Prim->IsA<ULandscapeHeightfieldCollisionComponent>()
					|| Prim->IsA<UInstancedStaticMeshComponent>()
					|| (Prim->IsA<UStaticMeshComponent>() && !Actor->IsA<AStaticMeshActor>()))
				{
					const ECollisionEnabled::Type Before = Prim->GetCollisionEnabled();
					EnsurePrimitiveBlocksGround(Prim);
					if (Before != Prim->GetCollisionEnabled())
					{
						++FixedComponents;
					}
				}
			}
		}

		for (TActorIterator<ALandscapeProxy> LandscapeIt(World); LandscapeIt; ++LandscapeIt)
		{
			ALandscapeProxy* Landscape = *LandscapeIt;
			if (!Landscape)
			{
				continue;
			}

			for (ULandscapeHeightfieldCollisionComponent* CollisionComp : Landscape->CollisionComponents)
			{
				if (!CollisionComp)
				{
					continue;
				}

				const ECollisionEnabled::Type Before = CollisionComp->GetCollisionEnabled();
				EnsurePrimitiveBlocksGround(CollisionComp);
				if (Before != CollisionComp->GetCollisionEnabled())
				{
					++FixedComponents;
				}
			}
		}

		UE_LOG(LogTemp, Warning, TEXT("ModularCarMapCollision | Map=%s FixedComponents=%d"),
			*World->GetMapName(), FixedComponents);
	}

	AModularCarMapBootstrap* PrepareMapForVehicle(UWorld* World, const FVector& Location, AModularCarMapBootstrap* ExistingBootstrap)
	{
		if (!World)
		{
			return nullptr;
		}

		AModularCarMapBootstrap* Bootstrap = ExistingBootstrap;
		if (!Bootstrap)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Params.ObjectFlags |= RF_Transient;
			Bootstrap = World->SpawnActor<AModularCarMapBootstrap>(
				AModularCarMapBootstrap::StaticClass(), Location, FRotator::ZeroRotator, Params);
		}

		if (Bootstrap)
		{
			Bootstrap->SetActorLocation(Location);
		}

		EnsureGroundReadyAtLocation(World, Location, 10.0f);
		EnsureLevelGroundCollision(World);

		UE_LOG(LogTemp, Warning, TEXT("ModularCarMapCollision | PrepareMapForVehicle Map=%s Loc=%s Ground=%d Bootstrap=%s"),
			*World->GetMapName(),
			*Location.ToString(),
			ProbeGroundAtLocation(World, Location) ? 1 : 0,
			Bootstrap ? *Bootstrap->GetName() : TEXT("NULL"));

		return Bootstrap;
	}
}