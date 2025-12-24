#include <limits.h>

#include "major_functions.hpp"
#include "spinlock.hpp"
#include "device_extension.hpp"
#include "usb.hpp"

NTSTATUS signal_event_complete(_DEVICE_OBJECT *DeviceObject, _IRP *Irp, void* Event) {
    KeSetEvent((PRKEVENT)Event, EVENT_INCREMENT, false);
    
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static bool delete_is_not_pending(__in struct _DEVICE_OBJECT *DeviceObject) {
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;
    
    // return if the device is not being removed or ejected and has a configuration descriptor
    return !dev_ext->is_ejecting && dev_ext->has_config_desc && !dev_ext->is_removing && !dev_ext->is_stopped;
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

static void set_power_complete(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction, POWER_STATE PowerState, PVOID Context, PIO_STATUS_BLOCK IoStatus) {
    // TODO: why are they not using the DeviceObject parameter directly? No need
    // for a context here
    PDEVICE_OBJECT device_object = (PDEVICE_OBJECT)Context;

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)device_object->DeviceExtension;

    if (PowerState.DeviceState < dev_ext->target_power_state.DeviceState) {
        // set the event to signal the power request is complete
        KeSetEvent(&dev_ext->power_complete_event, EVENT_INCREMENT, false);
    }

    // release the spinlock
    spinlock_release(device_object);
}

static NTSTATUS change_power_state_impl(_DEVICE_OBJECT* DeviceObject, const POWER_STATE state) {
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
        if (state.DeviceState < dev_ext->target_power_state.DeviceState) {
            KeWaitForSingleObject(
                &dev_ext->power_complete_event,
                Suspended,
                KernelMode,
                false,
                nullptr
            );
        }

        // TODO: why is this not using the status from the completion routine?
        status = STATUS_SUCCESS;

        // TODO: if the status is not pending this variable will never 
        // be cleared. This doesnt look correct. This might need to be
        // done in the completion routine instead
        dev_ext->power_request_busy = false;
    }

    return status;
}

static NTSTATUS change_power_state(_DEVICE_OBJECT* DeviceObject, const bool a2) {
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
        if (dev_ext->pipe_open_count) {
            return STATUS_SUCCESS;
        }
    }
    else if (!dev_ext->pipe_open_count) {
        return STATUS_SUCCESS;
    }

    // get the power state
    const POWER_STATE state = dev_ext->target_power_state;

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

static void power_request_complete(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction, POWER_STATE PowerState, PVOID Context, PIO_STATUS_BLOCK IoStatus) {
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

static NTSTATUS power_state_systemworking_complete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
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

static bool update_power_state(_DEVICE_OBJECT* DeviceObject, const DEVICE_POWER_STATE state) {
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

static NTSTATUS usb_cleanup_memory(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;
    
    // clear the bcdUSB value
    dev_ext->bcdUSB.clear();

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

    return STATUS_SUCCESS;
}

static NTSTATUS mj_read_write_impl(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp, bool read) {
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
    if (length <= 64000) {
        // we can do it in one transfer
        return usb_send_bulk_or_interrupt_transfer(DeviceObject, Irp, read);
    }

    // TODO: change to better error code
    return STATUS_NOT_IMPLEMENTED;
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
                InterlockedIncrement(&dev_ext->pipe_open_count);

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
                InterlockedDecrement(&dev_ext->pipe_open_count);
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
                if (dev_ext->bcdUSB.has_value()) {
                    // copy the bcdUSB value to the vendor request
                    vendor_request->Reqeuest = dev_ext->bcdUSB.has_value();

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
                const DEVICE_POWER_STATE device_wake = dev_ext->device_capabilities.DeviceWake;

                // set target_power_state to the device wake state
                dev_ext->target_power_state.DeviceState = device_wake;

                // check if we are already in the PowerDeviceD0 state
                if (state.DeviceState != PowerDeviceD0 || dev_ext->target_power_state.DeviceState < state.DeviceState) {
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
                                device_state.DeviceState = dev_ext->device_capabilities.DeviceState[system_state];
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
    // the device and symbolic link names
    constexpr static wchar_t symbolic_link_name[] = L"\\DosDevices\\ChiefUSB";

    // get the current stack location
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    // get the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)DeviceObject->DeviceExtension;   

    // acquire the spinlock
    spinlock_acquire(DeviceObject);

    NTSTATUS status = STATUS_SUCCESS;

    // check the minor function
    switch (stack->MinorFunction) {
        case IRP_MN_START_DEVICE:
            {
                // create an event
                KEVENT event;
                KeInitializeEvent(&event, NotificationEvent, false);

                // copy the current stack location to the next
                IoCopyCurrentIrpStackLocationToNext(Irp);

                // get the next stack location
                PIO_STACK_LOCATION nextStack = IoGetNextIrpStackLocation(Irp);
                nextStack->Context = &event;
                nextStack->CompletionRoutine = signal_event_complete;
                nextStack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

                // call the next driver
                status = IofCallDriver(dev_ext->attachedDeviceObject, Irp);

                if (status == STATUS_PENDING) {
                    // wait for the event to be signaled
                    KeWaitForSingleObject(
                        &event,
                        Suspended,
                        KernelMode,
                        false,
                        nullptr
                    );

                    status = Irp->IoStatus.Status;
                }

                if (NT_SUCCESS(status)) {
                    // get the device descriptor
                    USB_DEVICE_DESCRIPTOR device_desc;

                    // get the device descriptor
                    status = usb_get_device_desc(DeviceObject, device_desc);

                    // check if we got the device descriptor
                    if (NT_SUCCESS(status)) {
                        // store the bcdUSB value
                        dev_ext->bcdUSB.set(device_desc.bcdUSB);

                        // try to get the configuration descriptor
                        status = usb_get_configuration_desc(DeviceObject, dev_ext->usb_config_desc);

                        // mark if we have a config descriptor
                        if (NT_SUCCESS(status)) {
                            dev_ext->has_config_desc = true;

                            // set the alternate setting to 0
                            status = usb_set_alternate_setting(DeviceObject, dev_ext->usb_config_desc, 0);
                        }
                        else {
                            dev_ext->has_config_desc = false;
                        }
                    }
                    else {
                        // if we failed to get the device descriptor 
                        // clear the bcdUSB value
                        dev_ext->bcdUSB.clear();
                    }

                    Irp->IoStatus.Status = status;
                }

                IofCompleteRequest(Irp, IO_NO_INCREMENT);
                spinlock_release(DeviceObject);
                break;
            }

        case IRP_MN_REMOVE_DEVICE:
            // stop everything that is running
            spinlock_release(DeviceObject);
            dev_ext->is_ejecting = true;
            usb_pipe_abort(DeviceObject);

            // copy the current irp stack location to the next
            IoCopyCurrentIrpStackLocationToNext(Irp);

            // call the next driver
            status = IofCallDriver(dev_ext->attachedDeviceObject, Irp);

            // release the spinlock again
            spinlock_release(DeviceObject);

            KeWaitForSingleObject(
                &dev_ext->event0, Suspended, KernelMode, false, nullptr
            );

            usb_cleanup_memory(DeviceObject);

            // create unicode strings for the names
            UNICODE_STRING symbolic_link_name_unicode;

            // initialize the unicode strings
            RtlInitUnicodeString(&symbolic_link_name_unicode, symbolic_link_name);

            // delete the symbolic link
            IoDeleteSymbolicLink(&symbolic_link_name_unicode);

            // detach and delete the device
            IoDetachDevice(dev_ext->attachedDeviceObject);
            IoDeleteDevice(DeviceObject);
            break;

        case IRP_MN_STOP_DEVICE:
            // select the config descriptor
            status = usb_clear_config_desc(DeviceObject);

            if (NT_SUCCESS(status)) {
                // Forward the IRP to the next driver
                IoCopyCurrentIrpStackLocationToNext(Irp);

                // call the next driver
                status = IofCallDriver(dev_ext->attachedDeviceObject, Irp);
            }
            else {
                Irp->IoStatus.Status = status;
                IofCompleteRequest(Irp, IO_NO_INCREMENT);
            }

            spinlock_release(DeviceObject);
            break;

        case IRP_MN_QUERY_REMOVE_DEVICE:
            // check if we have a config descriptor
            if (!dev_ext->has_config_desc) {
                // skip the irp
                IoSkipCurrentIrpStackLocation(Irp);
            }
            else {
                dev_ext->is_removing = true;

                // wait for event2
                KeWaitForSingleObject(
                    &dev_ext->event2, Suspended, KernelMode, false, nullptr
                );

                Irp->IoStatus.Status = STATUS_SUCCESS;

                // copy the current irp stack location to the next
                IoCopyCurrentIrpStackLocationToNext(Irp);
            }

            // call the next driver
            status = IofCallDriver(dev_ext->attachedDeviceObject, Irp);

            // release the spinlock
            spinlock_release(DeviceObject);
            break;

        case IRP_MN_QUERY_STOP_DEVICE:
            // check if we have a config descriptor
            if (!dev_ext->has_config_desc) {
                // skip the irp
                IoSkipCurrentIrpStackLocation(Irp);

                // call the next driver
                status = IofCallDriver(dev_ext->attachedDeviceObject, Irp);
            }
            else {
                dev_ext->is_stopped = true;
                Irp->IoStatus.Status = status = STATUS_SUCCESS;

                IofCompleteRequest(Irp, IO_NO_INCREMENT);
            }

            // release the spinlock
            spinlock_release(DeviceObject);
            break;

        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
            // check if we have a config descriptor
            if (!dev_ext->has_config_desc) {
                // skip the irp
                IoSkipCurrentIrpStackLocation(Irp);
            }
            else {
                if (stack->MinorFunction == IRP_MN_CANCEL_STOP_DEVICE) {
                    dev_ext->is_stopped = false;
                }
                else if (stack->MinorFunction == IRP_MN_CANCEL_REMOVE_DEVICE) {
                    dev_ext->is_removing = false;
                }

                Irp->IoStatus.Status = STATUS_SUCCESS;

                // Forward the IRP to the next driver
                IoCopyCurrentIrpStackLocationToNext(Irp);
            }

            // call the next driver
            status = IofCallDriver(dev_ext->attachedDeviceObject, Irp);

            // release the spinlock
            spinlock_release(DeviceObject);
            break;

        case IRP_MN_EJECT:
            // release the spinlock
            spinlock_release(DeviceObject);

            // mark we are ejecting
            dev_ext->is_ejecting = true;

            // stop the device
            usb_pipe_abort(DeviceObject);

            // set the irp status to success
            Irp->IoStatus.Status = STATUS_SUCCESS;

            // copy the current irp stack location to the next
            IoCopyCurrentIrpStackLocationToNext(Irp);

            // call the next driver
            status = IofCallDriver(dev_ext->attachedDeviceObject, Irp);
            break;

        default:
            // Forward the IRP to the next driver
            IoCopyCurrentIrpStackLocationToNext(Irp);

            // call the next driver
            status = IofCallDriver(dev_ext->attachedDeviceObject, Irp);

            // release the spinlock
            spinlock_release(DeviceObject);
            break;
    }

    return status;
}
