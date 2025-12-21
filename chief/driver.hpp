#pragma once

extern "C" {
    #include <wdm.h>
}

/**
 * @brief Driver entry point.
 * 
 * @param DriverObject 
 * @param RegistryPath 
 * @return NTSTATUS 
 */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);