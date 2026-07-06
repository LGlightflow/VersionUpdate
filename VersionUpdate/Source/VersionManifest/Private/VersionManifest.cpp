

#include "VersionManifest.h"

#define LOCTEXT_NAMESPACE "VersionManifest"

void FVersionManifestModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module

}

void FVersionManifestModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,

}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FVersionManifestModule, VersionManifest)