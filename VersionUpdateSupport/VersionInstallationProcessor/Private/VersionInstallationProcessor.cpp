// Copyright Epic Games, Inc. All Rights Reserved.

#include "VersionInstallationProcessor.h"
#include "RequiredProgramMainCPPInclude.h"
#include "StandaloneRenderer.h"
#include "Core/Widget/SMainScreen.h"
#include "ThreadManage.h"
#include "VersionInstallationProcessorType.h"
#include "Core/Installation/VersionInstallation.h"
#include "Core/Log/VersionInstallationProcessorLog.h"
#include "Core/Style/VersionInstallationProcessorStyle.h"

IMPLEMENT_APPLICATION(VersionInstallationProcessor, "VersionInstallationProcessor");

#define LOCTEXT_NAMESPACE "VersionInstallationProcessor"

int32 RunVersionInstallationProcessor(const TCHAR* CommandLine)
{
	//I.引擎初始化
	GEngineLoop.PreInit(CommandLine);

	//II.命令初始化
	VersionInstallation::InitCommandInstallationProcessor();

	//III.UObject对象初始化
	ProcessNewlyLoadedUObjects();
	FModuleManager::Get().StartProcessingNewlyLoadedObjects();

	//V.应用程序渲染初始化
	FSlateApplication::InitializeAsStandaloneApplication(GetStandardStandaloneRenderer());

	//VI.DPI初始化
	FSlateApplication::InitHighDPI(true);

	//V.初始化我们的资源
	FVersionInstallationProcessorStyle::Initialize();
	FVersionInstallationProcessorStyle::ReloadTextures();

	//VI.生成自定义的画面
	VersionInstallation::SpawnInstallationProcessorUI();

	//VII.异步运行资源的拷贝
	GThread::Get()->GetProxy().CreateLambda([&]()
	{
		VersionInstallation::Run();
	});

	//VII.渲染和Tick
	while (!IsEngineExitRequested())
	{
		BeginExitIfRequested();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
#if ENGINE_MAJOR_VERSION >= 5
		FTSTicker::GetCoreTicker().Tick(FApp::GetDeltaTime());
#else
		FTicker::GetCoreTicker().Tick(FApp::GetDeltaTime());
#endif
		FSlateApplication::Get().PumpMessages();
		FSlateApplication::Get().Tick();
		GThread::Get()->Tick(FApp::GetDeltaTime());
		FPlatformProcess::Sleep(0.01);
		GFrameCounter++;
	}

	//VIII.结束引擎
	FVersionInstallationProcessorStyle::Shutdown();
	FCoreDelegates::OnExit.Broadcast();
	FSlateApplication::Shutdown();
	FModuleManager::Get().UnloadModulesAtShutdown();
	GThread::Destroy();

	GEngineLoop.AppPreExit();
	GEngineLoop.AppExit();

	return 0;
}


#undef LOCTEXT_NAMESPACE