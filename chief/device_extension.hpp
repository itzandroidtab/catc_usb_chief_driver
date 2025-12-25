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
  IRP *power_irp;
  KEVENT pipe_count_empty;
  KEVENT event1;
  KEVENT event2;
  KEVENT power_complete_event;
  KSPIN_LOCK device_lock;
  LONG InterlockedValue1;
  LONG pipe_open_count;
  bool *allocated_pipes;
  volatile bool is_ejecting;
  volatile bool is_removing;
  volatile bool is_stopped;
  volatile bool wait_wake_in_progress;
  volatile bool power_request_busy;
  POWER_STATE target_power_state;

  // The BCD version of the connected USB device
  maybe<unsigned short> bcdUSB;
};

