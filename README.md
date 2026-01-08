# Networked Physics – Chaos Modular Vehicle & Async Physics (UE5.7)

<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/1.gif" alt="networked-physics-splash" width="100%"/>

## About This Project

This project serves as a technical showcase for **Advanced Networked Physics** and the **Chaos Modular Vehicle** system in Unreal Engine 5.7. It is built upon the foundational concepts from the [Networked Physics Pawn Tutorial](https://dev.epicgames.com/community/learning/tutorials/MoBq/unreal-engine-networked-physics-pawn-tutorial) by the original developer, extending those principles into a complex modular vehicle environment.

The primary focus of this repository is the practical implementation and sophisticated extension of these systems, specifically:
- **Modular Vehicle Evolution**: Pushing the boundaries of the experimental Chaos Modular Vehicle plugin through custom C++ sub-modules.
- **Physics-Driven Mechanics**: Implementing real-world mechanical behaviors (like hydraulic arms and buckets) directly within the Physics Thread.
- **Networked Precision**: Investigating and solving the complexities of asynchronous physics synchronization in high-latency environments.

### A Deep Dive into Chaos Physics
This project is an ongoing learning endeavor and a work in progress, evolving as I delve deeper into the complexities of real-time simulation. While the fundamental principles of physics are long-standing laws of nature, their implementation within a highly sophisticated, networked, and asynchronous engine architecture is a modern challenge.

Beyond a simple showcase, this project represents my personal journey into the "under-the-hood" world of Unreal's physics engine. It is an active playground for understanding:
- **Low-Level Simulation**: Deciphering the interaction between Game Thread inputs and Physics Thread execution.
- **Structural Integrity**: Investigating how hierarchical simulation trees maintain stability under stress.
- **Experimental Boundaries**: Testing the limits of the new Modular Vehicle architecture to bridge the gap between "standard" vehicle sims and complex industrial machinery.

---

## Key Components

### 1. Loader Truck (LoaderPawn)
The `ALoaderPawn` demonstrates a significant extension of the Chaos Modular Vehicle architecture through specialized mechanical systems.

<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/3.gif" alt="networked-physics-splash" width="100%"/>

- **VehicleSimArmComponent & Follower**: A primary technical highlight. This is a custom-built simulation module that adds interactive arm manipulation (e.g., for loaders, cranes, or excavators). It demonstrates how to create custom PT (Physics Thread) simulation logic that integrates seamlessly with the modular vehicle tree.
- **LoaderSimComponent**: An extension of `UModularVehicleBaseComponent` that manages the core vehicle dynamics while coordinating the custom arm and bucket sub-modules.
- **Hierarchical Simulation**: Showcases complex part-to-part physics relationships and animation synchronization between the Physics Thread and Game Thread.

### 2. Wrecking Ball Controller (MyPhysicsPawn)
This component implements the manual asynchronous networked physics logic detailed in the community tutorial, serving as a robust base for state synchronization.

<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/2.gif" alt="networked-physics-splash" width="100%"/>

- **Tutorial Implementation**: Provides a clean, documented implementation of `Chaos::TSimCallbackObject` and `FNetworkPhysicsPayload`.
- **Synchronization Logic**: Demonstrates reliable input and state replication across the network with client-side prediction and server reconciliation.

---

## Technical Flow

### Modular Vehicle Simulation
1. `ALoaderPawn` initializes with `ULoaderSimComponent`.
2. `UVehicleSimArmComponent` is registered as a sub-module.
3. Chaos Physics handles the wheel/suspension simulation via the Modular Vehicle plugin's async tree.

### Async Networked Physics
1. Inputs are captured on the **Game Thread**.
2. Inputs are sent to the **Physics Thread** via `FAsyncInputPhysicsPawn`.
3. `FPhysicsPawnAsync` processes movement and forces during `OnPreSimulate_Internal`.
4. State is synchronized back using the `UNetworkPhysicsComponent`.

