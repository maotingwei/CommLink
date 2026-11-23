/*++
    CommLinkCommon.h
    =====================================================================
    虚拟串口驱动(CommLink.sys) 与 用户态服务(app.py) 的共享协议定义。

    该文件由驱动(C)和用户态(Python,手动换算同值)共同遵守。
    数据方向约定(以本软件/虚拟串口为中心):
        TX = 串口助手 WriteFile -> 虚拟串口 -> 本软件 -> (将来)板子
        RX = (将来)板子 -> 本软件 -> 虚拟串口 -> 串口助手 ReadFile
    =====================================================================
--*/

#ifndef _COMMLINK_COMMON_H_
#define _COMMLINK_COMMON_H_

// 自定义设备类型(用户态换算 CTL_CODE 时必须一致)
#define FILE_DEVICE_COMMLINK             0x8A00
#define COMMLINK_IOCTL_BASE              0x800

// METHOD_BUFFERED + FILE_ANY_ACCESS => 低 2 位=0, Access 位=0
// CTL_CODE = (DeviceType<<16) | (Access<<14) | (Function<<2) | Method
#define IOCTL_COMMLINK_GET_TX             CTL_CODE(FILE_DEVICE_COMMLINK, COMMLINK_IOCTL_BASE + 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_COMMLINK_PUT_RX             CTL_CODE(FILE_DEVICE_COMMLINK, COMMLINK_IOCTL_BASE + 1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_COMMLINK_SET_EVENT          CTL_CODE(FILE_DEVICE_COMMLINK, COMMLINK_IOCTL_BASE + 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_COMMLINK_GET_STATUS         CTL_CODE(FILE_DEVICE_COMMLINK, COMMLINK_IOCTL_BASE + 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_COMMLINK_PURGE              CTL_CODE(FILE_DEVICE_COMMLINK, COMMLINK_IOCTL_BASE + 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_COMMLINK_GET_VERSION        CTL_CODE(FILE_DEVICE_COMMLINK, COMMLINK_IOCTL_BASE + 5, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 用户态事件通知:驱动在 TxRing 有数据时按字节数累计,事件为手动重置。
// 传入缓冲: HANDLE (8 bytes on x64 / 4 bytes on x86)

typedef struct _COMMLINK_RING_STATUS {
    ULONG TxLength;       // 待用户态读取的 TX 字节数
    ULONG TxCapacity;
    ULONG RxLength;       // 待串口助手读取的 RX 字节数
    ULONG RxCapacity;
    ULONG PendingReads;   // 挂起的串口助手读请求数
    ULONG TotalTxBytes;   // 累计
    ULONG TotalRxBytes;
} COMMLINK_RING_STATUS, *PCOMMLINK_RING_STATUS;

typedef struct _COMMLINK_VERSION {
    ULONG Major;
    ULONG Minor;
    ULONG Build;
} COMMLINK_VERSION, *PCOMMLINK_VERSION;

#define COMMLINK_VERSION_MAJOR   0
#define COMMLINK_VERSION_MINOR   1
#define COMMLINK_VERSION_BUILD   0

// 设备名称
#define COMMLINK_NT_DEVICE_COM      L"\\Device\\CommLinkCom0"
#define COMMLINK_NT_DEVICE_CTL      L"\\Device\\CommLinkCtl"
#define COMMLINK_DOS_DEVICE_CTL     L"\\DosDevices\\CommLinkCtl"
#define COMMLINK_DOS_DEVICE_COM     L"\\DosDevices\\COM"      // 后接数字,如 COM8

// 注册表参数(位于 Services\CommLink\Parameters)
#define COMMLINK_REG_COMPORT        L"ComPort"      // REG_DWORD,默认 8
#define COMMLINK_REG_TXCAPACITY     L"TxCapacity"   // REG_DWORD,默认 65536
#define COMMLINK_REG_RXCAPACITY     L"RxCapacity"   // REG_DWORD,默认 65536

// 环形缓冲默认容量
#define COMMLINK_DEFAULT_CAPACITY   65536UL

#endif // _COMMLINK_COMMON_H_
