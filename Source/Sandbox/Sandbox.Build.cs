// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Sandbox : ModuleRules
{
	public Sandbox(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			"Chaos",
			"ChaosVehicles",
			"ChaosVehiclesCore",
			"ChaosVehiclesEngine",
			"ChaosModularVehicleEngine"
		});
		
		PublicIncludePaths.AddRange(new string[] {
			"Sandbox",
			"Sandbox/Variant_Platforming",
			"Sandbox/Variant_Platforming/Animation",
			"Sandbox/Variant_Combat",
			"Sandbox/Variant_Combat/AI",
			"Sandbox/Variant_Combat/Animation",
			"Sandbox/Variant_Combat/Gameplay",
			"Sandbox/Variant_Combat/Interfaces",
			"Sandbox/Variant_Combat/UI",
			"Sandbox/Variant_SideScrolling",
			"Sandbox/Variant_SideScrolling/AI",
			"Sandbox/Variant_SideScrolling/Gameplay",
			"Sandbox/Variant_SideScrolling/Interfaces",
			"Sandbox/Variant_SideScrolling/UI"
		});

		SetupIrisSupport(Target);
		
		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
