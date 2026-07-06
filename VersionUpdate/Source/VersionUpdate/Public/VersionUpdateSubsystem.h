#pragma once

#include "CoreMinimal.h"
#include "Settings/VersionManifestClientObject.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UnHTTPType.h"
#include "Version/PatchManifest.h"
#include "VersionUpdateType.h"
#include "VersionUpdateSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVersionUpdateInitializedDelegate, const FString&, CurrentVersion);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnlineVersionCheckRequestedDelegate, const FString&, ManifestURL);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVersionCheckCompletedDelegate, EServerVersionResponseType, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FVersionDownloadProgressDelegate, float, Percentage, const FString&, FileName, int64, TotalBytes, int64, BytesReceived);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPatchFileDownloadCompletedDelegate, const FString&, FileName, bool, bSucceeded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVersionDownloadCompletedDelegate, bool, bSucceeded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVersionInstallCompletedDelegate, bool, bSucceeded);

struct FVersionUpdateDownloadChunk
{
	// 该分片所属的文件名，用于日志和单文件完成事件。
	FString FileName;

	// 该分片请求的完整下载 URL。
	FString URL;

	// HTTP Range 请求头，例如 bytes=0-8388607；为空时表示整文件下载。
	FString RangeHeader;

	// 当前分片临时落盘路径，成功后会被合并进 .part 文件。
	FString ChunkPath;

	// UnHTTP 返回的请求句柄，用于 fallback 时尽量取消仍在运行的 Range 请求。
	FName RequestHandle;

	// 当前分片在目标文件中的起始字节位置。
	int64 Start = 0;

	// 当前分片在目标文件中的结束字节位置，包含该字节。
	int64 End = 0;

	// UnHTTP 进度回调中当前分片已收到的字节数。
	uint64 BytesReceived = 0;

	// 该分片是否已经结束；成功和失败都会标记为完成。
	bool bCompleted = false;

	// 该分片是否下载并保存成功。
	bool bSucceeded = false;

	// 是否是无 Range 的整文件请求；用于 Size<=0 或服务端不支持 Range 的 fallback。
	bool bFullFileRequest = false;

	int64 GetExpectedSize() const
	{
		return End >= Start ? End - Start + 1 : 0;
	}
};

struct FVersionUpdateDownloadFileState
{
	// Manifest 中描述的远端文件信息，包含名称、大小、Hash、安装目录等。
	FRemotePatchFile File;

	// 当前文件的完整下载 URL。
	FString URL;

	// 当前文件名，作为 ActiveDownloadFiles 的 key。
	FString FileName;

	// 下载成功后的最终临时文件路径，安装阶段会从这里取文件。
	FString SavePath;

	// 可续传的主临时文件路径；已确认顺序正确的内容会追加到这里。
	FString PartPath;

	// 本轮开始前 .part 文件已有的字节数，作为断点续传起点。
	int64 ExistingBytes = 0;

	// 已完成的分片数量，仅用于调试观察，当前流程主要依赖 Chunks 状态。
	int32 CompletedChunks = 0;

	// 当前文件是否已有任意分片、合并、校验或移动失败。
	bool bFailed = false;

	// 是否已经广播过该文件的 OnPatchFileDownloadCompleted，防止重复广播。
	bool bReported = false;

	// 是否已发现服务端不支持 Range，并切换到无 Range 整文件下载。
	bool bFallbackToFullDownload = false;

	// 当前文件的所有分片任务，包括 Range 分片和可能追加的整文件 fallback 任务。
	TArray<FVersionUpdateDownloadChunk> Chunks;
};

/**
 * 事件驱动版本更新子系统。
 *
 * Subsystem 只负责流程调度和事件广播：
 * 1. InitializeVersionUpdate 设置本次运行的权威版本号，并广播 OnVersionUpdateInitialized。
 * 2. RequestOnlineVersionCheck 广播 OnOnlineVersionCheckRequested，业务层可自行下载 Manifest。
 * 3. SubmitServerManifest / SubmitServerManifestJson 注入 Manifest 并触发版本差异判断。
 * 4. DownloadPendingFiles 下载检测阶段生成的文件队列，并在完成后广播 OnVersionDownloadCompleted。
 * 5. InstallDownloadedFiles 安装已下载文件，并广播 OnVersionInstallCompleted。
 */
UCLASS(BlueprintType,Blueprintable)
class VERSIONUPDATE_API UVersionUpdateSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	// 流程事件1 初始化完成事件：业务层可在这里开始绑定 UI 或触发服务器版本检测
	UPROPERTY(BlueprintAssignable, Category = "Version|Events")
	FVersionUpdateInitializedDelegate OnVersionUpdateInitialized;

	// 流程事件2.1 请求服务器下载 Manifest
	UPROPERTY(BlueprintAssignable, Category = "Version|Events")
	FOnlineVersionCheckRequestedDelegate OnServerManifestRequested;

	// 流程事件2.2 版本检测完成事件：返回网络错误、已最新、热更新或大版本更新。
	UPROPERTY(BlueprintAssignable, Category = "Version|Events")
	FVersionCheckCompletedDelegate OnVersionCheckCompleted;

	//流程事件3辅助 下载进度事件：单个文件下载过程中持续广播。
	UPROPERTY(BlueprintAssignable, Category = "Version|Events")
	FVersionDownloadProgressDelegate OnVersionDownloadProgress;

	//流程事件3辅助 单个补丁文件下载完成事件：成功或失败都会广播文件名。
	UPROPERTY(BlueprintAssignable, Category = "Version|Events")
	FPatchFileDownloadCompletedDelegate OnPatchFileDownloadCompleted;

	// 流程事件3 所有待下载文件完成事件：业务层通常在这里决定是否调用 InstallDownloadedFiles。
	UPROPERTY(BlueprintAssignable, Category = "Version|Events")
	FVersionDownloadCompletedDelegate OnVersionDownloadCompleted;

	//流程事件4 安装完成事件：安装成功或失败后广播。
	UPROPERTY(BlueprintAssignable, Category = "Version|Events")
	FVersionInstallCompletedDelegate OnVersionInstallCompleted;

	
	//1. 手动初始化版本更新流程
	// 版本号可以由外部提供
	UFUNCTION(BlueprintCallable, Category = "Version|Flow")
	void InitializeVersionUpdate(const FString& InCurrentVersion);

	// 请求在线版本检测。默认只广播 OnServerManifestRequested，由业务层自行下载 Manifest。
	// bUseInternalHttp 为 true 时，子系统使用内置 HTTP 下载配置中的 ServerJsonURL。
	UFUNCTION(BlueprintCallable, Category = "Version|Flow")
	bool RequestServerVersionCheck(bool bUseInternalHttp = false, bool bSynchronous = true);

	// 外部已经拿到 FPatchList 时调用。bEvaluateImmediately 为 true 时会立即进行版本差异判断。
	UFUNCTION(BlueprintCallable, Category = "Version|Flow")
	EServerVersionResponseType SubmitServerManifest(const FPatchList& InServerManifest, bool bEvaluateImmediately = true);

	// 外部已经拿到 Manifest JSON 字符串时调用。通常用于接入其他 HTTP 插件或平台 SDK。
	UFUNCTION(BlueprintCallable, Category = "Version|Flow")
	EServerVersionResponseType SubmitServerManifestJson(const FString& InServerManifestJson, bool bEvaluateImmediately = true);

	// 使用当前 ServerManifest 和 CurrentVersion 进行差异判断，并广播 OnVersionCheckCompleted。
	UFUNCTION(BlueprintCallable, Category = "Version|Flow")
	EServerVersionResponseType EvaluateServerManifest();

	// 下载版本检测阶段生成的 PendingDownloadFiles。
	UFUNCTION(BlueprintCallable, Category = "Version|Flow")
	void DownloadPendingFiles();

	// 安装已下载文件。业务层一般在 OnVersionDownloadCompleted 成功后调用。
	UFUNCTION(BlueprintCallable, Category = "Version|Flow")
	bool InstallDownloadedFiles();

	// 修改当前权威版本号。一般只应由 InitializeVersionUpdate 调用。
	UFUNCTION(BlueprintCallable, Category = "Version|State")
	void SetCurrentVersion(const FString& InCurrentVersion);

	// 获取当前权威版本号。
	UFUNCTION(BlueprintPure, Category = "Version|State")
	FString GetCurrentVersion() const;

	// 是否已经完成 InitializeVersionUpdate。
	UFUNCTION(BlueprintPure, Category = "Version|State")
	bool IsVersionUpdateInitialized() const;

	// 获取安装阶段进度，下载进度请使用 OnVersionDownloadProgress。
	UFUNCTION(BlueprintPure, Category = "Version|State")
	float GetInstallationProgress() const;

	// 重置安装阶段进度。通常由内部流程调用。
	UFUNCTION(BlueprintCallable, Category = "Version|State")
	void ResetInstallationProgress();

	// 保存本地 ClientManifest。通常由安装完成后内部调用。
	UFUNCTION(BlueprintCallable, Category = "Version|Client Manifest")
	bool SaveClientManifest();

	// 读取本地 ClientManifest。通常由初始化流程调用。
	UFUNCTION(BlueprintCallable, Category = "Version|Client Manifest")
	bool LoadClientManifest();

	// 获取本地 ClientManifest 路径，主要用于调试和工具。
	UFUNCTION(BlueprintCallable, Category = "Version|Client Manifest")
	FString GetClientManifestPath() const;

private:
	// 使用插件内置 HTTP 下载服务器 Manifest。外部下载方式不走这里。
	bool RequestServerManifestByInternalHttp(bool bSynchronous);

	// 普通热更新文件差异比较，生成下载和废弃队列。
	EServerVersionResponseType ComparePatchFiles();

	// 收集服务器 Patches 与本地文件的差异。普通热更和大版本补丁补齐共用这套判断。
	void CollectPatchFileDifferences();

	// 大版本更新准备：先下载 MajorVersionFiles，再补齐基于该大版本的 Patches。
	EServerVersionResponseType PrepareMajorVersionUpdate();

	// 从服务器 Manifest 中取语义版本号最大的版本，安装写入 ClientManifest 时也使用同一结果。
	FString GetLatestServerVersion() const;

	// 下载临时目录。
	FString GetDownloadPathToPak() const;
	FString GetDownloadPathToCustom() const;

	// 内置批量下载回调。
	void QueueResumableDownload(const FRemotePatchFile& File);
	void StartQueuedDownloadChunks();
	bool StartDownloadChunk(const FString& ChunkKey);
	bool StartFullFileDownloadFallback(const FString& FileName);
	void OnDownloadChunkComplete(const FString& ChunkKey, const FUnHttpRequest& Request, const FUnHttpResponse& Response, bool bSucceeded);
	void OnDownloadChunkProgress(const FString& ChunkKey, const FUnHttpRequest& Request, uint64 BytesSent, uint64 BytesReceived);
	void FinishDownloadFileIfReady(const FString& FileName);
	void FinishAllDownloadsIfReady();

	// 安装实现：无感安装或外部安装器。
	bool ExecuteSeamlessInstall();
	bool LaunchExternalInstaller();
	bool UnmountMountedPaks();
	bool MountInstalledPatchPaks(const FClientVersionFilesList& InstalledManifest);
	void UpdateClientManifestAfterInstall();

	// 初始化本地 ClientManifest，不存在时创建。
	void LoadOrCreateClientManifest();

protected:
	// 本次运行的权威版本号，不信任本地 JSON 的版本字段。
	UPROPERTY()
	FString CurrentVersion;
	
	
private:
	// 运行期状态。
	bool bVersionUpdateInitialized = false;
	bool bSeamlessHotUpdate = false;
	bool bLastDownloadSucceeded = true;

	// Manifest 和阶段性队列。
	FPatchList ServerManifest;
	FClientVersionFilesList ClientManifest;

	TArray<FRemotePatchFile> PendingDownloadFiles;
	TArray<FRemotePatchFile> PendingDiscardFiles;
	TArray<FString> RelativePatchs;
	TArray<FString> MountedPakCache;

	// 本轮正在处理的文件下载状态，key 是文件名；要求同一轮下载内文件名唯一。
	TMap<FString, FVersionUpdateDownloadFileState> ActiveDownloadFiles;

	// 分片 key 到文件名的反查表；分片回调只拿 ChunkKey，再通过这里找回文件状态。
	TMap<FString, FString> ActiveDownloadChunkToFile;

	// 分片 key 到 Chunks 数组下标的反查表。
	TMap<FString, int32> ActiveDownloadChunkIndex;

	// 等待启动的分片 key 队列。
	TArray<FString> PendingDownloadChunkKeys;

	// 已发起但尚未收到完成回调的分片 key 集合。
	TSet<FString> RunningDownloadChunkKeys;

	// 已经结束生命周期的分片任务数；成功、失败、启动失败都会计入。
	int32 CompletedDownloadChunkCount = 0;

	// 当前仍需要纳入统计的分片任务总数；fallback 会移除旧任务并添加整文件任务。
	int32 TotalDownloadChunkCount = 0;
};
