# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project identity

- The repo folder is `NetworkedPhysics`, but the **Unreal project is named `Sandbox`** — `Sandbox.uproject`, module `Sandbox`, targets `Sandbox` / `SandboxEditor`. Do not look for a "NetworkedPhysics" module.
- Forked from github.com/cem-akkaya/NetworkedPhysics; the project demonstrates **Chaos networked physics** (client prediction + server resimulation).
- **Engine: a source build of UE 5.7.4 at `Z:\UnrealEngineSource_5_7`** (registered in `HKCU\SOFTWARE\Epic Games\Unreal Engine\Builds` under GUID `{404B5AD4-45C0-680F-5FD0-1FA2988CF222}`, which is the `EngineAssociation` value). This is **not** a launcher engine — always build/run with the source-build binaries below.
- This repo originally targeted UE 5.8. The fix for 5.7 lives in `Source/SandboxEditor.Target.cs` (`IncludeOrderVersion`/`DefaultBuildSettings = .Latest`, since `Unreal5_8`/`V7` don't exist in 5.7). Plugins `ElectronicNodes` and `LowEntryExtStdLib` are disabled in `Sandbox.uproject` (missing on disk, code doesn't depend on them). Keep these settings when touching targets/plugins.

## Build / run / test

Build the editor target (does not require the editor to be closed; ~10s incremental):

```
Z:\UnrealEngineSource_5_7\Engine\Build\BatchFiles\Build.bat SandboxEditor Win64 Development -Project="<repo>\Sandbox.uproject" -WaitMutex
```

Run the editor — **always pass `-noxgeshadercompile`**. Without it the first launch deadlocks on the splash (XGE distributed shader-compile controller spawns workers that die while the editor waits forever). With the flag, local workers compile shaders normally.

```
Z:\UnrealEngineSource_5_7\Engine\Binaries\Win64\UnrealEditor.exe "<repo>\Sandbox.uproject" -noxgeshadercompile
```

**Headless self-tests** for `ModularCarPawn` (CVar `car.SelfTest`): `1` = drive in a circle, `2` = rest-stability check (asserts PeakSpeed ≤5 cm/s, ZBand ≤3 cm). They run in `-game` standalone, log `SELFTEST ...` lines, then quit:

```
UnrealEditor.exe "<repo>\Sandbox.uproject" /Game/Lvl_ModularCarTest -game ^
  -ExecCmds="car.SelfTest 2" -unattended -nopause -nosplash -noxgeshadercompile -log
```

Parse results from `Saved\Logs\Sandbox.log` (`SELFTEST REST | ... => PASS/FAIL`, `SELFTEST RIDE | MinZ MaxZ Band`, and `CARSETUP | ...` at spawn).

> **Important test limitation:** self-tests run standalone, where `ProcessInputs_Internal` and resimulation are **never invoked** — they only validate the force model and `OnPreSimulate`, NOT the networked path (input history / resim). Networking must be verified live in **PIE with Number of Players = 2** (+ Net Emulation for latency). For clean single-player runs of the cluster/prediction code, set PIE to **1 player / Standalone**.

## Networked-physics architecture (the core pattern)

The whole point of this project is **physics on the physics thread with prediction**, not gameplay-thread movement. Understanding one pawn lets you read them all. Reference implementation: `Source/Sandbox/MyPhysicsPawn.{h,cpp}` (wrecking ball). Main work-in-progress: `Source/Sandbox/ModularCarPawn.{h,cpp}` (modular car), which mirrors it. Each "physics pawn" has the same four-part shape:

1. **`APawn` subclass** — `bReplicates=true`; in the constructor, only when `UPhysicsSettings::Get()->PhysicsPrediction.bEnablePhysicsPrediction` is true, it creates a `UNetworkPhysicsComponent` (net-addressable, replicated). Its root is a simulating `UStaticMeshComponent`.
2. **`Chaos::FSimCallbackInput` struct** (`FAsyncInput*`) — GameThread→PhysicsThread input, filled each `Tick` via `GetProducerInputData_External()` **only when `IsLocallyControlled()`**.
3. **`FNetworkPhysicsPayload` structs** (`FNetInput*` / `FNetState*`) — the replicated, predicted input + state. Must implement `InterpolateData`/`MergeData`/`DecayData`/`CompareData`. `CompareData` returning false (a mismatch vs the server) is what triggers a resimulation.
4. **`Chaos::TSimCallbackObject` + `TNetworkPhysicsInputState_Internal`** (`F*Async`) — runs on the physics thread. `OnPreSimulate_Internal` applies all forces directly to the `Chaos::FPBDRigidParticleHandle` (`AddForce`/`AddTorque`). It holds a `Chaos::FConstPhysicsObjectHandle PhysicsObject`.

Wiring happens in `PostInitializeComponents`: get the solver (`World->GetPhysicsScene()->GetSolver()`), `CreateAndRegisterSimCallbackObject_External<F*Async>()`, capture `Root->GetPhysicsObjectByName(NAME_None)`, copy tuning into the async object, then `NetworkPhysicsComponent->CreateDataHistory<FNetInput*, FNetState*>(async)`. `EndPlay` must `UnregisterAndFreeSimCallbackObject_External`. The particle is set to `ESleepType::NeverSleep` (prediction needs a live particle to resimulate).

**Two rules that make prediction smooth — easy to get wrong:**
- Read live input (`GetConsumerInput_Internal()`) **only when `!GetSolver()->IsResimming()`**. During a resim, keep the input the framework already applied from history (`ApplyInput_Internal`); reading live input there makes the replay diverge and produces constant corrections seen as jitter.
- Call `SetPhysicsReplicationMode(EPhysicsReplicationMode::Resimulation)` in the constructor when prediction is on. The default (`Default`) snaps the replicated pose onto the body every update — that snapping *is* the network jitter. (Note: `MyPhysicsPawn` does NOT set this; `ModularCarPawn` does.)

Project settings backing this live in `Config/DefaultEngine.ini`: `PhysicsPrediction=(bEnablePhysicsPrediction=True,...,ResimulationSettings=...)`. `GlobalDefaultGameMode=/Script/Sandbox.ModularCarGameMode`.

## ModularCarPawn specifics

Physics model is **"box on the ground"**, not a raycast spring suspension (the spring model had a persistent multi-mode instability — see the work log history). The cube chassis `Body` is the only rigid body; cylinder wheels are **welded** into it (`AttachToComponent(..., FAttachmentTransformRules(KeepRelative, /*bWeld*/true))`) so it rests on four contact points. Forces are horizontal-only at the centre of mass (drive, lateral/longitudinal grip as frame-rate-independent damping `F=-v·m·rate`); steering is a separate **yaw torque** scaled by forward speed. A low-friction `UPhysicalMaterial` with `FrictionCombineMode=Min` lets these forces govern motion instead of contact friction.

Known sharp edges if you extend it:
- `EngineForceTotal` and `YawInertia` are copied into the async object **once** in `PostInitializeComponents`, when `Wheels` is still empty (count is hard-coded `?4:1`). Runtime `AddWheel`/`RemoveWheel` change visuals and welded mass but do **not** update drive force / yaw inertia, and the wheel components are not replicated — runtime wheel changes are not network-safe.
- `YawInertia` is estimated from `BodyMassKg` (1200) but welding wheels raises real mass to ~1634 (`SetMassOverrideInKg` doesn't hold after weld), so steering torque is slightly under-calibrated.
- Input is fed twice: Enhanced Input bindings *and* `PollKeyboard()` in `Tick` write the same `*_External` vars; `PollKeyboard` runs every frame and effectively overrides the Enhanced Input path.
- `bDebugDrive` defaults to `true` and `UE_LOG`s every tick on every instance — noisy in logs.

## Other physics pawns / components (upstream)

From the original NetworkedPhysics fork (`Source/Sandbox/`): `Ball`, `LoaderPawn` + `LoaderSimComponent` (loader truck), and the `VehicleSim*` components (`VehicleSimArmComponent`, `VehicleSimRotatorComponent`, `VehicleSimSpringJointComponent`) used by the experimental Pod Racer multi-body vehicle. The `Variant_Combat/`, `Variant_Platforming/`, `Variant_SideScrolling/` trees are stock Unreal third-person-template content (characters, AI, UI) — boilerplate, not the focus of this project.

Build deps that matter (`Source/Sandbox/Sandbox.Build.cs`): `Chaos`, `PhysicsCore`, `ChaosVehicles*`, `ChaosModularVehicleEngine`, `GeometryCollectionEngine`, `EnhancedInput`, plus `SetupIrisSupport(Target)` — networking uses **Iris**.

## Work log

`РАБОЧИЙ_ЛОГ.md` (Russian) is the running session log — read it first for current state and "next steps", and **keep it updated after important changes** (it references the old project path `Z:\NetworkedPhysics`; this checkout is a later copy). The ModularCar work (pawn, game mode, `Content/Lvl_ModularCarTest.umap`, this log) is currently **untracked in git**.
