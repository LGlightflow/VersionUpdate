#include "VersionUpdateSubsystem.h"

#include "Settings/VersionManifestClientObject.h"

void UVersionUpdateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UVersionUpdateSubsystem::InitializeVersionUpdate(const FString& InCurrentVersion)
{
	SetCurrentVersion(InCurrentVersion);
	LoadOrCreateClientManifest();
	ResetInstallationProgress();

	PendingDownloadFiles.Empty();
	PendingDiscardFiles.Empty();
	RelativePatchs.Empty();
	MountedPakCache.Empty();

	bLastDownloadSucceeded = true;
	bSeamlessHotUpdate = GetDefault<UVersionManifestClientObject>()->bSeamlessHotUpdate;
	bVersionUpdateInitialized = true;

	OnVersionUpdateInitialized.Broadcast(CurrentVersion);
}

void UVersionUpdateSubsystem::SetCurrentVersion(const FString& InCurrentVersion)
{
	CurrentVersion = InCurrentVersion;
	CurrentVersion.TrimStartAndEndInline();
	ClientManifest.CurrentVersion = CurrentVersion;
}

FString UVersionUpdateSubsystem::GetCurrentVersion() const
{
	return CurrentVersion;
}

bool UVersionUpdateSubsystem::IsVersionUpdateInitialized() const
{
	return bVersionUpdateInitialized;
}
