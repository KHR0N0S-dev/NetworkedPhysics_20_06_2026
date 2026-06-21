// Keeps World Partition landscape streamed under the modular car (Map1 / open world).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ModularCarMapBootstrap.generated.h"

class UWorldPartitionStreamingSourceComponent;

UCLASS(NotPlaceable, Transient)
class SANDBOX_API AModularCarMapBootstrap : public AActor
{
	GENERATED_BODY()

public:
	AModularCarMapBootstrap();

	UPROPERTY(VisibleAnywhere, Category = "Map Bootstrap")
	TObjectPtr<UWorldPartitionStreamingSourceComponent> StreamingSource = nullptr;
};