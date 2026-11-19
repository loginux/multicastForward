@echo off
setlocal

rem 客户端需要接收组播，必须放行其入站 UDP；请右键本文件选择"以管理员身份运行"

net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] 需要管理员权限，请右键本文件选择"以管理员身份运行"
    pause
    exit /b 1
)

set "EXE="
if exist "%~dp0deploy\multicastForward.exe" set "EXE=%~dp0deploy\multicastForward.exe"
if not defined EXE if exist "%~dp0build\multicastForward.exe" set "EXE=%~dp0build\multicastForward.exe"

if not defined EXE (
    echo [ERROR] 找不到 multicastForward.exe，请先运行 build.bat 或 deploy.bat
    pause
    exit /b 1
)

netsh advfirewall firewall delete rule name="mc_forward" >nul 2>&1
netsh advfirewall firewall add rule name="mc_forward" dir=in action=allow program="%EXE%" enable=yes profile=any

if errorlevel 1 (
    echo [ERROR] 添加防火墙规则失败
    pause
    exit /b 1
)

echo.
echo [OK] 已放行入站: %EXE%
echo 说明: 规则按程序放行，只在客户端需要；服务端只监听 TCP 入站，若连不上也需放行同一个 exe。
pause
