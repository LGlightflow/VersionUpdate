

#include "VersionPakBPLibrary.h"
#include "VersionPak.h"
#include "IPlatformFilePak.h"
#include "VersionPakMethod.hpp"
#include "VersionPakLogChannels.h"
#include "Blueprint/UserWidget.h"

FVersionPakConfig HotPakConfig;

FPakPlatformFile* UVersionPakBPLibrary::GetPakPlatformFile()
{
	FVersionPakModule& UnrealPakModule = FModuleManager::LoadModuleChecked<FVersionPakModule>(TEXT("VersionPak"));
	return UnrealPakModule.GetPakPlatformFile();
}

void UVersionPakBPLibrary::InitConfig(const FVersionPakConfig& InConfig)
{
	HotPakConfig = InConfig;
}

void UVersionPakBPLibrary::GetFilenamesInPakFile(const FString& InPakFilename, TArray<FString>& OutFileList)
{
	GetPakPlatformFile()->GetPrunedFilenamesInPakFile(InPakFilename, OutFileList);
}

bool UVersionPakBPLibrary::IsMounted(const FString& PakFilename)
{
	if (FPakPlatformFile* InPakFile = GetPakPlatformFile())
	{
		TArray<FString> PakFilenames;
		InPakFile->GetMountedPakFilenames(PakFilenames);

		return PakFilenames.Contains(PakFilename);
	}

	return false;
}

TArray<FString> UVersionPakBPLibrary::GetMountedReturnPakFilenames()
{
	if (FPakPlatformFile* InPakFile = GetPakPlatformFile())
	{
		TArray<FString> PakFilenames;
		InPakFile->GetMountedPakFilenames(PakFilenames);

		return PakFilenames;
	}

	return TArray<FString>();
}

void UVersionPakBPLibrary::GetMountedPakFilenames(TArray<FString>& OutPakFilenames)
{
	if (FPakPlatformFile* InPakFile = GetPakPlatformFile())
	{
		InPakFile->GetMountedPakFilenames(OutPakFilenames);
	}	
}

bool UVersionPakBPLibrary::FindFile(const FString& Directory, TArray<FString>& File, bool bRecursively)
{
	class FPakFindFile :public IPlatformFile::FDirectoryVisitor
	{
	public:
		FPakFindFile(TArray<FString>& VisitFiles)
			:VisitFiles(VisitFiles)
		{}

		/**
		 * Callback for a single file or a directory in a directory iteration.
		 * @param FilenameOrDirectory		If bIsDirectory is true, this is a directory (with no trailing path delimiter), otherwise it is a file name.
		 * @param bIsDirectory				true if FilenameOrDirectory is a directory.
		 * @return							true if the iteration should continue.
		**/
		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory)
		{
			if (!bIsDirectory)
			{
				VisitFiles.Add(FilenameOrDirectory);
			}

			return true;
		}

		TArray<FString>& VisitFiles;
	};

	FPakFindFile Visitor(File);

	if (bRecursively)
	{
		return GetPakPlatformFile()->IterateDirectoryRecursively(*Directory, Visitor);
	}

	return GetPakPlatformFile()->IterateDirectory(*Directory, Visitor);
}

UVersionPakBPLibrary::UVersionPakBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

bool UVersionPakBPLibrary::MountPak(const FString& PakFilename, int32 PakOrder, const FString& MountPath, bool bSelfAdaptionMountProjectPath)
{
	bool bMounted = false;

	FPakPlatformFile* PakFileMgr = GetPakPlatformFile();
	if (!PakFileMgr)
	{
		
		return false;
	}

	PakOrder = FMath::Max(0, PakOrder);

	if (FPaths::FileExists(PakFilename) && FPaths::GetExtension(PakFilename) == TEXT("pak"))
	{
		bool bIsEmptyMountPoint = MountPath.IsEmpty();
		const TCHAR* MountPoint = bIsEmptyMountPoint ? NULL : MountPath.GetCharArray().GetData();

#if !WITH_EDITOR

		if (PakFileMgr->Mount(*PakFilename, PakOrder, MountPoint))
		{
			
			bMounted = true;
		}
		else {
			bMounted = false;
		}

#endif
	}

	return bMounted;

}

bool UVersionPakBPLibrary::UnmountPak(const FString& PakFilename)
{
	bool bMounted = false;
	FPakPlatformFile* PakFileMgr = GetPakPlatformFile();
	if (!PakFileMgr)
	{
		return false;
	}
	if (!FPaths::FileExists(PakFilename))
	{
		return false;
	}
	return PakFileMgr->Unmount(*PakFilename);
}

UObject* UVersionPakBPLibrary::GetObjectFromPak(const FString& Filename)
{
	return VersionPak::StaticLoadObjectFromPak<UObject>(Filename);
}

UClass* UVersionPakBPLibrary::GetClassFromPak(const FString& Filename,const FString & InSuffix, const FString& InPrefix)
{
	return VersionPak::StaticLoadObjectFromPak<UClass>(Filename,
		(!InSuffix.IsEmpty() ? *InSuffix : NULL),
		(!InPrefix.IsEmpty() ? *InPrefix : NULL));
}

UMaterial* UVersionPakBPLibrary::GetMaterialFromPak(const FString& Filename)
{
	return Cast<UMaterial>(GetObjectFromPak(Filename));
}

UTexture2D* UVersionPakBPLibrary::GetTexture2DFromPak(const FString& Filename)
{
	return Cast<UTexture2D>(GetObjectFromPak(Filename));
}

USkeletalMesh* UVersionPakBPLibrary::GetSkeletalMeshFromPak(const FString& Filename)
{
	return Cast<USkeletalMesh>(GetObjectFromPak(Filename));
}

UStaticMesh* UVersionPakBPLibrary::GetStaticMeshFromPak(const FString& Filename)
{
	return Cast<UStaticMesh>(GetObjectFromPak(Filename));
}

AActor* UVersionPakBPLibrary::GetActorFromPak(UObject* WorldContextObject, const FString& Filename, const FVector& InLocation, const FRotator& InRotator)
{
	UWorld* World = Cast<UWorld>(WorldContextObject);
	if (!World)
	{
		World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	}

	if (World)
	{
		if (UClass* InGeneratedClass = GetClassFromPak(Filename,TEXT("_C"), TEXT("Blueprint")))
		{
			return World->SpawnActor<AActor>(InGeneratedClass, InLocation, InRotator);
		}
	}

	return nullptr;
}

UUserWidget* UVersionPakBPLibrary::GetUMGFromPak(UObject* WorldContextObject, const FString& Filename)
{
	UWorld* World = Cast<UWorld>(WorldContextObject);
	if (!World)
	{
		World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	}

	if (World)
	{
		if (TSubclassOf<UUserWidget> InGeneratedClass = GetClassFromPak(Filename,TEXT("_C"),TEXT("WidgetBlueprint")))
		{
			return CreateWidget<UUserWidget>(World,InGeneratedClass);
		}
	}

	return nullptr;
}

