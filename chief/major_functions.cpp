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
    return !dev_ext->is_removing && dev_ext->has_config_desc && !dev_ext->is_removing && !dev_ext->someflag_22;
}

static ULONG get_pipe_from_unicode_str(_DEVICE_OBJECT* DeviceObject, _UNICODE_STRING* FileName) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;
    
    // get the length in characters
    const unsigned int length_in_chars = FileName->Length / sizeof(wchar_t);

    // search backwards for the last digit in the file name. Then we will read all the 
    // digits until we hit a non-digit character. This is the offset of the pipe in the
    // allocated pipes array.
    // example: "Device\ChiefUSB\Pipe13" -> we want to read '13' as the pipe index
    int index;
    for (index = (length_in_chars - 1); index >= 0; index--) {
        // get the current character
        const wchar_t ch = FileName->Buffer[index];

        // check if its a digit
        if (ch < L'0' || ch > L'9') {
            break;
        }
    }

    // check if we found any digits
    if (!index) {
        return ULONG_MAX;
    }

    int pipe_offset = 0;
    int multiplier = 1;
    
    // convert the digits to a number. Convert backwards from index to 0
    for (; index >= 0; index--) {
        // get the current character
        const wchar_t ch = FileName->Buffer[index];

        // check if its a digit. If not we have reached the end of the number
        if (ch < L'0' || ch > L'9') {
            break;
        }

        // convert to number
        const int digit = ch - L'0';

        // add the digit to the offset
        pipe_offset += (digit * multiplier);
        multiplier *= 10;
    }

    return pipe_offset;
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
    if (state != PowerDeviceD0) {
        return true;
    }

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

NTSTATUS usb_send_urb(_DEVICE_OBJECT* DeviceObject, void* Urb) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;

    // create a event
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, false);

    struct _IO_STATUS_BLOCK IoStatusBlock = {};

    PIRP irp = IoBuildDeviceIoControlRequest(
        CTL_CODE(FILE_DEVICE_USB, 0x0, METHOD_NEITHER, FILE_ANY_ACCESS),
        dev_ext->attachedDeviceObject,
        0,
        0,
        0,
        0,
        TRUE,
        &event,
        &IoStatusBlock
    );

    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // get the current stack location and store the context
    IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1 = Urb;

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
    constexpr static unsigned int size = 0x50;

    _URB_CONTROL_VENDOR_OR_CLASS_REQUEST * usb = (_URB_CONTROL_VENDOR_OR_CLASS_REQUEST*)ExAllocatePoolWithTag(
        NonPagedPool, size, 0x206D6457u
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
    usb->Hdr.Function = URB_FUNCTION_VENDOR_DEVICE;
    usb->Hdr.Length = size;
    usb->TransferBufferLength = Request->length;
    usb->TransferBufferMDL = 0;
    usb->TransferBuffer = buffer;
    usb->RequestTypeReservedBits = (
        ((receive ? BMREQUEST_DEVICE_TO_HOST : BMREQUEST_HOST_TO_DEVICE) << 7) |
        (BMREQUEST_VENDOR << 5) | BMREQUEST_TO_DEVICE
    );
    usb->Request = Request->Reqeuest & 0xff;
    usb->Value = Request->value;
    usb->Index = Request->index;
    usb->TransferFlags = receive ? 3 : 0;
    usb->UrbLink = 0;

    // send the urb
    NTSTATUS status = usb_send_urb(DeviceObject, (_URB*)usb);

    // check if we need to copy data back
    if (NT_SUCCESS(status) && receive && buffer) {
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
    if (AlternateSetting >= 2) {
        // TODO: invalid parameter sounds better here as error
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)deviceObject->DeviceExtension;

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

    struct _USBD_INTERFACE_LIST_ENTRY InterfaceList;
    InterfaceList.InterfaceDescriptor = nullptr;
    InterfaceList.Interface = 0;

    // create the urb
    PURB urb = USBD_CreateConfigurationRequestEx(
        ConfigurationDescriptor,
        &InterfaceList
    );

    if (!urb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // free the allocated pipes if we have any
    if (dev_ext->allocated_pipes) {
        ExFreePool(dev_ext->allocated_pipes);
    }

    // allocate new memory for the allocated pipes
    dev_ext->allocated_pipes = (bool*)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(bool) * InterfaceList.Interface->NumberOfPipes,
        0x206D6457u
    );

    // check if we got memory
    if (!dev_ext->allocated_pipes) {
        // free the urb before we exit
        ExFreePool(urb);

        // return we have an error
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // zero the allocated pipes
    memset(dev_ext->allocated_pipes, 0x00, sizeof(bool) * InterfaceList.Interface->NumberOfPipes);

    // set the maximum transfer size for all pipes to the max_length
    for (ULONG i = 0; i < InterfaceList.Interface->NumberOfPipes; i++) {
        InterfaceList.Interface->Pipes[i].MaximumTransferSize = dev_ext->max_length;
    }

    // set the urb
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->UrbHeader.Length = InterfaceList.Interface->Length;
    urb->UrbSelectInterface.ConfigurationHandle = ConfigurationDescriptor;

    // send the urb
    NTSTATUS status = usb_send_urb(deviceObject, urb);

    // set the usb config handle
    // TODO: not sure if this changes with the usb_send_urb call
    dev_ext->usb_config_handle = urb->UrbSelectConfiguration.ConfigurationHandle;

    // check if we need to update the interface information
    if (NT_SUCCESS(status)) {
        if (dev_ext->usb_interface_info) {
            ExFreePool(dev_ext->usb_interface_info);
        }

        // allocate new memory for the usb interface info
        dev_ext->usb_interface_info = (PUSBD_INTERFACE_INFORMATION)ExAllocatePoolWithTag(
            NonPagedPool,
            InterfaceList.Interface->Length,
            0x206D6457u
        );

        if (dev_ext->usb_interface_info) {
            // copy the interface info
            memcpy(dev_ext->usb_interface_info, InterfaceList.Interface, InterfaceList.Interface->Length);
        }
    }
    
    ExFreePool(urb);

    return STATUS_SUCCESS;
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
            ULONG pipe_index = get_pipe_from_unicode_str(DeviceObject, &file->FileName);

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
        const ULONG pipe_index = get_pipe_from_unicode_str(DeviceObject, &file->FileName);

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
    return STATUS_SUCCESS;
}

NTSTATUS mj_write(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    return STATUS_SUCCESS;
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
