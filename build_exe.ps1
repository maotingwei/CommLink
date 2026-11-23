<#
  build_exe.ps1 - 把 CommLink 打成单文件 exe(供 GitHub Actions 调用,本地也能用)
  产物: dist/CommLink.exe  (内嵌: Python 后台 + 网页 UI + 驱动 sys/inf/cat/cer/mkroot.exe)
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$log = Join-Path $root "exe_build_log.txt"
function Log($m) { $line = "[$(Get-Date -Format 'HH:mm:ss')] $m"; Write-Host $line; Add-Content -Path $log -Value $line }
function Fail($m) { Log "[错误] $m"; Add-Content -Path $log -Value "::error::$m"; exit 1 }
Set-Content -Path $log -Value "CommLink exe 打包日志" -Encoding UTF8

# 1) 找 Python(CI 用 setup-python 装在 PATH;本地回退到常见路径)
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { Fail "未找到 python,请确认已加入 PATH" }
$PY = $py.Source
Log "Python: $PY (& $PY --version)"
& $PY --version 2>&1 | ForEach-Object { Log $_ }

# 2) 安装 PyInstaller
Log "安装/升级 PyInstaller ..."
& $PY -m pip install --disable-pip-version-check --upgrade pyinstaller 2>&1 | ForEach-Object { Log $_ }
if ($LASTEXITCODE -ne 0) { Fail "pip 安装 PyInstaller 失败" }

# 3) 检查驱动产物是否齐全(缺一个就编译产物不完整)
$need = @("CommLinkPnp.sys","CommLinkPnp.inf","CommLinkPnp.cat","CommLinkTest.cer","mkroot.exe")
foreach ($f in $need) {
    if (-not (Test-Path (Join-Path $root "driver\$f"))) { Fail "缺少驱动产物 driver\$f,请先运行 build_pnp.ps1" }
}
Log "驱动产物齐全。"

# 4) 打包
Log "开始打包(单文件 exe, UAC 提权, 无黑框) ..."
& $PY -m PyInstaller --noconfirm --clean CommLink.spec 2>&1 | ForEach-Object {
    Log $_
    if ($_ -match '\berror\b|Traceback|failed') { Add-Content -Path $log -Value "::error::$_" }
}
if ($LASTEXITCODE -ne 0) { Fail "PyInstaller 打包失败(见 exe_build_log.txt)" }

$exe = Join-Path $root "dist\CommLink.exe"
if (-not (Test-Path $exe)) { Fail "打包结束但未生成 dist\CommLink.exe" }
$size = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Log "[成功] 已生成 dist\CommLink.exe ($size MB)"
Get-Item $exe | Select-Object Name, Length, LastWriteTime | Format-List | Out-String | ForEach-Object { Log $_ }
