
#pragma once

#include "CoreMinimal.h"
#include "VersionPakType.h"
#include "VersionPakLogChannels.h"

namespace VersionPak
{
	FString AnalysisClassName(const FString& InName)
	{
		return InName;
	}

	template<class T>
	T* StaticLoadPakObject(const FString& Filename,const TCHAR* Suffix = NULL, const TCHAR* Prefix = NULL)
	{
		const FString ObjectName = (Prefix != NULL ? FString(Prefix) : AnalysisClassName(T::StaticClass()->GetName())) +
			TEXT("'") + Filename + TEXT(".") + FPaths::GetCleanFilename(Filename) +
			(Suffix == NULL ? TEXT("") : Suffix)+ TEXT("'");

		UE_LOG(LogVersionPak, Log, TEXT("StaticLoadPakObject>>ObjectName = %s"), *ObjectName);

		return Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *ObjectName));;
	}

	//WidgetBlueprint'/Game/ThirdPersonCPP/Blueprints/NewWidgetBlueprint.NewWidgetBlueprint'
	template<class T>
	T* StaticLoadObjectFromPak(const FString& Filename, const TCHAR* Suffix = NULL,const TCHAR* Prefix = NULL)
	{
		if (T* ObjectInstance = StaticLoadPakObject<T>(Filename, Suffix, Prefix))
		{
			return ObjectInstance;
		}

		return NULL;
	}

	FString GetShortMountPoint(const FString& PakMountPoint)
	{
		if (!PakMountPoint.Len())
		{
			return FPaths::ProjectDir() + FApp::GetProjectName() / FString(TEXT("Content/"));
		}

		auto Pos = PakMountPoint.Find("Content/");
		auto NewPoint = PakMountPoint.RightChop(Pos);

		NewPoint.RemoveFromStart(TEXT("/"));
		NewPoint.RemoveFromStart(TEXT("\\"));

		FString ProjectDir = FPaths::ProjectDir();
		FString ProjectName = FApp::GetProjectName();

		//../../../ProjectName/Content/SS/TT/
		//return FPaths::ProjectDir() / NewPoint;
		//return TEXT("../../..") / FString(FApp::GetProjectName()) / NewPoint / TEXT("");
		if (ProjectDir.Contains(TEXT("../../../")) && 
			ProjectDir.Contains(ProjectName))
		{
			return FPaths::ProjectDir() /*+ FApp::GetProjectName()*/ / NewPoint / TEXT("");
		}
		else
		{
			return TEXT("../../..") / FString(FApp::GetProjectName()) / NewPoint / TEXT("");
		}
	}

	void AESDecrypt(const FVersionPakConfig& InPakConfig)
	{
		if (!InPakConfig.AES.IsEmpty())
		{
			if (InPakConfig.AES.Len() == 32)
			{
				auto PakEncryptionKey = [&](uint8 OutKey[32])
				{
					FMemory::Memcpy(OutKey,TCHAR_TO_UTF8(*InPakConfig.AES), 32);
				};

				FCoreDelegates::GetPakEncryptionKeyDelegate().BindLambda(PakEncryptionKey);
			}
		}
		else if (InPakConfig.bConfiguration)
		{

		}
	}
}
