#pragma once

extern "C" {
    #include <wdm.h>
}

/**
 * @brief Increment the pipe_count with spinlock protection
 * 
 * @param DeviceObject 
 */
void spinlock_increment(PDEVICE_OBJECT DeviceObject);

/**
 * @brief Decrement the pipe_count with spinlock protection and notify events
 * 
 * @param DeviceObject 
 */
LONG spinlock_decrement_notify(PDEVICE_OBJECT DeviceObject);

/**
 * @brief Decrement the pipe_count with spinlock protection
 * 
 * @param DeviceObject 
 * @return LONG 
 */
LONG spinlock_decrement(PDEVICE_OBJECT DeviceObject);

/**
 * @brief Get the current pipe_count with spinlock protection
 * 
 * @param DeviceObject 
 * @return LONG 
 */
LONG spinlock_get_count(PDEVICE_OBJECT DeviceObject);