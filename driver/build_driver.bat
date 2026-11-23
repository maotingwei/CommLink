@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
REM =====================================================================
REM  build_driver.bat  -  编译 CommLink.sys
REM  依赖: Visual Studio 2019 BuildTools(已安装) + Windows Driver Kit (WDK)
REM  本机当前: VS2019 BuildTools 14.29 已安装, WDK 未安装。
REM           请先安装与 SDK 10.0.19041 匹配的 WDK(见 README),再运行本脚本。
REM =====================================================================

echo.
echo [CommLink] ===== 虚拟串口驱动编译 =====
echo.

set "VSBT=D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools"
set "VCVARS=%VSBT%\VC\Auxiliary\Build\vcvarsall.bat"
set "WDK_INC=C:\Program Files (x86)\Windows Kits\10\Include\10.0.19041.0"
set "WDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0"

REM --- 检测 vcvarsall ---
if not exist "%VCVARS%" (
    echo [错误] 未找到 vcvarsall.bat: %VCVARS%
    echo         请修改本脚本顶部的 VSBT 变量指向你的 VS BuildTools 路径。
    goto :fail
)

REM --- 检测 WDK 内核头文件 (km\wdm.h) ---
if not exist "%WDK_INC%\km\wdm.h" (
    echo [错误] 未检测到 WDK 内核头文件:
    echo         %WDK_INC%\km\wdm.h
    echo.
    echo         说明: 本机只安装了 Windows SDK(用户态),没有安装 WDK(内核态)。
    echo         驱动必须在安装了 WDK 的机器上编译。请按 README 的步骤
    echo         安装 WDK 10 (版本 10.0.19041.0),然后重新运行本脚本。
    echo.
    goto :fail
)

echo [信息] 初始化 MSVC x64 编译环境...
call "%VCVARS%" x64 >nul 2>&1
if errorlevel 1 (
    echo [错误] vcvarsall x64 初始化失败。
    goto :fail
)

echo [信息] 编译驱动 ...
cl /nologo /c /Cx /GF /Gw /W4 /wd4100 /wd4189 /wd4127 ^
   /D_X86_=1 /DAMD64=1 /D_WIN64=1 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 ^
   /I"%WDK_INC%\km" /I"%WDK_INC%\shared" ^
   /FoCommLink.obj CommLink.c
if errorlevel 1 (
    echo [错误] 编译失败。
    goto :fail
)

echo [信息] 链接驱动 ...
link /nologo /DRIVER /SUBSYSTEM:NATIVE /MACHINE:X64 /ENTRY:DriverEntry ^
     /LIBPATH:"%WDK_LIB%\km\x64" ^
     /OUT:CommLink.sys CommLink.obj ntoskrnl.lib hal.lib
if errorlevel 1 (
    echo [错误] 链接失败。
    goto :fail
)

echo.
echo [成功] 已生成 CommLink.sys
echo.
exit /b 0

:fail
echo.
echo [失败] 编译未成功。
exit /b 1
