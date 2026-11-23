# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec:把 Python 后台 + 网页 UI + 驱动产物打成单文件 exe
# 用法: pyinstaller --noconfirm --clean CommLink.spec
import os

# SPECPATH 由 PyInstaller 注入,指向本 spec 所在目录(项目根)
ROOT = os.path.abspath(SPECPATH)
SERVER = os.path.join(ROOT, "server")
DRIVER = os.path.join(ROOT, "driver")

# ---- 内嵌资源 ----
datas = []
# 网页 UI(public 整个目录 -> 运行时 sys._MEIPASS/public)
pub = os.path.join(SERVER, "public")
if os.path.isdir(pub):
    datas.append((pub, "public"))

# 驱动产物(-> 运行时 sys._MEIPASS/driver)
for fn in ("CommLinkPnp.sys", "CommLinkPnp.inf", "CommLinkPnp.cat",
           "CommLinkTest.cer", "mkroot.exe"):
    p = os.path.join(DRIVER, fn)
    if os.path.isfile(p):
        datas.append((p, "driver"))
    else:
        print(f"[WARN] 驱动文件缺失,exe 内将不含 {fn}: {p}")

a = Analysis(
    [os.path.join(SERVER, "app.py")],
    pathex=[SERVER],
    binaries=[],
    datas=datas,
    hiddenimports=["asyncio", "http.server", "ctypes", "ctypes.wintypes", "webbrowser"],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["tkinter", "matplotlib", "numpy", "PIL", "pandas", "scipy"],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="CommLink",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,          # 无黑框窗口
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch="x86_64",
    codesign_identity=None,
    entitlements_file=None,
    manifest=os.path.join(ROOT, "CommLink.manifest"),  # UAC requireAdministrator 提权
    icon=None,
)
