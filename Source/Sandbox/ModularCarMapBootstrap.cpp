#include "ModularCarMapBootstrap.h"

#include "Components/SceneComponent.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"

AModularCarMapBootstrap::AModularCarMapBootstrap()
{
	PrimaryActorTick.bCanEverTick = false;
	SetCanBeDamaged(false);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	StreamingSource = CreateDefaultSubobject<UWorldPartitionStreamingSourceComponent>(TEXT("StreamingSource"));
	StreamingSource->EnableStreamingSource();
}