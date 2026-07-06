

#include "VersionInstallationManage.h"
#include "Version/PatchManifest.h"

#define LOCTEXT_NAMESPACE "FVersionInstallationManage"

namespace
{
	// InstallDir 既可能来自编辑器配置，也可能来自 json。
	// 这里统一去掉首尾斜杠，避免后面拼接目标路径时产生重复或缺失分隔符。
	FString NormalizeInstallDir(FString InstallDir)
	{
		FPaths::NormalizeFilename(InstallDir);
		InstallDir.RemoveFromStart(TEXT("/"));
		InstallDir.RemoveFromEnd(TEXT("/"));
		return InstallDir;
	}

	// 安装完成后的目标路径必须和版本检测阶段使用的路径规则一致，
	// 否则会出现“下载时判定正确、安装后校验却找不到文件”的问题。
	FString GetInstallTargetPath(
		const FRemotePatchFile& File,
		const FString& InProjectToContentPaks,
		const FString& InProjectRootPath)
	{
		if (File.Tag == EPatchFileTag::Pak || File.Tag == EPatchFileTag::PakSig)
		{
			return FPaths::ConvertRelativePathToFull(InProjectToContentPaks / File.Name);
		}

		const FString InstallDir = NormalizeInstallDir(File.InstallDir);
		return InstallDir.IsEmpty()
			? FPaths::ConvertRelativePathToFull(InProjectRootPath / File.Name)
			: FPaths::ConvertRelativePathToFull(InProjectRootPath / InstallDir / File.Name);
	}

	// 下载阶段已经校验过临时文件，但复制到正式目录后仍可能因为磁盘、占用或中断导致目标文件异常。
	// 因此安装阶段要再次按 Manifest 对最终落盘结果做存在性、大小和 Hash 校验。
	bool IsInstalledFileValid(const FString& TargetPath, const FRemotePatchFile& File)
	{
		if (!FPaths::FileExists(TargetPath))
		{
			UE_LOG(LogVersionInstallation, Error, TEXT("Installed file missing: %s"), *TargetPath);
			return false;
		}

		const int64 LocalSize = IFileManager::Get().FileSize(*TargetPath);
		if (File.Size > 0 && LocalSize != File.Size)
		{
			UE_LOG(LogVersionInstallation, Error, TEXT("Installed file size mismatch: %s, Local=%lld, Expected=%lld"), *TargetPath, LocalSize, File.Size);
			return false;
		}

		if (!File.Hash.IsEmpty())
		{
			const FString LocalHash = LexToString(FMD5Hash::HashFile(*TargetPath));
			if (!LocalHash.Equals(File.Hash, ESearchCase::IgnoreCase))
			{
				UE_LOG(LogVersionInstallation, Error, TEXT("Installed file hash mismatch: %s, Local=%s, Expected=%s"), *TargetPath, *LocalHash, *File.Hash);
				return false;
			}
		}

		return true;
	}
}

FVersionInstallationManage::FVersionInstallationManage()
{
}

FVersionInstallationManage::~FVersionInstallationManage()
{
}

void FVersionInstallationManage::CollectFiles(
	const FString& InResourcesToCopied,
	TArray<FString>& OutFoundFiles)
{
	//资源遍历
	UE_LOG(LogVersionInstallation, Display, TEXT("Resource traversal."));

	if (!InResourcesToCopied.IsEmpty())
	{
		IFileManager::Get().FindFilesRecursive(OutFoundFiles, *InResourcesToCopied, TEXT("*"), true, false);
	}
}

void FVersionInstallationManage::CollectFilesCustom(
	const FString& InResourcesToCopiedCustom,
	const TArray<FString>& InRelativePatchs,
	TArray<FString>& OutCustomFoundFiles,
	TArray<FVersionInstallationCustomCopyPath>& OutCustomCopyPaths)
{
	UE_LOG(LogVersionInstallation, Display, TEXT("Resource Custom traversal."));

	//收集自定义拷贝路径
	if (InRelativePatchs.Num() > 0)
	{
		IFileManager::Get().FindFilesRecursive(OutCustomFoundFiles, *InResourcesToCopiedCustom, TEXT("*"), true, false);

		for (auto& Tmp : InRelativePatchs)
		{
			FString RelativePatchKey = FPaths::GetCleanFilename(Tmp);
			for (auto& CustomTmp : OutCustomFoundFiles)
			{
				FString CustomFoundFilesKey = FPaths::GetCleanFilename(CustomTmp);
				if (RelativePatchKey == CustomFoundFilesKey)
				{
					FVersionInstallationCustomCopyPath& CopyPath = OutCustomCopyPaths.AddDefaulted_GetRef();
					CopyPath.From = CustomTmp;
					CopyPath.To = Tmp;
					break;
				}
			}
		}
	}
}

void FVersionInstallationManage::Wait(float& InProgressInstallationPercentage, int32 InPakNumber, int32 InCustomPakNumber)
{
	if (InPakNumber != 0 || InCustomPakNumber != 0)
	{
		//IV.检测百分比的变化
		UE_LOG(LogVersionInstallation, Display, TEXT("4. Delete temporary package."));
		while (!FMath::IsNearlyEqual(InProgressInstallationPercentage, 1.f, 0.001f) && !IsEngineExitRequested())
		{
			UE_LOG(LogVersionInstallation, Display, TEXT("Start waiting for program progress...[If the program is stuck here for a long time, it may be occupied]"));
			UE_LOG(LogVersionInstallation, Display, TEXT("InProgressInstallationPercentage = %f"),InProgressInstallationPercentage);
		
			FPlatformProcess::Sleep(0.5f);
		}
	}
}

void FVersionInstallationManage::DeleteResourceFile(const TArray<FString>& InFoundFiles)
{
	for (auto& Tmp : InFoundFiles)
	{
		DeleteFile(*Tmp);
	}
}

void FVersionInstallationManage::HandleDescribe(
	const FString& InProjectToContentPaks,
	const FString& InProjectRootPath,
	const TArray<FRemotePatchFile>& InDiscardInfo)
{
	for (const auto& Tmp : InDiscardInfo)
	{
		if (!Tmp.Name.IsEmpty())
		{
			DeleteFile(*GetInstallTargetPath(Tmp, InProjectToContentPaks, InProjectRootPath));
		}
	}
}

bool FVersionInstallationManage::VerifyInstalledFiles(
	const FString& InProjectToContentPaks,
	const FString& InProjectRootPath,
	const TArray<FRemotePatchFile>& InInstalledFiles) const
{
	// 这里校验的是“本轮应该安装成功的文件集合”。
	// 只要有一个目标文件和 Manifest 不一致，就视为安装失败，并保留临时下载文件供下次恢复或排查。
	for (const FRemotePatchFile& File : InInstalledFiles)
	{
		if (File.Name.IsEmpty() || File.bDiscard)
		{
			continue;
		}

		const FString TargetPath = GetInstallTargetPath(File, InProjectToContentPaks, InProjectRootPath);
		if (!IsInstalledFileValid(TargetPath, File))
		{
			return false;
		}
	}

	return true;
}

void FVersionInstallationManage::DeleteFile(const TCHAR* NewFilename)
{
	if (IFileManager::Get().Delete(NewFilename))
	{
		UE_LOG(LogVersionInstallation, Display, TEXT("Delete = [%s] Success."), NewFilename);
	}
	else
	{
		UE_LOG(LogVersionInstallation, Error, TEXT("Delete = [%s] Fail."), NewFilename);
	}
}

#undef LOCTEXT_NAMESPACE
