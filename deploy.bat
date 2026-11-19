@echo off
setlocal

rem 打包成单个 deploy 目录：一个程序 + 两份配置（client.ini / server.ini）
set "QTDIR=C:\Qt\5.15.2\msvc2019_64"
set "ROOT=%~dp0"
set "OUT=%ROOT%deploy"
set "REDISTROOT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Redist\MSVC"

if not exist "%QTDIR%\bin\windeployqt.exe" (
    echo [ERROR] 找不到 %QTDIR%\bin\windeployqt.exe
    exit /b 1
)
if not exist "%ROOT%build\multicastForward.exe" (
    echo [ERROR] 找不到 build\multicastForward.exe，请先运行 build.bat
    exit /b 1
)

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

echo [pack] 主程序与配置
copy /y "%ROOT%build\multicastForward.exe"         "%OUT%\" >nul
copy /y "%ROOT%build\multicastForwardLauncher.exe" "%OUT%\" >nul
copy /y "%ROOT%packaging\config.client.ini"        "%OUT%\client.ini" >nul
copy /y "%ROOT%packaging\config.server.ini"        "%OUT%\server.ini" >nul
copy /y "%ROOT%packaging\README.txt"               "%OUT%\使用说明.txt" >nul

echo [pack] Qt 运行库
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler "%OUT%\multicastForward.exe" >nul
if errorlevel 1 (
    echo [ERROR] Qt 运行库收集失败
    exit /b 1
)

rem ---- VC 运行库 ----
set "CRTDIR="
for /f "delims=" %%F in ('dir /s /b "%REDISTROOT%\vcruntime140.dll" 2^>nul') do set "CRTDIR=%%~dpF"
if defined CRTDIR (
    xcopy /y /q "%CRTDIR%*.dll" "%OUT%\" >nul
    echo [pack] VC 运行库已拷贝
) else (
    echo [warn] 未找到 VC 运行库，目标机需安装 VC++ 2015-2022 运行库
)

echo.
echo [OK] 已生成 deploy\
echo      整个 deploy 目录拷到目标机即可，不再区分客户端/服务端
echo      首次运行前：把 client.ini 或 server.ini 复制为 config.ini
