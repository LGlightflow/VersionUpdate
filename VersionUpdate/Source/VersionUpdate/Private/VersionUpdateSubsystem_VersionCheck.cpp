#include "VersionUpdateSubsystem.h"

#include "Settings/VersionManifestClientObject.h"
#include "UnHTTPManage.h"
#include "VersionUpdateLogChannels.h"

namespace
{
	// HTTP 返回内容可能带 UTF-8 BOM，解析 JSON 前先移除，避免 PatchManifest::Read 失败。
	FString GetHttpResponseString(const TArray<uint8>& RawContent)
	{
		TArray<uint8> Raw = RawContent;
		if (Raw.Num() >= 3 && Raw[0] == 0xEF && Raw[1] == 0xBB && Raw[2] == 0xBF)
		{
			Raw.RemoveAt(0, 3);
		}

		FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Raw.GetData()), Raw.Num());
		FString JsonStr(Conv.Length(), Conv.Get());
		JsonStr.TrimStartInline();
		return JsonStr;
	}

	// 版本检测阶段需要按 Manifest 描述定位本地文件，用于 Size / Hash 对比。
	FString GetVersionCheckProjectFilePath(const FRemotePatchFile& File)
	{
		return PatchManifest::GetInstallAbsolutePath(File, FPaths::ProjectDir());
	}

	void RemoveRemoteFileByKey(TArray<FRemotePatchFile>& Files, const FString& FileKey)
	{
		Files.RemoveAll([&FileKey](const FRemotePatchFile& ExistingFile)
		{
			return PatchManifest::GetInstallRelativePath(ExistingFile).Equals(FileKey, ESearchCase::IgnoreCase);
		});
	}

	// 同一路径后加入的记录覆盖先加入的记录；大版本基础文件先入队，补丁文件后入队。
	void AddOrReplaceRemoteFile(TArray<FRemotePatchFile>& Files, const FRemotePatchFile& File)
	{
		if (File.Name.IsEmpty())
		{
			return;
		}

		const FString FileKey = PatchManifest::GetInstallRelativePath(File);
		RemoveRemoteFileByKey(Files, FileKey);
		Files.Add(File);
	}
}

bool UVersionUpdateSubsystem::RequestServerVersionCheck(bool bUseInternalHttp, bool bSynchronous)
{
	const FString ManifestURL = GetDefault<UVersionManifestClientObject>()->ServerJsonURL;

	// 广播请求事件，让业务层可以从外部获取manifest json
	OnServerManifestRequested.Broadcast(ManifestURL);

	if (!bUseInternalHttp)
	{
		return true;
	}
	else
	{
		return RequestServerManifestByInternalHttp(bSynchronous);
	}
}

bool UVersionUpdateSubsystem::RequestServerManifestByInternalHttp(bool bSynchronous)
{
	const FString ManifestURL = GetDefault<UVersionManifestClientObject>()->ServerJsonURL;
	UE_LOG(LogVersionUpdate, Display, TEXT("[VersionUpdate] Request manifest: %s"), *ManifestURL);

	TSharedRef<bool, ESPMode::ThreadSafe> bRequestFinished = MakeShared<bool, ESPMode::ThreadSafe>(false);
	
	// 使用 UnHTTP 获取服务器 Manifest。外部下载方式仍然可以直接调用 SubmitServerManifestJson/SubmitServerManifest 注入结果。
	FUnHTTPResponseDelegate Delegate;
	Delegate.SingleCompleteDelegate.BindLambda([this, bRequestFinished](const FUnHttpRequest& Request, const FUnHttpResponse& Response, bool bConnected)
	{
		*bRequestFinished = true;

		if (!bConnected || Response.ResponseCode < 200 || Response.ResponseCode >= 300 || !Response.Content)
		{
			// 网络或响应对象无效，统一广播连接错误。
			OnVersionCheckCompleted.Broadcast(EServerVersionResponseType::CONNECTION_ERROR);
			return;
		}

		SubmitServerManifestJson(GetHttpResponseString(Response.Content->GetContent()), true);
	});

	if (!UN_HTTP.GetObjectToMemory(Delegate, ManifestURL, bSynchronous))
	{
		// HTTP 请求发起失败，也按连接错误处理。
		OnVersionCheckCompleted.Broadcast(EServerVersionResponseType::CONNECTION_ERROR);
		return false;
	}

	return bSynchronous ? *bRequestFinished : true;
}

EServerVersionResponseType UVersionUpdateSubsystem::SubmitServerManifestJson(const FString& InServerManifestJson, bool bEvaluateImmediately)
{
	// JSON 入口方便业务层接入任意下载方式；解析后转成结构体入口。
	FPatchList ParsedManifest;
	if (!PatchManifest::Read(InServerManifestJson, ParsedManifest))
	{
		UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Failed to parse server manifest."));
		OnVersionCheckCompleted.Broadcast(EServerVersionResponseType::CONNECTION_ERROR);
		return EServerVersionResponseType::CONNECTION_ERROR;
	}

	return SubmitServerManifest(ParsedManifest, bEvaluateImmediately);
}


EServerVersionResponseType UVersionUpdateSubsystem::SubmitServerManifest(const FPatchList& InServerManifest, bool bEvaluateImmediately)
{
	// 无论哪种方式下载Manifest，最终都会走到这里
	// bEvaluateImmediately=false 允许先缓存 Manifest，之后在合适时机手动调用 EvaluateServerManifest。
	ServerManifest = InServerManifest;
	return bEvaluateImmediately ? EvaluateServerManifest() : EServerVersionResponseType::VERSION_EQUAL;
}


EServerVersionResponseType UVersionUpdateSubsystem::EvaluateServerManifest()
{
	// 所有检测都必须先初始化
	if (!bVersionUpdateInitialized || CurrentVersion.IsEmpty())
	{
		UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Call InitializeVersionUpdate before version check."));
		OnVersionCheckCompleted.Broadcast(EServerVersionResponseType::INITIALIZATION_ERROR);
		return EServerVersionResponseType::INITIALIZATION_ERROR;
	}

	ResetInstallationProgress();
	RelativePatchs.Empty();
	ClientManifest.CurrentVersion = CurrentVersion;

	// 当前版本是否在server支持列表中。
	// SupportVersions 是server侧的兼容性白名单；只有在白名单内才允许走普通热更新。
	const EServerVersionResponseType Result = ServerManifest.SupportVersions.Contains(CurrentVersion)
		? ComparePatchFiles() // 热更
		: PrepareMajorVersionUpdate(); //大版本更新

	OnVersionCheckCompleted.Broadcast(Result);
	return Result;
}

EServerVersionResponseType UVersionUpdateSubsystem::ComparePatchFiles()
{
	// 普通热更新只比较服务器 Patches 中声明的文件。
	// 注意：版本号只用于分流，真正决定是否下载的是本地文件 Size / Hash。
	PendingDownloadFiles.Empty();
	PendingDiscardFiles.Empty();
	RelativePatchs.Empty();
	CollectPatchFileDifferences();

	// 有下载或废弃任务即视为需要热更新，否则认为客户端文件已是最新。
	return (PendingDownloadFiles.Num() > 0 || PendingDiscardFiles.Num() > 0)
		? EServerVersionResponseType::VERSION_HOTUPDATE
		: EServerVersionResponseType::VERSION_EQUAL;
}

void UVersionUpdateSubsystem::CollectPatchFileDifferences()
{
	for (const FPatchVersion& PatchVersion : ServerManifest.Patches)
	{
		// Patches 可以包含多个补丁版本；这里不按版本号排序或截断，而是以服务器下发的文件清单为准。
		for (const FRemotePatchFile& ServerFile : PatchVersion.Files)
		{
			if (ServerFile.Name.IsEmpty())
			{
				continue;
			}

			const FString LocalPath = GetVersionCheckProjectFilePath(ServerFile);
			const bool bLocalExists = FPaths::FileExists(LocalPath);
			if (ServerFile.bDiscard)
			{
				const FString FileKey = PatchManifest::GetInstallRelativePath(ServerFile);
				RemoveRemoteFileByKey(PendingDownloadFiles, FileKey);

				if (bLocalExists)
				{
					// 服务器标记为废弃且本地存在，安装阶段会处理删除。
					AddOrReplaceRemoteFile(PendingDiscardFiles, ServerFile);
				}
				continue;
			}

			if (!PatchManifest::IsLocalFileMatched(LocalPath, ServerFile))
			{
				// 文件缺失、大小不一致或 Hash 不一致，都需要重新下载。
				AddOrReplaceRemoteFile(PendingDownloadFiles, ServerFile);
			}
		}
	}
}

EServerVersionResponseType UVersionUpdateSubsystem::PrepareMajorVersionUpdate()
{
	// 大版本更新先下载基础大版本文件，再复用普通补丁差异逻辑，把基于该大版本的补丁也补齐。
	// 这里生成的下载队列通常交给外部安装器处理，避免在当前进程内替换代码/蓝图相关文件。
	
	bSeamlessHotUpdate = false;
	PendingDownloadFiles.Empty();
	PendingDiscardFiles.Empty();
	RelativePatchs.Empty();

	for (const FRemotePatchFile& File : ServerManifest.MajorVersionFiles)
	{
		if (!File.Name.IsEmpty())
		{
			// MajorVersionFiles 是跨大版本更新的基础文件，默认强制下载；后续补丁若同路径会覆盖这里的记录。
			AddOrReplaceRemoteFile(PendingDownloadFiles, File);
		}
	}

	CollectPatchFileDifferences();

	return (PendingDownloadFiles.Num() > 0 || PendingDiscardFiles.Num() > 0)
		? EServerVersionResponseType::MAJORVERSION_UPDATE
		: EServerVersionResponseType::CONNECTION_ERROR;
}

FString UVersionUpdateSubsystem::GetLatestServerVersion() const
{
	FString LatestVersion;
	auto TryUseVersion = [&LatestVersion](const FString& CandidateVersion)
	{
		if (CandidateVersion.IsEmpty())
		{
			return;
		}

		if (LatestVersion.IsEmpty() || PatchManifest::CompareVersion(CandidateVersion, LatestVersion) > 0)
		{
			LatestVersion = CandidateVersion;
		}
	};

	// Patches 通常是目标版本号的主要来源。
	for (const FPatchVersion& PatchVersion : ServerManifest.Patches)
	{
		TryUseVersion(PatchVersion.Version);
	}

	// 兼容当前 Manifest 约定：SupportVersions 里也可能包含服务端当前版本，取最大值作为最终安装版本。
	for (const FString& SupportVersion : ServerManifest.SupportVersions)
	{
		TryUseVersion(SupportVersion);
	}

	return LatestVersion.IsEmpty() ? CurrentVersion : LatestVersion;
}
