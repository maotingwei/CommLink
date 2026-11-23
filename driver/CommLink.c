/*++
    CommLink.c
    =====================================================================
    虚拟串口驱动 (Legacy WDM, x86/x64)

    功能:
      1. 虚拟 COM 口设备(如 \\.\COM8):
           串口助手 WriteFile  -> TxRing -> 通知用户态服务取走
           用户态写 RxRing     -> 串口助手 ReadFile 返回
      2. 控制设备 \\.\CommLinkCtl,供用户态服务交换数据:
           GET_TX / PUT_RX / SET_EVENT / GET_STATUS / PURGE / GET_VERSION

    关键设计: 两个设备对象共用同一个 DEVICE_EXTENSION(全局 g_Shared),
              以保证事件通知与环形缓冲在两条路径间真正共享。

    编译: 需要 WDK。见 driver/build.bat。
    =====================================================================
--*/

#include <ntddk.h>
#include <ntddser.h>
#include "CommLinkCommon.h"

#define COMMLINK_TAG  'KNLC'

// 未文档化的内核导出:事件对象类型,供 ObReferenceObjectByHandle 使用
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

    UNICODE_STRING  ComSymbolicName;
    WCHAR           ComSymbolicBuf[64];

    ULONG           TotalTxBytes;
    ULONG           TotalRxBytes;
    LONG            OpenHandles;

    BOOLEAN         IsControlDevice;   // 各设备对象需要区分 -> 见下方说明

    ULONG           BaudRate;
    UCHAR           ByteSize;
    UCHAR           Parity;
    UCHAR           StopBits;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

// 由于两个设备对象必须共享数据又要区分角色,采用:
// 全局共享 SHARED 存放数据;每个设备对象自己的扩展只存一个指向 SHARED 的指针和角色标志。
typedef struct _OBJ_EXT {
    PDEVICE_EXTENSION Shared;
    BOOLEAN           IsControlDevice;
} OBJ_EXT, *POBJ_EXT;

typedef struct _GLOBAL_STATE {
    PDEVICE_OBJECT  ComDevice;
    PDEVICE_OBJECT  CtlDevice;
    DEVICE_EXTENSION Shared;
    ULONG           ComPortNumber;
} GLOBAL_STATE, *PGLOBAL_STATE;

static GLOBAL_STATE g_Global;

// ---------------------------------------------------------------------------
// 前向声明
// ---------------------------------------------------------------------------

DRIVER_INITIALIZE DriverEntry;
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
// 工具:内核模式不能调用 CRT 的 swprintf,手动把 COM 号拼进符号链接名
// ---------------------------------------------------------------------------
static VOID
BuildComSymbolicName(PWCHAR Buf, ULONG Port)
{
    static const WCHAR prefix[] = L"\\DosDevices\\COM";
    ULONG i, n, digits[12], t = 0;

    for (i = 0; prefix[i] != L'\0'; i++) {
        Buf[i] = prefix[i];
    }
    n = i;
    if (Port == 0) {
        digits[t++] = 0;
    }
    while (Port > 0) {
        digits[t++] = Port % 10;
        Port /= 10;
    }
    while (t > 0) {
        Buf[n++] = (WCHAR)(L'0' + digits[--t]);
    }
    Buf[n] = L'\0';
}

// ---------------------------------------------------------------------------
// 环形缓冲 (调用者持有 Ring->Lock, IRQL <= DISPATCH_LEVEL)
// ---------------------------------------------------------------------------

static BOOLEAN
RingPush(PRING_BUFFER Ring, PUCHAR Data, ULONG Length)
{
    ULONG i;
    if (Ring->Length + Length > Ring->Capacity) {
        return FALSE;
    }
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
// 完成一个挂起的串口助手读请求
// ---------------------------------------------------------------------------

static VOID
CompleteOnePendingRead(PDEVICE_EXTENSION Shared)
{
    PIRP    Irp = NULL;
    KIRQL   cancelIrql;
    ULONG   copied = 0;

    IoAcquireCancelSpinLock(&cancelIrql);
    KeAcquireSpinLockAtDpcLevel(&Shared->PendingLock);

    // 只有 RX 有数据且队列非空时才完成一个
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

// ---------------------------------------------------------------------------
// 取消所有挂起读请求
// ---------------------------------------------------------------------------

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
    POBJ_EXT    ext = (POBJ_EXT)DeviceObject->DeviceExtension;
    PDEVICE_EXTENSION Shared = ext->Shared;
    KIRQL irql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Shared->PendingLock, &irql);
    if (Irp->Tail.Overlay.ListEntry.Flink != NULL) {
        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    }
    KeReleaseSpinLock(&Shared->PendingLock, irql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

// ---------------------------------------------------------------------------
// IRP 处理
// ---------------------------------------------------------------------------

static NTSTATUS
CL_Create(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    POBJ_EXT ext = (POBJ_EXT)DeviceObject->DeviceExtension;
    InterlockedIncrement(&ext->Shared->OpenHandles);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
CL_Close(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    POBJ_EXT ext = (POBJ_EXT)DeviceObject->DeviceExtension;
    InterlockedDecrement(&ext->Shared->OpenHandles);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
CL_Cleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    POBJ_EXT ext = (POBJ_EXT)DeviceObject->DeviceExtension;
    // 仅当 COM 口最后一个句柄关闭时取消读;这里简单起见统一取消。
    CL_CancelPendingReads(ext->Shared);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
CL_Read(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    POBJ_EXT            ext = (POBJ_EXT)DeviceObject->DeviceExtension;
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

    // 无数据:挂起 IRP(cancel-safe)
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
    POBJ_EXT            ext = (POBJ_EXT)DeviceObject->DeviceExtension;
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
        if (Shared->TxEvent) {
            KeSetEvent(Shared->TxEvent, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&Shared->TxEventLock, irql);
    }

    Irp->IoStatus.Status = ok ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
    Irp->IoStatus.Information = ok ? len : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

// 串口助手侧 IOCTL_SERIAL_* :给合理默认值,保证配置阶段不失败
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
        // 其余 SET_*/PURGE/RESET/STATS 等一律成功
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
        else {
            info = inLen;
            CompleteOnePendingRead(Shared);
        }
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
        RtlZeroMemory(&st, sizeof(st));
        KeAcquireSpinLock(&Shared->TxRing.Lock, &irql);
        st.TxLength = Shared->TxRing.Length;
        st.TxCapacity = Shared->TxRing.Capacity;
        KeReleaseSpinLock(&Shared->TxRing.Lock, irql);
        KeAcquireSpinLock(&Shared->RxRing.Lock, &irql);
        st.RxLength = Shared->RxRing.Length;
        st.RxCapacity = Shared->RxRing.Capacity;
        KeReleaseSpinLock(&Shared->RxRing.Lock, irql);
        {
            PLIST_ENTRY le;
            KeAcquireSpinLock(&Shared->PendingLock, &irql);
            st.PendingReads = 0;
            for (le = Shared->PendingReads.Flink;
                 le != &Shared->PendingReads; le = le->Flink) st.PendingReads++;
            KeReleaseSpinLock(&Shared->PendingLock, irql);
        }
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
    POBJ_EXT    ext = (POBJ_EXT)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sl->Parameters.DeviceIoControl.IoControlCode;
    if (ext->IsControlDevice) {
        return HandleControlIoctl(ext->Shared, Irp, code);
    }
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

// ---------------------------------------------------------------------------
// 初始化 / 卸载
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
    UCHAR raw[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION pvi = (PKEY_VALUE_PARTIAL_INFORMATION)raw;
    ULONG result = 0, len;

    RtlInitUnicodeString(&subKey,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\CommLink\\Parameters");
    InitializeObjectAttributes(&oa, &subKey, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&hKey, KEY_READ, &oa))) return 8; // 默认 COM8

    if (NT_SUCCESS(ZwQueryValueKey(hKey,
            RTL_CONSTANT_STRING(COMMLINK_REG_COMPORT),
            KeyValuePartialInformation, raw, sizeof(raw), &len))) {
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
    NTSTATUS status;
    PDEVICE_OBJECT comDev = NULL, ctlDev = NULL;
    UNICODE_STRING comName, ctlName, symbolic;
    ULONG i;
    PDEVICE_EXTENSION Shared = &g_Global.Shared;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlZeroMemory(&g_Global, sizeof(g_Global));
    g_Global.ComPortNumber = ReadComPortFromRegistry();

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

    // 虚拟 COM 设备(每个设备对象扩展仅存角色+共享指针)
    RtlInitUnicodeString(&comName, COMMLINK_NT_DEVICE_COM);
    status = IoCreateDevice(DriverObject, sizeof(OBJ_EXT), &comName,
                            FILE_DEVICE_SERIAL_PORT, FILE_DEVICE_SECURE_OPEN,
                            FALSE, &comDev);
    if (!NT_SUCCESS(status)) goto fail;
    {
        POBJ_EXT e = (POBJ_EXT)comDev->DeviceExtension;
        e->Shared = Shared; e->IsControlDevice = FALSE;
    }
    g_Global.ComDevice = comDev;

    BuildComSymbolicName(Shared->ComSymbolicBuf, g_Global.ComPortNumber);
    RtlInitUnicodeString(&symbolic, Shared->ComSymbolicBuf);
    status = IoCreateSymbolicLink(&symbolic, &comName);
    if (!NT_SUCCESS(status)) goto fail;
    Shared->ComSymbolicName = symbolic; // ComSymbolicBuf 在全局结构内,生命周期安全

    // 控制设备
    RtlInitUnicodeString(&ctlName, COMMLINK_NT_DEVICE_CTL);
    status = IoCreateDevice(DriverObject, sizeof(OBJ_EXT), &ctlName,
                            FILE_DEVICE_COMMLINK, FILE_DEVICE_SECURE_OPEN,
                            FALSE, &ctlDev);
    if (!NT_SUCCESS(status)) goto fail;
    {
        POBJ_EXT e = (POBJ_EXT)ctlDev->DeviceExtension;
        e->Shared = Shared; e->IsControlDevice = TRUE;
    }
    g_Global.CtlDevice = ctlDev;

    RtlInitUnicodeString(&symbolic, COMMLINK_DOS_DEVICE_CTL);
    status = IoCreateSymbolicLink(&symbolic, &ctlName);
    if (!NT_SUCCESS(status)) goto fail;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = CL_DeviceControl;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE]           = CL_Create;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]            = CL_Close;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]          = CL_Cleanup;
    DriverObject->MajorFunction[IRP_MJ_READ]             = CL_Read;
    DriverObject->MajorFunction[IRP_MJ_WRITE]            = CL_Write;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]   = CL_DeviceControl;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]    = CL_Flush;
    DriverObject->DriverUnload = CL_Unload;

    comDev->Flags |= DO_BUFFERED_IO;
    ctlDev->Flags |= DO_BUFFERED_IO;
    comDev->Flags &= ~DO_DEVICE_INITIALIZING;
    ctlDev->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrint("[CommLink] Loaded. COM%u + \\\\.\\CommLinkCtl\n", g_Global.ComPortNumber);
    return STATUS_SUCCESS;

fail:
    DbgPrint("[CommLink] DriverEntry failed 0x%08X\n", status);
    if (Shared->ComSymbolicName.Buffer) IoDeleteSymbolicLink(&Shared->ComSymbolicName);
    {
        UNICODE_STRING tmp;
        RtlInitUnicodeString(&tmp, COMMLINK_DOS_DEVICE_CTL);
        IoDeleteSymbolicLink(&tmp);
    }
    if (ctlDev) IoDeleteDevice(ctlDev);
    if (comDev) IoDeleteDevice(comDev);
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

    if (Shared->ComSymbolicName.Buffer) {
        IoDeleteSymbolicLink(&Shared->ComSymbolicName);
        Shared->ComSymbolicName.Buffer = NULL;
    }
    RtlInitUnicodeString(&ctlLink, COMMLINK_DOS_DEVICE_CTL);
    IoDeleteSymbolicLink(&ctlLink);

    if (g_Global.CtlDevice) IoDeleteDevice(g_Global.CtlDevice);
    if (g_Global.ComDevice) IoDeleteDevice(g_Global.ComDevice);
    FreeRing(&Shared->TxRing);
    FreeRing(&Shared->RxRing);

    DbgPrint("[CommLink] Unloaded.\n");
}
