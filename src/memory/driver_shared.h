#pragma once

#include "driver_identity.h"

#include <Windows.h>
#include <winioctl.h>

#define DRV_USER_PATH         DRV_USER_DEVICE_PATH
#define DRV_FILE_NAME         DRV_SYS_FILE_NAME
#define DRV_MAPPER_NAME       DRV_MAPPER_EXE_NAME

#define IOCTL_READ_MEMORY     CTL_CODE(DRV_IOCTL_DEVICE_TYPE, DRV_IOCTL_FUNC_READ,        METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GET_MODULE_BASE CTL_CODE(DRV_IOCTL_DEVICE_TYPE, DRV_IOCTL_FUNC_MODULE_BASE, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PING            CTL_CODE(DRV_IOCTL_DEVICE_TYPE, DRV_IOCTL_FUNC_PING,        METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WRITE_MEMORY    CTL_CODE(DRV_IOCTL_DEVICE_TYPE, DRV_IOCTL_FUNC_WRITE,       METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GET_PEB         CTL_CODE(DRV_IOCTL_DEVICE_TYPE, DRV_IOCTL_FUNC_GET_PEB,     METHOD_BUFFERED, FILE_ANY_ACCESS)

#define PING_MAGIC            DRV_PING_MAGIC
#define DRIVER_MAX_READ_SIZE  0x10000u
#define DRIVER_MAX_WRITE_SIZE 0x10000u

#pragma pack(push, 1)
struct ReadMemoryRequest {
    unsigned long    target_pid;
    unsigned long long source_address;
    unsigned long    read_size;
    unsigned long    padding;
};

struct ModuleBaseRequest {
    unsigned long    target_pid;
    wchar_t          module_name[256];
    unsigned long long base_address;
    unsigned long    module_size;
    unsigned long    padding;
};

struct PingResponse {
    unsigned long long magic;
};

struct WriteMemoryRequest {
    unsigned long    target_pid;
    unsigned long long dest_address;
    unsigned long    write_size;
    unsigned long    padding;
};

struct GetPebRequest {
    unsigned long    target_pid;
    unsigned long long peb_address;
    unsigned long    padding;
    unsigned long    padding2;
};
#pragma pack(pop)
