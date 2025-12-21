#include <limits.h>

#include "major_functions.hpp"
#include "spinlock.hpp"
#include "device_extension.hpp"

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
    if (dev_ext->powerstate0.DeviceState == state.DeviceState) {
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

    // check if we have a power irq pening or we have a power request busy
    if (dev_ext->irp0 || dev_ext->power_request_busy) {
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
    return STATUS_SUCCESS;
}

NTSTATUS mj_read(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    return STATUS_SUCCESS;
}

NTSTATUS mj_write(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    return STATUS_SUCCESS;
}

NTSTATUS mj_device_control(__in struct _DEVICE_OBJECT *DeviceObject, __inout struct _IRP *Irp) {
    return STATUS_SUCCESS;
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
                const POWER_STATE state = dev_ext->powerstate0;

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
        
        case IRP_MN_POWER_SEQUENCE:
        case IRP_MN_SET_POWER:
        case IRP_MN_QUERY_POWER:
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
