chcp 65001 >nul
@echo off
setlocal enabledelayedexpansion

echo 创建 VersionUpdateSupport 的软链接到 {UE源码目录}\Engine\Source\Programs
echo.

:: 手动输入 UE 源码路径
set /p "UnrealEngineDir=请输入 UE 源码根目录路径，例如 D:\UE5\UE_5.4 : "

if "%UnrealEngineDir%"=="" (
    echo 未输入路径，无法继续！
    pause
    exit /b
)

echo UE 源码目录: %UnrealEngineDir%
echo.

:: 当前目录
set "CurrentDir=%~dp0"
echo 当前目录: %CurrentDir%

:: 本地 VersionUpdateSupport 目录
set "LocalSupport=%CurrentDir%VersionUpdateSupport"

if not exist "%LocalSupport%" (
    echo 未找到 VersionUpdateSupport 文件夹: %LocalSupport%
    pause
    exit /b
)

:: UE Programs 目标路径
set "UEPrograms=%UnrealEngineDir%\Engine\Source\Programs"

if not exist "%UEPrograms%" (
    echo UE Programs 目录不存在: %UEPrograms%
    echo 请确认输入的是完整 UE 源码根目录！
    pause
    exit /b
)

echo UE Programs 目录: %UEPrograms%

:: 最终软链接路径
set "TargetLink=%UEPrograms%\VersionUpdateSupport"

echo.
echo 将创建软连接: %TargetLink%
echo 指向本地: %LocalSupport%
echo.

:: 删除旧软链接或文件夹
if exist "%TargetLink%" (
    echo 已存在 VersionUpdateSupport，正在删除旧内容...
    rmdir "%TargetLink%" /S /Q
)

:: 执行创建软链接
echo 创建软连接...
mklink /D "%TargetLink%" "%LocalSupport%"

if errorlevel 1 (
    echo 创建失败！请以管理员身份运行此脚本。
) else (
    echo 创建成功！
)

pause