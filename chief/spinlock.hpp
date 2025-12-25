#pragma once

extern "C" {
    #include <wdm.h>
}

/**
 * @brief Aquire the spinlock for the device object
 * 
 * @param DeviceObject 
 */
void spinlock_increment(PDEVICE_OBJECT DeviceObject);

/**
 * @brief Release the spinlock for the device object
 * 
 * @param DeviceObject 
 */
LONG spinlock_decrement(PDEVICE_OBJECT DeviceObject);