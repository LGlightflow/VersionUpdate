

#pragma once

#include "CoreMinimal.h"
#include "VersionInstallationLogChannels.h"
#include "VersionInstallationType.h"
#include "HAL/FileManager.h"

struct FRemotePatchFile;

class VERSIONINSTALLATION_API FVersionInstallationManage
{
public:
	FVersionInstallationManage();
	virtual ~FVersionInstallationManage();

public:
	template<class T>
	bool HandleHotInstallation(
		const FString& InResourcesToCopied,
		const FString& InResourcesToCopiedCustom,
		const FString& InProjectToContentPaks,
		const FString& InProjectRootPath,
		const TArray<FString>& InRelativePatchs,
		const TArray<FRemotePatchFile>& InInstalledFiles,
		const TArray<FRemotePatchFile>& InDiscardInfo,
		float& OutPakNumber,
		float& OutCustomPakNumber,
		float& OutProgressInstallationPercentage);

public:
	//收集资源
	void CollectFiles(
		const FString& InResourcesToCopied,
		TArray<FString>& OutFoundFiles);

	void CollectFilesCustom(
		const FString& InResourcesToCopiedCustom,
		const TArray<FString>& InRelativePatchs,
		TArray<FString>& OutCustomFoundFiles,
		TArray<FVersionInstallationCustomCopyPath>& OutCustomCopyPaths);

	template<class T>
	bool ResourceCopy(const TArray<FString>& InFoundFiles,
		const FString& InProjectToContentPaks,
		TArray<T*>& OutInstallationProgressArray);

	template<class T>
	bool ResourceCopyCustom(
		const TArray<FVersionInstallationCustomCopyPath>& InCustomCopyPaths,
		TArray<T*>& OutCustomCopyPaths);

	void Wait(float& InProgressInstallationPercentage, int32 InPakNumber, int32 InCustomPakNumber);

	void DeleteResourceFile(const TArray<FString>& InFoundFiles);

	void HandleDescribe(
		const FString& InProjectToContentPaks,
		const FString& InProjectRootPath,
		const TArray<FRemotePatchFile>& InDiscardInfo);

	bool VerifyInstalledFiles(
		const FString& InProjectToContentPaks,
		const FString& InProjectRootPath,
		const TArray<FRemotePatchFile>& InInstalledFiles) const;

	template<class T>
	void DeleteProgress(const TArray<T*>& InProgress);

protected:
	void DeleteFile(const TCHAR* NewFilename);
};

template<class T>
inline bool FVersionInstallationManage::ResourceCopy(const TArray<FString>& InFoundFiles, const FString& InProjectToContentPaks, TArray<T*>& OutInstallationProgressArray)
{
	//资源拷贝
	UE_LOG(LogVersionInstallation, Display, TEXT("Resource copy."));

	for (auto& Tmp : InFoundFiles)
	{
		UE_LOG(LogVersionInstallation, Display, TEXT("Resource = [%s]"), *Tmp);

		FString TargetPath = InProjectToContentPaks / FPaths::GetCleanFilename(Tmp);
		T* InstallationProgress = new T();
		const uint32 CopyResult = IFileManager::Get().Copy(*TargetPath, *Tmp, true, false, false, InstallationProgress);

		//作为后面回收
		OutInstallationProgressArray.Add(InstallationProgress);
		if (CopyResult != COPY_OK)
		{
			UE_LOG(LogVersionInstallation, Error, TEXT("Copy failed. From=[%s], To=[%s], Result=%u"), *Tmp, *TargetPath, CopyResult);
			return false;
		}
	}

	return true;
}

template<class T>
inline bool FVersionInstallationManage::ResourceCopyCustom(const TArray<FVersionInstallationCustomCopyPath>& InCustomCopyPaths, TArray<T*>& OutCustomCopyPaths)
{
	UE_LOG(LogVersionInstallation, Display, TEXT("Custom Resource copy."));
	for (auto& Tmp : InCustomCopyPaths)
	{
		UE_LOG(LogVersionInstallation, Display, TEXT("From [%s] => [%s]"), *Tmp.From, *Tmp.To);

		T* InstallationProgress = new T();
		const uint32 CopyResult = IFileManager::Get().Copy(*Tmp.To, *Tmp.From, true, false, false, InstallationProgress);

		OutCustomCopyPaths.Add(InstallationProgress);
		if (CopyResult != COPY_OK)
		{
			UE_LOG(LogVersionInstallation, Error, TEXT("Custom copy failed. From=[%s], To=[%s], Result=%u"), *Tmp.From, *Tmp.To, CopyResult);
			return false;
		}
	}

	return true;
}

template<class T>
inline void FVersionInstallationManage::DeleteProgress(const TArray<T*>& InProgress)
{
	for (const auto& Tmp : InProgress)
	{
		delete Tmp;
	}
}

template<class T>
inline bool FVersionInstallationManage::HandleHotInstallation(
	const FString& InResourcesToCopied,
	const FString& InResourcesToCopiedCustom,
	const FString& InProjectToContentPaks,
	const FString& InProjectRootPath,
	const TArray<FString>& InRelativePatchs,
	const TArray<FRemotePatchFile>& InInstalledFiles,
	const TArray<FRemotePatchFile>& InDiscardInfo,
	float& OutPakNumber,
	float& OutCustomPakNumber,
	float& OutProgressInstallationPercentage)
{
	//II.资源遍历
	UE_LOG(LogVersionInstallation, Display, TEXT("2. Resource traversal."));

	TArray<FString> FoundFiles;
	CollectFiles(InResourcesToCopied, FoundFiles);

	//收集自定义拷贝路径
	TArray<FVersionInstallationCustomCopyPath> CustomCopyPaths;
	TArray<FString> CustomFoundFiles;
	CollectFilesCustom(
		InResourcesToCopiedCustom,
		InRelativePatchs,
		CustomFoundFiles,
		CustomCopyPaths);

	OutPakNumber = FoundFiles.Num();
	OutCustomPakNumber = CustomCopyPaths.Num();

	TArray<T*> InstallationProgressArray;

	if (!ResourceCopy(FoundFiles, InProjectToContentPaks, InstallationProgressArray))
	{
		DeleteProgress(InstallationProgressArray);
		return false;
	}

	UE_LOG(LogVersionInstallation, Display, TEXT("3.Custom Resource copy."));
	if (!ResourceCopyCustom(CustomCopyPaths, InstallationProgressArray))
	{
		DeleteProgress(InstallationProgressArray);
		return false;
	}

	//等待处理 等待异步处理完成
	Wait(OutProgressInstallationPercentage, OutPakNumber, OutCustomPakNumber);

	if (IsEngineExitRequested())
	{
		UE_LOG(LogVersionInstallation, Display, TEXT("5. Program forced exit."));
		DeleteProgress(InstallationProgressArray);
		return false;
	}

	if (!VerifyInstalledFiles(InProjectToContentPaks, InProjectRootPath, InInstalledFiles))
	{
		DeleteProgress(InstallationProgressArray);
		return false;
	}

	//V.删除临时包
	UE_LOG(LogVersionInstallation, Display, TEXT("5. Delete temporary package."));

	DeleteResourceFile(FoundFiles);
	DeleteResourceFile(CustomFoundFiles);

	//V.删除丢弃的包
	HandleDescribe(InProjectToContentPaks, InProjectRootPath, InDiscardInfo);

	//删除进度
	DeleteProgress(InstallationProgressArray);

	return true;
}
