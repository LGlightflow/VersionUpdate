#include "VersionUpdateSubsystem.h"

#include "Misc/FileHelper.h"
#include "UnHTTPManage.h"
#include "VersionUpdateLogChannels.h"

// DownloadPendingFiles
//   └─ QueueResumableDownload     // 为每个文件生成分片任务
// 	  └─ PendingDownloadChunkKeys //加入等待队列
//
// StartQueuedDownloadChunks
//   └─ StartDownloadChunk         // 按最大并发数发起 HTTP 请求
//
// OnDownloadChunkProgress         // 分片进度回调，换算成文件总进度
//
// OnDownloadChunkComplete         // 分片完成回调
//   ├─ 保存 chunk 文件
//   ├─ 如果 Range 被服务端忽略，切换到整文件下载
//   ├─ FinishDownloadFileIfReady  // 文件所有分片完成后合并
//   ├─ StartQueuedDownloadChunks  // 补充新的并发任务
//   └─ FinishAllDownloadsIfReady  // 全部完成后广播总完成

namespace
{
	// 单个 Range 分片大小。分片越大，请求数越少；分片越小，断点恢复粒度越细
	constexpr int64 ChunkSize = 8 * 1024 * 1024;

	// 同时运行的分片请求数
	constexpr int32 MaxConcurrentChunkRequests = 4;

	// 下载保存前确保目录存在
	void EnsureDirectory(const FString& InPath)
	{
		if (!IFileManager::Get().DirectoryExists(*InPath))
		{
			IFileManager::Get().MakeDirectory(*InPath, true);
		}
	}

	// 服务端 Manifest 只存根 URL 和文件名；下载前统一拼成完整 URL
	FString CombineURL(const FString& BaseURL, const FString& FileName)
	{
		FString Result = BaseURL;
		Result.RemoveFromEnd(TEXT("/"));
		return Result / FileName;
	}

	// HTTP Range 标准格式：bytes=起始字节-结束字节，服务端支持时应返回 206
	FString MakeRangeHeader(int64 Start, int64 End)
	{
		return FString::Printf(TEXT("bytes=%lld-%lld"), Start, End);
	}
	
	// 找分片辅助 用 URL + Range 组成稳定 key，回调时再反查文件名和分片索引
	FString MakeChunkKey(const FString& URL, const FString& RangeHeader)
	{
		return URL + TEXT("|") + RangeHeader;
	}

	bool SaveResponseContentToFile(const FUnHttpResponse& Response, const FString& SavePath)
	{
		if (!Response.Content)
		{
			return false;
		}

		EnsureDirectory(FPaths::GetPath(SavePath));
		return FFileHelper::SaveArrayToFile(Response.Content->GetContent(), *SavePath);
	}

	bool AppendFileToArchive(FArchive& TargetArchive, const FString& SourcePath)
	{
		// 合并阶段按分片顺序把 chunk 追加到 .part 文件后面
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *SourcePath))
		{
			return false;
		}

		if (Data.Num() > 0)
		{
			TargetArchive.Serialize(Data.GetData(), Data.Num());
		}

		return !TargetArchive.IsError();
	}

	bool IsWholeFileRange(const FVersionUpdateDownloadChunk& Chunk, const FRemotePatchFile& File)
	{
		return File.Size > 0 && Chunk.Start == 0 && Chunk.End == File.Size - 1;
	}
}

void UVersionUpdateSubsystem::DownloadPendingFiles()
{
	// 每次下载开始时重置本轮状态；任意文件失败都会让最终完成事件返回 false。
	bLastDownloadSucceeded = true;

	// ActiveDownloadFiles 保存“文件级”状态；ChunkToFile/ChunkIndex 保存“分片级”反查表
	//任意分片回调回来时，都能根据 Request.URL + Request.Range 找回对应的文件和分片
	ActiveDownloadFiles.Empty();
	ActiveDownloadChunkToFile.Empty();
	ActiveDownloadChunkIndex.Empty();
	PendingDownloadChunkKeys.Empty();
	RunningDownloadChunkKeys.Empty();

	// Completed/Total 是本轮下载的全局任务计数，用于判断所有分片请求是否都结束。
	CompletedDownloadChunkCount = 0;
	TotalDownloadChunkCount = 0;

	for (const FRemotePatchFile& File : PendingDownloadFiles)
	{
		if (!File.Name.IsEmpty())
		{
			// 每个待下载文件都会被拆成 0-N 个分片任务；已完整存在且校验通过的文件不会进入队列。
			QueueResumableDownload(File);
		}
	}

	if (TotalDownloadChunkCount == 0)
	{
		// 可能只有废弃文件需要处理，或所有目标文件已经在本地且校验通过。
		OnVersionDownloadCompleted.Broadcast(bLastDownloadSucceeded);
		return;
	}

	StartQueuedDownloadChunks();
}
// 为每个文件生成分片任务
void UVersionUpdateSubsystem::QueueResumableDownload(const FRemotePatchFile& File)
{
	// 文件级状态只记录最终结果和断点文件；真正的网络请求由 Chunks 数组描述。
	// FileName 是本轮文件状态 key；URL 是最终提交给 UnHTTP 的远端地址。
	const FString FileName = File.Name;
	const FString URL = CombineURL(ServerManifest.Url, FileName);

	// SavePath 是下载完成后的临时成品路径，安装阶段会读取这里。
	FString SavePath;
	if (File.Tag == EPatchFileTag::Pak || File.Tag == EPatchFileTag::PakSig)
	{
		// Pak 类文件走 Pak 临时目录。
		SavePath = GetDownloadPathToPak() / FileName;
	}
	else
	{
		// Custom/DLL/DLC 等非 Pak 文件走 Custom 临时目录，并记录最终安装路径。
		SavePath = GetDownloadPathToCustom() / FileName;

		const FString FinalPath = PatchManifest::GetInstallAbsolutePath(File, FPaths::ProjectDir());
		EnsureDirectory(FPaths::GetPath(FinalPath));
		RelativePatchs.AddUnique(FinalPath);
	}

	EnsureDirectory(FPaths::GetPath(SavePath));

	if (PatchManifest::IsLocalFileMatched(SavePath, File))
	{
		// 上次已经完整下载并通过校验，本轮直接复用。
		// 广播单文件完成，让业务层的计数/UI进行处理
		OnPatchFileDownloadCompleted.Broadcast(FileName, true);
		return;
	}

	FVersionUpdateDownloadFileState FileState;
	FileState.File = File;
	FileState.URL = URL;
	FileState.FileName = FileName;
	FileState.SavePath = SavePath;

	// .part 是可续传的主文件：已经确认顺序正确的内容会合并进这里。
	// 本轮新下载的每个分片先保存为独立 .chunk，全部成功后再按顺序追加进 .part。
	FileState.PartPath = SavePath + TEXT(".part");

	if (FPaths::FileExists(FileState.PartPath))
	{
		// 断点续传的依据是 .part 当前大小：前 N 字节已经在上一次下载中合并完成。
		FileState.ExistingBytes = IFileManager::Get().FileSize(*FileState.PartPath);
		if (File.Size > 0 && FileState.ExistingBytes > File.Size)
		{
			// 本地断点文件比 Manifest 声明还大，视为损坏并重新下载。
			IFileManager::Get().Delete(*FileState.PartPath);
			FileState.ExistingBytes = 0;
		}
	}

	if (File.Size > 0 && FileState.ExistingBytes == File.Size && PatchManifest::IsLocalFileMatched(FileState.PartPath, File))
	{
		IFileManager::Get().Move(*SavePath, *FileState.PartPath, true, true);
		OnPatchFileDownloadCompleted.Broadcast(FileName, true);
		return;
	}
	else if (File.Size > 0 && FileState.ExistingBytes == File.Size)
	{
		// 断点文件大小正确但校验失败，说明本地缓存损坏，不能继续复用。
		IFileManager::Get().Delete(*FileState.PartPath);
		FileState.ExistingBytes = 0;
	}

	const int64 DownloadStart = FMath::Max<int64>(0, FileState.ExistingBytes);
	if (File.Size <= 0)
	{
		// 没有 Size 时无法计算 Range，退化为普通下载
		IFileManager::Get().Delete(*FileState.PartPath);
		FileState.ExistingBytes = 0;

		FVersionUpdateDownloadChunk Chunk;
		Chunk.FileName = FileName;
		Chunk.URL = URL;
		// full.chunk 表示没有 Range 的整文件临时响应体。
		Chunk.ChunkPath = FileState.PartPath + TEXT(".full.chunk");
		Chunk.bFullFileRequest = true;
		FileState.Chunks.Add(Chunk);
	}
	else
	{
		// 从断点位置开始切分剩余区间。例如已下载 10MB，文件 30MB，则只生成 10MB-30MB 的 Range 请求。
		for (int64 Start = DownloadStart; Start < File.Size; Start += ChunkSize)
		{
			const int64 End = FMath::Min(Start + ChunkSize - 1, File.Size - 1);

			FVersionUpdateDownloadChunk Chunk;
			Chunk.FileName = FileName;
			Chunk.URL = URL;
			// RangeHeader 决定服务端返回目标文件的哪一段。
			Chunk.RangeHeader = MakeRangeHeader(Start, End);
			Chunk.Start = Start;
			Chunk.End = End;
			// 每个 Range 分片先单独保存，避免并发写同一个文件。
			Chunk.ChunkPath = FString::Printf(TEXT("%s.%lld-%lld.chunk"), *FileState.PartPath, Start, End);
			FileState.Chunks.Add(Chunk);
		}
	}

	ActiveDownloadFiles.Add(FileName, FileState);

	FVersionUpdateDownloadFileState& StoredState = ActiveDownloadFiles[FileName];
	for (int32 Index = 0; Index < StoredState.Chunks.Num(); ++Index)
	{
		const FVersionUpdateDownloadChunk& Chunk = StoredState.Chunks[Index];
		// ChunkKey 是回调和队列系统使用的唯一任务标识。
		const FString ChunkKey = MakeChunkKey(Chunk.URL, Chunk.RangeHeader);

		// 分片请求是异步并发完成的，不能依赖完成顺序；必须用 key 找回原始索引。
		ActiveDownloadChunkToFile.Add(ChunkKey, FileName);
		ActiveDownloadChunkIndex.Add(ChunkKey, Index);
		PendingDownloadChunkKeys.Add(ChunkKey);
		++TotalDownloadChunkCount;
	}
}

void UVersionUpdateSubsystem::StartQueuedDownloadChunks()
{
	// 并发调度 只要还有空位，就从等待队列取一个分片发起 UnHTTP 请求
	while (PendingDownloadChunkKeys.Num() > 0 && RunningDownloadChunkKeys.Num() < MaxConcurrentChunkRequests)
	{
		// ChunkKey 从等待队列头部取出，成功启动后会转入 RunningDownloadChunkKeys。
		const FString ChunkKey = PendingDownloadChunkKeys[0];
		PendingDownloadChunkKeys.RemoveAt(0);

		if (!StartDownloadChunk(ChunkKey))
		{
			++CompletedDownloadChunkCount;
			FinishAllDownloadsIfReady();
		}
	}
}

bool UVersionUpdateSubsystem::StartDownloadChunk(const FString& ChunkKey)
{
	// ChunkKey 来自等待队列，理论上一定能反查到文件名和分片索引；失败说明内部状态被破坏。
	// FileName/ChunkIndex 把异步 HTTP 回调重新定位到具体文件和具体分片。
	const FString* FileName = ActiveDownloadChunkToFile.Find(ChunkKey);
	const int32* ChunkIndex = ActiveDownloadChunkIndex.Find(ChunkKey);
	if (!FileName || !ChunkIndex)
	{
		bLastDownloadSucceeded = false;
		return false;
	}

	FVersionUpdateDownloadFileState* FileState = ActiveDownloadFiles.Find(*FileName);
	if (!FileState || !FileState->Chunks.IsValidIndex(*ChunkIndex))
	{
		bLastDownloadSucceeded = false;
		return false;
	}

	FVersionUpdateDownloadChunk& Chunk = FileState->Chunks[*ChunkIndex];

	// Headers 只在 Range 分片时包含 Range；整文件请求保持为空。
	TMap<FString, FString> Headers;
	if (!Chunk.RangeHeader.IsEmpty())
	{
		// Range 是断点续传和分片下载的关键：服务端应返回 206 Partial Content。
		Headers.Add(TEXT("Range"), Chunk.RangeHeader);
	}

	FUnHTTPResponseDelegate Delegate;
	
	// 不直接保存文件，而是统一回到 OnDownloadChunkComplete 做状态更新、落盘和合并判断。
	Delegate.SingleCompleteDelegate.BindLambda([this, ChunkKey](const FUnHttpRequest& Request, const FUnHttpResponse& Response, bool bSucceeded)
	{
		OnDownloadChunkComplete(ChunkKey, Request, Response, bSucceeded);
	});

	// 进度回调是单分片进度；OnDownloadChunkProgress 会把它换算成整个文件进度。
	Delegate.SingleRequestProgressDelegate.BindLambda([this, ChunkKey](const FUnHttpRequest& Request, uint64 BytesSent, uint64 BytesReceived)
	{
		OnDownloadChunkProgress(ChunkKey, Request, BytesSent, BytesReceived);
	});

	RunningDownloadChunkKeys.Add(ChunkKey);

	// 使用 UnHTTP 的通用请求接口发送 GET，这个接口允许传自定义 Header。
	// 这里 Content 为空字符串，仅用于满足接口签名；真正的语义由 Verb=GET 和 Range Header 决定。
	const bool bStarted = UN_HTTP.RequestByConentString(EUnHTTPVerbType::UN_GET, Chunk.URL, Headers, TEXT(""), Delegate);
	if (!bStarted)
	{
		// 请求发起失败不会进入完成回调，因此这里手动标记分片完成并尝试收尾。
		RunningDownloadChunkKeys.Remove(ChunkKey);
		FileState->bFailed = true;
		Chunk.bCompleted = true;
		Chunk.bSucceeded = false;
		FinishDownloadFileIfReady(*FileName);
	}
	else
	{
		// 记录 UnHTTP handle，fallback 时可以尽量取消同文件还在飞行中的 Range 分片。
		Chunk.RequestHandle = UN_HTTP.GetHandleByLastExecutionRequest();
	}

	return bStarted;
}

bool UVersionUpdateSubsystem::StartFullFileDownloadFallback(const FString& FileName)
{
	// FileState 是当前文件的总状态，fallback 会修改它的任务集合和断点文件。
	FVersionUpdateDownloadFileState* FileState = ActiveDownloadFiles.Find(FileName);
	if (!FileState || FileState->bReported)
	{
		return false;
	}

	// 同一个文件只允许切换一次 fallback；后续旧 Range 回调都会被忽略。
	if (FileState->bFallbackToFullDownload)
	{
		return true;
	}

	FileState->bFallbackToFullDownload = true;
	FileState->bFailed = false;
	FileState->ExistingBytes = 0;

	// 删除断点主文件和已落盘 Range chunk。服务端不支持 Range 时，断点前缀无法保证可继续复用。
	IFileManager::Get().Delete(*FileState->PartPath);
	for (const FVersionUpdateDownloadChunk& Chunk : FileState->Chunks)
	{
		if (!Chunk.bFullFileRequest)
		{
			IFileManager::Get().Delete(*Chunk.ChunkPath);
		}
	}

	// 从等待队列里移除同文件尚未发起的 Range 分片，并同步减少总分片数。
	for (int32 Index = PendingDownloadChunkKeys.Num() - 1; Index >= 0; --Index)
	{
		// PendingKey 是还未启动的旧 Range 任务，fallback 后不再需要它。
		const FString& PendingKey = PendingDownloadChunkKeys[Index];
		const FString* PendingFileName = ActiveDownloadChunkToFile.Find(PendingKey);
		if (PendingFileName && *PendingFileName == FileName)
		{
			PendingDownloadChunkKeys.RemoveAt(Index);
			--TotalDownloadChunkCount;
		}
	}

	// 已经发出的 Range 分片尽量取消；即使底层之后仍回调，也会因为 fallback 状态被忽略。
	for (int32 ChunkIndex = 0; ChunkIndex < FileState->Chunks.Num(); ++ChunkIndex)
	{
		FVersionUpdateDownloadChunk& Chunk = FileState->Chunks[ChunkIndex];
		if (Chunk.bFullFileRequest)
		{
			continue;
		}

		const FString ChunkKey = MakeChunkKey(Chunk.URL, Chunk.RangeHeader);
		if (RunningDownloadChunkKeys.Remove(ChunkKey) > 0)
		{
			if (!Chunk.RequestHandle.IsNone())
			{
				UN_HTTP.Cancel(Chunk.RequestHandle);
			}

			--TotalDownloadChunkCount;
		}
	}

	// 新增一个无 Range Header 的整文件请求。它仍然使用同一套 UnHTTP 完成/进度回调。
	FVersionUpdateDownloadChunk FullChunk;
	FullChunk.FileName = FileState->FileName;
	FullChunk.URL = FileState->URL;
	// 整文件 fallback 使用单独的 full.chunk，下载完成后仍合并进 .part 再校验。
	FullChunk.ChunkPath = FileState->PartPath + TEXT(".full.chunk");
	FullChunk.Start = 0;
	FullChunk.End = FileState->File.Size > 0 ? FileState->File.Size - 1 : 0;
	FullChunk.bFullFileRequest = true;

	const int32 FullChunkIndex = FileState->Chunks.Add(FullChunk);
	// 整文件请求没有 RangeHeader，因此 key 形如 URL|。
	const FString FullChunkKey = MakeChunkKey(FullChunk.URL, FullChunk.RangeHeader);
	ActiveDownloadChunkToFile.Add(FullChunkKey, FileName);
	ActiveDownloadChunkIndex.Add(FullChunkKey, FullChunkIndex);
	++TotalDownloadChunkCount;

	if (!StartDownloadChunk(FullChunkKey))
	{
		++CompletedDownloadChunkCount;
		FinishDownloadFileIfReady(FileName);
		return false;
	}

	UE_LOG(LogVersionUpdate, Warning, TEXT("[VersionUpdate] Server ignored Range for %s, fallback to full download."), *FileName);
	return true;
}

void UVersionUpdateSubsystem::OnDownloadChunkComplete(const FString& ChunkKey, const FUnHttpRequest& Request, const FUnHttpResponse& Response, bool bSucceeded)
{
	// ChunkKey 在绑定 lambda 时捕获，避免依赖 UnHTTP 回调里的 Request Header 是否保留 Range。
	const FString* FileName = ActiveDownloadChunkToFile.Find(ChunkKey);
	const int32* ChunkIndex = ActiveDownloadChunkIndex.Find(ChunkKey);
	if (!FileName || !ChunkIndex)
	{
		bLastDownloadSucceeded = false;
		if (RunningDownloadChunkKeys.Remove(ChunkKey) > 0)
		{
			++CompletedDownloadChunkCount;
		}

		FinishAllDownloadsIfReady();
		UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Lost download chunk state: %s"), *ChunkKey);
		return;
	}

	FVersionUpdateDownloadFileState* FileStatePtr = ActiveDownloadFiles.Find(*FileName);
	if (!FileStatePtr || !FileStatePtr->Chunks.IsValidIndex(*ChunkIndex))
	{
		bLastDownloadSucceeded = false;
		if (RunningDownloadChunkKeys.Remove(ChunkKey) > 0)
		{
			++CompletedDownloadChunkCount;
		}

		FinishAllDownloadsIfReady();
		UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Invalid download chunk state: %s"), *ChunkKey);
		return;
	}

	FVersionUpdateDownloadFileState& FileState = *FileStatePtr;
	FVersionUpdateDownloadChunk& Chunk = FileState.Chunks[*ChunkIndex];

	// 文件已经切到整文件 fallback 后，旧 Range 请求的迟到回调不再参与计数和落盘。
	if (FileState.bFallbackToFullDownload && !Chunk.bFullFileRequest)
	{
		RunningDownloadChunkKeys.Remove(ChunkKey);
		return;
	}

	RunningDownloadChunkKeys.Remove(ChunkKey);
	++CompletedDownloadChunkCount;

	// bRangeRequest 表示当前请求是否要求服务端只返回文件的一段。
	const bool bRangeRequest = !Chunk.RangeHeader.IsEmpty();

	if (bRangeRequest && Response.ResponseCode == 200 && !IsWholeFileRange(Chunk, FileState.File))
	{
		// 服务端忽略 Range 时通常会返回 200 + 完整文件。当前分片不能直接合并，
		// 需要丢弃该文件剩余分片并重新发起一个无 Range 的整文件请求。
		Chunk.bCompleted = true;
		Chunk.bSucceeded = false;
		StartFullFileDownloadFallback(*FileName);
		StartQueuedDownloadChunks();
		FinishAllDownloadsIfReady();
		return;
	}

	// Range 分片正常应返回 206；只有“单分片且从 0 到文件末尾”的请求允许 200，
	// 用于兼容小文件或不支持 Range 的服务端退化成整文件下载
	// bStatusOK 只判断 HTTP 状态码是否符合预期，内容大小和落盘结果在后面继续校验。
	const bool bStatusOK = bRangeRequest
		? (Response.ResponseCode == 206 || (Chunk.Start == 0 && Chunk.End == FileState.File.Size - 1 && Response.ResponseCode >= 200 && Response.ResponseCode < 300))
		: (Response.ResponseCode >= 200 && Response.ResponseCode < 300);

	// bChunkSucceeded 是当前分片最终是否成功的临时判定，会逐步叠加网络、状态码、内容、落盘校验。
	bool bChunkSucceeded = bSucceeded && bStatusOK && Response.Content;
	if (bChunkSucceeded && bRangeRequest && Response.ResponseCode == 206)
	{
		// 206 返回体必须刚好等于请求的 Range 长度，否则合并后的 .part 会错位。
		bChunkSucceeded = Response.Content->GetContent().Num() == Chunk.GetExpectedSize();
	}

	if (bChunkSucceeded)
	{
		bChunkSucceeded = SaveResponseContentToFile(Response, Chunk.ChunkPath);
	}

	Chunk.bCompleted = true;
	Chunk.bSucceeded = bChunkSucceeded;
	FileState.bFailed |= !bChunkSucceeded;

	// 单个分片完成后可能触发三个动作：
	// 1. 当前文件所有分片完成则合并；
	// 2. 释放一个并发槽位后继续启动等待队列；
	// 3. 如果所有文件都完成则广播总完成事件。
	FinishDownloadFileIfReady(*FileName);
	StartQueuedDownloadChunks();
	FinishAllDownloadsIfReady();
}

void UVersionUpdateSubsystem::OnDownloadChunkProgress(const FString& ChunkKey, const FUnHttpRequest& Request, uint64 BytesSent, uint64 BytesReceived)
{
	// 进度回调同样通过启动时捕获的 ChunkKey 找回文件状态。
	const FString* FileName = ActiveDownloadChunkToFile.Find(ChunkKey);
	const int32* ChunkIndex = ActiveDownloadChunkIndex.Find(ChunkKey);
	if (!FileName || !ChunkIndex)
	{
		return;
	}

	FVersionUpdateDownloadFileState* FileStatePtr = ActiveDownloadFiles.Find(*FileName);
	if (!FileStatePtr || !FileStatePtr->Chunks.IsValidIndex(*ChunkIndex))
	{
		return;
	}

	FVersionUpdateDownloadFileState& FileState = *FileStatePtr;
	FVersionUpdateDownloadChunk& Chunk = FileState.Chunks[*ChunkIndex];
	if (FileState.bFallbackToFullDownload && !Chunk.bFullFileRequest)
	{
		return;
	}

	Chunk.BytesReceived = BytesReceived;

	// DownloadedBytes = 断点已有字节 + 已完成分片 + 正在下载分片的当前进度。
	int64 DownloadedBytes = FileState.ExistingBytes;
	for (const FVersionUpdateDownloadChunk& Item : FileState.Chunks)
	{
		if (FileState.bFallbackToFullDownload && !Item.bFullFileRequest)
		{
			continue;
		}

		// 已完成分片按完整分片大小计入；下载中的分片按当前收到字节计入。
		DownloadedBytes += Item.bCompleted
			? Item.GetExpectedSize()
			: FMath::Min<int64>(static_cast<int64>(Item.BytesReceived), Item.GetExpectedSize());
	}

	// Percentage 是当前文件级进度，而不是全局所有文件进度。
	const float Percentage = FileState.File.Size > 0
		? static_cast<float>(static_cast<double>(DownloadedBytes) / static_cast<double>(FileState.File.Size))
		: 0.f;

	OnVersionDownloadProgress.Broadcast(
		FMath::Clamp(Percentage, 0.f, 1.f),
		*FileName,
		FileState.File.Size,
		DownloadedBytes);
}

void UVersionUpdateSubsystem::FinishDownloadFileIfReady(const FString& FileName)
{
	// 只有文件的所有分片都完成后才会进入合并；任何一个分片失败都会保留 .part，便于下次重新判断。
	FVersionUpdateDownloadFileState* FileState = ActiveDownloadFiles.Find(FileName);
	if (!FileState || FileState->bReported)
	{
		return;
	}

	for (const FVersionUpdateDownloadChunk& Chunk : FileState->Chunks)
	{
		if (FileState->bFallbackToFullDownload && !Chunk.bFullFileRequest)
		{
			continue;
		}

		if (!Chunk.bCompleted)
		{
			return;
		}
	}

	FileState->CompletedChunks = FileState->Chunks.Num();

	if (!FileState->bFailed)
	{
		// 分片是并发完成的，落盘顺序不可靠；合并前必须按 Start 排序。
		FileState->Chunks.Sort([](const FVersionUpdateDownloadChunk& L, const FVersionUpdateDownloadChunk& R)
		{
			return L.Start < R.Start;
		});

		// 使用 Append 追加到 .part，保留上次已完成的断点内容。
		TUniquePtr<FArchive> PartWriter(IFileManager::Get().CreateFileWriter(*FileState->PartPath, FILEWRITE_Append));
		if (!PartWriter.IsValid())
		{
			FileState->bFailed = true;
		}
		else
		{
			for (const FVersionUpdateDownloadChunk& Chunk : FileState->Chunks)
			{
				if (FileState->bFallbackToFullDownload && !Chunk.bFullFileRequest)
				{
					continue;
				}

				if (!AppendFileToArchive(*PartWriter, Chunk.ChunkPath))
				{
					FileState->bFailed = true;
					break;
				}
			}

			PartWriter->Close();
		}
	}

	for (const FVersionUpdateDownloadChunk& Chunk : FileState->Chunks)
	{
		// chunk 只是本轮临时文件，无论最终成功失败都清理，避免下次误复用半截分片。
		IFileManager::Get().Delete(*Chunk.ChunkPath);
	}

	if (!FileState->bFailed)
	{
		// 合并完成后先做大小校验，再把 .part 提升为本轮下载产物。
		if (FileState->File.Size > 0 && IFileManager::Get().FileSize(*FileState->PartPath) != FileState->File.Size)
		{
			FileState->bFailed = true;
		}
		else
		{
			IFileManager::Get().Delete(*FileState->SavePath);
			FileState->bFailed = !IFileManager::Get().Move(*FileState->SavePath, *FileState->PartPath, true, true);
		}
	}

	if (!FileState->bFailed && !FileState->File.Hash.IsEmpty())
	{
		// 最后做 Hash 校验，确保服务端内容、断点缓存和合并顺序都正确。
		const FString LocalHash = LexToString(FMD5Hash::HashFile(*FileState->SavePath));
		FileState->bFailed = !LocalHash.Equals(FileState->File.Hash, ESearchCase::IgnoreCase);
		if (FileState->bFailed)
		{
			IFileManager::Get().Delete(*FileState->SavePath);
		}
	}

	FileState->bReported = true;
	bLastDownloadSucceeded &= !FileState->bFailed;

	// 单文件完成事件只广播一次；总完成事件由 FinishAllDownloadsIfReady 统一判断。
	OnPatchFileDownloadCompleted.Broadcast(FileName, !FileState->bFailed);
}

void UVersionUpdateSubsystem::FinishAllDownloadsIfReady()
{
	// 所有分片请求都完成、等待队列为空、运行队列为空，才允许进入总完成判断。
	if (CompletedDownloadChunkCount < TotalDownloadChunkCount || PendingDownloadChunkKeys.Num() > 0 || RunningDownloadChunkKeys.Num() > 0)
	{
		return;
	}

	for (const TPair<FString, FVersionUpdateDownloadFileState>& Pair : ActiveDownloadFiles)
	{
		// 文件可能还在合并或校验阶段；必须等每个文件都完成单文件广播后，再广播总完成。
		if (!Pair.Value.bReported)
		{
			return;
		}
	}

	OnVersionDownloadCompleted.Broadcast(bLastDownloadSucceeded);
}

FString UVersionUpdateSubsystem::GetDownloadPathToPak() const
{
	// Pak / PakSig 下载到 Saved/TmpPak，安装阶段再复制到 Content/Paks。
	const FString DownloadPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("TmpPak"));
	EnsureDirectory(DownloadPath);
	return DownloadPath;
}

FString UVersionUpdateSubsystem::GetDownloadPathToCustom() const
{
	// 非 Pak 文件下载到 Saved/TmpCustom，安装阶段按 RelativePatchs 映射复制到最终目录。
	const FString DownloadPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("TmpCustom"));
	EnsureDirectory(DownloadPath);
	return DownloadPath;
}
