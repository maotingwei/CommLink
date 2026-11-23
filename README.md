# CommLink — Windows 虚拟串口桥接软件

在 Windows 下**自研内核驱动**创建一个虚拟 COM 口，串口助手打开该串口收发数据时，由本软件捕获/注入，
并通过网页 UI 显示 TX/RX、过滤、搜索、记录日志。板子通信部分已预留接口，后续按你提供的协议接入即可。

---

## 0. 如何编译驱动（三种方案，任选其一）

本机当前**未安装 WDK**，无法直接编译 `.sys`。以下三种方案都可拿到 `CommLink.sys`：

### 方案 A：GitHub Actions 在线编译（推荐，本机零安装）
微软官方 Windows 驱动示例就是用这种方式编译的。`windows-2022` 云端 runner **开箱预装 WDK 22H2 + VS2022**。

1. 在 GitHub 新建一个空仓库（public/private 均可）。
2. 把整个 `CommLink` 目录推送上去（仓库已自带 `.github/workflows/build.yml`）：
   ```bat
   cd /d <项目目录>
   git init
   git add .
   git commit -m "init"
   git remote add origin https://github.com/<你的用户名>/<仓库名>.git
   git push -u origin main
   ```
3. 打开仓库的 **Actions** 页 → 等待 `Build CommLink Driver` 运行完成（约 1–3 分钟）。
4. 在本次运行页面底部 **Artifacts** 下载 `CommLink-sys-x64`，解压得到 `CommLink.sys`。
   （同时提供 `CommLink-server`，含后端和网页。）

> 没有 GitHub 账号也可以：用 Gitee 的 Go 流水线 / 腾讯云 CODING 等，提供一个 Windows 构建节点跑 `driver\build_driver.ps1` 即可。

### 方案 B：本机 winget 一键装 WDK 后本地编译
本机已具备 VS2019 BuildTools + SDK 10.0.19041，且 winget 源里有**完全匹配**的 WDK 版本：

```bat
REM 管理员权限运行
tools\install_wdk.bat
REM 安装后重新打开终端,自适应编译脚本会自动探测 VS 与 WDK 路径
cd /d <项目目录>\driver
powershell -ExecutionPolicy Bypass -File build_driver.ps1
```

### 方案 C：手工安装 WDK（离线/无 winget 时）
见下方第 3 节。装好后同样用 `build_driver.ps1` 编译（或直接 `build_driver.bat`，它写死了 19041 路径）。

> 编译脚本说明：`build_driver.ps1` 会自动探测最新 VS + WDK 版本目录，本地和 CI 通用，推荐优先用；
> `build_driver.bat` 是早期写死 10.0.19041.0 路径的版本，仅在路径完全匹配时可用。

---

## 1. 当前完成范围

| 模块 | 状态 | 说明 |
|---|---|---|
| 内核虚拟串口驱动 | ✅ 源码完成 | Legacy WDM，创建 COM 口 + 控制设备，双向环形缓冲 |
| 驱动编译/安装脚本 | ✅ 完成 | 检测 WDK、编译、sc 安装/卸载 |
| Python 后端服务 | ✅ 完成 | ctypes 直连驱动、WebSocket 推送、日志、过滤、搜索、TCP 预留 |
| 网页 UI | ✅ 完成 | 配置、实时 TX/RX、过滤规则、搜索、手动注入 RX |
| **驱动 `.sys` 二进制** | ⚠️ 需你在有 WDK 的机器编译 | 本机未装 WDK，无法编译内核驱动（见第 3 节） |
| 板子端物理串口协议 | ⏳ 待你提供 | 已预留 `BoardTransport` 接口 |

---

## 2. 架构与数据流

```
 串口助手 / 任意程序                          浏览器(任意电脑)
      │  CreateFile(\\.\COM8)                      │ HTTP / WebSocket
      ▼                                            ▼
 ┌──────────────────────────┐            ┌──────────────────────────┐
 │  虚拟 COM 口 (COM8)       │            │  Python 后端 app.py       │
 │  IRP_MJ_READ / WRITE      │◀────IOCTL──▶│  DriverClient(ctypes)     │
 │  串口 IOCTL_SERIAL_*      │  \\.\CommLinkCtl │  Filter / Logger / WS   │
 └────────────┬─────────────┘            │  BoardTransport(预留)     │
              │  共享环形缓冲              └────────────┬─────────────┘
              │  TxRing / RxRing                        │ TCP(预留)
              ▼                                        ▼
        驱动 CommLink.sys                        板子(将来:物理串口转发)
```

**数据方向约定（以本软件为中心）：**

- **TX** = 串口助手写入 COM8 → 驱动 TxRing → 本服务读取 → 记录/显示 →（将来）发往板子
- **RX** = 板子/网页注入 → 本服务写入驱动 RxRing → 串口助手 ReadFile 读到

**TX/RX 缓冲是分开的两个环形缓冲：**
- `TxRing`：串口助手 → 驱动，驱动收到数据会 `SetEvent` 通知本服务来取
- `RxRing`：本服务 → 驱动，写入后会完成一个挂起的串口助手读请求

---

## 3. 环境现状与 WDK 安装（重要）

### 3.1 本机检测结果

| 项目 | 状态 |
|---|---|
| 操作系统 | Windows（SDK 10.0.19041.0） |
| Visual Studio 2019 BuildTools (MSVC 14.29.30133) | ✅ 已装，路径 `D:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools` |
| Windows SDK 10.0.19041.0 | ✅ 已装 |
| **WDK（Windows Driver Kit）** | ❌ **未装** — 缺 `Include\10.0.19041.0\km\wdm.h` 和 `Lib\...\km` |

> 内核驱动**必须**用 WDK 编译。没有 WDK 时 `build_driver.bat` 会打印明确错误并退出，不会误编译。
> Python 后端与网页 UI **不依赖 WDK**，可直接在当前机器运行（只是驱动没加载时显示"未连接"）。

### 3.2 安装 WDK

推荐用与 SDK 一致的 **WDK 10.0.19041.0**（Windows 10 2004 / 20H1 那一版）：

1. 下载 "Windows Driver Kit (WDK) 10" ISO，从微软官网检索 `WDK 10.0.19041.0` 或 "Windows 10 Driver Kit"。
2. 挂载 ISO，运行 `KitSetup.exe`，至少勾选 **"Windows Driver Kit"**。
3. 安装后确认存在：
   - `C:\Program Files (x86)\Windows Kits\10\Include\10.0.19041.0\km\wdm.h`
   - `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0\km\x64\ntoskrnl.lib`

装好后，如果你的 VS/WDK 路径与脚本默认不同，编辑 `driver/build_driver.bat` 顶部变量。

---

## 4. 编译驱动

以管理员打开 CMD：

```bat
cd /d <项目目录>\driver
build_driver.bat
```

成功则生成 `driver\CommLink.sys`。

> 本驱动为纯 WDM（未用 KMDF），源码主要文件：
> - `CommLinkCommon.h`：IOCTL 协议（驱动与 Python 共享）
> - `CommLink.c`：驱动主体（设备创建、IRP 分发、环形缓冲、IOCTL 处理）

---

## 5. 驱动签名与测试模式（64 位 Windows 必须）

64 位 Windows 默认只允许加载有有效数字签名的驱动。开发阶段用**测试签名**：

### 5.1 开启测试签名模式（管理员 CMD，然后重启）

```bat
bcdedit /set testsigning on
```

重启后桌面右下角会显示"测试模式"水印。

### 5.2 用自签证书签名 .sys

```bat
REM 生成自签证书
makecert -r -pe -ss Private\My -n "CN=CommLinkTest" CommLinkTest.cer
makecert -sk CommLinkTest -iv CommLinkTest.pvk -n "CN=CommLinkTest" -ss Root -sr localMachine -r CommLinkTest.cer

REM 签名驱动（signtool 在 SDK bin 目录）
signtool sign /s Private\My /n CommLinkTest /t http://timestamp.digicert.com driver\CommLink.sys
```

> 若 `makecert` 不可用，可用 PowerShell `New-SelfSignedCertificate` 生成证书后导入"受信任的根证书颁发机构"和"个人"存储，再用 signtool 签名。

签名后把 `CommLink.sys` 放到 `driver\` 目录（安装脚本从此处读取）。

---

## 6. 安装 / 启动驱动

以管理员 CMD：

```bat
cd /d <项目目录>\driver

REM 安装并启动，虚拟串口设为 COM8（可改）
install_driver.bat install 8

REM 查看状态
install_driver.bat status

REM 停止 / 卸载
install_driver.bat stop
install_driver.bat uninstall
```

安装成功后：
- 虚拟串口：`\\.\COM8`（串口助手里选 COM8）
- 控制设备：`\\.\CommLinkCtl`（本服务使用）
- COM 口号也可通过注册表改：`HKLM\SYSTEM\CurrentControlSet\Services\CommLink\Parameters\ComPort`（DWORD）

> 虚拟串口**不会**出现在设备管理器"端口(COM 和 LPT)"里——本驱动是 Legacy WDM，通过符号链接提供 `\\.\COMn`，
> 普通串口助手（SSCOM、友善串口、PuTTY 等）用 `CreateFile(\\.\COM8)` 打开即可正常收发。
> 若你需要它出现在设备管理器，可后续改为 WDF + INF 方式（本版本未采用）。

---

## 7. 运行软件

```bat
<项目目录>\start_server.bat
```

脚本会启动后端并打开浏览器 `http://127.0.0.1:8765/`。
要从**其他电脑**访问，浏览器打开 `http://<这台电脑的IP>:8765/`（服务监听 `0.0.0.0`）。

停止：关闭 "CommLink Server" 控制台窗口。

---

## 8. 使用说明

1. 用串口助手打开 COM8（波特率等任意设置均可，数据为透传）。
2. 串口助手发送数据 → 网页 "数据监视" 实时显示 **TX**（红色），并写入 `server\logs\commlink.log`。
3. 网页"手动注入 RX"可模拟板子向串口助手发送数据（串口助手会收到）。
4. "过滤规则"里添加字符（每行一条），命中的 TX/RX **不显示、不写日志、不转发板子**。
5. 搜索框支持对 HEX / ASCII / 方向 / 时间进行关键字搜索并高亮。
6. 日志路径：`server\logs\commlink.log`。

---

## 9. HTTP API（供调试/二次开发）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/status` | 驱动连接状态、统计 |
| GET | `/api/history` | 当前内存历史（未过滤） |
| GET | `/api/config` | 当前配置 |
| POST | `/api/inject_rx` | `{"data":"...","hex":false}` 注入 RX |
| POST | `/api/filter` | `{"filter":{...}}` 更新过滤规则 |
| POST | `/api/config` | 更新 board/log/filter/display 配置 |
| POST | `/api/clear` | 清空显示历史 |
| POST | `/api/purge` | 清空驱动缓冲 |
| WS | `/ws` | 实时推送 `{"type":"data","data":{...}}` |

---

## 10. 后续：接入板子物理串口

`server/app.py` 中的 `BoardTransport` 类已预留：

- `on_board_data(data)`：板子收到物理串口数据 → 作为 RX 注入虚拟串口（已接通）
- `send(data)`：TX 数据发往板子（目前 TCP 客户端实现）

你提供"软件与板子端如何通信"的协议后（例如 TCP 上跑的自定义帧格式、或板子端 USB 转串口的协议），
只需在 `BoardTransport` 里实现对应的收发与封包/解包，应用层其余部分（过滤、日志、UI）无需改动。
