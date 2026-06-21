// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LoreQuickCommit : ModuleRules
{
	public LoreQuickCommit(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"Slate",
			"SlateCore",
			"ToolWidgets",       // Common editor Slate widgets / styling helpers
			"Projects",          // IPluginManager, plugin resources
			"DeveloperSettings", // UDeveloperSettings base class for our settings object
			"HTTP",              // Async server health-check requests

			// Editor-only modules. This module is Type "Editor" so these are always available.
			"UnrealEd",          // FEditorFileUtils (save-dirty-packages prompt)
			"LevelEditor",       // Level editor toolbar extension point
			"ToolMenus",         // UToolMenus toolbar registration
			"EditorStyle",       // FAppStyle icons
		});
	}
}
