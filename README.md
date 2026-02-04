# Networked Physics – Chaos Modular Vehicle & Async Physics (UE5.7)

> **Important Note**  
> This project is built using a **custom Unreal Engine 5.7 source build** with **engine-level modifications and extensions** applied. Several systems showcased here rely on changes made directly to the engine source, particularly around Chaos Physics, Modular Vehicles, and asynchronous simulation behavior that you can find in my repository.

<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/5.gif" alt="networked-physics-splash" width="100%"/>

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

### 1. Pod Racer (Experimental Multi-Body Physics Vehicle)

The Pod Racer is an experimental, high-velocity vehicle setup designed to explore **force-driven, multi-body physics behavior** under extreme conditions. Unlike the Loader and Mining Truck, this vehicle does **not** rely on the Chaos Modular Vehicle framework. Instead, it is built entirely from independently simulated physics bodies connected through carefully tuned soft constraints.

<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/6.gif" alt="networked-physics-splash" width="100%"/>

The setup consists of:
- A central pod acting as the control and stabilization body
- Two independent thrusters, each simulated as separate rigid bodies
- Motorcycle-like suspension systems on each thruster
- A chassis configuration responsible for aerodynamic drag
- Forward propulsion forces combined with upward aerofoil lift forces

All physics bodies are connected using soft, balanced constraints that allow controlled, chaotic motion while maintaining overall stability at very high speeds. The vehicle behaves almost like a low-altitude flying craft, while remaining constrained to ground-level interaction.

This setup runs through **async, networked physics** and is explicitly tuned to remain stable under extreme velocity, high force magnitudes, and aggressive player input. Rather than relying on heavy assists or self-correcting behavior, control responsiveness is preserved while requiring player mastery. The vehicle does not drive itself; understanding its dynamics is part of the experience.

The Pod Racer exists as a **physics and gameplay experiment**, focusing on:
- Multi-body force interaction at high velocity
- Stability and constraint behavior under stress
- Balancing realism with readable, skill-based control
- Exploring the limits of networked async physics outside traditional vehicle abstractions

This pawn serves as a contrast case to the Chaos Modular Vehicles in this repository, demonstrating a different approach to complex vehicle simulation where behavior emerges primarily from force balance and constraint tuning rather than predefined vehicle models.

### 2. Loader Truck (LoaderPawn)
The `ALoaderPawn` demonstrates a significant extension of the Chaos Modular Vehicle architecture through specialized mechanical systems.

<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/3.gif" alt="networked-physics-splash" width="100%"/>

- **VehicleSimArmComponent & Follower**: A primary technical highlight. This is a custom-built simulation module that adds interactive arm manipulation (e.g., for loaders, cranes, or excavators). It demonstrates how to create custom PT (Physics Thread) simulation logic that integrates seamlessly with the modular vehicle tree.
- **LoaderSimComponent**: An extension of `UModularVehicleBaseComponent` that manages the core vehicle dynamics while coordinating the custom arm and bucket sub-modules.
- **Hierarchical Simulation**: Showcases complex part-to-part physics relationships and animation synchronization between the Physics Thread and Game Thread.

### 3. Mining Truck
This additional pawn is a second **Chaos Modular Vehicle–based implementation** designed to more closely resemble **real-life industrial machinery** rather than arcade-style vehicles.

<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/4.gif" alt="networked-physics-splash" width="100%"/>

- **Shared Arm System**: Uses the same `VehicleSimArmComponent` and follower setup as the LoaderPawn, validating that the arm simulation is reusable across different vehicle configurations.
- **Heavy Mass Configuration**: Significantly increased vehicle mass and inertia to emphasize momentum, stability, and resistance to sudden directional changes.
- **Low-Speed Dynamics**: Tuned for slow, deliberate movement typical of industrial vehicles rather than road cars.
- **High Gear Ratios**: Transmission and drivetrain configured with high gear ratios to provide strong torque at low speeds, closely matching real-world construction and utility vehicles.
- **Realistic Handling Feel**: Emphasizes correct torque distribution, reduced wheel slip, and physically plausible turning behavior under load.

This pawn exists primarily as a **physics validation case**, ensuring that the modular vehicle and arm systems behave correctly under heavy loads and realistic drivetrain constraints.

### 4. Wrecking Ball Controller (MyPhysicsPawn)
This component implements the manual asynchronous networked physics logic detailed in the community tutorial, serving as a robust base for state synchronization.

<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/2.gif" alt="networked-physics-splash" width="100%"/>

- **Tutorial Implementation**: Provides a clean, documented implementation of `Chaos::TSimCallbackObject` and `FNetworkPhysicsPayload`.
- **Synchronization Logic**: Demonstrates reliable input and state replication across the network with client-side prediction and server reconciliation.

---
<img src="https://raw.githubusercontent.com/cem-akkaya/NetworkedPhysics/refs/heads/master/Source/1.gif" alt="networked-physics-splash" width="100%"/>

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
