@echo off
setlocal

rem 直接设置 MSVC / Windows SDK 环境（不调用 vcvars，避免依赖 reg.exe）
if "%VCTOOLS%"=="" set "VCTOOLS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231"
if "%SDKVER%"==""  set "SDKVER=10.0.26100.0"
if "%SDKROOT%"=="" set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
if "%QTDIR%"==""   set "QTDIR=C:\Qt\5.15.2\msvc2019_64"

set "PATH=%VCTOOLS%\bin\Hostx64\x64;%SDKROOT%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%VCTOOLS%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\winrt"
set "LIB=%VCTOOLS%\lib\x64;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"

cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QTDIR% %*
if errorlevel 1 goto :fail
cmake --build "%~dp0build"
if errorlevel 1 goto :fail
echo [OK] 构建完成，产物在 build\
exit /b 0

:fail
echo.
echo [ERROR] 构建失败。若提示 LNK1104 无法打开 exe，请先关闭正在运行的 mc_forward 进程
exit /b 1
