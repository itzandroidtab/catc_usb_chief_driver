#include <limits.h>

#include "major_functions.hpp"
#include "spinlock.hpp"
#include "device_extension.hpp"

extern "C" {
    #include <usbdlib.h>
}

NTSTATUS signal_event_complete(_DEVICE_OBJECT *DeviceObject, _IRP *Irp, void* Event) {
    KeSetEvent((PRKEVENT)Event, EVENT_INCREMENT, false);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static bool delete_is_not_pending(__in struct _DEVICE_OBJECT *DeviceObject) {
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;
    
    // return if the device is not being removed or ejected and has a configuration descriptor
    return !dev_ext->is_ejecting && dev_ext->has_config_desc && !dev_ext->is_removing && !dev_ext->someflag_22;
}

/**
 * @brief Get the pipe from unicode str
 * 
 * @param FileName 
 * @return ULONG 
 */
static ULONG get_pipe_from_unicode_str(_UNICODE_STRING* FileName) {
    // Parse pipe number from end of filename
    // Example: "\PIPE00" -> returns 0
    
    const int length = FileName->Length / sizeof(wchar_t);
    
    ULONG result = 0;
    ULONG multiplier = 1;
    bool found_digit = false;
    
    // Parse digits from right to left
    for (int i = length - 1; i >= 0; i--) {
        const wchar_t ch = FileName->Buffer[i];
        
        if (ch >= L'0' && ch <= L'9') {
            result += (ch - L'0') * multiplier;
            multiplier *= 10;
            found_digit = true;
        }
        else if (found_digit) {
            // Hit non-digit after finding digits, we're done
            break;
        }
    }
    
    return found_digit ? result : ULONG_MAX;
}

void set_power_complete(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction, POWER_STATE PowerState, PVOID Context, PIO_STATUS_BLOCK IoStatus) {
    // TODO: why are they not using the DeviceObject parameter directly? No need
    // for a context here
    PDEVICE_OBJECT device_object = (PDEVICE_OBJECT)Context;

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)device_object->DeviceExtension;

    if (PowerState.DeviceState < dev_ext->powerstate2.DeviceState) {
        // set the event to signal the power request is complete
        KeSetEvent(&dev_ext->event3, EVENT_INCREMENT, false);
    }

    // release the spinlock
    spinlock_release(device_object);
}

NTSTATUS change_power_state_impl(_DEVICE_OBJECT* DeviceObject, const POWER_STATE state) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // check if we are already in the requested state
    if (dev_ext->current_power_state.DeviceState == state.DeviceState) {
        return STATUS_SUCCESS;
    }

    // acquire the spinlock
    spinlock_acquire(DeviceObject);

    // mark the power request as busy
    dev_ext->power_request_busy = true;

    // send a IRP_MN_SET_POWER request to the driver
    NTSTATUS status = PoRequestPowerIrp(
        dev_ext->physicalDeviceObject,
        IRP_MN_SET_POWER,
        state,
        set_power_complete,
        DeviceObject,
        nullptr
    );

    // check if we have a pending status
    if (status == STATUS_PENDING) {
        // check if we need to wait for the request to complete
        if (state.DeviceState < dev_ext->powerstate2.DeviceState) {
            KeWaitForSingleObject(
                &dev_ext->event3,
                Suspended,
                KernelMode,
                false,
                nullptr
            );
        }

        status = STATUS_SUCCESS;

        // TODO: if the status is not pending this variable will never 
        // be cleared. This doesnt look correct.
        dev_ext->power_request_busy = false;
    }

    return status;
}

NTSTATUS change_power_state(_DEVICE_OBJECT* DeviceObject, const bool a2) {
    // check if we have a delete pending
    if (!delete_is_not_pending(DeviceObject)) {
        return STATUS_DELETE_PENDING;
    }

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // check if we have a power irq pending or we have a power request busy
    if (dev_ext->power_irp || dev_ext->power_request_busy) {
        return STATUS_SUCCESS;
    }

    // TODO: figure out what the remainder of this function does. It doesnt
    // make too much sense
    if (a2) {
        if (dev_ext->lock_count) {
            return STATUS_SUCCESS;
        }
    }
    else if (!dev_ext->lock_count) {
        return STATUS_SUCCESS;
    }

    // get the power state
    const POWER_STATE state = dev_ext->powerstate2;

    // check if we are currently in a working, unspecified or 
    // hibernate state (we only pass here if we are in a 
    // sleeping1/2/3 or shutdown state)
    if (state.DeviceState == PowerDeviceD0 || 
        state.DeviceState == PowerDeviceUnspecified ||
        state.DeviceState >= PowerDeviceMaximum) 
    {
        return STATUS_SUCCESS;
    } 

    if (!a2) {
        POWER_STATE ps = {};
        ps.DeviceState = PowerDeviceD0;

        return change_power_state_impl(DeviceObject, ps);
    }

    return change_power_state_impl(DeviceObject, state);
}

void power_request_complete(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction, POWER_STATE PowerState, PVOID Context, PIO_STATUS_BLOCK IoStatus) {
    // TODO: why are they not using the DeviceObject parameter directly? No need
    // for a context here
    PDEVICE_OBJECT device_object = (PDEVICE_OBJECT)Context;

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)device_object->DeviceExtension;

    // copy the current irp stack location to the next
    IoCopyCurrentIrpStackLocationToNext(dev_ext->power_irp);

    // Mark we are done with the power request
    PoStartNextPowerIrp(dev_ext->power_irp);

    // call the driver
    PoCallDriver(dev_ext->attachedDeviceObject, dev_ext->power_irp);

    // clear the power irp pointer
    // TODO: this was done below the spinlock release. I think
    // it makes more sense to do it before releasing the spinlock
    dev_ext->power_irp = nullptr;

    // release the spinlock
    spinlock_release(device_object);
}

NTSTATUS power_state_systemworking_complete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    // TODO: why are they not using the DeviceObject parameter directly? No need
    // for a context here
    PDEVICE_OBJECT device_object = (PDEVICE_OBJECT)Context;

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)device_object->DeviceExtension;

    // check if we have a pending return
    if (Irp->PendingReturned) {
        // TODO: doesnt this mean we need to return STATUS_PENDING here?
        IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;
    }

    dev_ext->current_power_state.DeviceState = PowerDeviceD0;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    // release the spinlock
    spinlock_release(device_object);

    // return success
    return STATUS_SUCCESS;
}

bool update_power_state(_DEVICE_OBJECT* DeviceObject, const DEVICE_POWER_STATE state) {
    // check if we are changing to a non D0 state
    if (state == PowerDeviceD0) {
        return true;
    }
    
    // TODO: this doesnt do anything. It copies the state to the 
    // current_power_state but it does extra checking that seems 
    // useless
    if (state > PowerDeviceD0) {
        // get the device extension
        chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

        if (state <= PowerDeviceD2) {
            dev_ext->current_power_state.DeviceState = state;
        }
        else if (state == PowerDeviceD3) {
            // set the current power state to D3
            dev_ext->current_power_state.DeviceState = PowerDeviceD3;
        }
    }

    return false;
}

NTSTATUS usb_send_urb(_DEVICE_OBJECT* DeviceObject, PURB Urb) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // create a event
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, false);

    struct _IO_STATUS_BLOCK IoStatusBlock = {};

    PIRP irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        dev_ext->attachedDeviceObject,
        nullptr,
        0,
        nullptr,
        0,
        true,
        &event,
        &IoStatusBlock
    );

    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // get the next stack location and store the URB pointer for the USB stack
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->Parameters.Others.Argument1 = (void*)Urb;

    NTSTATUS status = IofCallDriver(dev_ext->attachedDeviceObject, irp);

    // check if we have a pending status
    if (status == STATUS_PENDING) {
        // wait for the event to be signaled
        KeWaitForSingleObject(
            &event,
            Suspended,
            KernelMode,
            false,
            nullptr
        );

        status = IoStatusBlock.Status;
    }

    return status;
}

NTSTATUS usb_send_receive_vendor_request(_DEVICE_OBJECT* DeviceObject, usb_chief_vendor_request* Request, bool receive) {
    // allocate memeory for the urb
    _URB_CONTROL_VENDOR_OR_CLASS_REQUEST * usb = (_URB_CONTROL_VENDOR_OR_CLASS_REQUEST*)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(_URB_CONTROL_VENDOR_OR_CLASS_REQUEST), 0x206D6457u
    );

    // check if we got memory
    if (!usb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    void* buffer = nullptr;

    // check if we need to allocate memeory
    if (Request->length) {
        buffer = ExAllocatePoolWithTag(NonPagedPool, Request->length, 0x206D6457u);

        if (!buffer) {
            // TODO: this should free the usb variable before exiting
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        memcpy(buffer, Request->data, Request->length);
    }

    // initialize the urb
    memset(usb, 0x00, sizeof(_URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    usb->Hdr.Function = URB_FUNCTION_VENDOR_DEVICE;
    usb->Hdr.Length = sizeof(_URB_CONTROL_VENDOR_OR_CLASS_REQUEST);
    usb->TransferBufferLength = Request->length;
    usb->TransferBufferMDL = nullptr;
    usb->TransferBuffer = buffer;
    usb->RequestTypeReservedBits = (
        ((receive ? BMREQUEST_DEVICE_TO_HOST : BMREQUEST_HOST_TO_DEVICE) << 7) |
        (BMREQUEST_VENDOR << 5) | BMREQUEST_TO_DEVICE
    );
    usb->Request = Request->Reqeuest & 0xff;
    usb->Value = Request->value;
    usb->Index = Request->index;
    usb->TransferFlags = (
        receive ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : (USBD_TRANSFER_DIRECTION_OUT)
    );
    usb->UrbLink = nullptr;

    // send the urb
    NTSTATUS status = usb_send_urb(DeviceObject, (PURB)usb);

    // check if we need to copy data back
    if (NT_SUCCESS(status) && receive && buffer) {
        // update the request length
        Request->length = usb->TransferBufferLength & 0xffff;

        // copy the data back to the request structure
        memcpy(Request->data, buffer, usb->TransferBufferLength);
    }

    if (buffer) {
        ExFreePool(buffer);
    }

    ExFreePool(usb);

    return status;
}

NTSTATUS usb_set_alternate_setting(_DEVICE_OBJECT *deviceObject, PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor, unsigned char AlternateSetting) {
    // check if we have a valid alternate setting
    // TODO: hardcoded 2?
    if (AlternateSetting >= 2) {
        // TODO: invalid parameter sounds better here as error
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // switch to the alternate setting
    _USB_INTERFACE_DESCRIPTOR *descriptor = USBD_ParseConfigurationDescriptorEx(
        ConfigurationDescriptor,
        ConfigurationDescriptor,
        0,
        AlternateSetting,
        -1,
        -1,
        -1
    );

    if (!descriptor) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // array must have N+1 elements for N interfaces, with last 
    // element null-terminated
    struct _USBD_INTERFACE_LIST_ENTRY InterfaceList[2];
    InterfaceList[0].InterfaceDescriptor = descriptor;
    InterfaceList[0].Interface = nullptr;
    InterfaceList[1].InterfaceDescriptor = nullptr;
    InterfaceList[1].Interface = nullptr;

    // create the urb
    PURB urb = USBD_CreateConfigurationRequestEx(
        ConfigurationDescriptor,
        InterfaceList
    );

    if (!urb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)deviceObject->DeviceExtension;

    // free the allocated pipes if we have any
    if (dev_ext->allocated_pipes) {
        // TODO: maybe we dont need to free this. If the number of pipes
        // doesnt change between alternate settings?
        ExFreePool(dev_ext->allocated_pipes);
    }

    // allocate new memory for the allocated pipes
    dev_ext->allocated_pipes = (bool*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(bool) * InterfaceList[0].Interface->NumberOfPipes,
        0x206D6457u
    );

    // check if we got memory
    if (!dev_ext->allocated_pipes) {
        // The original driver doesnt do this, but we should
        // free the urb before we exit
        ExFreePool(urb);
        
        // return we have an error
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // zero the allocated pipes
    memset(dev_ext->allocated_pipes, 0x00, sizeof(bool) * InterfaceList[0].Interface->NumberOfPipes);

    // set the maximum transfer size for all pipes to the max_length
    // TODO: documentation says this is not used anymore.
    for (ULONG i = 0; i < InterfaceList[0].Interface->NumberOfPipes; i++) {
        InterfaceList[0].Interface->Pipes[i].MaximumTransferSize = dev_ext->max_length;
    }

    // set the urb
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;

    // fixes the original issue. This was skipping the size of the
    // _URB_SELECT_CONFIGURATION structure and causing a 0x7e error
    urb->UrbHeader.Length = (USHORT)GET_SELECT_CONFIGURATION_REQUEST_SIZE(
        1, InterfaceList[0].Interface->NumberOfPipes
    );
    urb->UrbSelectConfiguration.ConfigurationDescriptor = ConfigurationDescriptor;

    // send the urb
    NTSTATUS status = usb_send_urb(deviceObject, (PURB)urb);

    // set the usb config handle
    // TODO: not sure if this changes with the usb_send_urb call
    dev_ext->usb_config_handle = urb->UrbSelectConfiguration.ConfigurationHandle;

    // check if we need to update the interface information
    if (NT_SUCCESS(status)) {
        // TODO: check if free is needed. What if the usb_interface_info is the 
        // same size? Big chance it is
        if (dev_ext->usb_interface_info) {
            ExFreePool(dev_ext->usb_interface_info);
        }

        // allocate new memory for the usb interface info
        dev_ext->usb_interface_info = (PUSBD_INTERFACE_INFORMATION)ExAllocatePoolWithTag(
            NonPagedPool,
            InterfaceList[0].Interface->Length,
            0x206D6457u
        );

        if (dev_ext->usb_interface_info) {
            // copy the interface info
            memcpy(dev_ext->usb_interface_info, InterfaceList[0].Interface, InterfaceList[0].Interface->Length);
        }
    }
    
    ExFreePool(urb);

    return STATUS_SUCCESS;
}

_URB_BULK_OR_INTERRUPT_TRANSFER* usb_create_bulk_or_interrupt_transfer(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp, USBD_PIPE_INFORMATION* Payload, bool isInDirection) {
    // get the amount of data to transfer
    const int length = (Irp->MdlAddress) ? MmGetMdlByteCount(Irp->MdlAddress) : 0;

    // create the urb
    _URB_BULK_OR_INTERRUPT_TRANSFER* request = (_URB_BULK_OR_INTERRUPT_TRANSFER*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER),
        0x206D6457u
    );

    // check if we got memory
    if (!request) {
        return nullptr;
    }

    // initialize the urb
    memset(request, 0x00, sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER));
    request->Hdr.Length = sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER);
    request->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    request->PipeHandle = Payload->PipeHandle;
    request->UrbLink = nullptr;
    request->TransferFlags = (
        (isInDirection ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT) | USBD_SHORT_TRANSFER_OK
    );
    request->TransferBufferMDL = Irp->MdlAddress;
    request->TransferBufferLength = length;
    request->TransferBuffer = nullptr;

    return request;
}

NTSTATUS usb_bulk_or_interrupt_transfer_complete_0(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    // check if we have a pending return
    if (Irp->PendingReturned) {
        IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;
    }
    
    // TODO: Shouldnt the spinlock be released at the end of this function?
    spinlock_release(DeviceObject);

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // set the irp status to success
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = dev_ext->bulk_interrupt_request->TransferBufferLength;

    // complete the irp
    IofCompleteRequest(Irp, IO_NO_INCREMENT);

    // free the bulk or interrupt request
    ExFreePool(dev_ext->bulk_interrupt_request);

    // clear the bulk or interrupt request pointer
    dev_ext->bulk_interrupt_request = nullptr;

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS usb_send_bulk_or_interrupt_transfer(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp, bool read) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // get the current stack location
    PIO_STACK_LOCATION current_stack = IoGetCurrentIrpStackLocation(Irp);

    // get the file object
    PFILE_OBJECT file = current_stack->FileObject;

    // check for a valid fs context
    if (!file || !file->FsContext) {
        Irp->IoStatus.Status = STATUS_INVALID_HANDLE;
        Irp->IoStatus.Information = 0;

        // complete the irp
        IofCompleteRequest(Irp, 0);

        return STATUS_INVALID_HANDLE;
    }

    // get the payload from the fs context
    USBD_PIPE_INFORMATION* pipe_info = (USBD_PIPE_INFORMATION*)file->FsContext;

    _URB_BULK_OR_INTERRUPT_TRANSFER* request = usb_create_bulk_or_interrupt_transfer(
        DeviceObject, Irp,
        pipe_info, read
    );

    if (!request) {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;

        // complete the irp
        IofCompleteRequest(Irp, 0);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // store the request in the device extension for later use
    dev_ext->bulk_interrupt_request = request;

    // get the next stack location
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(Irp);

    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = request;
    stack->CompletionRoutine = usb_bulk_or_interrupt_transfer_complete_0;
    stack->Context = DeviceObject;
    stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

    // acquire the spinlock
    spinlock_acquire(DeviceObject);

    return IofCallDriver(dev_ext->attachedDeviceObject, Irp);
}

NTSTATUS get_usb_port_status(_DEVICE_OBJECT* DeviceObject, ULONG* Status) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    struct _IO_STATUS_BLOCK IoStatusBlock;

    // TODO: no null check for Status?
    // clear the status 
    *Status = 0;

    // create an event
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, false);

    // get the usb port status IOCTL_INTERNAL_USB_GET_PORT_STATUS
    PIRP irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_GET_PORT_STATUS,
        dev_ext->attachedDeviceObject,
        nullptr,
        0,
        nullptr,
        0,
        true,
        &event,
        &IoStatusBlock
    );

    // get the next stack location
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->Parameters.Others.Argument1 = Status;

    // call the driver
    NTSTATUS status = IofCallDriver(dev_ext->attachedDeviceObject, irp);

    if (status == STATUS_PENDING) {
        // wait for the event to be signaled
        KeWaitForSingleObject(
            &event,
            Suspended,
            KernelMode,
            false,
            nullptr
        );

        status = IoStatusBlock.Status;
    }

    return status;
}

NTSTATUS usb_reset_upstream_port(_DEVICE_OBJECT *deviceObject) {
    struct _IO_STATUS_BLOCK IoStatusBlock;

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)deviceObject->DeviceExtension;

    // create an event
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, false);

    // reset the upstream usb port IOCTL_INTERNAL_USB_RESET_PORT
    PIRP request = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_RESET_PORT,
        dev_ext->attachedDeviceObject,
        nullptr,
        0,
        nullptr,
        0,
        true,
        &event,
        &IoStatusBlock
    );

    NTSTATUS status = IofCallDriver(dev_ext->attachedDeviceObject, request);

    if (status == STATUS_PENDING) {
        // wait for the event to be signaled
        KeWaitForSingleObject(
            &event,
            Suspended,
            KernelMode,
            false,
            nullptr
        );

        status = IoStatusBlock.Status;
    }

    return status;
}

NTSTATUS usb_reset_if_not_enabled_but_conected(_DEVICE_OBJECT* DeviceObject) {
    ULONG status;

    // get the current port status
    NTSTATUS res = get_usb_port_status(DeviceObject, &status);

    // check if we got a success and if the port is not enabled (bit 0) 
    // and if we are connected (bit 1)
    if (NT_SUCCESS(res) && ((status & 0x1) == 0) || ((status & 0x2) != 0)) {
        // we are connected but not enabled, reset the upstream port
        return usb_reset_upstream_port(DeviceObject);
    }

    return res;
}

NTSTATUS usb_bulk_or_interrupt_transfer_complete_1(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    // convert the context to a chief_transfer pointer
    chief_transfer* transfer = (chief_transfer*)Context;

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)transfer->deviceObject->DeviceExtension;

    // acquire the spinlock
    KIRQL irql;
    KeAcquireSpinLock(&dev_ext->spinlock1, &irql);

    // decrease the total transfers count
    dev_ext->total_transfers--;

    // release the spinlock
    spinlock_release(DeviceObject);
    
    // TODO: why is this changing something in the event0 header lock?
    dev_ext->event0.Header.Lock += transfer->transfer->TransferBufferLength;
    Irp->IoStatus.Information = transfer->transfer->TransferBufferLength;

    // free the irp
    IoFreeIrp(transfer->irp);
    transfer->irp = nullptr;

    // free the mdl
    IoFreeMdl(transfer->targetMdl);
    transfer->targetMdl = nullptr;

    // check if we have more transfers to do
    if (!dev_ext->total_transfers) {
        // set the irp status to success
        dev_ext->multi_transfer_irp->IoStatus.Status = STATUS_SUCCESS;

        // TODO: not sure if this is correct. It doesnt look like that for me
        dev_ext->multi_transfer_irp->IoStatus.Information = dev_ext->event0.Header.Lock;

        // complete the irp
        IofCompleteRequest(dev_ext->multi_transfer_irp, IO_NO_INCREMENT);

        // free the transfer memory
        ExFreePool(dev_ext->payload);
        dev_ext->payload = nullptr;
        dev_ext->multi_transfer_irp = nullptr;

        KeSetEvent(&dev_ext->event1, EVENT_INCREMENT, false);
    }

    ExFreePool(transfer->transfer);

    // release the spinlock
    KeReleaseSpinLock(&dev_ext->spinlock1, irql);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS usb_sync_reset_pipe_clear_stall(__in struct _DEVICE_OBJECT *DeviceObject, USBD_PIPE_INFORMATION* Pipe) {
    // create the urb
    _URB_PIPE_REQUEST* request = (_URB_PIPE_REQUEST*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(_URB_PIPE_REQUEST),
        0x206D6457u
    );

    // check if we got memory
    if (!request) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // initialize the urb
    request->Hdr.Length = sizeof(_URB_PIPE_REQUEST);
    request->Hdr.Function = URB_FUNCTION_RESET_PIPE;
    request->PipeHandle = Pipe->PipeHandle;

    // send the urb
    NTSTATUS status = usb_send_urb(DeviceObject, (PURB)request);

    ExFreePool(request);

    return status;
}

static void stop_device(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // check if we have any payload
    if (!dev_ext->payload) {
        return;
    }

    chief_transfer* transfer = dev_ext->payload;

    // check if we have any irps to cancel
    if (!transfer->irp) {
        return;
    }

    // cancel all pending irps
    bool last_result = false;
    int count = 0;

    // iterate through all irps and cancel them
    for (PIRP* irp = &transfer->irp; *irp; irp++, count++) {
        // cancel the irp
        last_result = IoCancelIrp(*irp);
        
        // check for failure
        if (!last_result) {
            break;
        }
    }

    // wait for all the cancelled irps to complete
    if (count && last_result) {
        KeWaitForSingleObject(
            &dev_ext->event1,
            Suspended,
            KernelMode,
            false,
            nullptr
        );
    }
}

static NTSTATUS usb_pipe_abort(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    NTSTATUS status = STATUS_SUCCESS;
    PUSBD_INTERFACE_INFORMATION interface_info = dev_ext->usb_interface_info;

    // check if we have any pipes to abort
    if (!interface_info || !interface_info->NumberOfPipes) {
        return status;
    }

    // iterate through all pipes
    for (ULONG i = 0; i < interface_info->NumberOfPipes; i++) {
        // check if the pipe is allocated
        if (!dev_ext->allocated_pipes[i]) {
            continue;
        }

        // allocate memory for the URB
        _URB_PIPE_REQUEST* urb = (_URB_PIPE_REQUEST*)ExAllocatePoolWithTag(
            NonPagedPool,
            sizeof(_URB_PIPE_REQUEST),
            0x206D6457u
        );

        if (!urb) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // initialize the URB for abort pipe
        urb->Hdr.Length = sizeof(_URB_PIPE_REQUEST);
        urb->Hdr.Function = URB_FUNCTION_ABORT_PIPE;
        urb->PipeHandle = interface_info->Pipes[i].PipeHandle;

        // send the URB
        status = usb_send_urb(DeviceObject, (PURB)urb);

        // free the URB
        ExFreePool(urb);

        // check if we have an error
        if (!NT_SUCCESS(status)) {
            return status;
        }

        // mark the pipe as free
        dev_ext->allocated_pipes[i] = false;

        // decrement the interlocked value
        InterlockedDecrement(&dev_ext->lock_count);
    }

    return status;
}

static NTSTATUS usb_get_configuration_desc(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // allocate memory for the URB
    _URB_CONTROL_DESCRIPTOR_REQUEST* urb = (_URB_CONTROL_DESCRIPTOR_REQUEST*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST),
        0x206D6457u
    );

    if (!urb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // clear the URB memory
    memset(urb, 0x00, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST));

    // start with an initial buffer size of 521 bytes
    // TODO: why 521? The actual descriptor is 55 bytes long if 
    // I check with a USB sniffer
    ULONG buffer_size = 521;

    while (true) {
        // allocate memory for the configuration descriptor
        PUSB_CONFIGURATION_DESCRIPTOR descriptor = (PUSB_CONFIGURATION_DESCRIPTOR)ExAllocatePoolWithTag(
            NonPagedPool,
            buffer_size,
            0x206D6457u
        );

        dev_ext->usb_config_desc = descriptor;

        if (!descriptor) {
            ExFreePool(urb);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // initialize the URB for getting configuration descriptor
        urb->Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
        urb->Hdr.Length = sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST);
        urb->TransferBufferLength = buffer_size;
        urb->TransferBufferMDL = nullptr;
        urb->TransferBuffer = dev_ext->usb_config_desc;
        urb->DescriptorType = USB_CONFIGURATION_DESCRIPTOR_TYPE;
        urb->Index = 0;
        urb->LanguageId = 0;
        urb->UrbLink = nullptr;

        // send the URB
        usb_send_urb(DeviceObject, (PURB)urb);

        // check if we got the complete descriptor
        // if TransferBufferLength is 0 or the total length fits in our buffer, we're done
        if (!urb->TransferBufferLength || dev_ext->usb_config_desc->wTotalLength <= buffer_size) {
            break;
        }

        // we need a larger buffer, free the current one and try again
        buffer_size = dev_ext->usb_config_desc->wTotalLength;
        ExFreePool(dev_ext->usb_config_desc);
        dev_ext->usb_config_desc = nullptr;
    }

    // free the URB
    ExFreePool(urb);

    // set the alternate setting to 0
    return usb_set_alternate_setting(DeviceObject, dev_ext->usb_config_desc, 0);
}

static NTSTATUS usb_get_device_desc(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // allocate memory for the URB
    _URB_CONTROL_DESCRIPTOR_REQUEST* usb_request = (_URB_CONTROL_DESCRIPTOR_REQUEST*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST),
        0x206D6457u
    );

    if (!usb_request) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // allocate memory for the device descriptor
    USB_DEVICE_DESCRIPTOR* buffer = (USB_DEVICE_DESCRIPTOR*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(USB_DEVICE_DESCRIPTOR),
        0x206D6457u
    );

    NTSTATUS status;

    if (!buffer) {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else {
        // initialize the URB for getting device descriptor
        memset(usb_request, 0x00, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST));
        usb_request->Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
        usb_request->Hdr.Length = sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST);
        usb_request->TransferBufferLength = sizeof(USB_DEVICE_DESCRIPTOR);
        usb_request->TransferBuffer = buffer;
        usb_request->DescriptorType = USB_DEVICE_DESCRIPTOR_TYPE;
        usb_request->Index = 0;
        usb_request->LanguageId = 0;

        // send the URB
        status = usb_send_urb(DeviceObject, (PURB)usb_request);
    }

    // check if we have an error
    if (!NT_SUCCESS(status)) {
        if (buffer) {
            ExFreePool(buffer);
        }
    }
    else {
        // store the device descriptor in the device extension
        dev_ext->usb_device_desc = buffer;
    }

    // free the URB
    ExFreePool(usb_request);

    // if successful, get the configuration descriptor
    if (NT_SUCCESS(status)) {
        status = usb_get_configuration_desc(DeviceObject);
        
        if (NT_SUCCESS(status)) {
            dev_ext->has_config_desc = true;
        }
    }

    return status;
}

static NTSTATUS usb_clear_config_desc(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // allocate memory for the URB
    _URB_SELECT_CONFIGURATION* urb = (_URB_SELECT_CONFIGURATION*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(_URB_SELECT_CONFIGURATION),
        0x206D6457u
    );

    NTSTATUS status;

    if (!urb) {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else {
        // initialize the URB to deselect configuration (set to NULL)
        urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
        urb->Hdr.Length = sizeof(_URB_SELECT_CONFIGURATION);
        urb->ConfigurationDescriptor = nullptr;

        // send the urb
        status = usb_send_urb(DeviceObject, (PURB)urb);

        // free the URB
        ExFreePool(urb);
    }

    // if successful, mark that we no longer have a config descriptor
    if (NT_SUCCESS(status)) {
        dev_ext->has_config_desc = false;
    }

    // clear the someflag_22 flag. TODO: find out what this is
    dev_ext->someflag_22 = false;

    return status;
}

static NTSTATUS usb_cleanup_memory(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;
    
    // free all the allocated usb memory
    if (dev_ext->usb_device_desc) {
        ExFreePool(dev_ext->usb_device_desc);
        dev_ext->usb_device_desc = nullptr;
    }

    // free the allocated pipes
    if (dev_ext->allocated_pipes) {
        ExFreePool(dev_ext->allocated_pipes);
        dev_ext->allocated_pipes = nullptr;
    }

    // free the usb interface info
    if (dev_ext->usb_interface_info) {
        ExFreePool(dev_ext->usb_interface_info);
        dev_ext->usb_interface_info = nullptr;
    }

    // free the usb configuration descriptor
    if (dev_ext->usb_config_desc) {
        ExFreePool(dev_ext->usb_config_desc);
        dev_ext->usb_config_desc = nullptr;
    }

    // free the payload
    if (dev_ext->payload) {
        auto** transfer = &dev_ext->payload;
        ExFreePool(dev_ext->payload);
        
        dev_ext->payload = nullptr;

        // TODO: not sure why they are doing this
        *transfer = nullptr;
    }

    return STATUS_SUCCESS;
}

NTSTATUS mj_read_write_impl(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp, bool read) {
    // clear the information field
    Irp->IoStatus.Information = 0;

    // check if we have a delete pending
    if (!delete_is_not_pending(DeviceObject)) {
        // set the irp status to delete pending
        Irp->IoStatus.Status = STATUS_DELETE_PENDING;

        // complete the irp
        IofCompleteRequest(Irp, IO_NO_INCREMENT);

        // return delete pending
        return STATUS_DELETE_PENDING;
    }

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // get the amount of data to transfer
    const int length = (Irp->MdlAddress) ? MmGetMdlByteCount(Irp->MdlAddress) : 0;

    // check if the length is more than the maximum length
    if (length <= dev_ext->max_length) {
        // we can do it in one transfer
        return usb_send_bulk_or_interrupt_transfer(DeviceObject, Irp, read);
    }

    // we need to do multiple transfers. Get the file object
    PFILE_OBJECT file = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    // check for a valid fs context
    if (!file || !file->FsContext) {
        Irp->IoStatus.Status = STATUS_INVALID_HANDLE;

        // complete the irp
        IofCompleteRequest(Irp, 0);

        return STATUS_INVALID_HANDLE;
    }

    auto* pipe_info = (USBD_PIPE_INFORMATION*)file->FsContext;

    // get the total amount of transfers needed (ceiling division)
    const int total_transfers = (length + dev_ext->max_length - 1) / dev_ext->max_length;

    // allocate memory for the chief_transfer add 1 for the extra null terminator
    chief_transfer* transfers = (chief_transfer*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(chief_transfer) * total_transfers,
        0x206D6457u
    );

    // check if we got memory
    if (!transfers) {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

        // complete the irp
        IofCompleteRequest(Irp, 0);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // clear the memory
    memset(transfers, 0x00, sizeof(chief_transfer) * total_transfers);

    // reset the event0
    KeResetEvent(&dev_ext->event0);

    // store the irp in the device extension
    dev_ext->multi_transfer_irp = Irp;

    // set the transfer in the payload
    dev_ext->payload = transfers;

    // store the total transfers in the device extension
    dev_ext->total_transfers = total_transfers;

    // TODO: rework this, why does this not just use the callback to start a new transfer?
    // get the current transfer
    chief_transfer* current_transfer = transfers;

    NTSTATUS status = STATUS_SUCCESS;

    unsigned long offset = 0;
    unsigned long current_max_length = dev_ext->max_length;
    int index;

    for (index = 0; index < total_transfers; index++, current_transfer++) {
        // check if we have a delete pending
        if (!delete_is_not_pending(DeviceObject)) {
            status = STATUS_DELETE_PENDING;

            break;
        }

        // allocate a irp for the transfer
        PIRP transfer_irp = IoAllocateIrp(DeviceObject->StackSize + 1, FALSE);

        // check if we got a irp
        if (!transfer_irp) {
            break;
        }

        // allocate memory for the mdl
        // TODO: does this need the full length? Shouldnt it just be the max_length?
        PMDL mdl = IoAllocateMdl(MmGetMdlVirtualAddress(Irp->MdlAddress), length, 0, 0, transfer_irp);

        if (!mdl) {
            // TODO: this might need to free the irp before exiting
            break;
        }

        // TODO: why is this here? We already know all the transfer sizes beforehand
        if (((long)(offset + current_max_length)) > length) {
            current_max_length = length - offset;
        }

        IoBuildPartialMdl(
            Irp->MdlAddress,
            mdl,
            (PCHAR)MmGetMdlVirtualAddress(Irp->MdlAddress) + offset,
            current_max_length
        );

        offset += current_max_length;

        // create the bulk or interrupt transfer request
        _URB_BULK_OR_INTERRUPT_TRANSFER* request = usb_create_bulk_or_interrupt_transfer(
            DeviceObject, transfer_irp,
            pipe_info, read
        );

        if (!request) {
            // TODO: free the mdl and irp before exiting. Why does this driver not free anything on error?
            break;
        }

        // set the context
        dev_ext->payload[0].transfer = request;
        dev_ext->payload[0].deviceObject = DeviceObject;
        dev_ext->payload[0].irp = transfer_irp;
        dev_ext->payload[0].targetMdl = mdl;

        // get the next stack location
        PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(transfer_irp);

        stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
        stack->Parameters.Others.Argument1 = request;
        stack->CompletionRoutine = usb_bulk_or_interrupt_transfer_complete_1;
        stack->Context = &dev_ext->payload[0];
        stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

        // acquire the spinlock
        spinlock_acquire(DeviceObject);

        // call the driver
        NTSTATUS result = IofCallDriver(dev_ext->attachedDeviceObject, transfer_irp);

        // check if we have an error
        if (!NT_SUCCESS(result)) {
            // TODO: free the allocated resources before exiting
            break;
        }

        // check if we are not done yet
        if (index != (total_transfers - 1)) {
            // increment the payload pointer
            dev_ext->payload++;
        }
    }

    if (!NT_SUCCESS(status) && delete_is_not_pending(DeviceObject) && usb_sync_reset_pipe_clear_stall(DeviceObject, pipe_info)) {
        usb_reset_if_not_enabled_but_conected(DeviceObject);
    }

    if (index) {
        // acquire the spinlock
        KIRQL irql;
        KeAcquireSpinLock(&dev_ext->spinlock1, &irql);

        if (dev_ext->multi_transfer_irp) {
            // set the status to pending
            Irp->IoStatus.Status = status = STATUS_PENDING;
            
            // set the pending returned flag
            IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;
        }
        else {
            status = STATUS_SUCCESS;
        }

        KeReleaseSpinLock(&dev_ext->spinlock1, irql);
    }
    else {
        IofCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return status;
}

NTSTATUS mj_create(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // aquire the spinlock
    spinlock_acquire(DeviceObject);

    NTSTATUS status = STATUS_SUCCESS;

    // check if the device is being removed or ejected
    if (!delete_is_not_pending(DeviceObject)) {
        status = STATUS_DELETE_PENDING;
    }
    else {
        // we are not being deleted. Get the current file object in the irp
        PFILE_OBJECT file = IoGetCurrentIrpStackLocation(Irp)->FileObject;
        file->FsContext = nullptr;
    
        // check if we have a file name
        if (file->FileName.Length) {
            // get the pipe index from the file name
            ULONG pipe_index = get_pipe_from_unicode_str(&file->FileName);

            // check if we got a valid pipe index
            if (pipe_index >= dev_ext->usb_interface_info->NumberOfPipes) {
                status = STATUS_INVALID_PARAMETER;
            }
            else {
                // store the pipe information in the fs context
                file->FsContext = (void*)&dev_ext->usb_interface_info->Pipes[pipe_index];

                // mark the pipe as allocated
                dev_ext->allocated_pipes[pipe_index] = true;

                // increment the interlocked value
                InterlockedIncrement(&dev_ext->lock_count);

                // change the power state
                change_power_state(DeviceObject, false);
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;

    IofCompleteRequest(Irp, IO_NO_INCREMENT);

    // release the spinlock
    spinlock_release(DeviceObject);

    return status;
}

NTSTATUS mj_close(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    // acquire the spinlock
    spinlock_acquire(DeviceObject);

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // get the current file object in the irp
    PFILE_OBJECT file = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    // check if we have a valid fs context
    if (file->FsContext) {
        // get the pipe from the filename
        const ULONG pipe_index = get_pipe_from_unicode_str(&file->FileName);

        // check if the pipe index is valid
        if (pipe_index < dev_ext->usb_interface_info->NumberOfPipes) {
            // check if the pipe is allocated
            if (dev_ext->allocated_pipes[pipe_index]) {
                // mark the pipe as free
                dev_ext->allocated_pipes[pipe_index] = false;

                // decrement the interlocked value
                InterlockedDecrement(&dev_ext->lock_count);
            }
        }
    }

    // release the spinlock
    spinlock_release(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    // complete the irp
    IofCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

NTSTATUS mj_read(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    return mj_read_write_impl(DeviceObject, Irp, true);
}

NTSTATUS mj_write(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    return mj_read_write_impl(DeviceObject, Irp, false);
}

NTSTATUS mj_device_control(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    // acquire the spinlock
    spinlock_acquire(DeviceObject);

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // return status
    NTSTATUS status = STATUS_SUCCESS;

    // check if we have a delete pending
    if (!delete_is_not_pending(DeviceObject)) {
        // set the status to delete pending
        status = STATUS_DELETE_PENDING;
        Irp->IoStatus.Information = 0;
    }
    else {
        // get the current irp stack location
        PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

        // get the values from the stack
        ULONG_PTR buffer_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
        const ULONG io_control_code = stack->Parameters.DeviceIoControl.IoControlCode;
        usb_chief_vendor_request* vendor_request = (usb_chief_vendor_request*)Irp->AssociatedIrp.SystemBuffer;

        // check the io control code
        switch (io_control_code) {
            case CTL_CODE(FILE_DEVICE_USB, 0, METHOD_BUFFERED, FILE_ANY_ACCESS): // 0x220000
                status = usb_send_receive_vendor_request(DeviceObject, vendor_request, false);
                break;
            case CTL_CODE(FILE_DEVICE_USB, 1, METHOD_BUFFERED, FILE_ANY_ACCESS): // 0x220004
                status = usb_send_receive_vendor_request(DeviceObject, vendor_request, true);

                // check the status for a success
                if (!NT_SUCCESS(status)) {
                    Irp->IoStatus.Information = 0;
                    status = STATUS_DEVICE_DATA_ERROR;
                }
                else {
                    // set the information to the buffer length
                    Irp->IoStatus.Information = buffer_length;
                }
                break;
            case CTL_CODE(FILE_DEVICE_USB, 2, METHOD_BUFFERED, FILE_ANY_ACCESS): // 0x220008
                status = usb_set_alternate_setting(DeviceObject, dev_ext->usb_config_desc, vendor_request->Reqeuest & 0xff);
                break;
            case CTL_CODE(FILE_DEVICE_USB, 3, METHOD_BUFFERED, FILE_ANY_ACCESS): // 0x22000c
                if (dev_ext->usb_device_desc) {
                    // copy the bcdUSB value to the vendor request
                    vendor_request->Reqeuest = dev_ext->usb_device_desc->bcdUSB;

                    // set the length to 2 bytes
                    Irp->IoStatus.Information = 2;

                    // set the status to success
                    status = STATUS_SUCCESS;
                }
                else {
                    // set the status to device data error
                    status = STATUS_DEVICE_DATA_ERROR;
                }
                break;
            default:
                // all other requests are invalid
                status = STATUS_INVALID_PARAMETER;
                break;
        }
    }

    // set the irp status based on the status
    Irp->IoStatus.Status = status;

    // complete the irp
    IofCompleteRequest(Irp, IO_NO_INCREMENT);

    // release the spinlock
    spinlock_release(DeviceObject);

    return status;
}

NTSTATUS mj_power(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // get the current stack location
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    // acquire the spinlock
    spinlock_acquire(DeviceObject);

    NTSTATUS status = STATUS_SUCCESS;

    switch (stack->MinorFunction) {
        case IRP_MN_WAIT_WAKE:
        {
                // TODO: check this out
                // get the current power state
                const POWER_STATE state = dev_ext->current_power_state;

                // check if the device is already awake
                const DEVICE_POWER_STATE device_wake = dev_ext->resource.DeviceWake;

                // set powerstate2 to the device wake state
                dev_ext->powerstate2.DeviceState = device_wake;

                // check if we are already in the PowerDeviceD0 state
                if (state.DeviceState != PowerDeviceD0 || dev_ext->powerstate2.DeviceState < state.DeviceState) {
                    dev_ext->power_1_request_busy = true;

                    // copy the current irp stack location to the next
                    IoCopyCurrentIrpStackLocationToNext(Irp);

                    // create a event
                    KEVENT event;
                    KeInitializeEvent(&event, NotificationEvent, false);

                    // get the next irp stack location
                    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(Irp);

                    stack->CompletionRoutine = signal_event_complete;
                    stack->Context = &event;
                    stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

                    PoStartNextPowerIrp(Irp);
                    status = PoCallDriver(dev_ext->attachedDeviceObject, Irp);

                    // check if we need to wait on the request
                    if (status == STATUS_PENDING) {
                        // wait for the event to be signaled
                        KeWaitForSingleObject(
                            &event,
                            Suspended,
                            KernelMode,
                            false,
                            nullptr
                        );

                        // TODO: shouldnt we use the IoStatus.Status here?
                        // status = Irp->IoStatus.Status;
                    }

                    // change the power state to the new value i guess
                    change_power_state(DeviceObject, false);
                    
                    dev_ext->power_1_request_busy = false;
                }
                else {
                    // set the status to invalid device state
                    status = Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;

                    // complete the irp
                    IofCompleteRequest(Irp, IO_NO_INCREMENT);
                }
            }   
            break;     
        
        case IRP_MN_SET_POWER:
            // check if we have a valid power type
            switch (stack->Parameters.Power.Type) {
                case SystemPowerState:
                    {
                        // get the requested system power state
                        const SYSTEM_POWER_STATE system_state = stack->Parameters.Power.State.SystemState;
                        POWER_STATE device_state;
                        device_state.DeviceState = PowerDeviceD0;
                        
                        // check if the system state is not working. If its not working we need
                        // to map it to a device power state
                        if (system_state != PowerSystemWorking) {
                            if (dev_ext->power_1_request_busy) {
                                device_state.DeviceState = dev_ext->resource.DeviceState[system_state];
                            }
                            else {
                                device_state.DeviceState = PowerDeviceD3;
                            }
                        }

                        if (device_state.DeviceState != dev_ext->current_power_state.DeviceState) {
                            // store the current IRP
                            dev_ext->power_irp = Irp;

                            // do a power request. The callback will release the spinlock
                            return PoRequestPowerIrp(
                                dev_ext->physicalDeviceObject,
                                IRP_MN_SET_POWER,
                                device_state,
                                power_request_complete,
                                DeviceObject,
                                nullptr
                            );
                        }
                        else {
                            // forward the irp to the next driver if we are not changing power states
                            // TODO: is this needed. We are not changing power states. We could just
                            // complete the irp right here. Figure out if this is needed

                            // copy the current irp stack location to the next
                            IoCopyCurrentIrpStackLocationToNext(Irp);

                            // mark we are ready for the next power irp
                            PoStartNextPowerIrp(Irp);
                            
                            // call the next driver
                            status = PoCallDriver(
                                ((chief_device_extension*)DeviceObject->DeviceExtension)->attachedDeviceObject,
                                Irp
                            );
                        }
                    }
                    break;

                case DevicePowerState:
                    {
                        // update the power state
                        const bool u = update_power_state(DeviceObject, stack->Parameters.Power.State.DeviceState);

                        // copy the current irp stack location to the next
                        IoCopyCurrentIrpStackLocationToNext(Irp);

                        // check if we need to add a new irp call
                        if (u) {
                            // get the next irp stack location
                            PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(Irp);

                            stack->CompletionRoutine = power_state_systemworking_complete;
                            stack->Context = DeviceObject;
                            stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;
                        }

                        // mark we are ready for the next power irp
                        PoStartNextPowerIrp(Irp);

                        // call the next driver 
                        status = PoCallDriver(
                            ((chief_device_extension*)DeviceObject->DeviceExtension)->attachedDeviceObject,
                            Irp
                        );

                        // check if we need to return early
                        if (u) {
                            // if we our callback is called we do not need to release the 
                            // spinlock here as it will be released in the callback
                            return status;
                        }
                    }    
                    break;

                default:
                    // TODO: there was a bug here in the driver. The spinlock 
                    // is was not being released. This would make it so the
                    // driver will never be deleted
                    status = STATUS_INVALID_PARAMETER_1;
                    break;
            }
            break;

        case IRP_MN_POWER_SEQUENCE:
        case IRP_MN_QUERY_POWER:
            // copy the current irp stack location to the next
            IoCopyCurrentIrpStackLocationToNext(Irp);

            // mark we are ready for the next power irp
            PoStartNextPowerIrp(Irp);
            
            // call the next driver
            status = PoCallDriver(
                ((chief_device_extension*)DeviceObject->DeviceExtension)->attachedDeviceObject,
                Irp
            );

        default:
            break;
    }

    // release the spinlock
    spinlock_release(DeviceObject);

    // return the status
    return status;
}

NTSTATUS mj_system_control(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    // clear the status and information
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    // aquire the spinlock
    spinlock_acquire(DeviceObject);

    // copy the current irp stack location to the next
    IoCopyCurrentIrpStackLocationToNext(Irp);

    // call the next driver
    const NTSTATUS status = IoCallDriver(
        ((chief_device_extension*)DeviceObject->DeviceExtension)->attachedDeviceObject,
        Irp
    );

    // release the spinlock
    spinlock_release(DeviceObject);

    return status;
}

NTSTATUS mj_pnp(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    return STATUS_SUCCESS;
}
