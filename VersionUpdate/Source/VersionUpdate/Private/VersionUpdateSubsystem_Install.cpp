#include "VersionUpdateSubsystem.h"

#include "GameMapsSettings.h"
#include "Kismet/GameplayStatics.h"
#include "VersionInstallationManage.h"
#include "VersionUpdateLogChannels.h"

#if VERSIONUPDATE_USE_HOTPATCHER_PAK
#include "FlibPakHelper.h"
#else
#include "VersionPakBPLibrary.h"
#endif

namespace
{
	// 安装阶段的全局进度状态。安装管理器通过 FCopyProgress::Poll 回调累加整体进度
	float PakNumber = 0.f;
	float CustomPakNumber = 0.f;
	float LastFraction = 0.f;
	float PercentageInterval = 0.f;
	float VersionUpdateInstallationProgressValue = 0.f;

	// 获取外部安装器和项目 exe 所在的平台目录名
	FString GetPlatformName()
	{
		const FString PlatformName = FPlatformProperties::PlatformName();
		return PlatformName.Contains(TEXT("Windows")) ? TEXT("Win64") : PlatformName;
	}

	// 临时 ClientManifest 路径。
	// 无感更新和独立安装器都会先写这个文件，安装成功后再提交到正式 ClientVersion.config。
	FString GetTempClientManifestPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TempClientVersionJsonRelativePath);
	}

	// 保存安装成功后应该提交的临时 ClientManifest。
	bool SaveTempClientManifest(const FClientVersionFilesList& Manifest)
	{
		return VersionClientJson::SaveToFile(Manifest, GetTempClientManifestPath());
	}

	// 安装成功并写入正式 ClientManifest 后，清理临时清单。
	void DeleteTempClientManifest()
	{
		IFileManager::Get().Delete(*GetTempClientManifestPath());
	}
	
	void GetMountedPakFilenames(TArray<FString>& OutPakFilenames)
	{
#if VERSIONUPDATE_USE_HOTPATCHER_PAK
		OutPakFilenames = UFlibPakHelper::GetAllMountedPaks();
#else
		UVersionPakBPLibrary::GetMountedPakFilenames(OutPakFilenames);
#endif
	}

	bool UnmountPakFile(const FString& PakFile)
	{
#if VERSIONUPDATE_USE_HOTPATCHER_PAK
		return UFlibPakHelper::UnMountPak(PakFile);
#else
		return UVersionPakBPLibrary::UnmountPak(PakFile);
#endif
	}

	bool MountPakFile(const FString& PakFile)
	{
#if WITH_EDITOR
		UE_LOG(LogVersionUpdate, Warning, TEXT("[VersionUpdate] Skip runtime Pak mount in editor: %s"), *PakFile);
		return true;
#else
#if VERSIONUPDATE_USE_HOTPATCHER_PAK
		const int32 PakOrder = FMath::Max(UFlibPakHelper::GetPakOrderByPakPath(PakFile), 100);
		return UFlibPakHelper::MountPak(PakFile, PakOrder, TEXT(""));
#else
		return UVersionPakBPLibrary::MountPak(PakFile, 100, TEXT(""));
#endif
#endif
	}

	// 主包通常是 ProjectName-Windows.pak / pakchunk0-Windows.pak 这类基础资源包；
	// 热更新补丁 Pak 通常带 patch 标识或 UE patch 约定的 _P 后缀。
	// 这里保守处理：只有明显是补丁 Pak 的文件才允许当前进程尝试卸载。
	bool IsPatchPakFile(const FString& PakFile)
	{
		FString BaseName = FPaths::GetBaseFilename(PakFile).ToLower();
		return BaseName.Contains(TEXT("patch")) || BaseName.EndsWith(TEXT("_p"));
	}

	// 文件复制进度回调。每个文件按同等权重累加到总安装进度。
	struct FPakInstallationProgress : public FCopyProgress
	{
		virtual ~FPakInstallationProgress() {}

		// Fraction 是当前正在复制的单个文件进度，函数内转换成整体安装进度。
		virtual bool Poll(float Fraction) override
		{
			PercentageInterval = Fraction - LastFraction;

			const float Total = PakNumber + CustomPakNumber;
			if (Total > 0.f)
			{
				VersionUpdateInstallationProgressValue += PercentageInterval / Total;
			}

			// 单文件完成后重置 LastFraction
			LastFraction = (Fraction == 1.f) ? 0.f : Fraction;
			return true;
		}
	};
}

// 获取安装阶段进度。下载进度请使用 OnVersionDownloadProgress。
float UVersionUpdateSubsystem::GetInstallationProgress() const
{
	return VersionUpdateInstallationProgressValue;
}

// 重置安装进度状态，避免上一次安装残留影响本轮 UI。
void UVersionUpdateSubsystem::ResetInstallationProgress()
{
	PakNumber = 0.f;
	CustomPakNumber = 0.f;
	LastFraction = 0.f;
	PercentageInterval = 0.f;
	VersionUpdateInstallationProgressValue = 0.f;
}

// 安装当前 PendingDownloadFiles。根据具体情况决定走无感安装，还是启动独立安装器。
bool UVersionUpdateSubsystem::InstallDownloadedFiles()
{
	const bool bSucceeded = bSeamlessHotUpdate ? ExecuteSeamlessInstall() : LaunchExternalInstaller();
	OnVersionInstallCompleted.Broadcast(bSucceeded);
	return bSucceeded;
}

// 无感安装
bool UVersionUpdateSubsystem::ExecuteSeamlessInstall()
{
	ResetInstallationProgress();

	// 先生成“安装成功后应该保存”的客户端清单，但暂不提交到正式 ClientVersion.config。
	// 如果后续任一步失败，会恢复 OriginalClientManifest，避免内存状态误变成新版本。
	const FClientVersionFilesList OriginalClientManifest = ClientManifest;
	UpdateClientManifestAfterInstall();
	const FClientVersionFilesList InstalledClientManifest = ClientManifest;
	if (!SaveTempClientManifest(InstalledClientManifest))
	{
		ClientManifest = OriginalClientManifest;
		return false;
	}

	// 当前进程内覆盖 Pak 前需要卸载已挂载 Pak，避免文件占用导致复制失败。
	if (!UnmountMountedPaks())
	{
		ClientManifest = OriginalClientManifest;
		return false;
	}

	const FString ResourcesToCopiedCustom = GetDownloadPathToCustom();
	const FString ResourcesToCopied = GetDownloadPathToPak();
	const FString ProjectToContentPaks = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("Paks"));
	const FString ProjectRootPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// 安装管理器负责复制、复制后校验、删除临时下载文件和删除废弃文件。
	// 校验依据使用临时 ClientManifest 中的最终 InstalledPatchFiles，保证无感和独立安装器使用同一份清单。
	const bool bInstallOK = FVersionInstallationManage().HandleHotInstallation<FPakInstallationProgress>(
		ResourcesToCopied,
		ResourcesToCopiedCustom,
		ProjectToContentPaks,
		ProjectRootPath,
		RelativePatchs,
		InstalledClientManifest.InstalledPatchFiles,
		PendingDiscardFiles,
		PakNumber,
		CustomPakNumber,
		VersionUpdateInstallationProgressValue);

	if (!bInstallOK)
	{
		// 安装失败时尽量恢复之前卸载的 Pak，并恢复内存里的旧 ClientManifest。
		MountInstalledPatchPaks(OriginalClientManifest);
		ClientManifest = OriginalClientManifest;
		return false;
	}

	// 文件已安装成功，但重新挂载失败也会导致当前进程状态不可靠。
	// 文件复制、废弃文件删除和安装后校验完成后，按最终清单挂载仍然有效的补丁 Pak。
	// 这样本轮新增 Pak 会立即进入当前进程，被废弃的旧 Pak 不会再挂回去。
	if (!MountInstalledPatchPaks(InstalledClientManifest))
	{
		return false;
	}

	// 只有安装和校验都成功后，才提交正式 ClientManifest。
	ClientManifest = InstalledClientManifest;
	if (!SaveClientManifest())
	{
		return false;
	}

	DeleteTempClientManifest();

	// 无感安装完成后重进启动地图，让已更新资源在当前进程里重新加载。
	const FString StartupMap = GetDefault<UGameMapsSettings>()->GetGameDefaultMap();
	if (!StartupMap.IsEmpty())
	{
		UGameplayStatics::OpenLevel(GetWorld(), *StartupMap);
	}

	return true;
}

// 根据服务端 Manifest、本地已安装记录、本轮下载列表和废弃列表，生成安装成功后的 ClientManifest。
void UVersionUpdateSubsystem::UpdateClientManifestAfterInstall()
{
	// 安装阶段统一使用版本检测模块计算出的服务端目标版本，避免大版本安装和普通热更写入不同规则的版本号。
	ClientManifest.CurrentVersion = GetLatestServerVersion();

	// 先把旧 InstalledPatchFiles 转成 Map，后续按安装相对路径覆盖，避免重复线性扫描。
	TMap<FString, FRemotePatchFile> InstalledFilesByPath;
	for (const FRemotePatchFile& InstalledFile : ClientManifest.InstalledPatchFiles)
	{
		if (!InstalledFile.Name.IsEmpty())
		{
			InstalledFilesByPath.Add(PatchManifest::GetInstallRelativePath(InstalledFile), InstalledFile);
		}
	}

	TSet<FString> DiscardKeys;
	auto MarkDiscarded = [&DiscardKeys, &InstalledFilesByPath](const FRemotePatchFile& File)
	{
		if (File.Name.IsEmpty())
		{
			return;
		}

		// 废弃记录只影响 ClientManifest 状态，实际文件删除由安装管理器或独立安装器完成。
		const FString FileKey = PatchManifest::GetInstallRelativePath(File);
		DiscardKeys.Add(FileKey);
		InstalledFilesByPath.Remove(FileKey);
	};

	// PendingDiscardFiles 是本轮比较阶段明确算出的废弃文件。
	for (const FRemotePatchFile& DiscardFile : PendingDiscardFiles)
	{
		MarkDiscarded(DiscardFile);
	}

	// 服务端补丁里也可能直接标记 bDiscard，需要一起移出 InstalledPatchFiles。
	for (const FPatchVersion& PatchVersion : ServerManifest.Patches)
	{
		for (const FRemotePatchFile& ServerFile : PatchVersion.Files)
		{
			if (ServerFile.bDiscard)
			{
				MarkDiscarded(ServerFile);
			}
		}
	}

	auto AddInstalled = [&DiscardKeys, &InstalledFilesByPath](const FRemotePatchFile& InstalledFile)
	{
		const FString InstalledKey = PatchManifest::GetInstallRelativePath(InstalledFile);
		if (InstalledFile.Name.IsEmpty() || InstalledFile.bDiscard || DiscardKeys.Contains(InstalledKey))
		{
			return;
		}

		// 同一路径后写入者覆盖先写入者，保证 Hash / Size 等元数据取最新状态。
		InstalledFilesByPath.Add(InstalledKey, InstalledFile);
	};

	// 先按服务端补丁列表重建安装后应存在的基础文件集合。
	for (const FPatchVersion& PatchVersion : ServerManifest.Patches)
	{
		for (const FRemotePatchFile& ServerFile : PatchVersion.Files)
		{
			AddInstalled(ServerFile);
		}
	}

	// 再用本轮实际下载文件覆盖一次，确保临时清单中的元数据与本轮安装目标一致。
	for (const FRemotePatchFile& InstalledFile : PendingDownloadFiles)
	{
		AddInstalled(InstalledFile);
	}

	ClientManifest.InstalledPatchFiles.Reset();
	InstalledFilesByPath.GenerateValueArray(ClientManifest.InstalledPatchFiles);
	ClientManifest.InstalledPatchFiles.Sort([](const FRemotePatchFile& Left, const FRemotePatchFile& Right)
	{
		return PatchManifest::GetInstallRelativePath(Left) < PatchManifest::GetInstallRelativePath(Right);
	});
}

// 启动独立安装器
bool UVersionUpdateSubsystem::LaunchExternalInstaller()
{
	// 独立安装器不持有 PendingDiscardFiles，把要删除的本地绝对路径拼到命令行里
	FString DiscardsPathsStr;
	for (const FRemotePatchFile& File : PendingDiscardFiles)
	{
		if (!File.Name.IsEmpty())
		{
			DiscardsPathsStr += PatchManifest::GetInstallAbsolutePath(File, FPaths::ProjectDir()) + TEXT("|");
		}
	}

	DiscardsPathsStr.RemoveFromEnd(TEXT("|"));
	if (DiscardsPathsStr.IsEmpty())
	{
		// 避免传空字符串导致安装器解析参数时产生歧义。
		DiscardsPathsStr = TEXT("NONE");
	}

	const FString ExePath = FPaths::ConvertRelativePathToFull(
		FPaths::EngineDir() / TEXT("Binaries") / GetPlatformName() / TEXT("VersionInstallationProcessor.exe"));

	if (!FPaths::FileExists(ExePath))
	{
		UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Installer not found: %s"), *ExePath);
		return false;
	}

	// 和无感更新一样，先写临时 ClientManifest；独立安装器安装成功后会提交到正式 ClientVersion.config。
	const FClientVersionFilesList OriginalClientManifest = ClientManifest;
	UpdateClientManifestAfterInstall();
	const FClientVersionFilesList InstalledClientManifest = ClientManifest;
	if (!SaveTempClientManifest(InstalledClientManifest))
	{
		ClientManifest = OriginalClientManifest;
		return false;
	}

	FString Param;
	Param += TEXT("-ResourcesToCopiedCustom=") + GetDownloadPathToCustom();
	Param += TEXT(" -ResourcesToCopied=") + GetDownloadPathToPak();
	Param += TEXT(" -ProjectToContentPaks=") + FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("Paks"));
	Param += TEXT(" -ProjectStartupPath=") + FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Binaries") / GetPlatformName() / FApp::GetProjectName() + TEXT(".exe"));
	Param += TEXT(" -ProjectProcessID=") + FString::FromInt(FPlatformProcess::GetCurrentProcessId());
	Param += TEXT(" -Discards=") + DiscardsPathsStr;
	Param += TEXT(" -ProjectRootPath=") + FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Param += TEXT(" -LinkURL=") + ServerManifest.Url;

	if (RelativePatchs.Num() > 0)
	{
		// 非 Pak 文件需要传入最终安装路径，安装器会在临时自定义下载目录中按文件名匹配。
		FString RelativePatchParam;
		for (const FString& Path : RelativePatchs)
		{
			RelativePatchParam += FPaths::ConvertRelativePathToFull(Path) + TEXT("+");
		}
		RelativePatchParam.RemoveFromEnd(TEXT("+"));
		Param += TEXT(" -RelativePatchs=") + RelativePatchParam;
	}

	const FProcHandle Handle = FPlatformProcess::CreateProc(*ExePath, *Param, false, false, false, nullptr, 0, nullptr, nullptr);
	if (Handle.IsValid())
	{
		// 外部安装器接管后退出当前进程，避免当前进程占用待覆盖文件。
		FPlatformMisc::RequestExit(true);
	}
	else
	{
		// 安装器未启动时恢复内存状态；临时文件保留不会影响正式版本，但后续可考虑删除。
		ClientManifest = OriginalClientManifest;
	}

	return Handle.IsValid();
}

// 卸载当前挂载的 Pak，并缓存列表供安装后恢复。
bool UVersionUpdateSubsystem::UnmountMountedPaks()
{
	MountedPakCache.Empty();

	TArray<FString> MountedPaks;
	GetMountedPakFilenames(MountedPaks);

	for (const FString& PakFile : MountedPaks)
	{
		// 不卸载主包/基础包。它们通常一直有资源请求，强制卸载会触发 UE 的 Pak outstanding requests 断言。
		if (!IsPatchPakFile(PakFile))
		{
			UE_LOG(LogVersionUpdate, Display, TEXT("[VersionUpdate] Skip mounted base pak: %s"), *PakFile);
			continue;
		}

		if (!UnmountPakFile(PakFile))
		{
			return false;
		}

		MountedPakCache.Add(PakFile);
	}

	return true;
}

// 重新挂载无感安装前缓存的 Pak。
bool UVersionUpdateSubsystem::MountInstalledPatchPaks(const FClientVersionFilesList& InstalledManifest)
{
	bool bAllMounted = true;
	TSet<FString> MountedPakPaths;

	for (const FRemotePatchFile& File : InstalledManifest.InstalledPatchFiles)
	{
		if (File.Tag != EPatchFileTag::Pak || File.Name.IsEmpty() || File.bDiscard)
		{
			continue;
		}

		const FString PakFile = PatchManifest::GetInstallAbsolutePath(File, FPaths::ProjectDir());
		if (MountedPakPaths.Contains(PakFile))
		{
			continue;
		}

		if (!IsPatchPakFile(PakFile))
		{
			UE_LOG(LogVersionUpdate, Display, TEXT("[VersionUpdate] Skip non-patch pak mount: %s"), *PakFile);
			continue;
		}

		if (!FPaths::FileExists(PakFile))
		{
			UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Installed patch pak not found: %s"), *PakFile);
			bAllMounted = false;
			continue;
		}

		const bool bMounted = MountPakFile(PakFile);
		if (bMounted)
		{
			UE_LOG(LogVersionUpdate, Display, TEXT("[VersionUpdate] Mount installed patch pak succeeded: %s"), *PakFile);
		}
		else
		{
			UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Mount installed patch pak failed: %s"), *PakFile);
		}
		bAllMounted &= bMounted;
		MountedPakPaths.Add(PakFile);
	}

	return bAllMounted;
}
