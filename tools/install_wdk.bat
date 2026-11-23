@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
REM =====================================================================
REM  install_wdk.bat  -  通过 winget 一键安装与本机 SDK 匹配的 WDK
REM  本机 SDK 为 10.0.19041.0,因此安装 WDK 10.0.19041。
REM  需要管理员权限 + 联网。winget 会自动下载 ISO 并静默安装。
REM =====================================================================

echo [CommLink] ===== 安装 Windows Driver Kit (WDK) =====
echo.

net session >nul 2>&1
if errorlevel 1 (
    echo [错误] 请以管理员身份运行。
    exit /b 1
)

echo [信息] 安装 WDK 10.0.19041 (与本机 SDK 10.0.19041.0 匹配)...
echo [信息] 该过程会下载约 1GB+,请耐心等待。

winget install --id Microsoft.WindowsWDK.10.0.19041 ^
    --accept-package-agreements --accept-source-agreements --source winget

if errorlevel 1 (
    echo [错误] winget 安装失败。可改用 README 中的 GitHub Actions 在线编译方案(无需本机安装)。
    exit /b 1
)

echo.
echo [完成] WDK 已安装。请核实存在:
echo   C:\Program Files (x86)\Windows Kits\10\Include\10.0.19041.0\km\wdm.h
echo 然后进入 driver 目录运行 build_driver.ps1 或 build_driver.bat 编译驱动。
echo 注意: 安装 WDK 后可能需要重启或重新打开终端。
exit /b 0
