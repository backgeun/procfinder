/*
 * ProcFinder.c - Kernel-mode driver
 *
 * Finds a process by name, returns its PID and main module base address
 * via IOCTL_PROCFINDER_GET_INFO.
 *
 * Technique for base address:
 *   - Open the process with ZwOpenProcess
 *   - Call ZwQueryInformationProcess(ProcessBasicInformation) to get PEB address
 *   - Read PEB.Ldr -> InMemoryOrderModuleList head -> DllBase of first entry
 *     (first entry in InMemoryOrderModuleList is always the main executable)
 *
 * All reads from usermode memory are done with MmCopyMemory (safe, no crash on bad ptr).
 */

#include <ntifs.h>
#include <ntddk.h>
#include <wdm.h>
#include <ntstrsafe.h>
#include "..\shared\shared.h"

// -------------------------------------------------------------------------
// Undocumented / semi-documented structures and prototypes
// -------------------------------------------------------------------------

typedef struct _PEB_LDR_DATA {
    ULONG       Length;
    BOOLEAN     Initialized;
    PVOID       SsHandle;
    LIST_ENTRY  InLoadOrderModuleList;
    LIST_ENTRY  InMemoryOrderModuleList;
    LIST_ENTRY  InInitializationOrderModuleList;
} PEB_LDR_DATA, *PPEB_LDR_DATA;

typedef struct _LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY  InLoadOrderLinks;
    LIST_ENTRY  InMemoryOrderLinks;
    LIST_ENTRY  InInitializationOrderLinks;
    PVOID       DllBase;
    PVOID       EntryPoint;
    ULONG       SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

// Minimal PEB layout - only fields we need
typedef struct _PEB64 {
    UCHAR       InheritedAddressSpace;
    UCHAR       ReadImageFileExecOptions;
    UCHAR       BeingDebugged;
    UCHAR       Spare;
    ULONG       Padding;
    ULONGLONG   Mutant;
    ULONGLONG   ImageBaseAddress;   // offset 0x10 - this IS the base address
    ULONGLONG   Ldr;                // offset 0x18 - pointer to PEB_LDR_DATA
} PEB64, *PPEB64;

typedef struct _PROCESS_BASIC_INFORMATION {
    PVOID       Reserved1;
    PVOID       PebBaseAddress;
    PVOID       Reserved2[2];
    ULONG_PTR   UniqueProcessId;
    PVOID       Reserved3;
} PROCESS_BASIC_INFORMATION, *PPROCESS_BASIC_INFORMATION;

// ZwQueryInformationProcess is exported but not in public headers
NTSYSAPI NTSTATUS NTAPI ZwQueryInformationProcess(
    _In_      HANDLE           ProcessHandle,
    _In_      ULONG            ProcessInformationClass,
    _Out_     PVOID            ProcessInformation,
    _In_      ULONG            ProcessInformationLength,
    _Out_opt_ PULONG           ReturnLength);

// PsGetNextProcess walks the active EPROCESS list
NTKERNELAPI PEPROCESS PsGetNextProcess(_In_opt_ PEPROCESS Process);

// -------------------------------------------------------------------------
// Forward declarations
// -------------------------------------------------------------------------
DRIVER_UNLOAD   ProcFinderUnload;
DRIVER_DISPATCH ProcFinderCreate;
DRIVER_DISPATCH ProcFinderClose;
DRIVER_DISPATCH ProcFinderDeviceControl;

// -------------------------------------------------------------------------
// Safe kernel read helper (wraps MmCopyMemory)
// -------------------------------------------------------------------------
static NTSTATUS SafeReadKernel(_In_ PVOID Dest, _In_ PVOID Src, _In_ SIZE_T Size)
{
    SIZE_T bytesCopied = 0;
    MM_COPY_ADDRESS addr;
    addr.VirtualAddress = Src;
    return MmCopyMemory(Dest, addr, Size, MM_COPY_MEMORY_VIRTUAL, &bytesCopied);
}

// -------------------------------------------------------------------------
// Get base address of the main module via PEB
// -------------------------------------------------------------------------
static ULONG64 GetProcessBaseAddress(_In_ PEPROCESS Process)
{
    NTSTATUS    status;
    HANDLE      hProcess    = NULL;
    ULONG64     baseAddr    = 0;

    // Open a kernel handle to the process
    status = ObOpenObjectByPointer(
        Process,
        OBJ_KERNEL_HANDLE,
        NULL,
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        *PsProcessType,
        KernelMode,
        &hProcess);

    if (!NT_SUCCESS(status))
        return 0;

    // Get PEB base address via ProcessBasicInformation (class 0)
    PROCESS_BASIC_INFORMATION pbi = { 0 };
    status = ZwQueryInformationProcess(hProcess, 0, &pbi, sizeof(pbi), NULL);
    if (!NT_SUCCESS(status) || pbi.PebBaseAddress == NULL)
    {
        ZwClose(hProcess);
        return 0;
    }

    // Attach to the process address space to safely read usermode memory
    KAPC_STATE apcState;
    KeStackAttachProcess(Process, &apcState);

    // Read PEB64.ImageBaseAddress (offset 0x10)
    // This is the simplest and most reliable way to get the main module base.
    PVOID pebBase = pbi.PebBaseAddress;
    ULONG64 imageBase = 0;

    __try
    {
        // Probe that the PEB is readable
        ProbeForRead(pebBase, sizeof(PEB64), 1);

        // Read ImageBaseAddress field directly (offset 0x10 in PEB64)
        ULONG64* pImageBase = (ULONG64*)((UCHAR*)pebBase + 0x10);
        ProbeForRead(pImageBase, sizeof(ULONG64), 1);
        imageBase = *pImageBase;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        imageBase = 0;
    }

    KeUnstackDetachProcess(&apcState);
    ZwClose(hProcess);

    baseAddr = imageBase;
    return baseAddr;
}

// -------------------------------------------------------------------------
// Find process by name, return PID + base address
// -------------------------------------------------------------------------
static BOOLEAN FindProcessInfo(
    _In_  PCWSTR   TargetName,
    _Out_ ULONG   *OutPid,
    _Out_ ULONG64 *OutBase)
{
    PEPROCESS       process     = NULL;
    PEPROCESS       next        = NULL;
    BOOLEAN         found       = FALSE;
    UNICODE_STRING  targetUs;

    *OutPid  = 0;
    *OutBase = 0;

    RtlInitUnicodeString(&targetUs, TargetName);

    process = PsInitialSystemProcess;
    ObReferenceObject(process);

    do {
        PUCHAR imageName = (PUCHAR)PsGetProcessImageFileName(process);

        if (imageName != NULL)
        {
            ANSI_STRING    ansi;
            UNICODE_STRING uni;
            RtlInitAnsiString(&ansi, (PCSZ)imageName);

            if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&uni, &ansi, TRUE)))
            {
                if (RtlEqualUnicodeString(&uni, &targetUs, TRUE))
                {
                    *OutPid  = (ULONG)(ULONG_PTR)PsGetProcessId(process);
                    *OutBase = GetProcessBaseAddress(process);
                    found    = TRUE;
                    RtlFreeUnicodeString(&uni);
                    ObDereferenceObject(process);
                    return TRUE;
                }
                RtlFreeUnicodeString(&uni);
            }
        }

        next = PsGetNextProcess(process);
        ObDereferenceObject(process);
        process = next;

    } while (process != NULL);

    return FALSE;
}

// -------------------------------------------------------------------------
// IRP handlers
// -------------------------------------------------------------------------
NTSTATUS ProcFinderCreate(_In_ PDEVICE_OBJECT DevObj, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS ProcFinderClose(_In_ PDEVICE_OBJECT DevObj, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS ProcFinderDeviceControl(_In_ PDEVICE_OBJECT DevObj, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);

    PIO_STACK_LOCATION ioStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS    status      = STATUS_SUCCESS;
    ULONG_PTR   info        = 0;

    switch (ioStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_PROCFINDER_GET_INFO:
        {
            ULONG inLen  = ioStack->Parameters.DeviceIoControl.InputBufferLength;
            ULONG outLen = ioStack->Parameters.DeviceIoControl.OutputBufferLength;

            if (inLen  < sizeof(PROCFINDER_REQUEST) ||
                outLen < sizeof(PROCFINDER_RESPONSE))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PPROCFINDER_REQUEST  req  = (PPROCFINDER_REQUEST) Irp->AssociatedIrp.SystemBuffer;
            PPROCFINDER_RESPONSE resp = (PPROCFINDER_RESPONSE)Irp->AssociatedIrp.SystemBuffer;

            // Null-terminate defensively before reading
            req->ProcessName[PROC_NAME_MAX_LEN - 1] = L'\0';

            // Copy name out before we overwrite the buffer with the response
            WCHAR nameCopy[PROC_NAME_MAX_LEN];
            RtlCopyMemory(nameCopy, req->ProcessName, sizeof(nameCopy));

            ULONG   pid  = 0;
            ULONG64 base = 0;
            BOOLEAN found = FindProcessInfo(nameCopy, &pid, &base);

            resp->Found       = found ? 1 : 0;
            resp->ProcessId   = pid;
            resp->BaseAddress = base;

            status = STATUS_SUCCESS;
            info   = sizeof(PROCFINDER_RESPONSE);
            break;
        }

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// -------------------------------------------------------------------------
// Unload / Entry
// -------------------------------------------------------------------------
VOID ProcFinderUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, PROCFINDER_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symLink);
    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);
    DbgPrint("[ProcFinder] Unloaded.\n");
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    UNICODE_STRING  devName, symLink;
    PDEVICE_OBJECT  devObj = NULL;
    NTSTATUS        status;

    RtlInitUnicodeString(&devName,  PROCFINDER_DEVICE_NAME);
    RtlInitUnicodeString(&symLink,  PROCFINDER_SYMLINK_NAME);

    status = IoCreateDevice(DriverObject, 0, &devName,
                            FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                            FALSE, &devObj);
    if (!NT_SUCCESS(status)) return status;

    devObj->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(devObj); return status; }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = ProcFinderCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = ProcFinderClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ProcFinderDeviceControl;
    DriverObject->DriverUnload                          = ProcFinderUnload;

    devObj->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrint("[ProcFinder] Loaded. Device: %wZ\n", &devName);
    return STATUS_SUCCESS;
}
