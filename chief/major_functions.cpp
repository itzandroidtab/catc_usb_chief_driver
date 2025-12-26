#include <limits.h>

#include "major_functions.hpp"
#include "spinlock.hpp"
#include "device_extension.hpp"
#include "usb.hpp"

NTSTATUS signal_event_complete(_DEVICE_OBJECT *DeviceObject, _IRP *Irp, void* Event) {
    KeSetEvent(reinterpret_cast<PRKEVENT>(Event), EVENT_INCREMENT, false);
    
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static bool delete_is_not_pending(__in struct _DEVICE_OBJECT *DeviceObject) {
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);
    
    // return if the device is not being removed or ejected and has a configuration descriptor
    return !dev_ext->device_removed && (dev_ext->usb_config_desc != nullptr) && !dev_ext->remove_pending && !dev_ext->hold_new_requests;
}

static NTSTATUS query_complete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    // check if we have a pending return
    if (Irp->PendingReturned) {
        IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;
    }

    // check if we have a config descriptor
    if (dev_ext->usb_config_desc) {
        // mark we are stopped so we dont accept new
        // ioctls/reads/writes if we gotten permission
        // from the lower driver
        volatile bool* value = reinterpret_cast<bool*>(Context);
        *value = NT_SUCCESS(Irp->IoStatus.Status);
    }

    // release the spinlock
    spinlock_decrement_notify(DeviceObject);

    // return success
    return STATUS_SUCCESS;
}

static NTSTATUS forward_to_next_power_driver(
    PDEVICE_OBJECT attachedDeviceObject, PIRP Irp, 
    PIO_COMPLETION_ROUTINE CompletionRoutine = nullptr, 
    void* Context = nullptr)
{
    // copy the current irp stack location to the next
    IoCopyCurrentIrpStackLocationToNext(Irp);

    // check if we have a completion routine
    if (CompletionRoutine) {
        // set the completion routine
        IoSetCompletionRoutine(
            Irp, CompletionRoutine,
            Context, true, true, true
        );
    }

    // mark we are ready for the next power irp
    PoStartNextPowerIrp(Irp);

    // call the driver
    return PoCallDriver(attachedDeviceObject, Irp);
}

static NTSTATUS forward_to_next_driver(
    PDEVICE_OBJECT attachedDeviceObject, PIRP Irp, 
    const bool skip = false,
    PIO_COMPLETION_ROUTINE CompletionRoutine = nullptr, 
    void* Context = nullptr) 
{
    // check if we need to copy the current irp stack location
    if (!skip) {
        IoCopyCurrentIrpStackLocationToNext(Irp);
    } 
    else {
        IoSkipCurrentIrpStackLocation(Irp);
    }

    // check if we have a completion routine
    if (CompletionRoutine) {
        // set the completion routine
        IoSetCompletionRoutine(
            Irp, CompletionRoutine,
            Context, true, true, true
        );
    }

    // call the driver
    return IofCallDriver(attachedDeviceObject, Irp);
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

static void power_request_complete(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction, POWER_STATE PowerState, PVOID Context, PIO_STATUS_BLOCK IoStatus) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    // get the irp from the context
    PIRP Irp = reinterpret_cast<PIRP>(Context);
    
    // forward the request to the next power driver
    (void)forward_to_next_power_driver(dev_ext->attachedDeviceObject, Irp);

    // decrement the power irp count
    InterlockedDecrement(&dev_ext->power_irp_count);

    // release the spinlock
    spinlock_decrement_notify(DeviceObject);
}

static NTSTATUS power_state_systemworking_complete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    // check if we have a pending return
    if (Irp->PendingReturned) {
        // TODO: doesnt this mean we need to return STATUS_PENDING here?
        IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;
    }

    dev_ext->current_power_state.DeviceState = PowerDeviceD0;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    // release the spinlock
    spinlock_decrement_notify(DeviceObject);

    // return success
    return STATUS_SUCCESS;
}

static DEVICE_POWER_STATE system_state_to_device_power_state(_DEVICE_OBJECT* DeviceObject, SYSTEM_POWER_STATE state) {
    // check if we have a valid state
    if (state >= POWER_SYSTEM_MAXIMUM) {
        // if we dont have a valid state, return the deepest power state
        return PowerDeviceD3;
    }

    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    // return the device power state for the given system power state
    return dev_ext->device_capabilities.DeviceState[state];
}

static NTSTATUS usb_cleanup_memory(_DEVICE_OBJECT* DeviceObject) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);
    
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
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

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
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    // aquire the spinlock
    spinlock_increment(DeviceObject);

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
                file->FsContext = static_cast<void*>(&dev_ext->usb_interface_info->Pipes[pipe_index]);

                // mark the pipe as allocated
                dev_ext->allocated_pipes[pipe_index] = true;

                // increment the interlocked value
                spinlock_increment(DeviceObject);
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;

    IofCompleteRequest(Irp, IO_NO_INCREMENT);

    // release the spinlock
    spinlock_decrement_notify(DeviceObject);

    return status;
}

NTSTATUS mj_close(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    // acquire the spinlock
    spinlock_increment(DeviceObject);

    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

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

                // decrement the pipe count
                spinlock_decrement(DeviceObject);
            }
        }
    }

    // release the spinlock
    spinlock_decrement_notify(DeviceObject);

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
    spinlock_increment(DeviceObject);

    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

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
        usb_chief_vendor_request* vendor_request = reinterpret_cast<usb_chief_vendor_request*>(Irp->AssociatedIrp.SystemBuffer);

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
                status = usb_set_alternate_setting(DeviceObject, dev_ext->usb_config_desc, vendor_request->request & 0xff);
                break;
            case CTL_CODE(FILE_DEVICE_USB, 3, METHOD_BUFFERED, FILE_ANY_ACCESS): // 0x22000c
                if (dev_ext->bcdUSB.has_value()) {
                    // copy the bcdUSB value to the vendor request
                    vendor_request->request = dev_ext->bcdUSB.has_value();

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
    spinlock_decrement_notify(DeviceObject);

    return status;
}

NTSTATUS mj_power(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);   

    // get the current stack location
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    // acquire the spinlock
    spinlock_increment(DeviceObject);

    NTSTATUS status = STATUS_SUCCESS;

    switch (stack->MinorFunction) {
        case IRP_MN_WAIT_WAKE:
            // the USB chief doesnt support wake from any sleep state. Dont bother
            // checking the power state and just fail the request
            status = Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

            // complete the irp
            IofCompleteRequest(Irp, IO_NO_INCREMENT);
            break;     
        
        case IRP_MN_SET_POWER:
            // check if we have a valid power type
            switch (stack->Parameters.Power.Type) {
                case SystemPowerState:
                    {
                        // get the requested system power state
                        const SYSTEM_POWER_STATE system_state = stack->Parameters.Power.State.SystemState;
                        
                        // use the device mapping to get the device power state
                        POWER_STATE device_state;
                        device_state.DeviceState = system_state_to_device_power_state(DeviceObject, system_state);
                        
                        // check if we need to change the power state
                        if (device_state.DeviceState != dev_ext->current_power_state.DeviceState) {
                            // store the current IRP
                            InterlockedIncrement(&dev_ext->power_irp_count);

                            // do a power request. The callback will release the spinlock
                            return PoRequestPowerIrp(
                                dev_ext->physicalDeviceObject,
                                IRP_MN_SET_POWER,
                                device_state,
                                power_request_complete,
                                Irp,
                                nullptr
                            );
                        }
                        else {
                            // forward the request to the next power driver
                            status = forward_to_next_power_driver(
                                dev_ext->attachedDeviceObject,
                                Irp
                            );
                        }
                    }
                    break;

                case DevicePowerState:
                    {
                        // get the new state
                        const auto& new_state = stack->Parameters.Power.State.DeviceState;

                        // check if we are switching to PowerDeviceD0
                        const bool to_deviceD0 = (new_state == PowerDeviceD0);

                        // check if we have a valid new state. We dont store PowerDeviceUnspecified or
                        // above/equal to PowerDeviceMaximum
                        if (new_state > PowerDeviceUnspecified && new_state < PowerDeviceMaximum) {
                            // update the current power state
                            dev_ext->current_power_state.DeviceState = new_state;
                        }

                        // forward the request to the next power driver. Add
                        // the completion routine if needed based on the update 
                        // result. If we are going to D0 the completion routine 
                        // will set it to this state and release the spinlock
                        status = forward_to_next_power_driver(
                            dev_ext->attachedDeviceObject, Irp, 
                            (to_deviceD0 ? power_state_systemworking_complete : nullptr), 
                            nullptr
                        );

                        // check if we need to return early
                        if (to_deviceD0) {
                            // if we our callback is called we do not need to release the 
                            // spinlock here as it will be released in the callback
                            return status;
                        }
                    }    
                    break;

                default:
                    // return sucess. Still release the spinlock below
                    status = STATUS_SUCCESS;
                    break;
            }
            break;

        case IRP_MN_POWER_SEQUENCE:
        case IRP_MN_QUERY_POWER:
        default:
            status = forward_to_next_power_driver(
                dev_ext->attachedDeviceObject,
                Irp
            );
            break;
    }

    // release the spinlock
    spinlock_decrement_notify(DeviceObject);

    // return the status
    return status;
}

NTSTATUS mj_system_control(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    // clear the status and information
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    // aquire the spinlock
    spinlock_increment(DeviceObject);

    // get the device extension
    chief_device_extension* dev_ext = reinterpret_cast<chief_device_extension*>(DeviceObject->DeviceExtension);

    // call the next driver
    const NTSTATUS status = forward_to_next_driver(dev_ext->attachedDeviceObject, Irp);

    // release the spinlock
    spinlock_decrement_notify(DeviceObject);

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
    spinlock_increment(DeviceObject);

    NTSTATUS status = STATUS_SUCCESS;

    // check the minor function
    switch (stack->MinorFunction) {
        case IRP_MN_START_DEVICE:
            {
                // create an event
                KEVENT event;
                KeInitializeEvent(&event, NotificationEvent, false);

                // call the next driver
                status = forward_to_next_driver(
                    dev_ext->attachedDeviceObject, Irp,
                    false, signal_event_complete,
                    &event
                );

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

                // check if the start device was successful on the lower driver
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

                        // check if we can change to the first alternate setting
                        if (NT_SUCCESS(status)) {
                            // set the alternate setting to 0
                            status = usb_set_alternate_setting(DeviceObject, dev_ext->usb_config_desc, 0);
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
                spinlock_decrement_notify(DeviceObject);
                break;
            }

        case IRP_MN_REMOVE_DEVICE:
            // decrement the pipe_count we incremented at the
            // start of the mj_pnp function
            spinlock_decrement_notify(DeviceObject);
            
            // stop everything that is running
            dev_ext->device_removed = true;
            usb_pipe_abort(DeviceObject);

            // copy the current irp stack location to the next
            status = forward_to_next_driver(dev_ext->attachedDeviceObject, Irp);

            // Decrement the pipe_count again. This is to match the increment
            // we did when opening the device in add_device. This way we ensure
            // that the pipe count will reach zero when all pipes are closed
            spinlock_decrement_notify(DeviceObject);

            // wait for all pipes to be closed. The pipe count
            // reaching zero will signal the event
            KeWaitForSingleObject(
                &dev_ext->pipe_count_empty, Suspended, KernelMode, false, nullptr
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

            // clear the hold_new_requests flag
            dev_ext->hold_new_requests = false;
            
            if (NT_SUCCESS(status)) {
                // Forward the IRP to the next driver
                status = forward_to_next_driver(dev_ext->attachedDeviceObject, Irp);
            }
            else {
                Irp->IoStatus.Status = status;
                IofCompleteRequest(Irp, IO_NO_INCREMENT);
            }

            spinlock_decrement_notify(DeviceObject);
            break;

        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
            // forward the irp to the next driver. In the 
            // callback complete routine we will set the 
            // hold_new_requests/remove_pending flag based on 
            // the result and release the spinlock
            status = forward_to_next_driver(
                dev_ext->attachedDeviceObject, Irp,
                false, query_complete, (
                    (stack->MinorFunction == IRP_MN_QUERY_STOP_DEVICE) ? 
                    reinterpret_cast<void*>(const_cast<bool*>(&dev_ext->hold_new_requests)) : 
                    reinterpret_cast<void*>(const_cast<bool*>(&dev_ext->remove_pending))
                )
            );
            break;

        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
            // check if we have a config descriptor
            if (!dev_ext->usb_config_desc) {
                // skip the irp
                status = forward_to_next_driver(
                    dev_ext->attachedDeviceObject, Irp, true
                );
            }
            else {
                if (stack->MinorFunction == IRP_MN_CANCEL_STOP_DEVICE) {
                    dev_ext->hold_new_requests = false;
                }
                else if (stack->MinorFunction == IRP_MN_CANCEL_REMOVE_DEVICE) {
                    dev_ext->remove_pending = false;
                }

                Irp->IoStatus.Status = STATUS_SUCCESS;

                // forward the IRP to the next driver
                status = forward_to_next_driver(dev_ext->attachedDeviceObject, Irp);
            }

            // release the spinlock
            spinlock_decrement_notify(DeviceObject);
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            // release the spinlock
            spinlock_decrement_notify(DeviceObject);

            // mark we are ejecting
            dev_ext->device_removed = true;

            // stop the device
            usb_pipe_abort(DeviceObject);

            // set the irp status to success
            Irp->IoStatus.Status = STATUS_SUCCESS;

            // forward to the next driver
            status = forward_to_next_driver(dev_ext->attachedDeviceObject, Irp);
            break;

        default:
            // forward to the next driver
            status = forward_to_next_driver(dev_ext->attachedDeviceObject, Irp);

            // release the spinlock
            spinlock_decrement_notify(DeviceObject);
            break;
    }

    return status;
}
