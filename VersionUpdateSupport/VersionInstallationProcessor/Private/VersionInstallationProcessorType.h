// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Version/PatchManifest.h"

FString LinkURL;
FString ResourcesToCopiedCustom;
FString ResourcesToCopied;
FString ProjectToContentPaks;
FString ProjectStartupPath;
FString ProjectRootPath;
FString TempClientManifestJson;
TArray<FString> RelativePatchs;
TArray<FRemotePatchFile> InstalledPatchFiles;
uint32 ProjectProcessID = 0;
float ProgressInstallationPercentage = 0.f;
TArray<FString> DiscardPaths;
