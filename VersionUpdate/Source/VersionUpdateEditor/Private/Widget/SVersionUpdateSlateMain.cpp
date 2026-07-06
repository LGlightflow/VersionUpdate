#include "Widget/SVersionUpdateSlateMain.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Widgets/Layout/SScrollBox.h"
#include "VersionUpdateEditor.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "SVersionUpdateSlateMain"

void SVersionUpdateSlateMain::Construct(const FArguments& InArgs)
{
	// 创建 UObject
	ManifestObject = NewObject<UVersionManifestObject>();

	// Property Editor
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowMultipleTopLevelObjects = false;
	DetailsArgs.bShowPropertyMatrixButton = false;

	DetailsView = PropertyModule.CreateDetailView(DetailsArgs);
	DetailsView->SetObject(ManifestObject);

	ChildSlot
		[
			SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							DetailsView.ToSharedRef()
						]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.f)
				[
					SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
								.Text(LOCTEXT("RebuildFileInfo", "Rebuild File Info"))
								.OnClicked(this, &SVersionUpdateSlateMain::OnRebuildFileInfoClicked)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(4.f, 0.f)
						[
							SNew(SButton)
								.Text(LOCTEXT("ImportJson", "Import JSON"))
								.OnClicked(this, &SVersionUpdateSlateMain::OnImportJsonClicked)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
								.Text(LOCTEXT("ExportJson", "Export JSON"))
								.OnClicked(this, &SVersionUpdateSlateMain::OnExportJsonClicked)
						]
				]
		];
}


FReply SVersionUpdateSlateMain::OnRebuildFileInfoClicked()
{
	RebuildFileInfo();
	DetailsView->ForceRefresh();
	return FReply::Handled();
}


FReply SVersionUpdateSlateMain::OnImportJsonClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform) return FReply::Handled();

	TArray<FString> Files;
	const void* ParentHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	const bool bOpened = DesktopPlatform->OpenFileDialog(
		ParentHandle,
		LOCTEXT("ImportJsonFile", "Select JSON File").ToString(),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("JSON files (*.json)|*.json"),
		EFileDialogFlags::None,
		Files
	);

	if (bOpened && Files.Num() > 0)
	{
		ImportJson(Files[0]);
		DetailsView->ForceRefresh();
	}
	return FReply::Handled();
}

FReply SVersionUpdateSlateMain::OnExportJsonClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform) return FReply::Handled();

	TArray<FString> Files;
	const void* ParentHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	const bool bSaved = DesktopPlatform->SaveFileDialog(
		ParentHandle,
		LOCTEXT("ExportJsonFile", "Save JSON File").ToString(),
		FPaths::ProjectDir(),
		TEXT("PatchList.json"),
		TEXT("JSON files (*.json)|*.json"),
		EFileDialogFlags::None,
		Files
	);

	if (bSaved && Files.Num() > 0)
	{
		ExportJson(Files[0]);
	}
	return FReply::Handled();
}

void SVersionUpdateSlateMain::RebuildFileInfo()
{
	if (!ManifestObject) return;

	// ---------- 1. 生成 PatchList.Url ----------
	{
		FString Url = ManifestObject->DomainOrIP;

		if (!Url.EndsWith(TEXT("/")))
		{
			Url += TEXT("/");
		}

		Url += ManifestObject->BucketName;
		Url += TEXT("/");
		Url += ManifestObject->Platform;

		ManifestObject->PatchList.Url = Url;
	}


	auto GetTagByExtension = [](const FString& Ext) -> EPatchFileTag
		{
			if (Ext.Equals(TEXT("pak"), ESearchCase::IgnoreCase))
				return EPatchFileTag::Pak;
			if (Ext.Equals(TEXT("sig"), ESearchCase::IgnoreCase))
				return EPatchFileTag::PakSig;
			if (Ext.Equals(TEXT("dll"), ESearchCase::IgnoreCase) ||
				Ext.Equals(TEXT("exe"), ESearchCase::IgnoreCase))
				return EPatchFileTag::DLL;
			return EPatchFileTag::None;
		};

	auto GetInstallDirByTag = [](EPatchFileTag FileTag) -> FString
		{
			switch (FileTag)
			{
			case EPatchFileTag::Pak:
			case EPatchFileTag::PakSig:
				return TEXT("Content/Paks");

			case EPatchFileTag::DLC:
				return TEXT("Content/DLC");

			case EPatchFileTag::DLL: {
				FString PlatformName = FPlatformProperties::PlatformName();
				if (PlatformName.Contains(TEXT("Windows")))
				{
					return TEXT("Win64");
				}
				return FString("Binaries/") + PlatformName;
			}
				

			case EPatchFileTag::Custom:
			case EPatchFileTag::None:
			default:
				return TEXT("Content");
			}
		};

	for (FPatchVersion& Patch : ManifestObject->PatchList.Patches)
	{
		for (FRemotePatchFile& File : Patch.Files)
		{
			const FString SourcePath = File.SourceFile.FilePath;

			const bool bHasSourcePath =!SourcePath.IsEmpty() && FPaths::FileExists(SourcePath);

			const bool bHasCachedInfo =(!File.Hash.IsEmpty() || File.Size > 0);

			if (!bHasSourcePath && bHasCachedInfo)
			{
				continue;
			}

			// Name
			File.Name = FPaths::GetCleanFilename(SourcePath);

			// Size
			File.Size = IFileManager::Get().FileSize(*SourcePath);

			// Tag
			const FString Ext = FPaths::GetExtension(SourcePath);
			File.Tag = GetTagByExtension(Ext);

			// InstallDir
			File.InstallDir = GetInstallDirByTag(File.Tag);

			// Hash
			File.Hash = LexToString(FMD5Hash::HashFile(*SourcePath));
		}
	}
	for (FRemotePatchFile& File : ManifestObject->PatchList.MajorVersionFiles)
	{
		const FString SourcePath = File.SourceFile.FilePath;
		if (SourcePath.IsEmpty() || !FPaths::FileExists(SourcePath))
		{
			File.Hash.Empty();
			File.Size = 0;
			continue;
		}

		// Name
		File.Name = FPaths::GetCleanFilename(SourcePath);

		// Size
		File.Size = IFileManager::Get().FileSize(*SourcePath);

		// Tag
		const FString Ext = FPaths::GetExtension(SourcePath);
		File.Tag = GetTagByExtension(Ext);

		// InstallDir
		File.InstallDir = GetInstallDirByTag(File.Tag);

		// Hash
		File.Hash = LexToString(FMD5Hash::HashFile(*SourcePath));
	}

}

bool SVersionUpdateSlateMain::ImportJson(const FString& FilePath)
{
	if (FilePath.IsEmpty() || !ManifestObject) return false;

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("ImportJson failed: load json failed %s"), *FilePath);
		return false;
	}

	FPatchList LoadedList;
	if (!PatchManifest::Read(Json, LoadedList))
	{
		UE_LOG(LogTemp, Error, TEXT("ImportJson failed: parse json failed %s"), *FilePath);
		return false;
	}

	ManifestObject->PatchList = MoveTemp(LoadedList);
	return true;
}


bool SVersionUpdateSlateMain::ExportJson(const FString& FilePath)
{
	if (FilePath.IsEmpty() || !ManifestObject) return false;

	FString Json;
	PatchManifest::Save(ManifestObject->PatchList, Json);

	return FFileHelper::SaveStringToFile(Json, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8);
}

#undef LOCTEXT_NAMESPACE