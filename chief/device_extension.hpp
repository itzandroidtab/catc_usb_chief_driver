#pragma once

extern "C" {
    #include <wdm.h>
    #include <usb.h>
    #include <usbdi.h>
}

struct usb_chief_vendor_request {
    unsigned short Reqeuest;
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
  _POWER_STATE current_power_state;
  _USB_CONFIGURATION_DESCRIPTOR *usb_config_desc;
  USB_DEVICE_DESCRIPTOR *usb_device_desc;
  PUSBD_INTERFACE_INFORMATION usb_interface_info;
  _DEVICE_CAPABILITIES device_capabilities;
  _IRP *power_irp;
  _URB_BULK_OR_INTERRUPT_TRANSFER *bulk_interrupt_request;
  _KEVENT event0;
  _KEVENT event1;
  _KEVENT event2;
  _KEVENT power_complete_event;
  KSPIN_LOCK device_lock;
  LONG InterlockedValue1;
  LONG pipe_open_count;
  bool *allocated_pipes;
  volatile bool is_ejecting;
  volatile bool is_removing;
  volatile bool is_stopped;
  volatile bool has_config_desc;
  volatile bool power_1_request_busy;
  volatile bool power_request_busy;
  _POWER_STATE target_power_state;
};

