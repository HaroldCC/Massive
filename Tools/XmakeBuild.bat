@echo off
:: XmakeBuild.bat — 从 MSVC Developer Command Prompt 环境中调用 xmake
:: 用法:
::   Tools\XmakeBuild.bat                    → xmake build
::   Tools\XmakeBuild.bat f -c               → xmake f -c
::   Tools\XmakeBuild.bat build Fmt           → xmake build Fmt
::
:: Git Bash / MSYS2 下 xmake 会检测到 MinGW 工具链，
:: 此脚本通过 vcvars64.bat 强制使用 MSVC。

setlocal

:: 用 vswhere 找到 VS
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    set "VSWHERE=C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe"
)

if not exist "%VSWHERE%" (
    echo [X] 找不到 vswhere.exe，请确认 Visual Studio 已安装
    exit /b 1
)

for /f "tokens=*" %%i in ('"%VSWHERE%" -latest -property installationPath') do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo [X] 找不到 Visual Studio 安装路径
    exit /b 1
)

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [X] 找不到 vcvars64.bat: %VCVARS%
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [X] vcvars64.bat 执行失败
    exit /b 1
)

:: 执行 xmake 命令
if "%~1"=="" (
    xmake
) else (
    xmake %*
)
