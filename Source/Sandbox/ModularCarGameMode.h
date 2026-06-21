// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ModularCarGameMode.generated.h"

class AModularCarMapBootstrap;

/**
 * Game mode for the custom low-level networked physics car (ModularCarPawn).
 * Uses custom Chaos async callback + NetworkPhysicsComponent.
 */
UCLASS()
class SANDBOX_API AModularCarGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AModularCarGameMode();

	virtual void StartPlay() override;

	UPROPERTY(Transient)
	TObjectPtr<AModularCarMapBootstrap> MapBootstrap = nullptr;
};