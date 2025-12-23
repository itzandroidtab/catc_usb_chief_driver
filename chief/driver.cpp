

#include "driver.hpp"
#include "major_functions.hpp"
#include "device_extension.hpp"
#include "spinlock.hpp"

/**
 * @brief Unload routine for the driver.
 * 
 * @param DriverObject 
 */
static void driver_unload(__in struct _DRIVER_OBJECT *DriverObject) {
    return;
}

NTSTATUS add_chief_device(PDRIVER_OBJECT driver_object, PDEVICE_OBJECT* device_object) {
    // the device and symbolic link names
    constexpr static wchar_t device_name[] = L"\\Device\\ChiefUSB";
    constexpr static wchar_t symbolic_link_name[] = L"\\DosDevices\\ChiefUSB";

    // create unicode strings for the names
    UNICODE_STRING device_name_unicode;
    UNICODE_STRING symbolic_link_name_unicode;

    // initialize the unicode strings
    RtlInitUnicodeString(&device_name_unicode, device_name);
    RtlInitUnicodeString(&symbolic_link_name_unicode, symbolic_link_name);

    // create the device
    NTSTATUS status = IoCreateDevice(
        driver_object,
        sizeof(chief_device_extension),
        &device_name_unicode,
        FILE_DEVICE_USB,
        0,
        FALSE,
        device_object
    );

    // check for errors
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // create the symbolic link
    status = IoCreateSymbolicLink(&symbolic_link_name_unicode, &device_name_unicode);

    // initialize the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)((*device_object)->DeviceExtension);

    // TODO: if the symbolic link creation fails, we should delete the device object
    // initialize fields
    dev_ext->max_length = 64000;

    // initalize the events
    KeInitializeEvent(&dev_ext->event0, NotificationEvent, FALSE);
    KeInitializeEvent(&dev_ext->event1, NotificationEvent, FALSE);
    KeInitializeEvent(&dev_ext->event2, NotificationEvent, FALSE);
    KeInitializeEvent(&dev_ext->power_complete_event, NotificationEvent, FALSE);

    // initialize spinlocks
    KeInitializeSpinLock(&dev_ext->spinlock0);
    KeInitializeSpinLock(&dev_ext->spinlock1);

    // reset the allocated pipes and usb_interface_info
    dev_ext->allocated_pipes = nullptr;
    dev_ext->usb_interface_info = nullptr;

    return status;
}

NTSTATUS io_call_start_device(PDEVICE_OBJECT device_object, _DEVICE_CAPABILITIES* resource) {
    // allocate a irp
    PIRP irp = IoAllocateIrp(device_object->StackSize, FALSE);

    // check for errors
    if (irp == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    // get the current stack location
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);

    // setup the stack for a pnp irp query capabilities
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
    stack->CompletionRoutine = signal_event_complete;
    stack->Context = (void*)&event;
    stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;
    stack->Parameters.DeviceCapabilities.Capabilities = resource;

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    // send the irp down
    NTSTATUS status = IoCallDriver(device_object, irp);

    // check if we need to wait for the irp
    if (status == STATUS_PENDING) {
        // wait for the event to be signaled
        status = KeWaitForSingleObject(
            &event,
            Suspended,
            KernelMode,
            FALSE,
            nullptr
        );

        // update the status with the irp status
        status = irp->IoStatus.Status;
    }

    // free the irp
    IoFreeIrp(irp);

    return status;
}

/**
 * @brief Add the device
 * 
 * @param DriverObject 
 * @param PhysicalDeviceObject 
 * @return NTSTATUS 
 */
static NTSTATUS add_device(__in struct _DRIVER_OBJECT *DriverObject, __in struct _DEVICE_OBJECT *PhysicalDeviceObject) {
    PDEVICE_OBJECT device_object = nullptr;

    // Add the device using the device and symbolic link names
    NTSTATUS result = add_chief_device(DriverObject, &device_object);

    // check for errors
    if (!NT_SUCCESS(result)) {
        return result;
    }

    // set the flags of the device object
    device_object->Flags |= DO_DIRECT_IO | DO_POWER_PAGABLE;

    // initialize the device extension
    chief_device_extension* dev_ext = (chief_device_extension*)device_object->DeviceExtension;

    // store the physical device object
    dev_ext->physicalDeviceObject = PhysicalDeviceObject;

    // attach to the device stack
    dev_ext->attachedDeviceObject = IoAttachDeviceToDeviceStack(device_object, PhysicalDeviceObject);
    
    // check if we have a valid attached device object
    if (dev_ext->attachedDeviceObject == NULL) {
        // delete the allocated object
        IoDeleteDevice(device_object);

        // return no such device
        return STATUS_NO_SUCH_DEVICE;
    }

    // clear the resource structure
    dev_ext->resource = {};
    dev_ext->resource.Size = sizeof(_DEVICE_CAPABILITIES);
    dev_ext->resource.Version = 1;
    dev_ext->resource.Address = (ULONG)-1;
    dev_ext->resource.UINumber = (ULONG)-1;

    // start the device
    io_call_start_device(dev_ext->attachedDeviceObject, &dev_ext->resource);

    // set the current power state to unspecified
    dev_ext->powerstate2.DeviceState = PowerDeviceUnspecified;

    // acquire the spinlock
    spinlock_acquire(device_object);

    // clear the flag we are inititializing
    device_object->Flags &= ~DO_DEVICE_INITIALIZING;

    // return success
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    // setup the AddDevice and unload routines
    DriverObject->DriverUnload = driver_unload;
    DriverObject->DriverExtension->AddDevice = add_device;

    // setup all the major function pointers
    DriverObject->MajorFunction[IRP_MJ_CREATE] = mj_create;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = mj_close;
    DriverObject->MajorFunction[IRP_MJ_READ] = mj_read;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = mj_write;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = mj_device_control;
    DriverObject->MajorFunction[IRP_MJ_POWER] = mj_power;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = mj_system_control;
    DriverObject->MajorFunction[IRP_MJ_PNP] = mj_pnp;

    // return success
    return STATUS_SUCCESS;
}