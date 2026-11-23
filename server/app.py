#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
CommLink Server
=====================================================================
虚拟串口桥接服务(纯 Python 标准库,零第三方依赖)。

职责:
  1. 通过 ctypes 打开 \\.\CommLinkCtl 控制设备,与内核驱动交换数据:
       - 轮询/事件等待 TX(串口助手发来的数据) -> 记录 LOG / 推送前端 / 转发板子
       - 接收前端或板子发来的 RX 数据 -> 写入驱动 -> 串口助手读到
  2. HTTP + WebSocket 服务:
       - 静态网页 UI
       - /ws 实时推送 TX/RX 事件
       - /api/* 配置与查询接口
  3. 过滤:命中"字符匹配"规则的数据不写入 LOG、不进入历史、不推送前端。
  4. 板子通信(TCP 客户端):本版本提供完整可配置框架,默认禁用;
     后续接入物理串口协议时只需实现 BoardTransport。

数据方向(以本软件/虚拟串口为中心):
      TX = 串口助手 -> 虚拟串口 -> 本服务 -> (将来)板子
      RX = (将来)板子 -> 本服务 -> 虚拟串口 -> 串口助手
=====================================================================
"""
import os
import sys
import json
import time
import ctypes
from ctypes import wintypes
import threading
import asyncio
import struct
import hashlib
import base64
import re
import subprocess
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from datetime import datetime

# ---------------------------------------------------------------------------
# 运行路径适配:
#   - 源码运行: RES_DIR=BASE_DIR=server/,驱动产物在 ../driver/
#   - PyInstaller 打包: RES_DIR=sys._MEIPASS(内嵌的 public/driver),
#     BASE_DIR=exe 所在目录(config.json/logs 写这里,可写)
# ---------------------------------------------------------------------------
FROZEN = getattr(sys, "frozen", False)
RES_DIR = getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.abspath(__file__))
BASE_DIR = os.path.dirname(sys.executable) if FROZEN else os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(BASE_DIR, "config.json")
LOG_DIR = os.path.join(BASE_DIR, "logs")
PUBLIC_DIR = os.path.join(RES_DIR, "public")
DRIVER_DIR = os.path.join(RES_DIR, "driver")
if not os.path.isdir(DRIVER_DIR):  # 源码模式回退到项目根 driver 目录
    DRIVER_DIR = os.path.abspath(os.path.join(BASE_DIR, "..", "driver"))
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# ---------------------------------------------------------------------------
# Windows IOCTL 定义(与 driver/CommLinkCommon.h 保持一致)
# CTL_CODE = (DeviceType<<16) | (Access<<14) | (Function<<2) | Method
# ---------------------------------------------------------------------------
FILE_DEVICE_COMMLINK = 0x8A00
METHOD_BUFFERED = 0
FILE_ANY_ACCESS = 0
BASE = 0x800

def ctl_code(func):
    return (FILE_DEVICE_COMMLINK << 16) | (FILE_ANY_ACCESS << 14) | ((BASE + func) << 2) | METHOD_BUFFERED

IOCTL_GET_TX      = ctl_code(0)
IOCTL_PUT_RX      = ctl_code(1)
IOCTL_SET_EVENT   = ctl_code(2)
IOCTL_GET_STATUS  = ctl_code(3)
IOCTL_PURGE       = ctl_code(4)
IOCTL_GET_VERSION = ctl_code(5)

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
EVENT_MODIFY_STATE = 0x0002
SYNCHRONIZE = 0x00100000
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 0x102

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                 wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                     wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                     wintypes.LPVOID]
kernel32.DeviceIoControl.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CreateEventW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
kernel32.CreateEventW.restype = wintypes.HANDLE
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.ResetEvent.argtypes = [wintypes.HANDLE]
kernel32.ResetEvent.restype = wintypes.BOOL


def now_str():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def hexdump(data: bytes, max_len=4096):
    d = data[:max_len]
    hx = " ".join(f"{b:02X}" for b in d)
    asc = "".join(chr(b) if 32 <= b < 127 else "." for b in d)
    suffix = " ..." if len(data) > max_len else ""
    return hx + suffix, asc + suffix


# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------
DEFAULT_CONFIG = {
    "server": {"host": "0.0.0.0", "port": 8765, "open_browser": True},
    "driver": {"control_device": r"\\.\CommLinkCtl", "poll_interval_ms": 20, "read_chunk": 65536},
    "board": {
        "enabled": False,
        "type": "tcp_client",          # tcp_client / (未来) serial / udp
        "host": "192.168.1.100",
        "port": 5000,
        "auto_reconnect": True,
        "reconnect_interval_ms": 3000
    },
    "log": {"enabled": True, "dir": "logs", "file": "commlink.log", "max_bytes": 10485760},
    "filter": {
        "enabled": True,
        "mode": "contains",            # contains / regex
        "patterns": [],                # 命中即丢弃(不存储、不显示)
        "apply_to": ["TX", "RX"]       # 对哪些方向生效
    },
    "display": {"max_history": 500, "hex_max_len": 4096}
}


def load_config():
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))  # deep copy
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                user = json.load(f)
            def merge(a, b):
                for k, v in b.items():
                    if k in a and isinstance(a[k], dict) and isinstance(v, dict):
                        merge(a[k], v)
                    else:
                        a[k] = v
            merge(cfg, user)
        except Exception as e:
            print(f"[WARN] 读取 config.json 失败: {e},使用默认配置")
    else:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    return cfg


# ---------------------------------------------------------------------------
# 过滤
# ---------------------------------------------------------------------------
class FilterEngine:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lock = threading.Lock()
        self.reload()

    def reload(self, cfg=None):
        if cfg:
            self.cfg = cfg
        with self.lock:
            self.enabled = self.cfg["filter"].get("enabled", False)
            self.mode = self.cfg["filter"].get("mode", "contains")
            self.patterns = list(self.cfg["filter"].get("patterns", []))
            self.apply_to = set(self.cfg["filter"].get("apply_to", ["TX", "RX"]))
            if self.mode == "regex":
                import re
                self._regex = []
                for p in self.patterns:
                    try:
                        self._regex.append(re.compile(p, re.IGNORECASE))
                    except re.error:
                        pass
            else:
                self._regex = []

    def is_filtered(self, direction: str, data: bytes) -> bool:
        if not self.enabled or direction not in self.apply_to or not self.patterns:
            return False
        # 同时用 ASCII 文本与 HEX 大写串匹配,方便按字符或按字节匹配
        text = data.decode("utf-8", errors="ignore")
        hexs = data.hex().upper()
        with self.lock:
            if self.mode == "regex":
                for r in self._regex:
                    if r.search(text) or r.search(hexs):
                        return True
            else:
                for p in self.patterns:
                    if not p:
                        continue
                    if p.lower() in text.lower() or p.upper() in hexs:
                        return True
        return False


# ---------------------------------------------------------------------------
# 日志
# ---------------------------------------------------------------------------
class Logger:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lock = threading.Lock()
        self.path = ""
        self._setup()

    def _setup(self):
        lc = self.cfg["log"]
        d = lc.get("dir", "logs")
        d = d if os.path.isabs(d) else os.path.join(BASE_DIR, d)
        os.makedirs(d, exist_ok=True)
        self.path = os.path.join(d, lc.get("file", "commlink.log"))

    def write(self, direction: str, data: bytes, extra: str = ""):
        if not self.cfg["log"].get("enabled", True):
            return
        hx, asc = hexdump(data, self.cfg["display"]["hex_max_len"])
        line = f"[{now_str()}] {direction:3s} len={len(data):5d} HEX: {hx} | ASC: {asc}"
        if extra:
            line += f" | {extra}"
        with self.lock:
            try:
                with open(self.path, "a", encoding="utf-8") as f:
                    f.write(line + "\n")
            except Exception as e:
                print(f"[WARN] 写日志失败: {e}")


# ---------------------------------------------------------------------------
# 驱动封装
# ---------------------------------------------------------------------------
class DriverError(Exception):
    pass


class DriverClient:
    def __init__(self, cfg):
        self.cfg = cfg
        self.handle = None
        self.event = None
        self.status_struct_size = 28  # 7 * ULONG

    @staticmethod
    def _err():
        return ctypes.get_last_error()

    def open(self):
        dev = self.cfg["driver"]["control_device"]
        h = kernel32.CreateFileW(dev, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
        if h == INVALID_HANDLE_VALUE or not h:
            raise DriverError(f"打开 {dev} 失败,Win32 错误={self._err()} (驱动是否已加载?)")
        self.handle = h
        self.event = kernel32.CreateEventW(None, True, False, None)  # manual reset
        if not self.event:
            raise DriverError(f"创建事件失败,Win32 错误={self._err()}")
        self._ioctl(IOCTL_SET_EVENT, ctypes.byref(ctypes.c_void_p(self.event)),
                    ctypes.sizeof(ctypes.c_void_p), None, 0)
        ver = self.get_version()
        print(f"[驱动] 已连接 {dev},版本 {ver[0]}.{ver[1]}.{ver[2]}")
        return True

    def close(self):
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None
        if self.event:
            kernel32.CloseHandle(self.event)
            self.event = None

    def _ioctl(self, code, in_buf, in_len, out_buf, out_len):
        ret = wintypes.DWORD(0)
        ok = kernel32.DeviceIoControl(self.handle, code, in_buf, in_len, out_buf, out_len,
                                      ctypes.byref(ret), None)
        if not ok:
            err = self._err()
            raise DriverError(f"DeviceIoControl(0x{code:08X}) 失败,错误={err}")
        return ret.value

    def read_tx(self, max_len):
        buf = (ctypes.c_ubyte * max_len)()
        n = self._ioctl(IOCTL_GET_TX, None, 0, buf, max_len)
        if n == 0:
            return b""
        return bytes(buf[:n])

    def write_rx(self, data: bytes):
        if not data:
            return
        buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        self._ioctl(IOCTL_PUT_RX, buf, len(data), None, 0)

    def get_status(self):
        buf = (ctypes.c_ubyte * self.status_struct_size)()
        self._ioctl(IOCTL_GET_STATUS, None, 0, buf, self.status_struct_size)
        vals = struct.unpack("<7I", bytes(buf))
        return {
            "tx_length": vals[0], "tx_capacity": vals[1],
            "rx_length": vals[2], "rx_capacity": vals[3],
            "pending_reads": vals[4], "total_tx": vals[5], "total_rx": vals[6],
        }

    def get_version(self):
        buf = (ctypes.c_ubyte * 12)()
        self._ioctl(IOCTL_GET_VERSION, None, 0, buf, 12)
        return struct.unpack("<3I", bytes(buf))

    def purge(self):
        self._ioctl(IOCTL_PURGE, None, 0, None, 0)

    def wait_event(self, ms):
        r = kernel32.WaitForSingleObject(self.event, ms)
        if r == WAIT_OBJECT_0:
            kernel32.ResetEvent(self.event)
            return True
        return False


# ---------------------------------------------------------------------------
# 驱动安装管理:把 install_pnp.bat 的操作封装成可编程调用
#   (exe 已通过 UAC manifest 提权,进程本身就是管理员)
# ---------------------------------------------------------------------------
class DriverManager:
    SVC = "CommLinkPnp"

    def __init__(self, driver_dir):
        self.dir = driver_dir

    def files(self):
        names = ["CommLinkPnp.sys", "CommLinkPnp.inf", "CommLinkPnp.cat",
                 "CommLinkTest.cer", "mkroot.exe"]
        return {n: os.path.join(self.dir, n) for n in names}

    def missing_files(self):
        return [n for n, p in self.files().items() if not os.path.isfile(p)]

    @staticmethod
    def is_admin():
        try:
            return ctypes.windll.shell32.IsUserAnAdmin() != 0
        except Exception:
            return False

    def _run(self, args, timeout=60):
        try:
            p = subprocess.run(args, capture_output=True, text=True,
                               creationflags=CREATE_NO_WINDOW, timeout=timeout)
            return p.returncode, (p.stdout or "") + (p.stderr or "")
        except FileNotFoundError:
            return -1, f"命令不存在: {args[0]}"
        except Exception as e:
            return -1, str(e)

    def testsigning_on(self):
        rc, out = self._run(["bcdedit", "/enum", "{current}"])
        for line in out.splitlines():
            if "testsigning" in line.lower():
                return "yes" in line.lower()
        return False

    def set_testsigning(self, on):
        if not self.is_admin():
            return False, False, "没有管理员权限"
        rc, out = self._run(["bcdedit", "/set", "testsigning", "on" if on else "off"])
        ok = rc == 0
        return ok, (ok and on), (out.strip() or ("ok" if ok else "failed"))

    def service_running(self):
        rc, out = self._run(["sc", "query", self.SVC])
        return "RUNNING" in out.upper()

    def device_present(self):
        rc, out = self._run(["pnputil", "/enum-devices", "/class", "Ports"])
        return "CommLink" in out

    def get_com_port(self):
        rc, out = self._run(["reg", "query",
                             rf"HKLM\SYSTEM\CurrentControlSet\Services\{self.SVC}\Parameters",
                             "/v", "ComPort"])
        m = re.search(r"ComPort\s+REG_DWORD\s+0x([0-9a-fA-F]+)", out)
        return int(m.group(1), 16) if m else None

    def status(self):
        return {
            "admin": self.is_admin(),
            "testsigning": self.testsigning_on(),
            "service_running": self.service_running(),
            "device_present": self.device_present(),
            "com_port": self.get_com_port(),
            "missing_files": self.missing_files(),
        }

    def set_com(self, com):
        if not (1 <= com <= 255):
            return False, "COM 号须在 1-255"
        if not self.is_admin():
            return False, "没有管理员权限"
        rc, out = self._run(["reg", "add",
                             rf"HKLM\SYSTEM\CurrentControlSet\Services\{self.SVC}\Parameters",
                             "/v", "ComPort", "/t", "REG_DWORD", "/d", str(com), "/f"])
        return rc == 0, (out.strip() or ("ok" if rc == 0 else "failed"))

    def install(self, com):
        """返回 (ok, need_reboot, steps[])。"""
        steps = []
        def add(ok, msg):
            steps.append({"ok": bool(ok), "msg": msg})
        if not self.is_admin():
            add(False, "没有管理员权限(请以管理员身份运行 CommLink.exe)")
            return False, False, steps
        missing = self.missing_files()
        if missing:
            add(False, "缺少内嵌文件: " + ", ".join(missing))
            return False, False, steps
        if not (1 <= com <= 255):
            add(False, "COM 号须在 1-255")
            return False, False, steps

        f = self.files()
        if not self.testsigning_on():
            add(self.set_testsigning(True)[0], "开启测试签名模式 (bcdedit)")
            add(True, "⚠ 需要重启电脑;重启后再次点'一键生成串口'即可完成安装")
            return False, True, steps
        add(True, "测试签名模式已开启")

        for store in ("ROOT", "TrustedPublisher"):
            rc, out = self._run(["certutil", "-addstore", "-f", store, f["CommLinkTest.cer"]])
            add(rc == 0, f"导入证书到 {store}" + ("" if rc == 0 else f" : {out[-120:]}"))

        self.set_com(com)
        add(True, f"写入串口号 COM{com}")

        rc, out = self._run([f["mkroot.exe"], "install", f["CommLinkPnp.inf"]], timeout=120)
        add(rc == 0, "安装驱动并创建设备节点" + ("" if rc == 0 else f" : {out[-200:]}"))
        if rc == 0 and out.strip():
            add(True, out.strip()[-200:])

        self.set_com(com)
        add(True, "完成。请在设备管理器 端口(COM和LPT) 查看")
        return rc == 0, False, steps

    def uninstall(self):
        if not self.is_admin():
            return False, [{"ok": False, "msg": "没有管理员权限"}]
        f = self.files()
        if not os.path.isfile(f["mkroot.exe"]):
            return False, [{"ok": False, "msg": "未找到 mkroot.exe"}]
        rc, out = self._run([f["mkroot.exe"], "uninstall"], timeout=60)
        return rc == 0, [{"ok": rc == 0, "msg": (out.strip()[-300:] or
                          ("已移除设备节点" if rc == 0 else "失败"))}]


# ---------------------------------------------------------------------------
# 板子通信接口(预留):后续实现物理串口协议时只需替换此类
# ---------------------------------------------------------------------------
class BoardTransport:
    """板子传输抽象。当前为 TCP 客户端框架,默认禁用。
    后续提供物理串口交互方式时,在此实现 send/recv 并接到 on_board_data。"""
    def __init__(self, cfg, on_data):
        self.cfg = cfg
        self.on_data = on_data
        self.enabled = cfg["board"].get("enabled", False)
        self.sock = None
        self.thread = None
        self.running = False

    def start(self):
        if not self.enabled:
            print("[板子] 未启用(配置 board.enabled=false)")
            return
        self.running = True
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass

    def _loop(self):
        import socket
        bc = self.cfg["board"]
        host, port = bc["host"], int(bc["port"])
        while self.running:
            try:
                s = socket.create_connection((host, port), timeout=3)
                self.sock = s
                print(f"[板子] 已连接 TCP {host}:{port}")
                s.settimeout(0.5)
                while self.running:
                    try:
                        d = s.recv(4096)
                        if not d:
                            break
                        # 板子收到数据 -> 作为 RX 注入虚拟串口
                        self.on_data(d)
                    except socket.timeout:
                        continue
            except Exception as e:
                print(f"[板子] 连接失败/断开: {e}")
            if not bc.get("auto_reconnect", True):
                break
            time.sleep(bc.get("reconnect_interval_ms", 3000) / 1000.0)

    def send(self, data: bytes):
        """把 TX 数据发往板子。"""
        if not self.enabled or not self.sock:
            return
        try:
            self.sock.sendall(data)
        except Exception as e:
            print(f"[板子] 发送失败: {e}")


# ---------------------------------------------------------------------------
# 应用核心:串联驱动 / 日志 / 过滤 / WebSocket / 板子
# ---------------------------------------------------------------------------
class AppCore:
    def __init__(self, cfg):
        self.cfg = cfg
        self.filter = FilterEngine(cfg)
        self.logger = Logger(cfg)
        self.driver = DriverClient(cfg)
        self.driver_mgr = DriverManager(DRIVER_DIR)
        self.board = BoardTransport(cfg, on_data=self.on_board_data)
        self.history = []  # list of dict
        self.history_lock = threading.Lock()
        self.clients = set()  # websocket writer queues
        self.clients_lock = threading.Lock()
        self.loop = None
        self.running = False
        self.driver_ok = False
        self.stats = {"tx_frames": 0, "rx_frames": 0, "tx_bytes": 0, "rx_bytes": 0, "filtered": 0}

    def set_loop(self, loop):
        self.loop = loop

    def add_client(self, q):
        with self.clients_lock:
            self.clients.add(q)

    def remove_client(self, q):
        with self.clients_lock:
            self.clients.discard(q)

    def _push_ws(self, event: dict):
        if not self.loop:
            return
        async def _emit():
            with self.clients_lock:
                targets = list(self.clients)
            for q in targets:
                try:
                    q.put_nowait(event)
                except Exception:
                    pass
        asyncio.run_coroutine_threadsafe(_emit(), self.loop)

    def _record(self, direction: str, data: bytes, source: str = ""):
        # 过滤:命中则不存储、不推送、不给板子
        if self.filter.is_filtered(direction, data):
            self.stats["filtered"] += 1
            return False
        hx, asc = hexdump(data, self.cfg["display"]["hex_max_len"])
        item = {
            "time": now_str(), "dir": direction, "len": len(data),
            "hex": hx, "ascii": asc, "src": source
        }
        with self.history_lock:
            self.history.append(item)
            maxh = self.cfg["display"]["max_history"]
            if len(self.history) > maxh:
                del self.history[:len(self.history) - maxh]
        self.logger.write(direction, data, extra=source)
        self.stats[f"{direction.lower()}_frames"] += 1
        self.stats[f"{direction.lower()}_bytes"] += len(data)
        self._push_ws({"type": "data", "data": item})
        return True

    # TX: 串口助手 -> 本服务
    def on_tx(self, data: bytes):
        if self._record("TX", data, source="com"):
            self.board.send(data)   # 将来发给板子

    # RX: 板子 -> 本服务 -> 虚拟串口
    def on_board_data(self, data: bytes):
        if self._record("RX", data, source="board"):
            try:
                if self.driver_ok:
                    self.driver.write_rx(data)
            except Exception as e:
                print(f"[WARN] 写 RX 到驱动失败: {e}")

    # RX: 前端手动注入 -> 虚拟串口
    def inject_rx(self, text: str, is_hex: bool):
        try:
            if is_hex:
                cleaned = text.replace(" ", "").replace("\n", "").replace("\r", "")
                data = bytes.fromhex(cleaned)
            else:
                data = text.encode("utf-8")
        except Exception as e:
            raise ValueError(f"数据格式错误: {e}")
        if self._record("RX", data, source="web"):
            if self.driver_ok:
                self.driver.write_rx(data)
        return len(data)

    def poll_driver(self):
        """驱动数据读取线程:事件等待 + 轮询兜底。"""
        interval = self.cfg["driver"].get("poll_interval_ms", 20) / 1000.0
        chunk = self.cfg["driver"].get("read_chunk", 65536)
        while self.running:
            if not self.driver_ok:
                time.sleep(0.5)
                continue
            try:
                # 先立即读一次,再等待事件(避免事件与数据竞争导致的丢数据)
                data = self.driver.read_tx(chunk)
                if data:
                    self.on_tx(data)
                    continue
                self.driver.wait_event(int(interval * 1000) if interval * 1000 >= 1 else 1)
            except Exception as e:
                print(f"[ERROR] 驱动轮询异常: {e}")
                self.driver_ok = False
                time.sleep(1)

    def start(self):
        self.running = True
        try:
            self.driver.open()
            self.driver_ok = True
        except Exception as e:
            print(f"[WARN] {e}")
            print("[WARN] 将在后台重试连接驱动(每 1 秒)。")
        threading.Thread(target=self.poll_driver, daemon=True).start()
        self.board.start()

    def stop(self):
        self.running = False
        self.board.stop()
        self.driver.close()

    def reconnect_driver(self):
        """安装驱动后调用:关闭旧句柄并重新打开控制设备。"""
        self.driver.close()
        try:
            self.driver.open()
            self.driver_ok = True
            return True, "驱动已连接"
        except Exception as e:
            self.driver_ok = False
            return False, str(e)

    def get_status(self):
        st = {"driver_ok": self.driver_ok, "stats": dict(self.stats)}
        if self.driver_ok:
            try:
                st["driver"] = self.driver.get_status()
            except Exception:
                pass
        return st


# ---------------------------------------------------------------------------
# HTTP + WebSocket 服务器
# ---------------------------------------------------------------------------
class WebHandler(BaseHTTPRequestHandler):
    core: AppCore = None

    def log_message(self, fmt, *args):
        # 静默访问日志,避免刷屏
        pass

    def _send(self, code, body: bytes, ctype="text/plain; charset=utf-8", headers=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if headers:
            for k, v in headers.items():
                self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self._send(code, body, "application/json; charset=utf-8")

    def do_GET(self):
        u = urlparse(self.path)
        path = u.path
        qs = parse_qs(u.query)

        # WebSocket 升级
        if path == "/ws":
            self._handle_ws()
            return

        if path == "/api/status":
            self._send_json({"ok": True, "data": self.core.get_status()})
            return
        if path == "/api/history":
            with self.core.history_lock:
                items = list(self.core.history)
            self._send_json({"ok": True, "data": items})
            return
        if path == "/api/config":
            self._send_json({"ok": True, "data": self.core.cfg})
            return
        if path == "/api/ping":
            self._send_json({"ok": True, "time": now_str()})
            return
        if path == "/api/driver/status":
            try:
                self._send_json({"ok": True, "data": self.core.driver_mgr.status()})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 500)
            return

        # 静态文件
        if path in ("/", ""):
            path = "/index.html"
        rel = path.lstrip("/")
        full = os.path.normpath(os.path.join(PUBLIC_DIR, rel))
        if not full.startswith(PUBLIC_DIR):
            self._send(403, b"Forbidden")
            return
        if os.path.isfile(full):
            ext = os.path.splitext(full)[1].lower()
            ctype = {
                ".html": "text/html; charset=utf-8",
                ".css": "text/css; charset=utf-8",
                ".js": "application/javascript; charset=utf-8",
                ".json": "application/json; charset=utf-8",
                ".ico": "image/x-icon",
            }.get(ext, "application/octet-stream")
            with open(full, "rb") as f:
                self._send(200, f.read(), ctype)
        else:
            self._send(404, b"Not Found")

    def do_POST(self):
        u = urlparse(self.path)
        path = u.path
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""

        try:
            payload = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception:
            self._send_json({"ok": False, "error": "invalid json"}, 400)
            return

        if path == "/api/inject_rx":
            try:
                text = payload.get("data", "")
                is_hex = bool(payload.get("hex", False))
                n = self.core.inject_rx(text, is_hex)
                self._send_json({"ok": True, "bytes": n})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 400)
            return

        if path == "/api/filter":
            self.core.cfg["filter"] = payload.get("filter", self.core.cfg["filter"])
            self.core.filter.reload(self.core.cfg)
            self._save_cfg()
            self._send_json({"ok": True, "filter": self.core.cfg["filter"]})
            return

        if path == "/api/config":
            # 更新配置(简化:整体替换 board/server 之外的可热更部分)
            for key in ("board", "log", "filter", "display"):
                if key in payload:
                    self.core.cfg[key] = payload[key]
            self.core.filter.reload(self.core.cfg)
            self.core.logger.cfg = self.core.cfg
            self._save_cfg()
            self._send_json({"ok": True})
            return

        if path == "/api/clear":
            with self.core.history_lock:
                self.core.history.clear()
            self._send_json({"ok": True})
            return

        if path == "/api/purge":
            try:
                if self.core.driver_ok:
                    self.core.driver.purge()
                self._send_json({"ok": True})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/driver/install":
            try:
                com = int(payload.get("com", 8))
                ok, need_reboot, steps = self.core.driver_mgr.install(com)
                reconnect = False
                if ok and not need_reboot:
                    time.sleep(1.5)
                    reconnect, msg = self.core.reconnect_driver()
                    steps.append({"ok": reconnect, "msg": "连接驱动: " + msg})
                self._send_json({"ok": ok, "need_reboot": need_reboot, "steps": steps,
                                 "driver_connected": self.core.driver_ok})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/driver/uninstall":
            try:
                ok, steps = self.core.driver_mgr.uninstall()
                self.core.driver_ok = False
                self.core.driver.close()
                self._send_json({"ok": ok, "steps": steps})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/driver/com":
            try:
                com = int(payload.get("com", 8))
                ok, msg = self.core.driver_mgr.set_com(com)
                self._send_json({"ok": ok, "msg": msg})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/driver/testsigning":
            try:
                on = bool(payload.get("on", True))
                ok, need_reboot, msg = self.core.driver_mgr.set_testsigning(on)
                self._send_json({"ok": ok, "need_reboot": need_reboot, "msg": msg})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 500)
            return

        if path == "/api/driver/reconnect":
            try:
                ok, msg = self.core.reconnect_driver()
                self._send_json({"ok": ok, "msg": msg})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 500)
            return

        self._send_json({"ok": False, "error": "not found"}, 404)

    def _save_cfg(self):
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(self.core.cfg, f, ensure_ascii=False, indent=2)
        except Exception as e:
            print(f"[WARN] 保存配置失败: {e}")

    def _handle_ws(self):
        """极简 WebSocket(RFC6455) 握手与帧收发。"""
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self._send(400, b"Missing Sec-WebSocket-Key")
            return
        magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        accept = base64.b64encode(hashlib.sha1((key + magic).encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()

        q = asyncio.Queue()
        self.core.add_client(q)

        # 事件推送协程:从队列取事件并通过本连接的 socket 发出
        async def drain():
            while True:
                try:
                    ev = await asyncio.wait_for(q.get(), timeout=0.5)
                    payload = json.dumps(ev, ensure_ascii=False).encode("utf-8")
                    frame = bytearray([0x81])
                    n = len(payload)
                    if n < 126:
                        frame.append(n)
                    elif n < 65536:
                        frame.append(126); frame += struct.pack("!H", n)
                    else:
                        frame.append(127); frame += struct.pack("!Q", n)
                    frame += payload
                    self.wfile.write(bytes(frame))
                    self.wfile.flush()
                except asyncio.TimeoutError:
                    # 读心:检查连接是否还活着
                    if self.core.loop is None:
                        break
                except Exception:
                    break

        fut = asyncio.run_coroutine_threadsafe(drain(), self.core.loop)

        try:
            # 读客户端帧(保持连接,处理关闭)
            while True:
                hdr = self.rfile.read(2)
                if not hdr or len(hdr) < 2:
                    break
                opcode = hdr[0] & 0x0F
                masked = hdr[1] & 0x80
                ln = hdr[1] & 0x7F
                if ln == 126:
                    ln = struct.unpack("!H", self.rfile.read(2))[0]
                elif ln == 127:
                    ln = struct.unpack("!Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if masked else None
                data = self.rfile.read(ln) if ln else b""
                if mask:
                    data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
                if opcode == 0x8:  # close
                    break
                # 忽略客户端文本(前端用 HTTP POST 注入)
        except Exception:
            pass
        finally:
            fut.cancel()
            self.core.remove_client(q)


def run_http_server(core: AppCore, host, port):
    handler = type("BoundHandler", (WebHandler,), {"core": core})
    server = ThreadingHTTPServer((host, port), handler)
    print(f"[HTTP] 服务已启动: http://{host}:{port}  (UI 页面 /)")

    loop = asyncio.new_event_loop()
    core.set_loop(loop)

    def loop_thread():
        asyncio.set_event_loop(loop)
        loop.run_forever()

    threading.Thread(target=loop_thread, daemon=True).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        loop.call_soon_threadsafe(loop.stop)


def _redirect_output_to_file():
    """打包成无窗口 exe 后,把 stdout/stderr 写入 exe 同目录的 runtime.log,便于排查。"""
    if not FROZEN:
        return
    try:
        log_path = os.path.join(BASE_DIR, "runtime.log")
        f = open(log_path, "a", encoding="utf-8", buffering=1)
        sys.stdout = f
        sys.stderr = f
    except Exception:
        pass


def main():
    _redirect_output_to_file()
    print("=" * 60)
    print(" CommLink Server - 虚拟串口桥接服务")
    print("=" * 60)
    cfg = load_config()
    os.makedirs(LOG_DIR, exist_ok=True)
    core = AppCore(cfg)
    core.start()
    host = cfg["server"]["host"]
    port = int(cfg["server"]["port"])

    # 启动后自动打开浏览器(打包成 exe 时用户体验:双击即用)
    def open_browser_later():
        time.sleep(1.2)
        try:
            webbrowser.open(f"http://127.0.0.1:{port}/")
            print(f"[UI] 已打开浏览器: http://127.0.0.1:{port}/")
        except Exception as e:
            print(f"[WARN] 自动打开浏览器失败: {e}")
    if cfg.get("server", {}).get("open_browser", True):
        threading.Thread(target=open_browser_later, daemon=True).start()

    print(f"[提示] 若浏览器未自动打开,请手动访问: http://127.0.0.1:{port}/")
    try:
        run_http_server(core, host, port)
    finally:
        core.stop()
        print("[退出] 服务已停止。")


if __name__ == "__main__":
    main()
