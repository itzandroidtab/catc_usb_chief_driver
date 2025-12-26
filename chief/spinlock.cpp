#include "spinlock.hpp"
#include "device_extension.hpp"

void increment_active_pipe_count(PDEVICE_OBJECT DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);
    
    // acquire the spinlock
    KIRQL irql;
    KeAcquireSpinLock(&dev_ext->device_lock, &irql);

    // increment the lock count
    InterlockedIncrement(&dev_ext->active_pipe_count);

    // release the spinlock
    KeReleaseSpinLock(&dev_ext->device_lock, irql);
}

LONG decrement_active_pipe_count_and_notify(PDEVICE_OBJECT DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    // acquire the spinlock
    KIRQL irql;
    KeAcquireSpinLock(&dev_ext->device_lock, &irql);
    
    // decrement the lock count
    LONG new_count = InterlockedDecrement(&dev_ext->active_pipe_count);

    // check if we need to set and event
    switch (new_count) {
        case 0:
            KeSetEvent(&dev_ext->pipe_count_empty, EVENT_INCREMENT, false);
            break;
        default:
            // do nothing on the default case
            break;
    }

    // release the spinlock
    KeReleaseSpinLock(&dev_ext->device_lock, irql);

    // return the new count
    return new_count;
}

LONG decrement_active_pipe_count(PDEVICE_OBJECT DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    // acquire the spinlock
    KIRQL irql;
    KeAcquireSpinLock(&dev_ext->device_lock, &irql);
    
    // decrement the lock count
    LONG new_count = InterlockedDecrement(&dev_ext->active_pipe_count);

    // release the spinlock
    KeReleaseSpinLock(&dev_ext->device_lock, irql);

    // return the new count
    return new_count;
}
