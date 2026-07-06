// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class VersionInstallationProcessor : ModuleRules
{
	public VersionInstallationProcessor(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicIncludePaths.Add("Runtime/Launch/Public");

		PrivateIncludePaths.Add("Runtime/Launch/Private");		// For LaunchEngineLoop.cpp include

		PrivateDependencyModuleNames.Add("Core");
		PrivateDependencyModuleNames.Add("Projects");
		PrivateDependencyModuleNames.Add("ApplicationCore");
		PrivateDependencyModuleNames.Add("Slate");
		PrivateDependencyModuleNames.Add("SlateCore");
		PrivateDependencyModuleNames.Add("StandaloneRenderer");
		PrivateDependencyModuleNames.Add("Json");
		PrivateDependencyModuleNames.Add("UnThread");
		PrivateDependencyModuleNames.Add("VersionManifest");
		PrivateDependencyModuleNames.Add("VersionInstallation");
	}
}
