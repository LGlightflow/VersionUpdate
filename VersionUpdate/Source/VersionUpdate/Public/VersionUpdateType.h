
#pragma once

#include "CoreMinimal.h"
#ifdef VERSIONUPDATE_OPEN_ENGINE_MODULE
#include "Engine/EngineTypes.h"
#endif
#include "VersionUpdateType.generated.h"

#ifndef VERSIONUPDATE_OPEN_ENGINE_MODULE
struct FFilePath;
#endif



UENUM(BlueprintType)
enum class EServerVersionResponseType :uint8
{
	VERSION_HOTUPDATE			UMETA(DisplayName = "Version HotUpdate"),
	MAJORVERSION_UPDATE			UMETA(DisplayName = "MajorVersion Update"),
	VERSION_EQUAL					UMETA(DisplayName = "Version Equal"),
	CONNECTION_ERROR		UMETA(DisplayName = "Connection Error"),
	INITIALIZATION_ERROR UMETA(DisplayName = "Initialization Error"),
};
