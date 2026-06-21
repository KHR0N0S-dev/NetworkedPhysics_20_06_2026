// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "StandardChaosVehicleGameMode.generated.h"

/**
 * Separate Game Mode for testing the standard Epic Chaos vehicle approach.
 * Uses AStandardChaosVehiclePawn (based on UChaosWheeledVehicleMovementComponent)
 * so you can easily compare default Chaos vehicle behavior vs your custom ModularCarPawn.
 *
 * Usage:
 * - In World Settings → Game Mode Override → select StandardChaosVehicleGameMode
 * - Or launch with the map and override in editor
 */
UCLASS()
class SANDBOX_API AStandardChaosVehicleGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AStandardChaosVehicleGameMode();
};