#include "VersionInstallation.h"
#include "Core/Log/VersionInstallationProcessorLog.h"
#include "Core/Widget/SMainScreen.h"
#include "Json.h"
#include "Misc/FileHelper.h"
#include "VersionInstallationManage.h"
#include "VersionInstallationProcessorType.h"
#include "Widgets/Docking/SDockTab.h"

// 独立安装器的安装进度状态。
// 由Poll 在复制文件时按文件数量平均累加
float PakNumber = 0.f;
float CustomPakNumber = 0.f;
float LastFraction = 0.f;
float PercentageInterval = 0.f;

namespace VersionInstallation
{
	// 文件复制进度回调。
	// IFileManager::Copy 每复制一个文件都会回调当前文件进度，这里换算成整体安装进度。
	struct FInstallationProgress : public FCopyProgress
	{
		virtual bool Poll(float Fraction) override
		{
			PercentageInterval = Fraction - LastFraction;

			const float Total = PakNumber + CustomPakNumber;
			if (Total > 0.f)
			{
				ProgressInstallationPercentage += PercentageInterval / Total;
			}

			// 单文件完成后重置，保证下一个文件从 0 开始时不会产生负进度。
			LastFraction = (Fraction == 1.f) ? 0.f : Fraction;
			return true;
		}
	};

	// 获取 Subsystem 写出的临时 ClientManifest 路径。
	// 该文件描述“安装成功后正式 ClientVersion.config 应该长什么样”。
	FString GetTempClientManifestPath()
	{
		return FPaths::ConvertRelativePathToFull(ProjectRootPath / TEXT("Content") / TempClientVersionJsonRelativePath);
	}

	// 获取正式 ClientManifest 路径。
	// 独立安装器只有在文件复制和校验都成功后才会写入这里。
	FString GetClientManifestPath()
	{
		return FPaths::ConvertRelativePathToFull(ProjectRootPath / TEXT("Content") / ClientVersionJsonRelativePath);
	}

	// 读取临时 ClientManifest，并提取 installedPatchFiles 作为安装后校验依据。
	bool LoadTempClientManifest()
	{
		const FString TempClientManifestPath = GetTempClientManifestPath();
		if (!FFileHelper::LoadFileToString(TempClientManifestJson, *TempClientManifestPath))
		{
			// 没有临时清单说明 Subsystem 没有准备好安装目标，不能继续复制文件。
			UE_LOG(LogVersionInstallationProcessor, Error, TEXT("Failed to load temp client manifest: %s"), *TempClientManifestPath);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TempClientManifestJson), Root) || !Root)
		{
			UE_LOG(LogVersionInstallationProcessor, Error, TEXT("Failed to parse temp client manifest: %s"), *TempClientManifestPath);
			return false;
		}

		InstalledPatchFiles.Empty();
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (Root->TryGetArrayField(TEXT("installedPatchFiles"), Files))
		{
			for (const TSharedPtr<FJsonValue>& FileValue : *Files)
			{
				FRemotePatchFile File;
				PatchManifest::ReadPatchFile(FileValue->AsObject(), File);
				if (!File.Name.IsEmpty())
				{
					InstalledPatchFiles.Add(MoveTemp(File));
				}
			}
		}

		return true;
	}

	// 将临时 ClientManifest 提交为正式 ClientVersion.config。
	// 该函数必须在安装管理器完成复制和校验之后调用。
	bool CommitTempClientManifest()
	{
		const FString ClientManifestPath = GetClientManifestPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ClientManifestPath), true);
		if (!FFileHelper::SaveStringToFile(TempClientManifestJson, *ClientManifestPath))
		{
			UE_LOG(LogVersionInstallationProcessor, Error, TEXT("Failed to save client manifest: %s"), *ClientManifestPath);
			return false;
		}

		// 正式清单提交成功后再删除临时清单，避免失败时丢失安装目标信息。
		IFileManager::Get().Delete(*GetTempClientManifestPath());
		UE_LOG(LogVersionInstallationProcessor, Display, TEXT("Client manifest committed: %s"), *ClientManifestPath);
		return true;
	}

	// 删除指定文件，主要用于清理废弃资源。
	void DeleteFile(const TCHAR* NewFilename)
	{
		if (IFileManager::Get().Delete(NewFilename))
		{
			UE_LOG(LogVersionInstallationProcessor, Display, TEXT("Delete = [%s] Success."), NewFilename);
		}
		else
		{
			UE_LOG(LogVersionInstallationProcessor, Error, TEXT("Delete = [%s] Fail."), NewFilename);
		}
	}

	// 删除 Subsystem 通过命令行传入的废弃文件。
	// 这些文件已经是本地绝对路径，安装器不再根据 Manifest 重新计算。
	void DeleteDiscardFiles()
	{
		for (const FString& TmpPath : DiscardPaths)
		{
			DeleteFile(*TmpPath);
		}
	}

	// 安装完成后重新启动原项目。
	void StartProject()
	{
		UE_LOG(LogVersionInstallationProcessor, Display, TEXT("Start project."));
		const FProcHandle ProjectHandle = FPlatformProcess::CreateProc(*ProjectStartupPath, nullptr, false, false, false, nullptr, 0, nullptr, nullptr);
		if (ProjectHandle.IsValid())
		{
			UE_LOG(LogVersionInstallationProcessor, Display, TEXT("Project started successfully."));
		}
		else
		{
			UE_LOG(LogVersionInstallationProcessor, Error, TEXT("Failed to start project."));
		}

		// 安装器工作结束，主动退出。
		FPlatformMisc::RequestExit(true);
	}

	// 独立安装器主流程。
	// 读取临时清单 -> 等待游戏退出 -> 复制并校验文件 -> 提交正式清单 -> 删除废弃文件 -> 重启项目。
	void Run()
	{
		if (!LoadTempClientManifest())
		{
			return;
		}

		UE_LOG(LogVersionInstallationProcessor, Display, TEXT("Check if the game is still running."));
		while (FPlatformProcess::IsApplicationRunning(ProjectProcessID))
		{
			FPlatformProcess::Sleep(1.f);
		}

		// 废弃文件由 DeleteDiscardFiles 使用命令行传入的绝对路径处理。
		// 这里传空数组，避免安装管理器重复删除。
		TArray<FRemotePatchFile> EmptyDiscardFiles;
		const bool bInstallOK = FVersionInstallationManage().HandleHotInstallation<FInstallationProgress>(
			ResourcesToCopied,
			ResourcesToCopiedCustom,
			ProjectToContentPaks,
			ProjectRootPath,
			RelativePatchs,
			InstalledPatchFiles,
			EmptyDiscardFiles,
			PakNumber,
			CustomPakNumber,
			ProgressInstallationPercentage);

		if (!bInstallOK || IsEngineExitRequested())
		{
			// 安装失败时不提交正式 ClientManifest，避免本地状态错误地显示为新版本。
			UE_LOG(LogVersionInstallationProcessor, Error, TEXT("Installation failed."));
			return;
		}

		if (!CommitTempClientManifest())
		{
			return;
		}

		DeleteDiscardFiles();
		StartProject();
	}

	// 从命令行读取指定 key 的参数。
	template<class T>
	T GetParseValue(const FString& InKey)
	{
		T Value;
		if (!FParse::Value(FCommandLine::Get(), *InKey, Value))
		{
			UE_LOG(LogVersionInstallationProcessor, Error, TEXT("%s was not found value"), *InKey);
		}

		return Value;
	}

	// 初始化安装器命令行参数。
	// 这些参数由 UVersionUpdateSubsystem::LaunchExternalInstaller 拼接传入。
	void InitCommandInstallationProcessor()
	{
		ResourcesToCopiedCustom = GetParseValue<FString>(TEXT("-ResourcesToCopiedCustom="));
		ResourcesToCopied = GetParseValue<FString>(TEXT("-ResourcesToCopied="));
		ProjectToContentPaks = GetParseValue<FString>(TEXT("-ProjectToContentPaks="));
		ProjectStartupPath = GetParseValue<FString>(TEXT("-ProjectStartupPath="));
		ProjectRootPath = GetParseValue<FString>(TEXT("-ProjectRootPath="));
		LinkURL = GetParseValue<FString>(TEXT("-LinkURL="));
		ProjectProcessID = GetParseValue<uint32>(TEXT("-ProjectProcessID="));

		const FString Discards = GetParseValue<FString>(TEXT("-Discards="));
		const FString RelativePatchString = GetParseValue<FString>(TEXT("-RelativePatchs="));
		if (!RelativePatchString.IsEmpty())
		{
			// 自定义文件最终安装路径使用 + 分隔。
			RelativePatchString.ParseIntoArray(RelativePatchs, TEXT("+"));
		}

		// 路径参数统一标准化，后续拼接和比较时规则一致。
		FPaths::NormalizeFilename(ResourcesToCopiedCustom);
		FPaths::NormalizeFilename(ResourcesToCopied);
		FPaths::NormalizeFilename(ProjectToContentPaks);
		FPaths::NormalizeFilename(ProjectStartupPath);
		FPaths::NormalizeFilename(ProjectRootPath);

		if (!Discards.Equals(TEXT("NONE"), ESearchCase::IgnoreCase))
		{
			// 废弃文件路径使用 | 分隔；NONE 表示本轮没有需要删除的文件。
			Discards.ParseIntoArray(DiscardPaths, TEXT("|"));
		}
	}

	// 创建安装器 UI Tab。
	TSharedRef<SDockTab> SpawnDockTab(const FSpawnTabArgs& InArgs)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::PanelTab)
			[
				SNew(SMainScreen)
			];
	}

	// 启动安装进度 UI。
	void SpawnInstallationProcessorUI()
	{
		FGlobalTabmanager::Get()->RegisterTabSpawner(TEXT("InstallationProgressUI"), FOnSpawnTab::CreateStatic(&SpawnDockTab));

		TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("InstallationProgressUI_Layout")
		->AddArea
		(
			FTabManager::NewArea(1920, 1080)
			->Split
			(
				FTabManager::NewStack()
				->AddTab("InstallationProgressUI", ETabState::OpenedTab)
			)
		);

		FGlobalTabmanager::Get()->RestoreFrom(Layout, TSharedPtr<SWindow>());
	}
}
