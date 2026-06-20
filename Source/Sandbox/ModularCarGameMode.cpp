// Fill out your copyright notice in the Description page of Project Settings.

#include "ModularCarGameMode.h"
#include "ModularCarPawn.h"
#include "GameFramework/PlayerController.h"

AModularCarGameMode::AModularCarGameMode()
{
	// Spawn the modular car as the player's pawn and possess it automatically.
	DefaultPawnClass = AModularCarPawn::StaticClass();
	PlayerControllerClass = APlayerController::StaticClass();
}
