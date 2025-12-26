#pragma once

extern "C" {
    #include <wdm.h>
}

/**
 * @brief Increment the pipe_count with spinlock protection
 * 
 * @param DeviceObject 
 */
void increment_active_pipe_count(PDEVICE_OBJECT DeviceObject);

/**
 * @brief Decrement the pipe_count with spinlock protection and notify events
 * 
 * @param DeviceObject 
 */
LONG decrement_active_pipe_count_and_notify(PDEVICE_OBJECT DeviceObject);

/**
 * @brief Decrement the pipe_count with spinlock protection
 * 
 * @param DeviceObject 
 * @return LONG 
 */
LONG decrement_active_pipe_count(PDEVICE_OBJECT DeviceObject);
