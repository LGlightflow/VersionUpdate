#include "VersionUpdateSubsystem.h"

#include "Misc/FileHelper.h"
#include "Settings/VersionManifestClientObject.h"
#include "VersionUpdateLogChannels.h"

// 本地 ClientManifest 的读写。
// 本地 JSON 只是用于记录已安装文件等客户端状态，不是完全可信

void UVersionUpdateSubsystem::LoadOrCreateClientManifest()
{
	const FString ClientManifestPath = FPaths::ConvertRelativePathToFull(GetClientManifestPath());
	if (IFileManager::Get().FileExists(*ClientManifestPath))
	{
		// 已存在则读取本地状态，例如已安装文件列表。
		LoadClientManifest();
	}
	else
	{
		// 首次运行时创建默认文件，保证后续安装完成可以直接覆盖写入。
		ClientManifest.CurrentVersion = CurrentVersion;
		ClientManifest.InstalledPatchFiles.Empty();
		SaveClientManifest();
	}

	// 读取本地 JSON 后也要重新覆盖版本号，避免被篡改的本地版本影响本次判断。
	ClientManifest.CurrentVersion = CurrentVersion;
}

bool UVersionUpdateSubsystem::SaveClientManifest()
{
	// 保存前同步权威版本号。安装完成后调用时，可记录当前已安装到的版本。
	const FString FilePath = GetClientManifestPath();
	if (!VersionClientJson::SaveToFile(ClientManifest, FilePath))
	{
		// 序列化或写入失败通常是结构数据异常、路径、权限或文件占用问题。
		UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Failed to save client manifest: %s"), *FilePath);
		return false;
	}

	return true;
}

bool UVersionUpdateSubsystem::LoadClientManifest()
{
	// 这里只读取持久化状态，不在这里决定是否需要更新。
	const FString FilePath = GetClientManifestPath();

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *FilePath))
	{
		// 文件不存在时通常应由 LoadOrCreateClientManifest 创建；直接调用这里则返回失败。
		UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Failed to load client manifest: %s"), *FilePath);
		return false;
	}

	if (!VersionClientJson::Read(Json, ClientManifest))
	{
		// JSON 损坏时返回失败，业务层可选择重新初始化或走完整校验。
		UE_LOG(LogVersionUpdate, Error, TEXT("[VersionUpdate] Failed to parse client manifest: %s"), *FilePath);
		return false;
	}

	return true;
}

FString UVersionUpdateSubsystem::GetClientManifestPath() const
{
	// 路径由配置对象控制；根目录固定为 ProjectContentDir 以兼容现有安装器。
	return FPaths::ProjectContentDir() / GetDefault<UVersionManifestClientObject>()->ClientJsonSavePath;
}
