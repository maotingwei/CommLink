<#
    build_driver.ps1
    =====================================================================
    自适应编译 CommLink.sys —— 本地 / GitHub Actions 通用
    自动探测:
      1) 最新安装了 VC x64 工具的 Visual Studio (通过 vswhere)
      2) 最新含 km\wdm.h 的 WDK Include / Lib 版本目录
    无需手工改版本号。

    用法(本地): 以普通 PowerShell 运行  .\build_driver.ps1
    用法(CI)  : workflow 里 shell: pwsh 运行 ./build_driver.ps1
    =====================================================================
#>
$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot

function Fail($msg) {
    Write-Host "[错误] $msg" -ForegroundColor Red
    $safe = $msg -replace '%', '%25' -replace "`r", '' -replace "`n", ' '
    Write-Host "::error::$safe"
    Pop-Location
    exit 1
}

# ---- 1. 找 vswhere / vcvarsall ----
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "未找到 vswhere.exe,请确认已安装 Visual Studio 或 BuildTools" }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null | Select-Object -First 1
if (-not $vsPath) { Fail "vswhere 未找到带 VC x64 工具的 Visual Studio/BuildTools" }
$vcvarsall = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvarsall)) { Fail "未找到 vcvarsall.bat: $vcvarsall" }
Write-Host "[信息] VC 环境: $vcvarsall"

# ---- 2. 找 WDK ----
$kitRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
if (-not (Test-Path $kitRoot)) { Fail "未找到 Windows Kits 10,请先安装 WDK(见 tools/install_wdk.bat 或 README)" }

$incDir = Get-ChildItem (Join-Path $kitRoot "Include") -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "km\wdm.h") } |
    Sort-Object Name -Descending | Select-Object -First 1
$libDir = Get-ChildItem (Join-Path $kitRoot "Lib") -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "km\x64\ntoskrnl.lib") } |
    Sort-Object Name -Descending | Select-Object -First 1

if (-not $incDir -or -not $libDir) {
    Fail "未检测到 WDK 内核文件(km\wdm.h / km\x64\ntoskrnl.lib)。本机可能只装了 Windows SDK 而非 WDK。"
}
$INC = Join-Path $kitRoot "Include\$($incDir.Name)"
$LIB = Join-Path $kitRoot "Lib\$($libDir.Name)"
Write-Host "[信息] WDK Include: $INC"
Write-Host "[信息] WDK Lib    : $LIB"

# ---- 3. 编译 + 链接(放在同一个 cmd 会话里继承 vcvarsall 环境) ----
$clLine = '/nologo /c /Cx /GF /Gw /GS- /W4 /wd4100 /wd4189 /wd4127 ' +
          '/DAMD64=1 /D_WIN64=1 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 ' +
          '/I"' + $INC + '\km" /I"' + $INC + '\shared" ' +
          '/FoCommLink.obj CommLink.c'
# 注意:现代 WDK(Win10 之后)已无 hal.lib,HAL 功能并入 ntoskrnl.lib,链接它反而报 LNK1104
$linkLine = '/nologo /DRIVER /MACHINE:X64 /SUBSYSTEM:NATIVE /ENTRY:DriverEntry ' +
            '/LIBPATH:"' + $LIB + '\km\x64" ' +
            '/OUT:CommLink.sys CommLink.obj ntoskrnl.lib'
$full = 'call "' + $vcvarsall + '" x64 && cl ' + $clLine + ' && link ' + $linkLine

Write-Host "[信息] 编译中 ..."
$buildLog = Join-Path $PSScriptRoot "build_log.txt"
$rawOutput = cmd /c $full 2>&1 | Out-String
Write-Host $rawOutput
Set-Content -Path $buildLog -Value $rawOutput -Encoding UTF8

if ($LASTEXITCODE -ne 0) {
    # 把编译/链接错误逐行发成 GitHub Actions annotation,便于在 Checks 页与 API 中查看
    foreach ($line in ($rawOutput -split "`r?`n")) {
        if ($line -match '\berror\b|\bfatal\b|LNK[0-9]+|C[0-9]{4}|\[错误\]|未找到|失败') {
            $safe = $line -replace '%', '%25' -replace "`r", '' -replace "`n", ' '
            Write-Host "::error::$safe"
        }
    }
    Fail "编译/链接失败(退出码 $LASTEXITCODE),完整输出见 build_log.txt"
}

if (-not (Test-Path (Join-Path $PSScriptRoot "CommLink.sys"))) {
    Fail "编译结束但未生成 CommLink.sys"
}
Write-Host "[成功] 已生成 CommLink.sys" -ForegroundColor Green
Get-Item (Join-Path $PSScriptRoot "CommLink.sys") | Select-Object Name, Length, LastWriteTime
Pop-Location
