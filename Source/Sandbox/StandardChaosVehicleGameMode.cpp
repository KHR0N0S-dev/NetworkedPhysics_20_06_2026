// Fill out your copyright notice in the Description page of Project Settings.

#include "StandardChaosVehicleGameMode.h"
#include "StandardChaosVehiclePawn.h"
#include "GameFramework/PlayerController.h"

AStandardChaosVehicleGameMode::AStandardChaosVehicleGameMode()
{
	// Standard Epic Chaos vehicle (out-of-the-box behavior using ChaosWheeledVehicleMovementComponent)
	DefaultPawnClass = AStandardChaosVehiclePawn::StaticClass();
	PlayerControllerClass = APlayerController::StaticClass();
}