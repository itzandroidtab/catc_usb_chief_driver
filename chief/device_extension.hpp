#pragma once

extern "C" {
    #include <wdm.h>
    #include <usb.h>
    #include <usbdi.h>
}

#include "maybe.hpp"

struct usb_chief_vendor_request {
    unsigned short Request;
    unsigned short value;
    unsigned short index;
    unsigned short length;
    void *data;
};

/**
 * @brief Device extension structure
 * 
 */
struct chief_device_extension {
  PDEVICE_OBJECT attachedDeviceObject;
  PDEVICE_OBJECT physicalDeviceObject;
  POWER_STATE current_power_state;
  PUSB_CONFIGURATION_DESCRIPTOR usb_config_desc;
  PUSBD_INTERFACE_INFORMATION usb_interface_info;
  DEVICE_CAPABILITIES device_capabilities;
  KEVENT pipe_count_empty;
  KSPIN_LOCK device_lock;
  LONG pipe_open_count;
  bool *allocated_pipes;
  volatile bool is_ejecting;
  volatile bool is_removing;

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

