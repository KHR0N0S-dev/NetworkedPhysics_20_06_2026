// Runtime ground-collision bootstrap for modular-car maps (e.g. Map1).

#pragma once

#include "CoreMinimal.h"

class UWorld;
class AModularCarMapBootstrap;

namespace ModularCarMapCollision
{
	/** Ensure static world geometry blocks physics queries used by suspension raycasts. */
	void EnsureLevelGroundCollision(UWorld* World);

	/** True when a vertical trace finds blocking ground at Location. */
	bool ProbeGroundAtLocation(UWorld* World, const FVector& Location, float TraceUpCm = 50000.0f, float TraceDownCm = 50000.0f);

	/** Block until WP landscape at Location is loaded and raycasts hit ground (Map1). */
	void EnsureGroundReadyAtLocation(UWorld* World, const FVector& Location, float MaxWaitSeconds = 10.0f);

	/**
	 * Stream + fix collision at Location and keep a bootstrap streaming source alive.
	 * Call at the actual vehicle spawn point (not only PlayerStart).
	 */
	AModularCarMapBootstrap* PrepareMapForVehicle(UWorld* World, const FVector& Location, AModularCarMapBootstrap* ExistingBootstrap = nullptr);
}