#pragma once

//
// shared.h - Shared definitions between kernel driver and usermode client.
//

#define PROCFINDER_DEVICE_NAME  L"\\Device\\ProcFinder"
#define PROCFINDER_SYMLINK_NAME L"\\DosDevices\\ProcFinder"
#define PROCFINDER_WIN32_DEVICE L"\\\\.\\ProcFinder"

#define PROC_NAME_MAX_LEN 260

//
// IOCTL: get PID + base address for a named process
//
#define IOCTL_PROCFINDER_GET_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Sent from usermode → driver
typedef struct _PROCFINDER_REQUEST {
    WCHAR ProcessName[PROC_NAME_MAX_LEN];
} PROCFINDER_REQUEST, *PPROCFINDER_REQUEST;

// Returned from driver → usermode
typedef struct _PROCFINDER_RESPONSE {
    ULONG     Found;        // 1 = found, 0 = not found
    ULONG     ProcessId;
    ULONG64   BaseAddress;  // base address of the main executable module
} PROCFINDER_RESPONSE, *PPROCFINDER_RESPONSE;
