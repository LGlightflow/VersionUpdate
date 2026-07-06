
#include "VersionInstallationProcessorStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateStyleRegistry.h"

TSharedPtr< FSlateStyleSet > FVersionInstallationProcessorStyle::StyleInstance = NULL;

void FVersionInstallationProcessorStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FVersionInstallationProcessorStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

void FVersionInstallationProcessorStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FVersionInstallationProcessorStyle::Get()
{
	return *StyleInstance;
}

FName FVersionInstallationProcessorStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("VersionInstallationProgressStyle"));
	return StyleSetName;
}

#define IMAGE_BRUSH( RelativePath, ... ) FSlateImageBrush( Style->RootToContentDir( RelativePath, TEXT(".png") ), __VA_ARGS__ )

const FVector2D Icon1920x1080(1920.0f, 1080.0f);
TSharedRef<class FSlateStyleSet> FVersionInstallationProcessorStyle::Create()
{
	FString ProgressPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Resources"));
	TSharedRef< FSlateStyleSet > Style = MakeShareable(new FSlateStyleSet("VersionInstallationProgressStyle"));
	
	Style->SetContentRoot(ProgressPath);

	Style->Set("VersionInstallationProgressStyle.Background", new IMAGE_BRUSH(TEXT("1080x1920"), Icon1920x1080));

	return Style;
}
#undef IMAGE_BRUSH