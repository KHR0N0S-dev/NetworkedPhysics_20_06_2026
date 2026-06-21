import unreal

def create_blueprint(parent_class_path, package_path, asset_name):
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    
    # Create Blueprint Factory
    blueprint_factory = unreal.BlueprintFactory()
    
    # Load parent class
    parent_class = unreal.load_class(None, parent_class_path)
    if not parent_class:
        print("ERROR: Could not load parent class: " + parent_class_path)
        return None
    
    blueprint_factory.set_editor_property("ParentClass", parent_class)
    
    # Create the asset
    created_asset = asset_tools.create_asset(
        asset_name,
        package_path,
        unreal.Blueprint,
        blueprint_factory
    )
    
    if created_asset:
        print("SUCCESS: Created Blueprint " + package_path + "/" + asset_name)
        # Save it
        unreal.EditorAssetLibrary.save_asset(created_asset.get_path_name())
        return created_asset
    else:
        print("ERROR: Failed to create " + asset_name)
        return None

# Create Blueprint for GameMode
create_blueprint(
    "/Script/Sandbox.StandardChaosVehicleGameMode",
    "/Game/Blueprints",
    "StandardChaosVehicleGameMode_BP"
)

# Create Blueprint for Pawn
create_blueprint(
    "/Script/Sandbox.StandardChaosVehiclePawn",
    "/Game/Blueprints",
    "StandardChaosVehiclePawn_BP"
)

print("Blueprint creation script finished.")