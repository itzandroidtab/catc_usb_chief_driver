#pragma once

extern "C" {
    #include <wdm.h>
    #include <usb.h>
    #include <usbdi.h>
}

struct chief_transfer {
  _URB_BULK_OR_INTERRUPT_TRANSFER *transfer;
  PDEVICE_OBJECT deviceObject;
  PIRP irp;
  PMDL targetMdl;
};

struct usb_chief_vendor_request {
    unsigned short Reqeuest;
    unsigned short value;
    unsigned short index;
    unsigned short length;
    void *data;
};

struct usb_chief_payload {
  int field0;
  int field1;
  USBD_PIPE_HANDLE *pipe;
};

/**
 * @brief Device extension structure
 * 
 */
struct chief_device_extension {
  PDEVICE_OBJECT attachedDeviceObject;
  PDEVICE_OBJECT physicalDeviceObject;
  _POWER_STATE current_power_state;
  USBD_CONFIGURATION_HANDLE usb_config_handle;
  _USB_CONFIGURATION_DESCRIPTOR *usb_config_desc;
  USB_DEVICE_DESCRIPTOR *usb_device_desc;
  PUSBD_INTERFACE_INFORMATION usb_interface_info;
  _DEVICE_CAPABILITIES device_capabilities;
  _IRP *power_irp;
  _IRP *multi_transfer_irp;
  _URB_BULK_OR_INTERRUPT_TRANSFER *bulk_interrupt_request;
  int total_transfers;
  _KEVENT event0;
  _KEVENT event1;
  _KEVENT event2;
  _KEVENT power_complete_event;
  KSPIN_LOCK device_lock;
  LONG InterlockedValue1;
  LONG pipe_open_count;
  bool *allocated_pipes;
  chief_transfer *payload;
  volatile bool is_ejecting;
  volatile bool is_removing;
  volatile bool is_stopped;
  volatile bool has_config_desc;
  volatile bool power_1_request_busy;
  volatile bool power_request_busy;
  _POWER_STATE target_power_state;
  ULONG max_transfer_size;
  KSPIN_LOCK multi_transfer_lock;
};

