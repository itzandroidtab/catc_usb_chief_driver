#pragma once

extern "C" {
    #include <wdm.h>
    #include <usb.h>
    #include <usbdi.h>
}

#include "maybe.hpp"

/**
 * @brief Payload we are receiving/sending from the 
 * application in a mj device control
 * 
 */
struct usb_chief_vendor_request {
    // usb request fields. Is sometimes used as input 
    // and as output
    unsigned short request;

    // usb specific fields
    unsigned short value;
    unsigned short index;

    // length of the data buffer
    unsigned short length;

    // pointer to the data buffer
    void *data;
};

/**
 * @brief Device extension structure
 * 
 */
struct chief_device_extension {
    // the device objects we are connected to
    PDEVICE_OBJECT attachedDeviceObject;
    PDEVICE_OBJECT physicalDeviceObject;

    // the current power state of the device
    POWER_STATE current_power_state;

    // the current usb configuration descriptor
    PUSB_CONFIGURATION_DESCRIPTOR usb_config_desc;

    // the current usb interface information
    PUSBD_INTERFACE_INFORMATION usb_interface_info;

    // the device capabilities structure. This is used
    // to know what power state we need to go to for each
    // system power state
    DEVICE_CAPABILITIES device_capabilities;

    // event to signal when the pipe count is zero. This
    // can only happen when all opened pipes are closed
    // and IRP_MN_REMOVE_DEVICE is called
    KEVENT pipe_count_empty;

    // spinlock to protect the active_pipe_count
    KSPIN_LOCK device_lock;

    // count of opened pipes
    LONG active_pipe_count;

    // an array with flags for each pipe if it is opened 
    // or not
    bool *allocated_pipes;

    // flag if the device has been removed. This means
    // that we cannot accept new ioctls/reads/writes.
    // When this flag is set we cannot talk to the device 
    // anymore
    volatile bool device_removed;

    // flag if the device is remove pending. This means
    // that we should not accept new ioctls/reads/writes
    // and that the device should be considered as being 
    // removed until it is deleted or canceled
    volatile bool remove_pending;

    // flag if new requests should be held. This is used during
    // stop device to prevent new ioctls/reads/writes from
    // being processed until the device is started again
    volatile bool hold_new_requests;

    // The BCD version of the connected USB device
    maybe<unsigned short> bcdUSB;

    // count of active power irps. Should only be modified 
    // using Interlocked functions
    LONG power_irp_count;
};

