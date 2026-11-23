/*++
    mkroot.c
    =====================================================================
    Root 枚举设备安装工具 —— 创建/移除 Root\CommLink 设备节点

    Windows 下要让 INF 里的 Root\XXX 设备真正出现,必须显式创建设备节点
    (pnputil /add-driver 只把驱动加入驱动库,不会创建设备实例)。
    本工具用 SetupAPI 完成这件事,避免依赖 devcon.exe。

    用法(需管理员权限):
      mkroot.exe install <inf完整路径>
      mkroot.exe uninstall
    =====================================================================
--*/
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <devguid.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")

static const WCHAR *HWID = L"Root\\CommLink";

static void usage(void)
{
    wprintf(L"Usage:\n");
    wprintf(L"  mkroot.exe install <inf_full_path>\n");
    wprintf(L"  mkroot.exe uninstall\n");
}

static int DoInstall(const WCHAR *infPath)
{
    HDEVINFO        hdev;
    SP_DEVINFO_DATA did;
    DWORD           err;
    BOOL            reboot = FALSE;
    WCHAR           hwidMulti[64];

    hdev = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_PORTS, NULL);
    if (hdev == INVALID_HANDLE_VALUE) {
        wprintf(L"[错误] SetupDiCreateDeviceInfoList 失败: %u\n", GetLastError());
        return 1;
    }

    did.cbSize = sizeof(did);
    if (!SetupDiCreateDeviceInfo(hdev, HWID, &GUID_DEVCLASS_PORTS,
                                 L"CommLink Virtual Serial Port", NULL, 0, &did)) {
        err = GetLastError();
        if (err == ERROR_DUP_NAME) {
            wprintf(L"[信息] 设备节点已存在,将更新其驱动。\n");
        } else {
            wprintf(L"[错误] SetupDiCreateDeviceInfo 失败: %u\n", err);
            SetupDiDestroyDeviceInfoList(hdev);
            return 1;
        }
    } else {
        // MULTI_SZ: "Root\CommLink\0\0"
        int i = 0;
        const WCHAR *s = HWID;
        while (*s) hwidMulti[i++] = *s++;
        hwidMulti[i++] = L'\0';
        hwidMulti[i++] = L'\0';

        if (!SetupDiSetDeviceRegistryProperty(hdev, &did, SPDRP_HARDWAREID,
                                              (PBYTE)hwidMulti,
                                              (DWORD)(i * sizeof(WCHAR)))) {
            wprintf(L"[警告] SetupDiSetDeviceRegistryProperty 失败: %u\n", GetLastError());
        }
        if (!SetupDiRegisterDeviceInfo(hdev, &did, 0, NULL, NULL, NULL)) {
            wprintf(L"[警告] SetupDiRegisterDeviceInfo 失败: %u\n", GetLastError());
        }
    }
    SetupDiDestroyDeviceInfoList(hdev);

    // 用指定 INF 为该硬件 ID 安装/更新驱动
    if (!UpdateDriverForPlugAndPlayDevices(NULL, HWID, infPath,
                                           INSTALLFLAG_FORCE, &reboot)) {
        err = GetLastError();
        wprintf(L"[错误] UpdateDriverForPlugAndPlayDevices 失败: %u\n", err);
        if (err == ERROR_FILE_NOT_FOUND) {
            wprintf(L"[提示] 请确认 INF 路径正确: %s\n", infPath);
        }
        return 1;
    }

    wprintf(L"[成功] 驱动安装完成。需要重启: %s\n", reboot ? L"是" : L"否");
    return 0;
}

static int DoUninstall(void)
{
    HDEVINFO        hdev;
    SP_DEVINFO_DATA did;
    DWORD           i;
    int             removed = 0;

    hdev = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, NULL, NULL, 0);
    if (hdev == INVALID_HANDLE_VALUE) {
        wprintf(L"[错误] SetupDiGetClassDevs 失败: %u\n", GetLastError());
        return 1;
    }

    did.cbSize = sizeof(did);
    for (i = 0; SetupDiEnumDeviceInfo(hdev, i, &did); i++) {
        WCHAR buf[1024];
        DWORD type;
        if (SetupDiGetDeviceRegistryProperty(hdev, &did, SPDRP_HARDWAREID, &type,
                                             (PBYTE)buf, sizeof(buf), NULL)) {
            if (_wcsicmp(buf, HWID) == 0) {
                if (SetupDiRemoveDevice(hdev, &did)) {
                    wprintf(L"[成功] 已移除 CommLink 设备节点。\n");
                    removed = 1;
                } else {
                    wprintf(L"[错误] SetupDiRemoveDevice 失败: %u\n", GetLastError());
                }
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hdev);

    if (!removed) wprintf(L"[信息] 未找到已安装的 CommLink 设备节点。\n");
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc < 2) { usage(); return 1; }
    if (_wcsicmp(argv[1], L"install") == 0) {
        if (argc < 3) { usage(); return 1; }
        return DoInstall(argv[2]);
    }
    if (_wcsicmp(argv[1], L"uninstall") == 0) {
        return DoUninstall();
    }
    usage();
    return 1;
}
