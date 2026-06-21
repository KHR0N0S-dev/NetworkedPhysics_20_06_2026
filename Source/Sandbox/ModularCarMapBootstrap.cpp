#include "ModularCarMapBootstrap.h"

#include "Components/SceneComponent.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"

AModularCarMapBootstrap::AModularCarMapBootstrap()
{
	PrimaryActorTick.bCanEverTick = false;
	SetCanBeDamaged(false);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	StreamingSource = CreateDefaultSubobject<UWorldPartitionStreamingSourceComponent>(TEXT("StreamingSource"));
	StreamingSource->EnableStreamingSource();
	StreamingSource->Priority = EStreamingSourcePriority::Highest;
	FStreamingSourceShape& Shape = StreamingSource->Shapes.AddDefaulted_GetRef();
	Shape.bUseGridLoadingRange = true;
	Shape.LoadingRangeScale = 4.0f;
}