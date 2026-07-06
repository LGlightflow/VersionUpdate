
#include "Version/PatchManifest.h"

#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace PatchManifest
{
	FString GetInstallRelativePath(const FRemotePatchFile& File)
	{
		FString InstallDir = File.InstallDir;
		FPaths::NormalizeFilename(InstallDir);
		InstallDir.RemoveFromStart(TEXT("/"));
		InstallDir.RemoveFromEnd(TEXT("/"));
		return InstallDir.IsEmpty() ? File.Name : InstallDir / File.Name;
	}

	FString GetInstallAbsolutePath(const FRemotePatchFile& File, const FString& RootPath)
	{
		return FPaths::ConvertRelativePathToFull(RootPath / GetInstallRelativePath(File));
	}

	bool IsLocalFileMatched(const FString& FilePath, const FRemotePatchFile& File)
	{
		if (!FPaths::FileExists(FilePath))
		{
			return false;
		}

		if (File.Size > 0 && IFileManager::Get().FileSize(*FilePath) != File.Size)
		{
			return false;
		}

		return File.Hash.IsEmpty() || LexToString(FMD5Hash::HashFile(*FilePath)).Equals(File.Hash, ESearchCase::IgnoreCase);
	}

	int32 ParseVersionComponent(FString Component)
	{
		Component.TrimStartAndEndInline();

		int32 Value = 0;
		LexTryParseString(Value, *Component);
		return Value;
	}

	int32 CompareVersion(const FString& LeftVersion, const FString& RightVersion)
	{
		TArray<FString> LeftParts;
		TArray<FString> RightParts;
		LeftVersion.ParseIntoArray(LeftParts, TEXT("."), true);
		RightVersion.ParseIntoArray(RightParts, TEXT("."), true);

		const int32 ComparePartCount = FMath::Max(LeftParts.Num(), RightParts.Num());
		for (int32 Index = 0; Index < ComparePartCount; ++Index)
		{
			const int32 LeftValue = LeftParts.IsValidIndex(Index) ? ParseVersionComponent(LeftParts[Index]) : 0;
			const int32 RightValue = RightParts.IsValidIndex(Index) ? ParseVersionComponent(RightParts[Index]) : 0;
			if (LeftValue != RightValue)
			{
				return LeftValue > RightValue ? 1 : -1;
			}
		}

		return 0;
	}

	EPatchFileTag ParseTag(const FString& TagStr)
	{
		UEnum* Enum = StaticEnum<EPatchFileTag>();
		if (!Enum) return EPatchFileTag::None;

		// 1️⃣ 尝试 Name（Pak / PakSig）
		int64 Value = Enum->GetValueByNameString(TagStr);
		if (Value != INDEX_NONE)
		{
			return (EPatchFileTag)Value;
		}

		// 2️⃣ 尝试 DisplayName
		for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
		{
			if (Enum->GetDisplayNameTextByIndex(i).ToString().Equals(TagStr, ESearchCase::IgnoreCase))
			{
				return (EPatchFileTag)Enum->GetValueByIndex(i);
			}
		}

		return EPatchFileTag::None;
	}

	void WritePatchFile(TSharedRef<TJsonWriter<>> Writer, const FRemotePatchFile& File)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("name"), File.Name);
		Writer->WriteValue(TEXT("hash"), File.Hash);
		Writer->WriteValue(TEXT("size"), File.Size);
		Writer->WriteValue(TEXT("installDir"), File.InstallDir);
		Writer->WriteValue(TEXT("tag"),StaticEnum<EPatchFileTag>()->GetNameStringByValue((int64)File.Tag));
		Writer->WriteValue(TEXT("discard"), File.bDiscard);
		Writer->WriteObjectEnd();
	}

	void ReadPatchFile(const TSharedPtr<FJsonObject>& Obj, FRemotePatchFile& Out)
	{
		if (!Obj) return;

		Obj->TryGetStringField(TEXT("name"), Out.Name);
		Obj->TryGetStringField(TEXT("hash"), Out.Hash);
		Obj->TryGetStringField(TEXT("installDir"), Out.InstallDir);
		Obj->TryGetBoolField(TEXT("discard"), Out.bDiscard);

		// tag
		FString TagStr;
		if (Obj->TryGetStringField(TEXT("tag"), TagStr))
		{
			Out.Tag = ParseTag(TagStr);
		}

		// size
		double Size = 0.0;
		if (Obj->TryGetNumberField(TEXT("size"), Size))
		{
			Out.Size = (int64)Size;
		}
	}

	void Save(const FPatchList& Info, FString& OutJson)
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);

		Writer->WriteObjectStart();

		// supportVersions
		Writer->WriteArrayStart(TEXT("supportVersions"));
		for (const FString& V : Info.SupportVersions)
		{
			Writer->WriteValue(V);
		}
		Writer->WriteArrayEnd();

		// url
		Writer->WriteValue(TEXT("url"), Info.Url);

		// patches : Array<Object>
		Writer->WriteArrayStart(TEXT("patches"));
		for (const FPatchVersion& Patch : Info.Patches)
		{
			Writer->WriteObjectStart();

			// key = version
			Writer->WriteArrayStart(Patch.Version);
			for (const FRemotePatchFile& File : Patch.Files)
			{
				WritePatchFile(Writer, File);
			}
			Writer->WriteArrayEnd(); // version array

			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		// major version
		if (Info.MajorVersionFiles.Num() > 0)
		{
			Writer->WriteArrayStart(TEXT("majorVersionFiles"));
			for (const FRemotePatchFile& File : Info.MajorVersionFiles)
			{
				WritePatchFile(Writer, File);
			}
			Writer->WriteArrayEnd();
		}

		Writer->WriteObjectEnd();
		Writer->Close();
	}

	bool Read(const FString& Json, FPatchList& OutInfo)
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json), Root) || !Root)
		{
			return false;
		}

		// supportVersions
		const TArray<TSharedPtr<FJsonValue>>* Versions = nullptr;
		if (Root->TryGetArrayField(TEXT("supportVersions"), Versions))
		{
			OutInfo.SupportVersions.Empty();
			for (const auto& V : *Versions)
			{
				OutInfo.SupportVersions.Add(V->AsString());
			}
		}

		// url
		Root->TryGetStringField(TEXT("url"), OutInfo.Url);

		// patches
		const TArray<TSharedPtr<FJsonValue>>* PatchArray = nullptr;
		if (Root->TryGetArrayField(TEXT("patches"), PatchArray))
		{
			OutInfo.Patches.Empty();

			for (const auto& PatchValue : *PatchArray)
			{
				const TSharedPtr<FJsonObject> PatchObj = PatchValue->AsObject();
				if (!PatchObj) continue;

				for (const auto& Pair : PatchObj->Values)
				{
					FPatchVersion Patch;
					Patch.Version = Pair.Key;

					if (Pair.Value->Type == EJson::Array)
					{
						const TArray<TSharedPtr<FJsonValue>>& Files =
							Pair.Value->AsArray();

						for (const auto& FileValue : Files)
						{
							FRemotePatchFile File;
							ReadPatchFile(FileValue->AsObject(), File);
							Patch.Files.Add(MoveTemp(File));
						}
					}

					OutInfo.Patches.Add(MoveTemp(Patch));
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* MajorFiles = nullptr;
		if (Root->TryGetArrayField(TEXT("majorVersionFiles"), MajorFiles))
		{
			OutInfo.MajorVersionFiles.Empty();

			for (const auto& FileValue : *MajorFiles)
			{
				FRemotePatchFile File;
				ReadPatchFile(FileValue->AsObject(), File);
				OutInfo.MajorVersionFiles.Add(MoveTemp(File));
			}
		}

		return true;
	}

}
