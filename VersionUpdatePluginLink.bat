chcp 65001 >nul
@echo off
setlocal enabledelayedexpansion

echo 将当前目录软链接到 {UE源码目录}\Engine\Plugins
echo.

:: 手动输入 UE 源码根目录路径
set /p "UnrealEngineDir=请输入 UE 源码根目录路径，例如 D:\UE5\UE_5.4 : "

if "%UnrealEngineDir%"=="" (
    echo 未输入路径，无法继续！
    pause
    exit /b
)

echo UE 源码目录: %UnrealEngineDir%
echo.

:: 当前目录（本插件所在目录）
set "CurrentDir=%~dp0"
:: 去掉末尾的反斜杠（为了更规范）
set "CurrentDir=%CurrentDir:~0,-1%"

echo 当前目录: %CurrentDir%

:: 插件名（当前目录名）
for %%A in ("%CurrentDir%") do set "PluginName=%%~nA"

echo 检测到插件名称: %PluginName%
echo.

:: UE Engine Plugins 目标目录
set "UEPluginDir=%UnrealEngineDir%\Engine\Plugins"

if not exist "%UEPluginDir%" (
    echo UE Plugins 目录不存在: %UEPluginDir%
    echo 请确认输入的是正确 UE 源码根目录！
    pause
    exit /b
)

echo UE Plugins 目录: %UEPluginDir%

:: 最终软链接路径
set "TargetLink=%UEPluginDir%\%PluginName%"

echo.
echo 将创建软链接:
echo   %TargetLink%
echo 指向本地:
echo   %CurrentDir%
echo.

:: 删除旧软链接或文件夹
if exist "%TargetLink%" (
    echo 已存在该插件，正在删除旧内容...
    rmdir "%TargetLink%" /S /Q
)

:: 创建软链接
echo 创建软链接...
mklink /D "%TargetLink%" "%CurrentDir%"

if errorlevel 1 (
    echo 创建失败！请以管理员身份运行该脚本。
) else (
    echo 插件软链接创建成功！
)

pause