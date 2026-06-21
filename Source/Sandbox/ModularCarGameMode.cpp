// Fill out your copyright notice in the Description page of Project Settings.

#include "ModularCarGameMode.h"
#include "ModularCarPawn.h"
#include "ModularCarMapCollision.h"
#include "ModularCarMapBootstrap.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"

AModularCarGameMode::AModularCarGameMode()
{
	// Custom low-level networked physics pawn (async Chaos + NetworkPhysicsComponent)
	DefaultPawnClass = AModularCarPawn::StaticClass();
	PlayerControllerClass = APlayerController::StaticClass();
}

void AModularCarGameMode::StartPlay()
{
	if (UWorld* World = GetWorld())
	{
		FVector PrepareLocation = FVector::ZeroVector;
		if (AActor* PlayerStart = FindPlayerStart(nullptr))
		{
			PrepareLocation = PlayerStart->GetActorLocation();
		}

		MapBootstrap = ModularCarMapCollision::PrepareMapForVehicle(World, PrepareLocation);
	}

	Super::StartPlay();
}