import unreal

LEVEL_PATH = "/Game/Lvl_ModularCarTest"

les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

# --- create a fresh empty level ---
les.new_level(LEVEL_PATH)
print("Created level: " + LEVEL_PATH)

cube = unreal.load_object(None, "/Engine/BasicShapes/Cube.Cube")

# --- big floor ---
floor = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0))
floor.static_mesh_component.set_static_mesh(cube)
floor.set_actor_scale3d(unreal.Vector(80.0, 80.0, 1.0))  # 8000 x 8000 x 100 cm
floor.set_actor_label("Floor")
floor.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.QUERY_AND_PHYSICS)
floor.static_mesh_component.set_collision_object_type(unreal.CollisionChannel.ECC_WORLD_STATIC)
print("Floor spawned")

# --- player start above the floor ---
ps = eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(0.0, 0.0, 300.0))
ps.set_actor_label("PlayerStart")
print("PlayerStart spawned")

# --- lighting so we can see ---
eas.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0.0, 0.0, 1500.0), unreal.Rotator(-45.0, 0.0, 0.0))
eas.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0.0, 0.0, 1500.0))
try:
    eas.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0.0, 0.0, 0.0))
except Exception as e:
    print("SkyAtmosphere skip: " + str(e))
print("Lights spawned")

# --- assign our game mode in World Settings (GameMode Override) ---
gm_class = unreal.load_class(None, "/Script/Sandbox.ModularCarGameMode")
world = unreal.EditorLevelLibrary.get_editor_world()
ws = world.get_world_settings()
ws.set_editor_property("default_game_mode", gm_class)
print("GameMode override set to ModularCarGameMode")

# --- save ---
les.save_current_level()
print("Saved level OK")
