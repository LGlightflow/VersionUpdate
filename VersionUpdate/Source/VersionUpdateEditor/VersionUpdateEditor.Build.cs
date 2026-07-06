

using UnrealBuildTool;

public class VersionUpdateEditor : ModuleRules
{
	public VersionUpdateEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// ... add other public dependencies that you statically link with here ...
			}
			);

		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			});

		PublicDefinitions.Add("UE5_ENIGNE");
		if (PublicDefinitions.Contains("UE5_ENIGNE"))
        {
			PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"EditorFramework",
			});
		}

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"InputCore",	
				"UnrealEd",
				"ToolMenus",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"ContentBrowser",
				"Settings",
				"PropertyEditor",
				"VersionUpdate",
				"VersionManifest",
				"VersionPak",
				"AssetRegistry",
				"PakFileUtilities"
				// ... add private dependencies that you statically link with here ...	
			});

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			});
	}
}
