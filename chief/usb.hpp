#pragma once

#include "device_extension.hpp"

/**
 * @brief Send a usb bulk or interrupt transfer
 * 
 * @param DeviceObject 
 * @param Irp 
 * @param read 
 * @return NTSTATUS 
 */
NTSTATUS usb_send_bulk_or_interrupt_transfer(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp, bool read);

/**
 * @brief Send or receive a vendor-specific usb request
 * 
 * @param DeviceObject 
 * @param Request 
 * @param receive 
 * @return NTSTATUS 
 */
NTSTATUS usb_send_receive_vendor_request(_DEVICE_OBJECT* DeviceObject, usb_chief_vendor_request* Request, bool receive);

/**
 * @brief Set the alternate setting for the usb device
 * 
 * @param deviceObject 
 * @param ConfigurationDescriptor 
 * @param AlternateSetting 
 * @return NTSTATUS 
 */
NTSTATUS usb_set_alternate_setting(_DEVICE_OBJECT *deviceObject, PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor, unsigned char AlternateSetting);

/**
 * @brief Abort all usb pipes
 * 
 * @param DeviceObject 
 * @return NTSTATUS 
 */
NTSTATUS usb_pipe_abort(_DEVICE_OBJECT* DeviceObject);

/**
 * @brief Get the usb device descriptor
 * 
 * @param DeviceObject 
 * @return NTSTATUS 
 */
NTSTATUS usb_get_device_desc(_DEVICE_OBJECT* DeviceObject);

/**
 * @brief Clear the usb configuration descriptor
 * 
 * @param DeviceObject 
 * @return NTSTATUS 
 */
NTSTATUS usb_clear_config_desc(_DEVICE_OBJECT* DeviceObject);