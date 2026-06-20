// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ModularCarGameMode.generated.h"

/**
 * Game mode that spawns the modular car (AModularCarPawn) as the player's default pawn.
 */
UCLASS()
class SANDBOX_API AModularCarGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AModularCarGameMode();
};
