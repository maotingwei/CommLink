@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
REM =====================================================================
REM  install_driver.bat  -  安装/启动/停止/卸载 CommLink 虚拟串口驱动
REM  用法:
REM    install_driver.bat install    [COM号]   安装并启动(默认 COM8)
REM    install_driver.bat stop                 停止驱动
REM    install_driver.bat start                启动驱动
REM    install_driver.bat uninstall            卸载驱动
REM    install_driver.bat status               查看状态
REM  注意: 需要管理员权限。64位系统需要驱动签名或通过测试签名模式加载。
REM =====================================================================

set "ACTION=%~1"
set "COMPORT=%~2"
if "%COMPORT%"=="" set "COMPORT=8"
set "SVC=CommLink"
set "SYS=%~dp0CommLink.sys"

echo [CommLink] 动作: %ACTION%   COM口号: %COMPORT%

REM 检查管理员权限
net session >nul 2>&1
if errorlevel 1 (
    echo [错误] 请以管理员身份运行本脚本。
    exit /b 1
)

if /i "%ACTION%"=="install" (
    if not exist "%SYS%" (
        echo [错误] 未找到 %SYS%,请先编译驱动。
        exit /b 1
    )
    echo [1/4] 写入注册表参数(COM%COMPORT%)...
    reg add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ComPort /t REG_DWORD /d %COMPORT% /f >nul
    echo [2/4] 创建服务...
    sc create %SVC% type= kernel start= demand binPath= "%SYS%" error= normal >nul 2>&1
    if errorlevel 1 echo   (服务可能已存在,继续)
    echo [3/4] 启动驱动...
    sc start %SVC%
    if errorlevel 1 (
        echo [错误] 启动失败。常见原因: 驱动未签名 / 未开启测试签名模式。
        echo         见 README 中"驱动签名与测试模式"一节。
        exit /b 1
    )
    echo [4/4] 完成。虚拟串口为 \\.\COM%COMPORT% , 控制设备为 \\.\CommLinkCtl
    exit /b 0
)

if /i "%ACTION%"=="start" (
    sc start %SVC%
    exit /b 0
)

if /i "%ACTION%"=="stop" (
    sc stop %SVC%
    exit /b 0
)

if /i "%ACTION%"=="uninstall" (
    sc stop %SVC% >nul 2>&1
    sc delete %SVC%
    reg delete "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /f >nul 2>&1
    echo 已卸载。
    exit /b 0
)

if /i "%ACTION%"=="status" (
    sc query %SVC%
    echo.
    echo 注册表参数:
    reg query "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" 2>nul
    exit /b 0
)

echo 用法: install_driver.bat [install [COM号] ^| start ^| stop ^| uninstall ^| status]
exit /b 1
