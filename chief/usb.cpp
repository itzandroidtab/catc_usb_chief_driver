#include "usb.hpp"
#include "pipe.hpp"

extern "C" {
    #include <usbdlib.h>
}

// the amount of alternate settings we support
constexpr static ULONG max_alternate_settings = 2;


static NTSTATUS usb_send_urb(_DEVICE_OBJECT* DeviceObject, PURB Urb) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

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
    stack->Parameters.Others.Argument1 = static_cast<void*>(Urb);

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

static NTSTATUS usb_bulk_or_interrupt_transfer_complete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    // check if we have a pending return
    if (Irp->PendingReturned) {
        IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;
    }
    
    // decrement the pipe open count
    decrement_active_pipe_count_and_notify(DeviceObject);

    // get the context urb
    _URB_BULK_OR_INTERRUPT_TRANSFER* urb = reinterpret_cast<_URB_BULK_OR_INTERRUPT_TRANSFER*>(Context);

    // set the irp status to success
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = urb->TransferBufferLength;

    // complete the irp
    IofCompleteRequest(Irp, IO_NO_INCREMENT);

    // free the bulk or interrupt request
    ExFreePool(urb);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

_URB_BULK_OR_INTERRUPT_TRANSFER* usb_create_bulk_or_interrupt_transfer(__inout struct _IRP *Irp, USBD_PIPE_INFORMATION* Payload, bool isInDirection) {
    // get the amount of data to transfer
    const int length = (Irp->MdlAddress) ? MmGetMdlByteCount(Irp->MdlAddress) : 0;

    // create the urb
    _URB_BULK_OR_INTERRUPT_TRANSFER* request = reinterpret_cast<_URB_BULK_OR_INTERRUPT_TRANSFER*>(ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER),
        0x206D6457u
    ));

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

NTSTATUS usb_send_bulk_or_interrupt_transfer(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp, bool read) {
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
    USBD_PIPE_INFORMATION* pipe_info = reinterpret_cast<USBD_PIPE_INFORMATION*>(file->FsContext);

    _URB_BULK_OR_INTERRUPT_TRANSFER* request = usb_create_bulk_or_interrupt_transfer(
        Irp, pipe_info, read
    );

    if (!request) {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;

        // complete the irp
        IofCompleteRequest(Irp, 0);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // get the next stack location
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(Irp);

    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = request;
    stack->CompletionRoutine = usb_bulk_or_interrupt_transfer_complete;
    stack->Context = request;
    stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

    // acquire the spinlock
    increment_active_pipe_count(DeviceObject);

    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    return IofCallDriver(dev_ext->attachedDeviceObject, Irp);
}

NTSTATUS usb_send_receive_vendor_request(_DEVICE_OBJECT* DeviceObject, usb_chief_vendor_request* Request, bool receive) {
    void* buffer = nullptr;

    // check if we need to allocate memeory
    if (Request->length) {
        buffer = ExAllocatePoolWithTag(NonPagedPool, Request->length, 0x206D6457u);

        if (!buffer) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // check if we need to copy data to the buffer. If we are 
        // receiving data, we dont need to copy
        if (!receive) {
            memcpy(buffer, Request->data, Request->length);
        }
    }

    // initialize the urb
    _URB_CONTROL_VENDOR_OR_CLASS_REQUEST usb = {};
    usb.Hdr.Function = URB_FUNCTION_VENDOR_DEVICE;
    usb.Hdr.Length = sizeof(_URB_CONTROL_VENDOR_OR_CLASS_REQUEST);
    usb.TransferBufferLength = Request->length;
    usb.TransferBufferMDL = nullptr;
    usb.TransferBuffer = buffer;
    usb.RequestTypeReservedBits = (
        ((receive ? BMREQUEST_DEVICE_TO_HOST : BMREQUEST_HOST_TO_DEVICE) << 7) |
        (BMREQUEST_VENDOR << 5) | BMREQUEST_TO_DEVICE
    );
    usb.Request = Request->request & 0xff;
    usb.Value = Request->value;
    usb.Index = Request->index;
    usb.TransferFlags = (
        receive ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : (USBD_TRANSFER_DIRECTION_OUT)
    );
    usb.UrbLink = nullptr;

    // send the urb
    NTSTATUS status = usb_send_urb(DeviceObject, reinterpret_cast<PURB>(&usb));

    // check if we need to copy data back
    if (NT_SUCCESS(status) && receive && buffer) {
        // update the request length
        Request->length = usb.TransferBufferLength & 0xffff;

        // copy the data back to the request structure
        memcpy(Request->data, buffer, usb.TransferBufferLength);
    }

    if (buffer) {
        ExFreePool(buffer);
    }

    return status;
}

NTSTATUS usb_set_alternate_setting(_DEVICE_OBJECT *deviceObject, PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor, unsigned char AlternateSetting) {
    // check if we have a valid alternate setting
    if (AlternateSetting >= max_alternate_settings) {
        return STATUS_INVALID_PARAMETER;
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
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(deviceObject->DeviceExtension);

    // free the allocated pipes if we have any
    if (dev_ext->allocated_pipes) {
        // TODO: maybe we dont need to free this. If the number of pipes
        // doesnt change between alternate settings?
        ExFreePool(dev_ext->allocated_pipes);
    }

    // allocate new memory for the allocated pipes
    dev_ext->allocated_pipes = reinterpret_cast<bool*>(ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(bool) * InterfaceList[0].Interface->NumberOfPipes,
        0x206D6457u
    ));

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

    // set the urb
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;

    // fixes the original issue. This was skipping the size of the
    // _URB_SELECT_CONFIGURATION structure and causing a 0x7e error
    urb->UrbHeader.Length = static_cast<USHORT>(GET_SELECT_CONFIGURATION_REQUEST_SIZE(
        1, InterfaceList[0].Interface->NumberOfPipes
    ));
    urb->UrbSelectConfiguration.ConfigurationDescriptor = ConfigurationDescriptor;

    // send the urb
    NTSTATUS status = usb_send_urb(deviceObject, reinterpret_cast<PURB>(urb));

    // check if we need to update the interface information
    if (NT_SUCCESS(status)) {
        // check if we need to free the old usb interface info
        if (dev_ext->usb_interface_info) {
            ExFreePool(dev_ext->usb_interface_info);
        }

        // allocate new memory for the usb interface info
        dev_ext->usb_interface_info = reinterpret_cast<PUSBD_INTERFACE_INFORMATION>(ExAllocatePoolWithTag(
            NonPagedPool,
            InterfaceList[0].Interface->Length,
            0x206D6457u
        ));

        if (dev_ext->usb_interface_info) {
            // copy the interface info
            memcpy(dev_ext->usb_interface_info, InterfaceList[0].Interface, InterfaceList[0].Interface->Length);
        }
    }
    
    ExFreePool(urb);

    return STATUS_SUCCESS;
}

static NTSTATUS usb_get_port_status(_DEVICE_OBJECT* DeviceObject, ULONG& Status) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    struct _IO_STATUS_BLOCK IoStatusBlock;

    // clear the status 
    Status = 0;

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
    stack->Parameters.Others.Argument1 = &Status;

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
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(deviceObject->DeviceExtension);

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
    NTSTATUS res = usb_get_port_status(DeviceObject, status);

    // check if we got a success and if the port is not enabled (bit 0) 
    // and if we are connected (bit 1)
    if (NT_SUCCESS(res) && ((status & 0x1) == 0) || ((status & 0x2) != 0)) {
        // we are connected but not enabled, reset the upstream port
        return usb_reset_upstream_port(DeviceObject);
    }

    return res;
}

NTSTATUS usb_sync_reset_pipe_clear_stall(__in struct _DEVICE_OBJECT *DeviceObject, USBD_PIPE_INFORMATION* Pipe) {
    // create the urb
    _URB_PIPE_REQUEST request = {};

    // initialize the urb
    request.Hdr.Length = sizeof(_URB_PIPE_REQUEST);
    request.Hdr.Function = URB_FUNCTION_RESET_PIPE;
    request.PipeHandle = Pipe->PipeHandle;

    // send the urb
    return usb_send_urb(DeviceObject, reinterpret_cast<PURB>(&request));
}

NTSTATUS usb_pipe_abort(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

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
        
        // initialize the URB for abort pipe
        _URB_PIPE_REQUEST urb = {};
        urb.Hdr.Length = sizeof(_URB_PIPE_REQUEST);
        urb.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
        urb.PipeHandle = interface_info->Pipes[i].PipeHandle;

        // send the URB
        status = usb_send_urb(DeviceObject, reinterpret_cast<PURB>(&urb));

        // check if we have an error
        if (!NT_SUCCESS(status)) {
            return status;
        }

        // mark the pipe as free
        dev_ext->allocated_pipes[i] = false;

        // decrement the pipe count
        decrement_active_pipe_count(DeviceObject);
    }

    return status;
}

NTSTATUS usb_get_configuration_desc(_DEVICE_OBJECT* DeviceObject, PUSB_CONFIGURATION_DESCRIPTOR& OutDescriptor) {
    // initial buffer size. This will be increased if needed
    ULONG buffer_size = 64;

    // initialize status
    NTSTATUS status = STATUS_SUCCESS;

    // the descriptor pointer
    PUSB_CONFIGURATION_DESCRIPTOR descriptor = nullptr;

    while (true) {
        // allocate memory for the configuration descriptor
        descriptor = reinterpret_cast<PUSB_CONFIGURATION_DESCRIPTOR>(ExAllocatePoolWithTag(
            NonPagedPool,
            buffer_size,
            0x206D6457u
        ));

        // check if we got memory
        if (!descriptor) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        // initialize the URB for getting configuration descriptor
        _URB_CONTROL_DESCRIPTOR_REQUEST urb = {};
        urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
        urb.Hdr.Length = sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST);
        urb.TransferBufferLength = buffer_size;
        urb.TransferBufferMDL = nullptr;
        urb.TransferBuffer = reinterpret_cast<void*>(descriptor);
        urb.DescriptorType = USB_CONFIGURATION_DESCRIPTOR_TYPE;
        urb.Index = 0;
        urb.LanguageId = 0;
        urb.UrbLink = nullptr;

        // send the URB
        status = usb_send_urb(DeviceObject, reinterpret_cast<PURB>(&urb));

        // check if we got an error
        if (!NT_SUCCESS(status)) {
            // free the descriptor memory
            ExFreePool(descriptor);
            descriptor = nullptr;
            break;
        }

        // check if we got the complete descriptor
        // if TransferBufferLength is 0 or the total length fits in our buffer, we're done
        if (!urb.TransferBufferLength || descriptor->wTotalLength <= buffer_size) {
            break;
        }

        // update the buffer size to the total length
        buffer_size = descriptor->wTotalLength;

        // we need a larger buffer, free the current one and try again
        ExFreePool(descriptor);
        descriptor = nullptr;
    }

    // store the descriptor. If we failed, descriptor will be nullptr
    OutDescriptor = descriptor;

    // set the alternate setting to 0
    return status;
}

NTSTATUS usb_get_device_desc(_DEVICE_OBJECT* DeviceObject, USB_DEVICE_DESCRIPTOR& OutDescriptor) {
    // create a usb request
    _URB_CONTROL_DESCRIPTOR_REQUEST usb_request = {};

    // initialize the URB for getting device descriptor
    usb_request.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    usb_request.Hdr.Length = sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST);
    usb_request.TransferBufferLength = sizeof(USB_DEVICE_DESCRIPTOR);
    usb_request.TransferBuffer = reinterpret_cast<void*>(&OutDescriptor);
    usb_request.DescriptorType = USB_DEVICE_DESCRIPTOR_TYPE;
    usb_request.Index = 0;
    usb_request.LanguageId = 0;

    // send the URB
    return usb_send_urb(DeviceObject, reinterpret_cast<PURB>(&usb_request));;
}

NTSTATUS usb_clear_config_desc(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // initialize the URB to deselect configuration (set to NULL)
    _URB_SELECT_CONFIGURATION urb = {};
    urb.Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb.Hdr.Length = sizeof(_URB_SELECT_CONFIGURATION);
    urb.ConfigurationDescriptor = nullptr;

    // send the urb
    const NTSTATUS status = usb_send_urb(DeviceObject, reinterpret_cast<PURB>(&urb));

    // if successful, mark that we no longer have a config descriptor
    if (NT_SUCCESS(status)) {
        // free the config descriptor memory if we have any
        if (dev_ext->usb_config_desc) {
            // free the config descriptor memory
            ExFreePool(dev_ext->usb_config_desc);
            dev_ext->usb_config_desc = nullptr;
        }
    }

    return status;
}
