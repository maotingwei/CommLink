<#
    build_pnp.ps1
    =====================================================================
    编译并签名 PnP 版 CommLink 驱动 —— 本地 / GitHub Actions 通用

    产出(均在 driver 目录):
      CommLinkPnp.sys   PnP 驱动
      CommLinkPnp.inf   安装信息文件
      CommLinkPnp.cat   目录文件(由 inf2cat 生成)
      CommLinkTest.cer  自签测试证书(用户需导入信任)
      mkroot.exe        Root 设备安装工具
      CommLink-PnP-all.zip  打包以上全部

    签名采用自签测试证书 + 测试签名模式(testsigning),仅供开发测试。
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
if (-not (Test-Path $vswhere)) { Fail "未找到 vswhere.exe" }
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null | Select-Object -First 1
if (-not $vsPath) { Fail "vswhere 未找到带 VC x64 工具的 Visual Studio" }
$vcvarsall = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvarsall)) { Fail "未找到 vcvarsall.bat" }
Write-Host "[信息] VC 环境: $vcvarsall"

# ---- 2. 找 WDK Include/Lib/Bin ----
$kitRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
if (-not (Test-Path $kitRoot)) { Fail "未找到 Windows Kits 10,请先安装 WDK" }

$incDir = Get-ChildItem (Join-Path $kitRoot "Include") -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "km\wdm.h") } |
    Sort-Object Name -Descending | Select-Object -First 1
$libDir = Get-ChildItem (Join-Path $kitRoot "Lib") -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "km\x64\ntoskrnl.lib") } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $incDir -or -not $libDir) { Fail "未检测到 WDK 内核文件(km\wdm.h / ntoskrnl.lib)" }
$INC = Join-Path $kitRoot "Include\$($incDir.Name)"
$LIB = Join-Path $kitRoot "Lib\$($libDir.Name)"
Write-Host "[信息] WDK Include: $INC"

# 签名工具探测:
#   inf2cat 传统上只在 bin\<ver>\x86\ 下(32位工具),signtool 则 x86/x64 都有
#   这里在 bin 下递归查找,取最新版本目录,并按架构挑选
$binRoot = Join-Path $kitRoot "bin"
$verDir = Get-ChildItem $binRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object {
        (Test-Path (Join-Path $_.FullName "x86\inf2cat.exe")) -or
        (Test-Path (Join-Path $_.FullName "x64\inf2cat.exe"))
    } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $verDir) { Fail "未找到 inf2cat.exe(在 Windows Kits\10\bin\<ver>\{x86,x64} 中递归查找)" }

# inf2cat:x86 优先(最常见),退而求其次 x64
$inf2cat = Join-Path $verDir.FullName "x86\inf2cat.exe"
if (-not (Test-Path $inf2cat)) { $inf2cat = Join-Path $verDir.FullName "x64\inf2cat.exe" }

# signtool:x64 优先,退回 x86
$signtool = Join-Path $verDir.FullName "x64\signtool.exe"
if (-not (Test-Path $signtool)) { $signtool = Join-Path $verDir.FullName "x86\signtool.exe" }
if (-not (Test-Path $signtool)) { Fail "未找到 signtool.exe" }
Write-Host "[信息] inf2cat : $inf2cat"
Write-Host "[信息] signtool: $signtool"

$buildLog = Join-Path $PSScriptRoot "pnp_build_log.txt"

# ---- 3. 编译 PnP 驱动(.sys) ----
# 架构宏用 _AMD64_(ntdef.h 据此判断架构);同时保留旧宏兼容
$clLine = '/nologo /c /GF /Gw /GS- /W4 /wd4100 /wd4189 /wd4127 ' +
          '/D_AMD64_=1 /DAMD64=1 /D_WIN64=100 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 ' +
          '/I"' + $INC + '\km" /I"' + $INC + '\shared" ' +
          '/FoCommLinkPnp.obj CommLinkPnp.c'
$linkLine = '/nologo /DRIVER /MACHINE:X64 /SUBSYSTEM:NATIVE /ENTRY:DriverEntry ' +
            '/LIBPATH:"' + $LIB + '\km\x64" ' +
            '/OUT:CommLinkPnp.sys CommLinkPnp.obj ntoskrnl.lib'
$fullSys = 'call "' + $vcvarsall + '" x64 && cl ' + $clLine + ' && link ' + $linkLine

Write-Host "[信息] 编译 PnP 驱动 ..."
$rawSys = cmd /c $fullSys 2>&1 | Out-String
Write-Host $rawSys
Add-Content -Path $buildLog -Value ("=== SYS BUILD ===`n" + $rawSys) -Encoding UTF8
if ($LASTEXITCODE -ne 0) {
    foreach ($line in ($rawSys -split "`r?`n")) {
        if ($line -match '\berror\b|\bfatal\b|LNK[0-9]+|C[0-9]{4}') {
            $safe = $line -replace '%', '%25' -replace "`r", '' -replace "`n", ' '
            Write-Host "::error::$safe"
        }
    }
    Fail "PnP 驱动编译失败(退出码 $LASTEXITCODE)"
}
if (-not (Test-Path (Join-Path $PSScriptRoot "CommLinkPnp.sys"))) { Fail "编译结束但未生成 CommLinkPnp.sys" }
Write-Host "[成功] 已生成 CommLinkPnp.sys" -ForegroundColor Green

# ---- 4. 编译 mkroot.exe(用户态工具) ----
$mkCl = 'call "' + $vcvarsall + '" x64 && cl /nologo /DUNICODE /D_UNICODE /Fo"mkroot.obj" "mkroot.c" && link /nologo /OUT:"mkroot.exe" "mkroot.obj" setupapi.lib newdev.lib'
Write-Host "[信息] 编译 mkroot.exe ..."
$rawMk = cmd /c $mkCl 2>&1 | Out-String
Write-Host $rawMk
Add-Content -Path $buildLog -Value ("=== MKROOT BUILD ===`n" + $rawMk) -Encoding UTF8
if ($LASTEXITCODE -ne 0) {
    foreach ($line in ($rawMk -split "`r?`n")) {
        if ($line -match '\berror\b|\bfatal\b|LNK[0-9]+|C[0-9]{4}') {
            $safe = $line -replace '%', '%25' -replace "`r", '' -replace "`n", ' '
            Write-Host "::error::$safe"
        }
    }
    Fail "mkroot.exe 编译失败(退出码 $LASTEXITCODE)"
}
if (-not (Test-Path (Join-Path $PSScriptRoot "mkroot.exe"))) { Fail "编译结束但未生成 mkroot.exe" }
Write-Host "[成功] 已生成 mkroot.exe" -ForegroundColor Green

# ---- 5. 生成自签测试证书 ----
Write-Host "[信息] 生成自签测试证书 ..."
$certPass = "CommLinkTest2026"
$cert = New-SelfSignedCertificate -Type CodeSigningCert `
    -Subject "CN=CommLink Test Signing" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -KeyUsage DigitalSignature -ErrorAction Stop
$pfxPath = Join-Path $PSScriptRoot "CommLinkTest.pfx"
$cerPath = Join-Path $PSScriptRoot "CommLinkTest.cer"
$securePass = ConvertTo-SecureString -String $certPass -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $securePass -Force | Out-Null
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
Write-Host "[成功] 已生成 CommLinkTest.cer / .pfx"

# ---- 6. inf2cat 生成 .cat ----
Write-Host "[信息] 生成目录文件 .cat ..."
$rawCat = & $inf2cat /driver:$PSScriptRoot /os:10_X64 /verbose 2>&1 | Out-String
Write-Host $rawCat
Add-Content -Path $buildLog -Value ("=== INF2CAT ===`n" + $rawCat) -Encoding UTF8
if (-not (Test-Path (Join-Path $PSScriptRoot "CommLinkPnp.cat"))) { Fail "inf2cat 未生成 CommLinkPnp.cat,请检查 INF 与 buildLog" }
Write-Host "[成功] 已生成 CommLinkPnp.cat" -ForegroundColor Green

# ---- 7. 签名 .sys 与 .cat ----
Write-Host "[信息] 签名驱动与目录文件 ..."
& $signtool sign /fd SHA256 /f $pfxPath /p $certPass (Join-Path $PSScriptRoot "CommLinkPnp.sys") 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { Fail "签名 CommLinkPnp.sys 失败" }
& $signtool sign /fd SHA256 /f $pfxPath /p $certPass (Join-Path $PSScriptRoot "CommLinkPnp.cat") 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { Fail "签名 CommLinkPnp.cat 失败" }
Write-Host "[成功] 签名完成" -ForegroundColor Green

# ---- 8. 打包 ----
$zipPath = Join-Path $PSScriptRoot "CommLink-PnP-all.zip"
$filesToZip = @(
    "CommLinkPnp.sys", "CommLinkPnp.inf", "CommLinkPnp.cat",
    "CommLinkTest.cer", "mkroot.exe"
)
$stageDir = Join-Path $PSScriptRoot "_pnp_stage"
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir | Out-Null
foreach ($f in $filesToZip) { Copy-Item (Join-Path $PSScriptRoot $f) $stageDir -Force }
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $zipPath -Force
Remove-Item $stageDir -Recurse -Force
Write-Host "[成功] 已打包 $zipPath" -ForegroundColor Green
Get-Item $zipPath | Select-Object Name, Length

Pop-Location
