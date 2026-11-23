@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
REM =====================================================================
REM  install_pnp.bat  -  安装/卸载 PnP 版 CommLink 虚拟串口驱动
REM
REM  用法(需管理员权限):
REM    install_pnp.bat install    [COM号]   安装,默认 COM8
REM    install_pnp.bat uninstall             卸载设备与驱动
REM    install_pnp.bat com        [COM号]   仅修改虚拟串口号
REM    install_pnp.bat testsigning on|off    开启/关闭测试签名模式(需重启)
REM    install_pnp.bat status                查看状态
REM
REM  前提: 首次安装前需开启测试签名模式并重启:
REM    install_pnp.bat testsigning on   然后重启电脑
REM =====================================================================

set "ACTION=%~1"
set "ARG2=%~2"
set "COMPORT=%~2"
if "%COMPORT%"=="" set "COMPORT=8"
set "SVC=CommLinkPnp"
set "DIR=%~dp0"
set "INF=%DIR%CommLinkPnp.inf"
set "SYS=%DIR%CommLinkPnp.sys"
set "CER=%DIR%CommLinkTest.cer"
set "MKROOT=%DIR%mkroot.exe"

echo [CommLink] 动作: %ACTION%

REM ---- 管理员权限检查 ----
net session >nul 2>&1
if errorlevel 1 (
    echo [错误] 请以管理员身份运行本脚本。
    exit /b 1
)

REM ---- 测试签名模式开关 ----
if /i "%ACTION%"=="testsigning" (
    if /i "%ARG2%"=="on" (
        echo [1/1] 开启测试签名模式...
        bcdedit /set testsigning on
        echo [完成] 请重启电脑后再次运行本脚本安装驱动。
        exit /b 0
    )
    if /i "%ARG2%"=="off" (
        echo [1/1] 关闭测试签名模式...
        bcdedit /set testsigning off
        echo [完成] 请重启电脑生效。
        exit /b 0
    )
    echo 用法: install_pnp.bat testsigning on^|off
    exit /b 1
)

if /i "%ACTION%"=="install" (
    for %%F in ("%SYS%" "%INF%" "%CER%" "%MKROOT%") do (
        if not exist %%~F (
            echo [错误] 缺少文件: %%~F
            echo         请从 GitHub Actions 的 CommLink-PnP-x64 产物下载完整包并解压到本目录。
            exit /b 1
        )
    )

    echo.
    echo [0/5] 检查测试签名模式...
    bcdedit /enum "{current}" | find /i "testsigning" | find /i "Yes" >nul
    if errorlevel 1 (
        echo [警告] 测试签名模式未开启,驱动可能无法加载。
        echo         正在尝试开启...
        bcdedit /set testsigning on >nul
        echo [重要] 必须重启电脑后再次运行本脚本!现在先退出。
        exit /b 2
    )
    echo   测试签名模式已开启。

    echo [1/5] 导入自签证书到受信任根/发布者...
    certutil -addstore -f "ROOT" "%CER%" >nul 2>&1
    if errorlevel 1 echo   (ROOT 导入失败,继续)
    certutil -addstore -f "TrustedPublisher" "%CER%" >nul 2>&1
    if errorlevel 1 echo   (TrustedPublisher 导入失败,继续)
    echo   证书已导入。

    echo [2/5] 写入虚拟串口号 COM%COMPORT% 到注册表...
    reg add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ComPort /t REG_DWORD /d %COMPORT% /f >nul 2>&1
    REM 服务键可能尚未创建(首次安装),先放过,mkroot 装完后再写一次

    echo [3/5] 安装驱动并创建 Root 设备节点...
    "%MKROOT%" install "%INF%"
    if errorlevel 1 (
        echo [错误] 设备安装失败。
        exit /b 1
    )

    echo [4/5] 再次确认串口号 COM%COMPORT%...
    reg add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ComPort /t REG_DWORD /d %COMPORT% /f >nul 2>&1

    echo [5/5] 完成。
    echo.
    echo   请在设备管理器"端口(COM和LPT)"下查看:
    echo     CommLink Virtual Serial Port (COM%COMPORT%)
    echo   若串口号未更新,执行: install_pnp.bat com %COMPORT% 然后重新插拔/重启。
    echo   控制设备仍为: \\.\CommLinkCtl
    exit /b 0
)

if /i "%ACTION%"=="com" (
    echo [信息] 设置虚拟串口号为 COM%COMPORT% ...
    reg add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ComPort /t REG_DWORD /d %COMPORT% /f >nul
    if errorlevel 1 (
        echo [错误] 写注册表失败。驱动可能未安装,请先 install。
        exit /b 1
    )
    echo [完成] 已设为 COM%COMPORT%。请重启电脑或重新安装设备使符号链接生效。
    exit /b 0
)

if /i "%ACTION%"=="uninstall" (
    echo [1/2] 移除 Root 设备节点...
    if exist "%MKROOT%" (
        "%MKROOT%" uninstall
    ) else (
        echo   (未找到 mkroot.exe,跳过)
    )
    echo [2/2] 从驱动库移除驱动包...
    pnputil /enum-drivers > "%TEMP%\cl_drivers.txt" 2>nul
    echo   提示: 可在"设备管理器"中右键设备选择卸载。
    echo [完成] 已卸载。
    exit /b 0
)

if /i "%ACTION%"=="status" (
    echo === 服务状态 ===
    sc query %SVC% 2>nul
    echo.
    echo === 注册表参数 ===
    reg query "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" 2>nul
    echo.
    echo === 测试签名模式 ===
    bcdedit /enum "{current}" | find /i "testsigning"
    echo.
    echo === 设备节点 ===
    pnputil /enum-devices /class Ports 2>nul | find /i "CommLink"
    exit /b 0
)

echo.
echo 用法:
echo   install_pnp.bat install [COM号]
echo   install_pnp.bat uninstall
echo   install_pnp.bat com [COM号]
echo   install_pnp.bat testsigning on^|off
echo   install_pnp.bat status
exit /b 1
