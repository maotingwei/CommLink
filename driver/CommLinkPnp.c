/*++
    CommLinkPnp.c
    =====================================================================
    CommLink 虚拟串口驱动 —— PnP WDM 版

    与 Legacy 版(CommLink.c)的区别:
      * 由 INF 安装为 Root\CommLink 设备,出现在设备管理器"端口(COM和LPT)"
      * DriverEntry 注册 AddDevice;PnP 管理器枚举到设备时调用它创建 FDO
      * 实现 IRP_MJ_PNP / IRP_MJ_POWER 必要的分发
      * START_DEVICE 时创建 \\.\COMn 符号链接,并向设备实例键写 PortName
      * 控制设备 \\.\CommLinkCtl 仍在 DriverEntry 创建,用户态接口不变

    数据路径与 Legacy 版完全相同:
      串口助手 WriteFile -> TxRing -> 控制设备 GET_TX -> 用户态服务
      用户态 PUT_RX -> RxRing -> 串口助手 ReadFile
    =====================================================================
--*/

#include <ntddk.h>
#include <ntddser.h>
#include "CommLinkCommon.h"

#define COMMLINK_TAG  'KNLC'

// 未文档化的内核导出:事件对象类型
extern POBJECT_TYPE *ExEventObjectType;

// ---------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------
typedef struct _RING_BUFFER {
    PUCHAR      Buffer;
    ULONG       Capacity;
    ULONG       Head;
    ULONG       Tail;
    ULONG       Length;
    KSPIN_LOCK  Lock;
} RING_BUFFER, *PRING_BUFFER;

typedef struct _DEVICE_EXTENSION {
    RING_BUFFER     TxRing;
    RING_BUFFER     RxRing;
    PKEVENT         TxEvent;
    KSPIN_LOCK      TxEventLock;
    LIST_ENTRY      PendingReads;
    KSPIN_LOCK      PendingLock;
    ULONG           TotalTxBytes;
    ULONG           TotalRxBytes;
    LONG            OpenHandles;
    ULONG           BaudRate;
    UCHAR           ByteSize;
    UCHAR           Parity;
    UCHAR           StopBits;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

// 每个设备对象(FDO / 控制设备)的扩展
typedef struct _COMMLINK_EXT {
    PDEVICE_EXTENSION   Shared;           // 指向 g_Global.Shared
    BOOLEAN             IsControlDevice;  // TRUE=控制设备, FALSE=COM FDO
    PDEVICE_OBJECT      Pdo;              // 仅 FDO 有效
    PDEVICE_OBJECT      NextLowerDriver;  // 仅 FDO 有效
    BOOLEAN             Started;          // FDO 是否已完成 START_DEVICE
} COMMLINK_EXT, *PCOMMLINK_EXT;

typedef struct _GLOBAL_STATE {
    PDEVICE_OBJECT      ComFdo;
    PDEVICE_OBJECT      CtlDevice;
    DEVICE_EXTENSION    Shared;
    ULONG               ComPortNumber;
    UNICODE_STRING      ComSymbolicName;
    WCHAR               ComSymbolicBuf[64];
} GLOBAL_STATE, *PGLOBAL_STATE;

static GLOBAL_STATE g_Global;

// ---------------------------------------------------------------------------
// 前向声明
// ---------------------------------------------------------------------------
DRIVER_INITIALIZE DriverEntry;
static DRIVER_ADD_DEVICE CL_AddDevice;
static DRIVER_DISPATCH CL_Pnp;
static DRIVER_DISPATCH CL_Power;
static DRIVER_DISPATCH CL_Passthru;
static DRIVER_DISPATCH CL_Create;
static DRIVER_DISPATCH CL_Close;
static DRIVER_DISPATCH CL_Cleanup;
static DRIVER_DISPATCH CL_Read;
static DRIVER_DISPATCH CL_Write;
static DRIVER_DISPATCH CL_DeviceControl;
static DRIVER_DISPATCH CL_Flush;
static DRIVER_UNLOAD CL_Unload;

static VOID CL_CancelPendingReads(PDEVICE_EXTENSION Shared);

// ---------------------------------------------------------------------------
// 工具:手动拼字符串(内核不可用 CRT 的 swprintf)
// ---------------------------------------------------------------------------
static VOID
BuildComSymbolicName(PWCHAR Buf, ULONG Port)
{
    static const WCHAR prefix[] = L"\\DosDevices\\COM";
    ULONG i, n, digits[12], t = 0;

    for (i = 0; prefix[i] != L'\0'; i++) Buf[i] = prefix[i];
    n = i;
    if (Port == 0) digits[t++] = 0;
    while (Port > 0) { digits[t++] = Port % 10; Port /= 10; }
    while (t > 0) Buf[n++] = (WCHAR)(L'0' + digits[--t]);
    Buf[n] = L'\0';
}

// 返回字符个数(不含结尾 NULL)
static ULONG
BuildPortName(PWCHAR Buf, ULONG Port)
{
    ULONG n = 0, digits[12], t = 0;
    Buf[n++] = L'C'; Buf[n++] = L'O'; Buf[n++] = L'M';
    if (Port == 0) digits[t++] = 0;
    while (Port > 0) { digits[t++] = Port % 10; Port /= 10; }
    while (t > 0) Buf[n++] = (WCHAR)(L'0' + digits[--t]);
    Buf[n] = L'\0';
    return n;
}

static ULONG
WcslenFast(PCWCHAR S)
{
    ULONG n = 0;
    while (S[n] != L'\0') n++;
    return n;
}

// ---------------------------------------------------------------------------
// 环形缓冲
// ---------------------------------------------------------------------------
static BOOLEAN
RingPush(PRING_BUFFER Ring, PUCHAR Data, ULONG Length)
{
    ULONG i;
    if (Ring->Length + Length > Ring->Capacity) return FALSE;
    for (i = 0; i < Length; i++) {
        Ring->Buffer[Ring->Tail] = Data[i];
        Ring->Tail = (Ring->Tail + 1) % Ring->Capacity;
    }
    Ring->Length += Length;
    return TRUE;
}

static ULONG
RingPop(PRING_BUFFER Ring, PUCHAR Out, ULONG MaxLength)
{
    ULONG n = (Ring->Length < MaxLength) ? Ring->Length : MaxLength;
    ULONG i;
    for (i = 0; i < n; i++) {
        Out[i] = Ring->Buffer[Ring->Head];
        Ring->Head = (Ring->Head + 1) % Ring->Capacity;
    }
    Ring->Length -= n;
    return n;
}

// ---------------------------------------------------------------------------
// 完成挂起的读请求
// ---------------------------------------------------------------------------
static VOID
CompleteOnePendingRead(PDEVICE_EXTENSION Shared)
{
    PIRP    Irp = NULL;
    KIRQL   cancelIrql;
    ULONG   copied = 0;

    IoAcquireCancelSpinLock(&cancelIrql);
    KeAcquireSpinLockAtDpcLevel(&Shared->PendingLock);

    KeAcquireSpinLockAtDpcLevel(&Shared->RxRing.Lock);
    if (Shared->RxRing.Length > 0 && !IsListEmpty(&Shared->PendingReads)) {
        PLIST_ENTRY le = RemoveHeadList(&Shared->PendingReads);
        Irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        IoSetCancelRoutine(Irp, NULL);
        {
            PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
            PUCHAR out = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
            copied = RingPop(&Shared->RxRing, out, sl->Parameters.Read.Length);
            Shared->TotalRxBytes += copied;
        }
    }
    KeReleaseSpinLockFromDpcLevel(&Shared->RxRing.Lock);
    KeReleaseSpinLockFromDpcLevel(&Shared->PendingLock);
    IoReleaseCancelSpinLock(cancelIrql);

    if (Irp) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = copied;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
}

static VOID
CL_CancelPendingReads(PDEVICE_EXTENSION Shared)
{
    KIRQL cancelIrql;
    IoAcquireCancelSpinLock(&cancelIrql);
    KeAcquireSpinLockAtDpcLevel(&Shared->PendingLock);

    while (!IsListEmpty(&Shared->PendingReads)) {
        PLIST_ENTRY le = RemoveHeadList(&Shared->PendingReads);
        PIRP irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        IoSetCancelRoutine(irp, NULL);
        KeReleaseSpinLockFromDpcLevel(&Shared->PendingLock);
        IoReleaseCancelSpinLock(cancelIrql);

        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);

        IoAcquireCancelSpinLock(&cancelIrql);
        KeAcquireSpinLockAtDpcLevel(&Shared->PendingLock);
    }

    KeReleaseSpinLockFromDpcLevel(&Shared->PendingLock);
    IoReleaseCancelSpinLock(cancelIrql);
}

static VOID
CL_CancelRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT       ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    PDEVICE_EXTENSION   Shared = ext->Shared;
    KIRQL               irql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);
    KeAcquireSpinLock(&Shared->PendingLock, &irql);
    if (Irp->Tail.Overlay.ListEntry.Flink != NULL) RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Shared->PendingLock, irql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

// ---------------------------------------------------------------------------
// 普通 IRP 处理(COM FDO 与控制设备共用)
// ---------------------------------------------------------------------------
static NTSTATUS
CL_Create(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    InterlockedIncrement(&ext->Shared->OpenHandles);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
CL_Close(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    InterlockedDecrement(&ext->Shared->OpenHandles);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
CL_Cleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    CL_CancelPendingReads(ext->Shared);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
CL_Read(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT       ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    PDEVICE_EXTENSION   Shared = ext->Shared;
    KIRQL               irql, cancelIrql;
    ULONG               copied = 0;
    BOOLEAN             completedNow = FALSE;

    KeAcquireSpinLock(&Shared->RxRing.Lock, &irql);
    if (Shared->RxRing.Length > 0) {
        PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
        PUCHAR out = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
        copied = RingPop(&Shared->RxRing, out, sl->Parameters.Read.Length);
        Shared->TotalRxBytes += copied;
        completedNow = TRUE;
    }
    KeReleaseSpinLock(&Shared->RxRing.Lock, irql);

    if (completedNow) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = copied;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    IoAcquireCancelSpinLock(&cancelIrql);
    if (Irp->Cancel) {
        IoReleaseCancelSpinLock(cancelIrql);
        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
    }
    IoSetCancelRoutine(Irp, CL_CancelRoutine);
    InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    KeAcquireSpinLockAtDpcLevel(&Shared->PendingLock);
    InsertTailList(&Shared->PendingReads, &Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLockFromDpcLevel(&Shared->PendingLock);
    IoReleaseCancelSpinLock(cancelIrql);

    IoMarkIrpPending(Irp);
    return STATUS_PENDING;
}

static NTSTATUS
CL_Write(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT       ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    PDEVICE_EXTENSION   Shared = ext->Shared;
    PIO_STACK_LOCATION  sl = IoGetCurrentIrpStackLocation(Irp);
    ULONG               len = sl->Parameters.Write.Length;
    PUCHAR              data = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    KIRQL               irql;
    BOOLEAN             ok;

    KeAcquireSpinLock(&Shared->TxRing.Lock, &irql);
    ok = RingPush(&Shared->TxRing, data, len);
    if (ok) Shared->TotalTxBytes += len;
    KeReleaseSpinLock(&Shared->TxRing.Lock, irql);

    if (ok) {
        KeAcquireSpinLock(&Shared->TxEventLock, &irql);
        if (Shared->TxEvent) KeSetEvent(Shared->TxEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&Shared->TxEventLock, irql);
    }

    Irp->IoStatus.Status = ok ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
    Irp->IoStatus.Information = ok ? len : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

// 串口助手侧 IOCTL_SERIAL_*
static NTSTATUS
HandleSerialIoctl(PDEVICE_EXTENSION Shared, PIRP Irp, ULONG code)
{
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    ULONG inLen = sl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;

    switch (code) {
    case IOCTL_SERIAL_SET_BAUD_RATE:
        if (inLen >= sizeof(SERIAL_BAUD_RATE)) Shared->BaudRate = ((PSERIAL_BAUD_RATE)buf)->BaudRate;
        else status = STATUS_INVALID_PARAMETER;
        break;
    case IOCTL_SERIAL_GET_BAUD_RATE:
        if (outLen >= sizeof(SERIAL_BAUD_RATE)) {
            ((PSERIAL_BAUD_RATE)buf)->BaudRate = Shared->BaudRate;
            info = sizeof(SERIAL_BAUD_RATE);
        } else status = STATUS_BUFFER_TOO_SMALL;
        break;
    case IOCTL_SERIAL_SET_LINE_CONTROL:
        if (inLen >= sizeof(SERIAL_LINE_CONTROL)) {
            PSERIAL_LINE_CONTROL lc = (PSERIAL_LINE_CONTROL)buf;
            Shared->ByteSize = lc->WordLength;
            Shared->StopBits = lc->StopBits;
            Shared->Parity = lc->Parity;
        } else status = STATUS_INVALID_PARAMETER;
        break;
    case IOCTL_SERIAL_GET_LINE_CONTROL:
        if (outLen >= sizeof(SERIAL_LINE_CONTROL)) {
            PSERIAL_LINE_CONTROL lc = (PSERIAL_LINE_CONTROL)buf;
            lc->WordLength = Shared->ByteSize;
            lc->StopBits = Shared->StopBits;
            lc->Parity = Shared->Parity;
            info = sizeof(SERIAL_LINE_CONTROL);
        } else status = STATUS_BUFFER_TOO_SMALL;
        break;
    case IOCTL_SERIAL_GET_TIMEOUTS:
        if (outLen >= sizeof(SERIAL_TIMEOUTS)) {
            RtlZeroMemory(buf, sizeof(SERIAL_TIMEOUTS));
            info = sizeof(SERIAL_TIMEOUTS);
        } else status = STATUS_BUFFER_TOO_SMALL;
        break;
    case IOCTL_SERIAL_GET_COMMSTATUS:
        if (outLen >= sizeof(SERIAL_STATUS)) {
            KIRQL irql;
            PSERIAL_STATUS st = (PSERIAL_STATUS)buf;
            RtlZeroMemory(st, sizeof(SERIAL_STATUS));
            KeAcquireSpinLock(&Shared->RxRing.Lock, &irql);
            st->AmountInInQueue = Shared->RxRing.Length;
            KeReleaseSpinLock(&Shared->RxRing.Lock, irql);
            info = sizeof(SERIAL_STATUS);
        } else status = STATUS_BUFFER_TOO_SMALL;
        break;
    case IOCTL_SERIAL_GET_MODEMSTATUS:
        if (outLen >= sizeof(ULONG)) { *(PULONG)buf = 0; info = sizeof(ULONG); }
        else status = STATUS_BUFFER_TOO_SMALL;
        break;
    case IOCTL_SERIAL_GET_PROPERTIES:
        if (outLen >= sizeof(SERIAL_COMMPROP)) {
            PSERIAL_COMMPROP cp = (PSERIAL_COMMPROP)buf;
            RtlZeroMemory(cp, sizeof(SERIAL_COMMPROP));
            cp->PacketLength = sizeof(SERIAL_COMMPROP);
            cp->PacketVersion = 2;
            cp->ServiceMask = SERIAL_SP_SERIALCOMM;
            cp->MaxTxQueue = Shared->TxRing.Capacity;
            cp->MaxRxQueue = Shared->RxRing.Capacity;
            info = sizeof(SERIAL_COMMPROP);
        } else status = STATUS_BUFFER_TOO_SMALL;
        break;
    case IOCTL_SERIAL_WAIT_ON_MASK:
        if (outLen >= sizeof(ULONG)) { *(PULONG)buf = 0; info = sizeof(ULONG); }
        else status = STATUS_BUFFER_TOO_SMALL;
        break;
    default:
        status = STATUS_SUCCESS;
        info = 0;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// 控制设备私有 IOCTL
static NTSTATUS
HandleControlIoctl(PDEVICE_EXTENSION Shared, PIRP Irp, ULONG code)
{
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    ULONG inLen = sl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;

    switch (code) {
    case IOCTL_COMMLINK_GET_TX: {
        KIRQL irql;
        KeAcquireSpinLock(&Shared->TxRing.Lock, &irql);
        info = RingPop(&Shared->TxRing, (PUCHAR)buf, outLen);
        KeReleaseSpinLock(&Shared->TxRing.Lock, irql);
        break;
    }
    case IOCTL_COMMLINK_PUT_RX: {
        KIRQL irql;
        BOOLEAN ok;
        KeAcquireSpinLock(&Shared->RxRing.Lock, &irql);
        ok = RingPush(&Shared->RxRing, (PUCHAR)buf, inLen);
        if (ok) Shared->TotalRxBytes += inLen;
        KeReleaseSpinLock(&Shared->RxRing.Lock, irql);
        if (!ok) { status = STATUS_BUFFER_OVERFLOW; info = 0; }
        else { info = inLen; CompleteOnePendingRead(Shared); }
        break;
    }
    case IOCTL_COMMLINK_SET_EVENT: {
        HANDLE hEvent;
        PKEVENT pEvent = NULL;
        KIRQL irql;
        if (inLen < sizeof(HANDLE)) { status = STATUS_INVALID_PARAMETER; break; }
        hEvent = *((PHANDLE)buf);
        status = ObReferenceObjectByHandle(hEvent, EVENT_MODIFY_STATE,
                     *ExEventObjectType, UserMode, (PVOID*)&pEvent, NULL);
        if (!NT_SUCCESS(status)) { info = 0; break; }
        KeAcquireSpinLock(&Shared->TxEventLock, &irql);
        if (Shared->TxEvent) ObDereferenceObject(Shared->TxEvent);
        Shared->TxEvent = pEvent;
        KeReleaseSpinLock(&Shared->TxEventLock, irql);
        info = 0;
        break;
    }
    case IOCTL_COMMLINK_GET_STATUS: {
        COMMLINK_RING_STATUS st;
        KIRQL irql;
        PLIST_ENTRY le;
        RtlZeroMemory(&st, sizeof(st));
        KeAcquireSpinLock(&Shared->TxRing.Lock, &irql);
        st.TxLength = Shared->TxRing.Length;
        st.TxCapacity = Shared->TxRing.Capacity;
        KeReleaseSpinLock(&Shared->TxRing.Lock, irql);
        KeAcquireSpinLock(&Shared->RxRing.Lock, &irql);
        st.RxLength = Shared->RxRing.Length;
        st.RxCapacity = Shared->RxRing.Capacity;
        KeReleaseSpinLock(&Shared->RxRing.Lock, irql);
        KeAcquireSpinLock(&Shared->PendingLock, &irql);
        st.PendingReads = 0;
        for (le = Shared->PendingReads.Flink; le != &Shared->PendingReads; le = le->Flink) st.PendingReads++;
        KeReleaseSpinLock(&Shared->PendingLock, irql);
        st.TotalTxBytes = Shared->TotalTxBytes;
        st.TotalRxBytes = Shared->TotalRxBytes;
        if (outLen >= sizeof(st)) { RtlCopyMemory(buf, &st, sizeof(st)); info = sizeof(st); }
        else status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    case IOCTL_COMMLINK_PURGE: {
        KIRQL irql;
        KeAcquireSpinLock(&Shared->TxRing.Lock, &irql);
        Shared->TxRing.Head = Shared->TxRing.Tail = Shared->TxRing.Length = 0;
        KeReleaseSpinLock(&Shared->TxRing.Lock, irql);
        KeAcquireSpinLock(&Shared->RxRing.Lock, &irql);
        Shared->RxRing.Head = Shared->RxRing.Tail = Shared->RxRing.Length = 0;
        KeReleaseSpinLock(&Shared->RxRing.Lock, irql);
        break;
    }
    case IOCTL_COMMLINK_GET_VERSION: {
        COMMLINK_VERSION ver;
        ver.Major = COMMLINK_VERSION_MAJOR;
        ver.Minor = COMMLINK_VERSION_MINOR;
        ver.Build = COMMLINK_VERSION_BUILD;
        if (outLen >= sizeof(ver)) { RtlCopyMemory(buf, &ver, sizeof(ver)); info = sizeof(ver); }
        else status = STATUS_BUFFER_TOO_SMALL;
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS
CL_DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT       ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION  sl = IoGetCurrentIrpStackLocation(Irp);
    ULONG               code = sl->Parameters.DeviceIoControl.IoControlCode;
    if (ext->IsControlDevice) return HandleControlIoctl(ext->Shared, Irp, code);
    return HandleSerialIoctl(ext->Shared, Irp, code);
}

static NTSTATUS
CL_Flush(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// 未显式处理的 IRP:有下层则转发,否则直接成功完成
static NTSTATUS
CL_Passthru(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    if (ext->NextLowerDriver) {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->NextLowerDriver, Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// PnP / Power
// ---------------------------------------------------------------------------
static NTSTATUS
CL_PnpComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
CL_ForwardAndWait(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    KEVENT event;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, CL_PnpComplete, &event, TRUE, TRUE, TRUE);
    status = IoCallDriver(ext->NextLowerDriver, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }
    return status;
}

static NTSTATUS
CL_CreateComLink(PCOMMLINK_EXT ext)
{
    UNICODE_STRING ntName;
    UNICODE_STRING valueName;
    HANDLE key = NULL;
    WCHAR portBuf[16];
    NTSTATUS status;

    // \\.\COMn -> FDO 的 NT 设备名
    RtlInitUnicodeString(&ntName, COMMLINK_NT_DEVICE_COM);
    status = IoCreateSymbolicLink(&g_Global.ComSymbolicName, &ntName);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[CommLinkPnp] IoCreateSymbolicLink(%wZ) failed 0x%08X\n",
                 &g_Global.ComSymbolicName, status);
        return status;
    }

    // 向设备实例键写 PortName,设备管理器据此显示 "(COMn)"
    status = IoOpenDeviceRegistryKey(ext->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key);
    if (NT_SUCCESS(status)) {
        ULONG portNameChars = BuildPortName(portBuf, g_Global.ComPortNumber);
        RtlInitUnicodeString(&valueName, L"PortName");
        ZwSetValueKey(key, &valueName, 0, REG_SZ, portBuf, portNameChars * sizeof(WCHAR));
        ZwClose(key);
    } else {
        DbgPrint("[CommLinkPnp] IoOpenDeviceRegistryKey failed 0x%08X (link still created)\n", status);
    }
    return STATUS_SUCCESS;
}

static VOID
CL_DeleteComLink(VOID)
{
    if (g_Global.ComSymbolicName.Buffer) {
        IoDeleteSymbolicLink(&g_Global.ComSymbolicName);
    }
}

static NTSTATUS
CL_Pnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION  sl = IoGetCurrentIrpStackLocation(Irp);
    PCOMMLINK_EXT       ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    NTSTATUS            status = STATUS_SUCCESS;

    // 控制设备不在 PnP 栈中,直接完成
    if (ext->IsControlDevice || !ext->NextLowerDriver) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    switch (sl->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = CL_ForwardAndWait(DeviceObject, Irp);
        if (NT_SUCCESS(status)) {
            status = CL_CreateComLink(ext);
            if (NT_SUCCESS(status)) ext->Started = TRUE;
        }
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        DbgPrint("[CommLinkPnp] START_DEVICE status=0x%08X COM%u\n",
                 status, g_Global.ComPortNumber);
        return status;

    case IRP_MN_REMOVE_DEVICE:
        CL_DeleteComLink();
        CL_CancelPendingReads(ext->Shared);
        ext->Started = FALSE;
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->NextLowerDriver, Irp);
        IoDetachDevice(ext->NextLowerDriver);
        IoDeleteDevice(DeviceObject);
        DbgPrint("[CommLinkPnp] REMOVE_DEVICE\n");
        return status;

    case IRP_MN_SURPRISE_REMOVAL:
        CL_DeleteComLink();
        CL_CancelPendingReads(ext->Shared);
        ext->Started = FALSE;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->NextLowerDriver, Irp);

    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    default:
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->NextLowerDriver, Irp);
    }
}

static NTSTATUS
CL_Power(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PCOMMLINK_EXT ext = (PCOMMLINK_EXT)DeviceObject->DeviceExtension;
    PoStartNextPowerIrp(Irp);
    if (ext->NextLowerDriver) {
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(ext->NextLowerDriver, Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
CL_AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT  fdo = NULL;
    PCOMMLINK_EXT   ext;
    UNICODE_STRING  ntName;
    NTSTATUS        status;

    // FDO 使用固定 NT 名,供 \\.\COMn 符号链接指向
    RtlInitUnicodeString(&ntName, COMMLINK_NT_DEVICE_COM);
    status = IoCreateDevice(DriverObject, sizeof(COMMLINK_EXT),
                            &ntName, FILE_DEVICE_SERIAL_PORT,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &fdo);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[CommLinkPnp] IoCreateDevice(FDO) failed 0x%08X\n", status);
        return status;
    }

    ext = (PCOMMLINK_EXT)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(COMMLINK_EXT));
    ext->Shared = &g_Global.Shared;
    ext->IsControlDevice = FALSE;
    ext->Pdo = PhysicalDeviceObject;

    ext->NextLowerDriver = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (!ext->NextLowerDriver) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    fdo->Flags |= DO_BUFFERED_IO;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    g_Global.ComFdo = fdo;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// 初始化 / 清理
// ---------------------------------------------------------------------------
static NTSTATUS
CreateRing(PRING_BUFFER Ring, ULONG Capacity)
{
    Ring->Buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Capacity, COMMLINK_TAG);
    if (!Ring->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    Ring->Capacity = Capacity;
    Ring->Head = Ring->Tail = Ring->Length = 0;
    KeInitializeSpinLock(&Ring->Lock);
    return STATUS_SUCCESS;
}

static VOID
FreeRing(PRING_BUFFER Ring)
{
    if (Ring->Buffer) { ExFreePoolWithTag(Ring->Buffer, COMMLINK_TAG); Ring->Buffer = NULL; }
}

static ULONG
ReadComPortFromRegistry(VOID)
{
    HANDLE hKey = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING subKey;
    UNICODE_STRING valueName;
    UCHAR raw[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION pvi = (PKEY_VALUE_PARTIAL_INFORMATION)raw;
    ULONG result = 0, len;

    RtlInitUnicodeString(&subKey,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\CommLinkPnp\\Parameters");
    InitializeObjectAttributes(&oa, &subKey, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&hKey, KEY_READ, &oa))) return 8;

    RtlInitUnicodeString(&valueName, COMMLINK_REG_COMPORT);
    if (NT_SUCCESS(ZwQueryValueKey(hKey, &valueName, KeyValuePartialInformation,
                                   raw, sizeof(raw), &len))) {
        if (pvi->Type == REG_DWORD && pvi->DataLength >= sizeof(ULONG)) {
            result = *(PULONG)pvi->Data;
        }
    }
    ZwClose(hKey);
    return (result >= 1 && result <= 255) ? result : 8;
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS        status;
    PDEVICE_OBJECT  ctlDev = NULL;
    UNICODE_STRING  ctlName, ctlLink;
    ULONG           i;
    PDEVICE_EXTENSION Shared = &g_Global.Shared;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlZeroMemory(&g_Global, sizeof(g_Global));
    g_Global.ComPortNumber = ReadComPortFromRegistry();
    BuildComSymbolicName(g_Global.ComSymbolicBuf, g_Global.ComPortNumber);
    g_Global.ComSymbolicName.Buffer = g_Global.ComSymbolicBuf;
    g_Global.ComSymbolicName.Length = (USHORT)(WcslenFast(g_Global.ComSymbolicBuf) * sizeof(WCHAR));
    g_Global.ComSymbolicName.MaximumLength = sizeof(g_Global.ComSymbolicBuf);

    Shared->BaudRate = 9600; Shared->ByteSize = 8; Shared->Parity = 0; Shared->StopBits = 0;
    InitializeListHead(&Shared->PendingReads);
    KeInitializeSpinLock(&Shared->PendingLock);
    KeInitializeSpinLock(&Shared->TxRing.Lock);
    KeInitializeSpinLock(&Shared->RxRing.Lock);
    KeInitializeSpinLock(&Shared->TxEventLock);

    status = CreateRing(&Shared->TxRing, COMMLINK_DEFAULT_CAPACITY);
    if (!NT_SUCCESS(status)) goto fail;
    status = CreateRing(&Shared->RxRing, COMMLINK_DEFAULT_CAPACITY);
    if (!NT_SUCCESS(status)) goto fail;

    // ---- 控制设备 \\.\CommLinkCtl (不属于 PnP 树,用户态接口不变) ----
    RtlInitUnicodeString(&ctlName, COMMLINK_NT_DEVICE_CTL);
    status = IoCreateDevice(DriverObject, sizeof(COMMLINK_EXT), &ctlName,
                            FILE_DEVICE_COMMLINK, FILE_DEVICE_SECURE_OPEN,
                            FALSE, &ctlDev);
    if (!NT_SUCCESS(status)) goto fail;
    {
        PCOMMLINK_EXT e = (PCOMMLINK_EXT)ctlDev->DeviceExtension;
        RtlZeroMemory(e, sizeof(COMMLINK_EXT));
        e->Shared = Shared;
        e->IsControlDevice = TRUE;
    }
    g_Global.CtlDevice = ctlDev;
    RtlInitUnicodeString(&ctlLink, COMMLINK_DOS_DEVICE_CTL);
    status = IoCreateSymbolicLink(&ctlLink, &ctlName);
    if (!NT_SUCCESS(status)) goto fail;
    ctlDev->Flags |= DO_BUFFERED_IO;
    ctlDev->Flags &= ~DO_DEVICE_INITIALIZING;

    // ---- 分发函数 ----
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = CL_Passthru;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE]           = CL_Create;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]            = CL_Close;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]          = CL_Cleanup;
    DriverObject->MajorFunction[IRP_MJ_READ]             = CL_Read;
    DriverObject->MajorFunction[IRP_MJ_WRITE]            = CL_Write;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]   = CL_DeviceControl;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]    = CL_Flush;
    DriverObject->MajorFunction[IRP_MJ_PNP]              = CL_Pnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]            = CL_Power;

    DriverObject->DriverExtension->AddDevice = CL_AddDevice;
    DriverObject->DriverUnload = CL_Unload;

    DbgPrint("[CommLinkPnp] DriverEntry OK. COM%u target\n", g_Global.ComPortNumber);
    return STATUS_SUCCESS;

fail:
    DbgPrint("[CommLinkPnp] DriverEntry failed 0x%08X\n", status);
    {
        UNICODE_STRING tmp;
        RtlInitUnicodeString(&tmp, COMMLINK_DOS_DEVICE_CTL);
        IoDeleteSymbolicLink(&tmp);
    }
    if (ctlDev) IoDeleteDevice(ctlDev);
    FreeRing(&Shared->TxRing);
    FreeRing(&Shared->RxRing);
    return status;
}

static VOID
CL_Unload(PDRIVER_OBJECT DriverObject)
{
    PDEVICE_EXTENSION Shared = &g_Global.Shared;
    UNICODE_STRING ctlLink;
    KIRQL irql;

    UNREFERENCED_PARAMETER(DriverObject);

    CL_CancelPendingReads(Shared);

    KeAcquireSpinLock(&Shared->TxEventLock, &irql);
    if (Shared->TxEvent) { ObDereferenceObject(Shared->TxEvent); Shared->TxEvent = NULL; }
    KeReleaseSpinLock(&Shared->TxEventLock, irql);

    CL_DeleteComLink();
    RtlInitUnicodeString(&ctlLink, COMMLINK_DOS_DEVICE_CTL);
    IoDeleteSymbolicLink(&ctlLink);
    if (g_Global.CtlDevice) IoDeleteDevice(g_Global.CtlDevice);

    FreeRing(&Shared->TxRing);
    FreeRing(&Shared->RxRing);
    DbgPrint("[CommLinkPnp] Unloaded.\n");
}
