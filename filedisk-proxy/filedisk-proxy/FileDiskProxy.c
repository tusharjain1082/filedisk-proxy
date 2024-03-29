/*
    This is a virtual disk driver for Windows that uses one or more files to
    emulate physical disks.
    Copyright (C) 1999-2015 Bo Brant�n.
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <ntifs.h>
#include <ntdddisk.h>
#include <ntddcdrm.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include <mountmgr.h>
#include <ntddvol.h>
#include <ntddscsi.h>
#include <windef.h>
#include <tchar.h>
#include <wchar.h>
#include ".\SHM\helper.h"

NTSYSAPI
NTSTATUS
NTAPI
ZwOpenProcessToken(
    IN HANDLE       ProcessHandle,
    IN ACCESS_MASK  DesiredAccess,
    OUT PHANDLE     TokenHandle
);

NTSYSAPI
NTSTATUS
NTAPI
ZwAdjustPrivilegesToken(
    IN HANDLE               TokenHandle,
    IN BOOLEAN              DisableAllPrivileges,
    IN PTOKEN_PRIVILEGES    NewState,
    IN ULONG                BufferLength,
    OUT PTOKEN_PRIVILEGES   PreviousState OPTIONAL,
    OUT PULONG              ReturnLength
);

#include "FileDiskShared.h"

#define PARAMETER_KEY           L"\\Parameters"

#define NUMBEROFDEVICES_VALUE   L"NumberOfDevices"

#define DEFAULT_NUMBEROFDEVICES 4

#define TOC_DATA_TRACK          0x04

HANDLE dir_handle;

typedef struct _DEVICE_EXTENSION {
    BOOLEAN                     media_in_device;
    UNICODE_STRING              device_name;
    ULONG                       device_number;
    DEVICE_TYPE                 device_type;
//    HANDLE                      file_handle;
    ANSI_STRING                 file_name;
    LARGE_INTEGER               file_size;
    BOOLEAN                     read_only;
    PSECURITY_CLIENT_CONTEXT    security_client_context;
    LIST_ENTRY                  list_head;
    KSPIN_LOCK                  list_lock;
    KEVENT                      request_event;
    PVOID                       thread_pointer;
    BOOLEAN                     terminate_thread;
    BOOLEAN                     fileClosingNow;
 //   HANDLE                      shmSemaphoreSync;
//    PKSEMAPHORE                 pshmSemaphoreSyncObj;

    // driver's shared memory
    PVOID                   	g_pSharedSection;
    PVOID                   	g_pSectionObj;
    HANDLE                  	g_hSection;
//    PVOID                   	pSharedDriverRequestDataSet;
    PVOID                   	pDriverRequestDataSetObj;
    HANDLE                      DriverRequestDataSet;
    PKEVENT                     KeDriverRequestDataSetObj;
    PVOID                   	pSharedProxyIdle;
    PVOID                   	pProxyIdleObj;
    HANDLE                      ProxyIdle;
    PKEVENT                     KeProxyIdleObj;
    PVOID                   	pSharedRequestComplete;
    PVOID                   	pRequestCompleteObj;
    HANDLE                      RequestComplete;
    PKEVENT                     KeRequestCompleteObj;
    //PRKMUTEX                    shmMutexObj;

    // pipelines
    //HANDLE                      hReqServerPipe;
} DEVICE_EXTENSION, * PDEVICE_EXTENSION;

#ifdef _PREFAST_
DRIVER_INITIALIZE DriverEntry;
__drv_dispatchType(IRP_MJ_CREATE) __drv_dispatchType(IRP_MJ_CLOSE) DRIVER_DISPATCH FileDiskCreateClose;
__drv_dispatchType(IRP_MJ_READ) __drv_dispatchType(IRP_MJ_WRITE) DRIVER_DISPATCH FileDiskReadWrite;
__drv_dispatchType(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH FileDiskDeviceControl;
KSTART_ROUTINE FileDiskThread;
DRIVER_UNLOAD FileDiskUnload;
#endif // _PREFAST_

NTSTATUS CreateSharedMemory(PDEVICE_OBJECT device_object);
void ReleaseSharedMemory(PDEVICE_OBJECT device_object);
NTSTATUS CreateSharedEvents(PDEVICE_OBJECT device_object);
NTSTATUS CreateSharedEventsKe(PDEVICE_OBJECT device_object);
void DeleteSharedEvents(PDEVICE_OBJECT device_object);
NTSTATUS SetSecurityAllAccess(HANDLE h, PVOID obj);
NTSTATUS connectReqServerPipeline(PDEVICE_OBJECT device_object);
NTSTATUS disconnectReqServerPipeline(PDEVICE_OBJECT device_object);
void printStatusFile(char* path, char* status);
//VOID CancelIrpRoutine(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
//NTSTATUS DispatchCleanup(PDEVICE_OBJECT fdo, PIRP Irp);
//NTSTATUS CompleteRequest(PIRP Irp, NTSTATUS status, ULONG_PTR Information);
//VOID FileDiskClearQueue(IN PVOID Context);
VOID FileDiskFreeIrpWithMdls(PIRP Irp);

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT   DriverObject,
    IN PUNICODE_STRING  RegistryPath
);

NTSTATUS
FileDiskCreateDevice(
    IN PDRIVER_OBJECT   DriverObject,
    IN ULONG            Number,
    IN DEVICE_TYPE      DeviceType
);

VOID
FileDiskUnload(
    IN PDRIVER_OBJECT   DriverObject
);

PDEVICE_OBJECT
FileDiskDeleteDevice(
    IN PDEVICE_OBJECT   DeviceObject
);

NTSTATUS
FileDiskCreateClose(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
);

NTSTATUS
FileDiskReadWrite(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
);

NTSTATUS
FileDiskDeviceControl(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
);

VOID
FileDiskThread(
    IN PVOID            Context
);

NTSTATUS
FileDiskOpenFile(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
);

NTSTATUS
FileDiskCloseFile(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
);

NTSTATUS
FileDiskAdjustPrivilege(
    IN ULONG            Privilege,
    IN BOOLEAN          Enable
);

#pragma code_seg("INIT")

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT   DriverObject,
    IN PUNICODE_STRING  RegistryPath
)
{
    UNICODE_STRING              parameter_path;
    RTL_QUERY_REGISTRY_TABLE    query_table[2];
    ULONG                       n_devices;
    NTSTATUS                    status;
    UNICODE_STRING              device_dir_name;
    OBJECT_ATTRIBUTES           object_attributes;
    ULONG                       n;
    USHORT                      n_created_devices;

    parameter_path.Length = 0;

    parameter_path.MaximumLength = RegistryPath->Length + sizeof(PARAMETER_KEY);

    parameter_path.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, parameter_path.MaximumLength, FILE_DISK_POOL_TAG);

    if (parameter_path.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyUnicodeString(&parameter_path, RegistryPath);

    RtlAppendUnicodeToString(&parameter_path, PARAMETER_KEY);

    RtlZeroMemory(&query_table[0], sizeof(query_table));

    query_table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED;
    query_table[0].Name = NUMBEROFDEVICES_VALUE;
    query_table[0].EntryContext = &n_devices;

    status = RtlQueryRegistryValues(
        RTL_REGISTRY_ABSOLUTE,
        parameter_path.Buffer,
        &query_table[0],
        NULL,
        NULL
    );

    ExFreePool(parameter_path.Buffer);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("FileDisk: Query registry failed, using default values.\n");
        n_devices = DEFAULT_NUMBEROFDEVICES;
    }

    RtlInitUnicodeString(&device_dir_name, DEVICE_DIR_NAME);

    InitializeObjectAttributes(
        &object_attributes,
        &device_dir_name,
        OBJ_PERMANENT,
        NULL,
        NULL
    );

    status = ZwCreateDirectoryObject(
        &dir_handle,
        DIRECTORY_ALL_ACCESS,
        &object_attributes
    );

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    ZwMakeTemporaryObject(dir_handle);

    for (n = 0, n_created_devices = 0; n < n_devices; n++)
    {
        status = FileDiskCreateDevice(DriverObject, n, FILE_DEVICE_DISK);

        if (NT_SUCCESS(status))
        {
            n_created_devices++;
        }
    }

    /* invalid original code
    for (n = 0; n < n_devices; n++)
    {
        status = FileDiskCreateDevice(DriverObject, n, FILE_DEVICE_CD_ROM);

        if (NT_SUCCESS(status))
        {
            n_created_devices++;
        }
    }
    */

    if (n_created_devices == 0)
    {
        ZwClose(dir_handle);
        return status;
    }
   

    DriverObject->MajorFunction[IRP_MJ_CREATE] = FileDiskCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = FileDiskCreateClose;
    DriverObject->MajorFunction[IRP_MJ_READ] = FileDiskReadWrite;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = FileDiskReadWrite;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = FileDiskDeviceControl;
    //DriverObject->MajorFunction[IRP_MJ_CLEANUP] = DispatchCleanup;

    DriverObject->DriverUnload = FileDiskUnload;

    return STATUS_SUCCESS;
}

NTSTATUS
FileDiskCreateDevice(
    IN PDRIVER_OBJECT   DriverObject,
    IN ULONG            Number,
    IN DEVICE_TYPE      DeviceType
)
{
    UNICODE_STRING      device_name;
    NTSTATUS            status;
    PDEVICE_OBJECT      device_object;
    PDEVICE_EXTENSION   device_extension;
    HANDLE              thread_handle;
    UNICODE_STRING      sddl;

    ASSERT(DriverObject != NULL);
    
    device_name.Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool, MAXIMUM_FILENAME_LENGTH * 2, FILE_DISK_POOL_TAG);

    if (device_name.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    device_name.Length = 0;
    device_name.MaximumLength = MAXIMUM_FILENAME_LENGTH * 2;

    /* original
    if (DeviceType == FILE_DEVICE_CD_ROM)
    {
        RtlUnicodeStringPrintf(&device_name, DEVICE_NAME_PREFIX L"Cd" L"%u", Number);
    }
    else
    {
        RtlUnicodeStringPrintf(&device_name, DEVICE_NAME_PREFIX L"%u", Number);
    }
    */
    // todo tushar
    RtlUnicodeStringPrintf(&device_name, DEVICE_NAME_PREFIX L"%u", Number);

    /* original
    RtlInitUnicodeString(&sddl, _T("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;BU)"));

    
    status = IoCreateDeviceSecure(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        &device_name,
        DeviceType,
        0,
        FALSE,
        &sddl,
        NULL,
        &device_object
    );
    */

    status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        &device_name,
        DeviceType,
        0,
        FALSE,
        &device_object
    );
    
    if (!NT_SUCCESS(status))
    {
        ExFreePool(device_name.Buffer);
        return status;
    }

    device_object->Flags |= DO_DIRECT_IO;

    device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;

    device_extension->media_in_device = FALSE;

    device_extension->device_name.Length = device_name.Length;
    device_extension->device_name.MaximumLength = device_name.MaximumLength;
    device_extension->device_name.Buffer = device_name.Buffer;
    device_extension->device_number = Number;
    device_extension->device_type = DeviceType;

    /* original
    if (DeviceType == FILE_DEVICE_CD_ROM)
    {
        device_object->Characteristics |= FILE_READ_ONLY_DEVICE;
        device_extension->read_only = TRUE;
    }
    */

    InitializeListHead(&device_extension->list_head);

    KeInitializeSpinLock(&device_extension->list_lock);

    KeInitializeEvent(
        &device_extension->request_event,
        SynchronizationEvent,
        FALSE
    );

    device_extension->terminate_thread = FALSE;
    
    // TODO TUSHAR
    device_extension->fileClosingNow = FALSE;

    // TODO tushar: create current device's shared memory
    CreateSharedMemory(device_object);

    // TODO tushar: create current device's events
    CreateSharedEventsKe(device_object);

    status = PsCreateSystemThread(
        &thread_handle,
        (ACCESS_MASK)0L,
        NULL,
        NULL,
        NULL,
        FileDiskThread,
        device_object
    );

    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(device_object);
        ExFreePool(device_name.Buffer);
        return status;
    }

    status = ObReferenceObjectByHandle(
        thread_handle,
        THREAD_ALL_ACCESS,
        NULL,
        KernelMode,
        &device_extension->thread_pointer,
        NULL
    );

    if (!NT_SUCCESS(status))
    {
        ZwClose(thread_handle);

        device_extension->terminate_thread = TRUE;

        KeSetEvent(
            &device_extension->request_event,
            (KPRIORITY)0,
            FALSE
        );

        IoDeleteDevice(device_object);

        ExFreePool(device_name.Buffer);

        return status;
    }

    ZwClose(thread_handle);

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")

VOID
FileDiskUnload(
    IN PDRIVER_OBJECT DriverObject
)
{
    PDEVICE_OBJECT device_object;

    PAGED_CODE();

    device_object = DriverObject->DeviceObject;

    while (device_object)
    {
        device_object = FileDiskDeleteDevice(device_object);
    }

    ZwClose(dir_handle);
}

PDEVICE_OBJECT
FileDiskDeleteDevice(
    IN PDEVICE_OBJECT DeviceObject
)
{
    PDEVICE_EXTENSION   device_extension;
    PDEVICE_OBJECT      next_device_object;

    PAGED_CODE();

    ASSERT(DeviceObject != NULL);

    device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    device_extension->terminate_thread = TRUE;

    KeSetEvent(
        &device_extension->request_event,
        (KPRIORITY)0,
        FALSE
    );

    KeWaitForSingleObject(
        device_extension->thread_pointer,
        Executive,
        KernelMode,
        FALSE,
        NULL
    );

    ObDereferenceObject(device_extension->thread_pointer);

    if (device_extension->device_name.Buffer != NULL)
    {
        ExFreePool(device_extension->device_name.Buffer);
    }

    if (device_extension->security_client_context != NULL)
    {
        SeDeleteClientSecurity(device_extension->security_client_context);
        ExFreePool(device_extension->security_client_context);
    }

    // finally delete device's shared memory
    ReleaseSharedMemory(DeviceObject);

    // finally delete device's shared events
    DeleteSharedEvents(DeviceObject);

#pragma prefast( suppress: 28175, "allowed in unload" )
    next_device_object = DeviceObject->NextDevice;

    IoDeleteDevice(DeviceObject);

    return next_device_object;
}

#pragma code_seg() // end "PAGE"

NTSTATUS
FileDiskCreateClose(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = FILE_OPENED;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

NTSTATUS
FileDiskReadWrite(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
)
{
    PDEVICE_EXTENSION   device_extension;
    PIO_STACK_LOCATION  io_stack;

    device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (!device_extension->media_in_device)
    {
        Irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
        Irp->IoStatus.Information = 0;

        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_NO_MEDIA_IN_DEVICE;
    }

    io_stack = IoGetCurrentIrpStackLocation(Irp);

    if (io_stack->Parameters.Read.Length == 0)
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;

        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_SUCCESS;
    }

    IoMarkIrpPending(Irp);

    // todo tushar
    // set the cancel routine
    //
    //IoSetCancelRoutine(Irp, CancelIrpRoutine);

    ExInterlockedInsertTailList(
        &device_extension->list_head,
        &Irp->Tail.Overlay.ListEntry,
        &device_extension->list_lock
    );

    KeSetEvent(
        &device_extension->request_event,
        (KPRIORITY)0,
        FALSE
    );

    return STATUS_PENDING;
}
/*
NTSTATUS DispatchCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PDEVICE_EXTENSION device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION io_stack;// = IoGetCurrentIrpStackLocation(Irp);
    PLIST_ENTRY         request;
    PIRP                irp;

//    KeAcquireSpinLockAtDpcLevel(&DeviceObject->DeviceQueue.Lock);
    DbgPrint("DispatchCleanup method running.\r\n");

    while ((request = ExInterlockedRemoveHeadList(
        &device_extension->list_head,
        &device_extension->list_lock
    )) != NULL)
    {
        irp = CONTAINING_RECORD(request, IRP, Tail.Overlay.ListEntry);
        io_stack = IoGetCurrentIrpStackLocation(irp);
        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
//        IoCompleteRequest(irp, IO_NO_INCREMENT);
        IoCompleteRequest(
            irp,
            (CCHAR)(NT_SUCCESS(irp->IoStatus.Status) ?
                IO_DISK_INCREMENT : IO_NO_INCREMENT)
        );
    }

    DbgPrint("DispatchCleanup method completed.\r\n");
    return STATUS_SUCCESS;
  //  KeReleaseSpinLockFromDpcLevel(&DeviceObject->DeviceQueue.Lock);
}
*/

/*
NTSTATUS DispatchCleanup(PDEVICE_OBJECT fdo, PIRP Irp)
{
    PDEVICE_EXTENSION pdx = (PDEVICE_EXTENSION)fdo->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fop = stack->FileObject;
    LIST_ENTRY cancellist;
    InitializeListHead(&cancellist);

    DbgPrint("DispatchCleanup method init\r\n");

    KIRQL oldirql;
    IoAcquireCancelSpinLock(&oldirql);
    KeAcquireSpinLockAtDpcLevel(&fdo->DeviceQueue.Lock);

    PLIST_ENTRY first = &fdo->DeviceQueue.DeviceListHead;
    PLIST_ENTRY next;

    for (next = first->Flink; next != first; )
    {
        PIRP QueuedIrp = CONTAINING_RECORD(next,
            IRP, Tail.Overlay.ListEntry);
        PIO_STACK_LOCATION QueuedIrpStack =
            IoGetCurrentIrpStackLocation(QueuedIrp);

        PLIST_ENTRY current = next;
        next = next->Flink;

        if (QueuedIrpStack->FileObject != fop)
            continue;

        IoSetCancelRoutine(QueuedIrp, NULL);
        RemoveEntryList(current);
        InsertTailList(&cancellist, current);
    }

    KeReleaseSpinLockFromDpcLevel(&fdo->DeviceQueue.Lock);
    IoReleaseCancelSpinLock(oldirql);

    while (!IsListEmpty(&cancellist))
    {
        next = RemoveHeadList(&cancellist);
        PIRP CancelIrp = CONTAINING_RECORD(next, IRP, Tail.Overlay.ListEntry);
        CompleteRequest(CancelIrp, STATUS_CANCELLED, 0);
    }

    DbgPrint("DispatchCleanup method completed\r\n");
    return CompleteRequest(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS CompleteRequest(PIRP Irp, NTSTATUS status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

VOID CancelIrpRoutine(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)

{
    PDEVICE_EXTENSION devExtension = DeviceObject->DeviceExtension;
    PLIST_ENTRY nextEl = NULL;
    PIRP cancelIrp = NULL;
    KIRQL irql;
    KIRQL cancelIrql = Irp->CancelIrql;

    DbgPrint("CancelIrpRoutine method init\r\n");

    //
    // release the cancel spinlock now
    //

    IoReleaseCancelSpinLock(cancelIrql);

    //
    // A thread has terminated and we should find a
    // cancelled Irp in our queue and complete it
    //

    KeAcquireSpinLock(&devExtension->list_lock, &irql);

    //
    // search our queue for an Irp to cancel
    //

    for (nextEl = devExtension->list_head.Flink;
        nextEl != &devExtension->list_head; )

    {

        cancelIrp = CONTAINING_RECORD(nextEl, IRP, Tail.Overlay.ListEntry);
        nextEl = nextEl->Flink;
        if (cancelIrp->Cancel) {

            //
            // dequeue THIS irp
            //

            RemoveEntryList(&cancelIrp->Tail.Overlay.ListEntry);

            //
            // and stop right here
            //
            break;

        }
        cancelIrp = NULL;

    }
    KeReleaseSpinLock(&devExtension->list_lock, irql);

    //
    // now if we found an irp to cancel, cancel it
    //

    if (cancelIrp) {

        //
        // this is our IRP to cancel
        // 
        cancelIrp->IoStatus.Status = STATUS_CANCELLED;
        cancelIrp->IoStatus.Information = 0;
        IoCompleteRequest(cancelIrp, IO_NO_INCREMENT);

        //IoCompleteRequest(cancelIrp, IO_NO_INCREMENT);

    }

    DbgPrint("CancelIrpRoutine method completed\r\n");

    //
    // we are done.
    //

}
*/


NTSTATUS
FileDiskDeviceControl(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
)
{
    PDEVICE_EXTENSION   device_extension;
    PIO_STACK_LOCATION  io_stack;
    NTSTATUS            status;
    NTSTATUS            WaitStatus;

    device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    io_stack = IoGetCurrentIrpStackLocation(Irp);
    
    if (!device_extension->media_in_device &&
        io_stack->Parameters.DeviceIoControl.IoControlCode !=
        IOCTL_REGISTER_FILE)
    {
        Irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
        Irp->IoStatus.Information = 0;

        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_NO_MEDIA_IN_DEVICE;
    }

    switch (io_stack->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_TEST:
    {
        // Test


        /*
        IoMarkIrpPending(Irp);


        ExInterlockedInsertTailList(
            &device_extension->list_head,
            &Irp->Tail.Overlay.ListEntry,
            &device_extension->list_lock
        );

        KeSetEvent(
            &device_extension->request_event,
            (KPRIORITY)0,
            FALSE
        );
         */


         //                 SeImpersonateClient(device_extension->security_client_context, NULL);

                          // TODO
        /*

        PCONTEXT_REQUEST ctx;
        IO_STATUS_BLOCK iostatus;

        // Test
        //for (ULONG i = 0; i < 100; i++)
        //{
        POPEN_FILE_INFORMATION open_file_information = (POPEN_FILE_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
        KeSetEvent(device_extension->KeDriverRequestDataSetObj, 1, TRUE);
        WaitStatus = KeWaitForSingleObject(device_extension->KeProxyIdleObj, UserRequest, UserMode, FALSE, NULL);// &timeout);
        KeResetEvent(device_extension->KeProxyIdleObj);
        connectReqServerPipeline(DeviceObject);
        ctx = (PCONTEXT_REQUEST)ExAllocatePoolWithTag(PagedPool, PIPE_BUFFER_SIZE_TRUE, FILE_DISK_POOL_TAG);
        ctx->signature = DRIVER_SIGNATURE;
        ctx->requestCtr = open_file_information->requestCtr;
        //ctx->requestCtr = i;
        ZwWriteFile(device_extension->hReqServerPipe, NULL, NULL, NULL, &iostatus, (PVOID)ctx, PIPE_BUFFER_SIZE + 1, NULL, NULL);
        //ZwFlushBuffersFile(device_extension->hReqServerPipe, &iostatus);
        ExFreePoolWithTag((PVOID)ctx, FILE_DISK_POOL_TAG);
        // wait for completion
        WaitStatus = KeWaitForSingleObject(device_extension->KeRequestCompleteObj, UserRequest, UserMode, FALSE, NULL);// &timeout);
        KeResetEvent(device_extension->KeRequestCompleteObj);
        // finally disconnect
        disconnectReqServerPipeline(DeviceObject);
        ctx = NULL;
        //}/
        */

        //                   PsRevertToSelf();

        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0; // io_stack->Parameters.Write.Length;

        //status = STATUS_SUCCESS;
        //Irp->IoStatus.Information = 0;
        break;
    }
    case IOCTL_REGISTER_FILE:
    {
        // TODO
        DbgPrint("FileDiskDeviceControl->IOCTL_REGISTER_FILE init.\r\n");

        SECURITY_QUALITY_OF_SERVICE security_quality_of_service;

        if (device_extension->media_in_device)
        {
            KdPrint(("FileDisk: IOCTL_REGISTER_FILE: Media already opened.\n"));

            status = STATUS_INVALID_DEVICE_REQUEST;
            Irp->IoStatus.Information = 0;
            break;
        }

        if (device_extension->security_client_context != NULL)
        {
            SeDeleteClientSecurity(device_extension->security_client_context);
        }
        else
        {
            device_extension->security_client_context =
                ExAllocatePoolWithTag(NonPagedPool, sizeof(SECURITY_CLIENT_CONTEXT), FILE_DISK_POOL_TAG);
        }

        RtlZeroMemory(&security_quality_of_service, sizeof(SECURITY_QUALITY_OF_SERVICE));

        security_quality_of_service.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
        security_quality_of_service.ImpersonationLevel = SecurityImpersonation;
        security_quality_of_service.ContextTrackingMode = SECURITY_STATIC_TRACKING;
        security_quality_of_service.EffectiveOnly = FALSE;

        SeCreateClientSecurity(
            PsGetCurrentThread(),
            &security_quality_of_service,
            FALSE,
            device_extension->security_client_context
        );

        // new code
        DbgPrint("FileDiskDeviceControl->IOCTL_REGISTER_FILE running.\r\n");
        SeImpersonateClient(device_extension->security_client_context, NULL);
        status = FileDiskOpenFile(DeviceObject, Irp);
        PsRevertToSelf();
        break;
    }

    case IOCTL_DEREGISTER_FILE:
    {
        //new code
        DbgPrint("FileDiskDeviceControl->IOCTL_DEREGISTER_FILE running.\r\n");
        status = FileDiskCloseFile(DeviceObject, Irp);
        break;
    }

    case IOCTL_FILE_DISK_QUERY_FILE:
    {
        POPEN_FILE_INFORMATION open_file_information;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(OPEN_FILE_INFORMATION) + device_extension->file_name.Length - sizeof(UCHAR))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        open_file_information = (POPEN_FILE_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

        open_file_information->FileSize.QuadPart = device_extension->file_size.QuadPart;
        open_file_information->ReadOnly = device_extension->read_only;
        open_file_information->FileNameLength = device_extension->file_name.Length;

        RtlCopyMemory(
            open_file_information->FileName,
            device_extension->file_name.Buffer,
            device_extension->file_name.Length
        );

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(OPEN_FILE_INFORMATION) +
            open_file_information->FileNameLength - sizeof(UCHAR);

        break;
    }

    case IOCTL_DISK_CHECK_VERIFY:
    case IOCTL_CDROM_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY2:
    {
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        break;
    }

    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
    {
        PDISK_GEOMETRY  disk_geometry;
        ULONGLONG       length;
        ULONG           sector_size;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(DISK_GEOMETRY))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        disk_geometry = (PDISK_GEOMETRY)Irp->AssociatedIrp.SystemBuffer;

        length = device_extension->file_size.QuadPart;

        if (device_extension->device_type != FILE_DEVICE_CD_ROM)
        {
            sector_size = 512;
        }
        else
        {
            sector_size = 2048;
        }

        disk_geometry->Cylinders.QuadPart = length / sector_size / 32 / 2;
        disk_geometry->MediaType = FixedMedia;
        disk_geometry->TracksPerCylinder = 2;
        disk_geometry->SectorsPerTrack = 32;
        disk_geometry->BytesPerSector = sector_size;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);

        break;
    }

    case IOCTL_DISK_GET_LENGTH_INFO:
    {
        PGET_LENGTH_INFORMATION get_length_information;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(GET_LENGTH_INFORMATION))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        get_length_information = (PGET_LENGTH_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

        get_length_information->Length.QuadPart = device_extension->file_size.QuadPart;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(GET_LENGTH_INFORMATION);

        break;
    }

    case IOCTL_DISK_GET_PARTITION_INFO:
    {
        PPARTITION_INFORMATION  partition_information;
        ULONGLONG               length;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(PARTITION_INFORMATION))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        partition_information = (PPARTITION_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

        length = device_extension->file_size.QuadPart;

        partition_information->StartingOffset.QuadPart = 0;
        partition_information->PartitionLength.QuadPart = length;
        partition_information->HiddenSectors = 1;
        partition_information->PartitionNumber = 0;
        partition_information->PartitionType = 0;
        partition_information->BootIndicator = FALSE;
        partition_information->RecognizedPartition = FALSE;
        partition_information->RewritePartition = FALSE;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION);

        break;
    }

    case IOCTL_DISK_GET_PARTITION_INFO_EX:
    {
        PPARTITION_INFORMATION_EX   partition_information_ex;
        ULONGLONG                   length;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(PARTITION_INFORMATION_EX))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        partition_information_ex = (PPARTITION_INFORMATION_EX)Irp->AssociatedIrp.SystemBuffer;

        length = device_extension->file_size.QuadPart;

        partition_information_ex->PartitionStyle = PARTITION_STYLE_MBR;
        partition_information_ex->StartingOffset.QuadPart = 0;
        partition_information_ex->PartitionLength.QuadPart = length;
        partition_information_ex->PartitionNumber = 0;
        partition_information_ex->RewritePartition = FALSE;
        partition_information_ex->Mbr.PartitionType = 0;
        partition_information_ex->Mbr.BootIndicator = FALSE;
        partition_information_ex->Mbr.RecognizedPartition = FALSE;
        partition_information_ex->Mbr.HiddenSectors = 1;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION_EX);

        break;
    }

    case IOCTL_DISK_IS_WRITABLE:
    {
        if (!device_extension->read_only)
        {
            status = STATUS_SUCCESS;
        }
        else
        {
            status = STATUS_MEDIA_WRITE_PROTECTED;
        }
        Irp->IoStatus.Information = 0;
        break;
    }

    case IOCTL_DISK_MEDIA_REMOVAL:
    case IOCTL_STORAGE_MEDIA_REMOVAL:
    {
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        break;
    }

    case IOCTL_CDROM_READ_TOC:
    {
        PCDROM_TOC cdrom_toc;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(CDROM_TOC))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        cdrom_toc = (PCDROM_TOC)Irp->AssociatedIrp.SystemBuffer;

        RtlZeroMemory(cdrom_toc, sizeof(CDROM_TOC));

        cdrom_toc->FirstTrack = 1;
        cdrom_toc->LastTrack = 1;
        cdrom_toc->TrackData[0].Control = TOC_DATA_TRACK;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(CDROM_TOC);

        break;
    }

    case IOCTL_CDROM_GET_LAST_SESSION:
    {
        PCDROM_TOC_SESSION_DATA cdrom_toc_s_d;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(CDROM_TOC_SESSION_DATA))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        cdrom_toc_s_d = (PCDROM_TOC_SESSION_DATA)Irp->AssociatedIrp.SystemBuffer;

        RtlZeroMemory(cdrom_toc_s_d, sizeof(CDROM_TOC_SESSION_DATA));

        cdrom_toc_s_d->FirstCompleteSession = 1;
        cdrom_toc_s_d->LastCompleteSession = 1;
        cdrom_toc_s_d->TrackData[0].Control = TOC_DATA_TRACK;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(CDROM_TOC_SESSION_DATA);

        break;
    }

    case IOCTL_DISK_SET_PARTITION_INFO:
    {
        if (device_extension->read_only)
        {
            status = STATUS_MEDIA_WRITE_PROTECTED;
            Irp->IoStatus.Information = 0;
            break;
        }

        if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
            sizeof(SET_PARTITION_INFORMATION))
        {
            status = STATUS_INVALID_PARAMETER;
            Irp->IoStatus.Information = 0;
            break;
        }

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;

        break;
    }

    case IOCTL_DISK_VERIFY:
    {
        PVERIFY_INFORMATION verify_information;

        if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
            sizeof(VERIFY_INFORMATION))
        {
            status = STATUS_INVALID_PARAMETER;
            Irp->IoStatus.Information = 0;
            break;
        }

        verify_information = (PVERIFY_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = verify_information->Length;

        break;
    }

    case IOCTL_STORAGE_GET_DEVICE_NUMBER:
    {
        PSTORAGE_DEVICE_NUMBER number;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(STORAGE_DEVICE_NUMBER))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        number = (PSTORAGE_DEVICE_NUMBER)Irp->AssociatedIrp.SystemBuffer;

        number->DeviceType = device_extension->device_type;
        number->DeviceNumber = device_extension->device_number;
        number->PartitionNumber = (ULONG)-1;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(STORAGE_DEVICE_NUMBER);

        break;
    }

    case IOCTL_STORAGE_GET_HOTPLUG_INFO:
    {
        PSTORAGE_HOTPLUG_INFO info;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(STORAGE_HOTPLUG_INFO))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        info = (PSTORAGE_HOTPLUG_INFO)Irp->AssociatedIrp.SystemBuffer;

        info->Size = sizeof(STORAGE_HOTPLUG_INFO);
        info->MediaRemovable = 0;
        info->MediaHotplug = 0;
        info->DeviceHotplug = 0;
        info->WriteCacheEnableOverride = 0;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(STORAGE_HOTPLUG_INFO);

        break;
    }

    case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
    {
        PVOLUME_GET_GPT_ATTRIBUTES_INFORMATION attr;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        attr = (PVOLUME_GET_GPT_ATTRIBUTES_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

        attr->GptAttributes = 0;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION);

        break;
    }

    case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS:
    {
        PVOLUME_DISK_EXTENTS ext;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(VOLUME_DISK_EXTENTS))
        {
            status = STATUS_INVALID_PARAMETER;
            Irp->IoStatus.Information = 0;
            break;
        }
        /*
                    // not needed since there is only one disk extent to return
                    if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
                        sizeof(VOLUME_DISK_EXTENTS) + ((NumberOfDiskExtents - 1) * sizeof(DISK_EXTENT)))
                    {
                        status = STATUS_BUFFER_OVERFLOW;
                        Irp->IoStatus.Information = 0;
                        break;
                    }
        */
        ext = (PVOLUME_DISK_EXTENTS)Irp->AssociatedIrp.SystemBuffer;

        ext->NumberOfDiskExtents = 1;
        ext->Extents[0].DiskNumber = device_extension->device_number;
        ext->Extents[0].StartingOffset.QuadPart = 0;
        ext->Extents[0].ExtentLength.QuadPart = device_extension->file_size.QuadPart;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(VOLUME_DISK_EXTENTS) /*+ ((NumberOfDiskExtents - 1) * sizeof(DISK_EXTENT))*/;

        break;
    }

#if (NTDDI_VERSION < NTDDI_VISTA)
#define IOCTL_DISK_IS_CLUSTERED CTL_CODE(IOCTL_DISK_BASE, 0x003e, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif  // NTDDI_VERSION < NTDDI_VISTA

    case IOCTL_DISK_IS_CLUSTERED:
    {
        PBOOLEAN clus;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(BOOLEAN))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0;
            break;
        }

        clus = (PBOOLEAN)Irp->AssociatedIrp.SystemBuffer;

        *clus = FALSE;

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(BOOLEAN);

        break;
    }

    case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
    {
        PMOUNTDEV_NAME name;

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(MOUNTDEV_NAME))
        {
            status = STATUS_INVALID_PARAMETER;
            Irp->IoStatus.Information = 0;
            break;
        }

        name = (PMOUNTDEV_NAME)Irp->AssociatedIrp.SystemBuffer;
        name->NameLength = device_extension->device_name.Length * sizeof(WCHAR);

        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            name->NameLength + sizeof(USHORT))
        {
            status = STATUS_BUFFER_OVERFLOW;
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            break;
        }

        RtlCopyMemory(name->Name, device_extension->device_name.Buffer, name->NameLength);

        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = name->NameLength + sizeof(USHORT);

        break;
    }

    case IOCTL_CDROM_READ_TOC_EX:
    {
        KdPrint(("FileDisk: Unhandled ioctl IOCTL_CDROM_READ_TOC_EX.\n"));
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        break;
    }
    case IOCTL_DISK_GET_MEDIA_TYPES:
    {
        KdPrint(("FileDisk: Unhandled ioctl IOCTL_DISK_GET_MEDIA_TYPES.\n"));
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        break;
    }
    case 0x66001b:
    {
        KdPrint(("FileDisk: Unhandled ioctl FT_BALANCED_READ_MODE.\n"));
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        break;
    }
    case IOCTL_SCSI_GET_CAPABILITIES:
    {
        KdPrint(("FileDisk: Unhandled ioctl IOCTL_SCSI_GET_CAPABILITIES.\n"));
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        break;
    }
    case IOCTL_SCSI_PASS_THROUGH:
    {
        KdPrint(("FileDisk: Unhandled ioctl IOCTL_SCSI_PASS_THROUGH.\n"));
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        break;
    }
    case IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES:
    {
        KdPrint(("FileDisk: Unhandled ioctl IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES.\n"));
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        break;
    }
    case IOCTL_STORAGE_QUERY_PROPERTY:
    {
        KdPrint(("FileDisk: Unhandled ioctl IOCTL_STORAGE_QUERY_PROPERTY.\n"));
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        break;
    }

#if (NTDDI_VERSION < NTDDI_VISTA)
#define IOCTL_VOLUME_QUERY_ALLOCATION_HINT CTL_CODE(IOCTL_VOLUME_BASE, 20, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#endif  // NTDDI_VERSION < NTDDI_VISTA

    case IOCTL_VOLUME_QUERY_ALLOCATION_HINT:
    {
        KdPrint(("FileDisk: Unhandled ioctl IOCTL_VOLUME_QUERY_ALLOCATION_HINT.\n"));
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        break;
    }
    default:
    {
        KdPrint((
            "FileDisk: Unknown IoControlCode %#x\n",
            io_stack->Parameters.DeviceIoControl.IoControlCode
            ));

        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
    }
    }

    if (status != STATUS_PENDING)
    {
        Irp->IoStatus.Status = status;

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return status;
}

#pragma code_seg("PAGE")


VOID FileDiskClearQueue(IN PVOID Context)
{
    PDEVICE_OBJECT      device_object;
    PDEVICE_EXTENSION   device_extension;
    PLIST_ENTRY         request;
    PIRP                irp;
    PIO_STACK_LOCATION  io_stack;

    device_object = (PDEVICE_OBJECT)Context;
    device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;

    // user requested to close the file, so cancel all requests and remove them from list and reset
    DbgPrint("FileDiskClearQueue method init\r\n");
    
    while ((request = ExInterlockedRemoveHeadList(
        &device_extension->list_head,
        &device_extension->list_lock
    )) != NULL)
    {
        irp = CONTAINING_RECORD(request, IRP, Tail.Overlay.ListEntry);
        io_stack = IoGetCurrentIrpStackLocation(irp);

        // cancel every queued irp-request if user set the termination flag
        irp->IoStatus.Status = STATUS_CANCELLED;// STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(
            irp,
            (CCHAR)(NT_SUCCESS(irp->IoStatus.Status) ?
                IO_DISK_INCREMENT : IO_NO_INCREMENT)
        );
        DbgPrint("FileDiskClearQueue method: queued irp cancelled.\r\n");
    }
    
    DbgPrint("FileDiskClearQueue method completed\r\n");

}

VOID
FileDiskThread(
    IN PVOID Context
)
{
    PDEVICE_OBJECT      device_object;
    PDEVICE_EXTENSION   device_extension;
    PLIST_ENTRY         request;
    PIRP                irp;
    PIO_STACK_LOCATION  io_stack;
    PUCHAR              system_buffer;
    PUCHAR              buffer;

    PAGED_CODE();

    ASSERT(Context != NULL);

    device_object = (PDEVICE_OBJECT)Context;

    device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    FileDiskAdjustPrivilege(SE_IMPERSONATE_PRIVILEGE, TRUE);

    DbgPrint("FileDiskThread %u running \r\n", device_extension->device_number);

    for (;;)
    {
        KeWaitForSingleObject(
            &device_extension->request_event,
            Executive,
            KernelMode,
            FALSE,
            NULL
        );

        if (device_extension->terminate_thread)
        {
            while ((request = ExInterlockedRemoveHeadList(
                &device_extension->list_head,
                &device_extension->list_lock
            )) != NULL)
            {
                irp = CONTAINING_RECORD(request, IRP, Tail.Overlay.ListEntry);
                io_stack = IoGetCurrentIrpStackLocation(irp);

                // terminate thread flag set, meaning driver is unloading. so cancel all queued requests and complete this current loop for further termination.
                // user has closed the file, meaning driver and this thread must remain existent, so cancel all queued requests and cancel all future incoming
                // requests until file's shutdown is complete.
                irp->IoStatus.Status = STATUS_CANCELLED;// STATUS_SUCCESS;
                irp->IoStatus.Information = 0;
                IoCompleteRequest(
                    irp,
                    (CCHAR)(NT_SUCCESS(irp->IoStatus.Status) ?
                        IO_DISK_INCREMENT : IO_NO_INCREMENT)
                );
                // reloop and cancel all requests and empty the loop's queue
            }
            PsTerminateSystemThread(STATUS_SUCCESS);
        }

        while ((request = ExInterlockedRemoveHeadList(
            &device_extension->list_head,
            &device_extension->list_lock
        )) != NULL)
        {
            irp = CONTAINING_RECORD(request, IRP, Tail.Overlay.ListEntry);
            io_stack = IoGetCurrentIrpStackLocation(irp);

            // validate event flags
            if (device_extension->terminate_thread)
            {
                // terminate thread flag set, meaning driver is unloading. so cancel all queued requests and complete this current loop for further termination.
                // user has closed the file, meaning driver and this thread must remain existent, so cancel all queued requests and cancel all future incoming
                // requests until file's shutdown is complete.
                irp->IoStatus.Status = STATUS_CANCELLED;// STATUS_SUCCESS;
                irp->IoStatus.Information = 0;
                IoCompleteRequest(
                    irp,
                    (CCHAR)(NT_SUCCESS(irp->IoStatus.Status) ?
                        IO_DISK_INCREMENT : IO_NO_INCREMENT)
                );
                // reloop and cancel all requests and empty the loop's queue
                continue;
            }

            if (device_extension->fileClosingNow)
            {
                // user has closed the file, meaning driver and this thread must remain existent, so cancel all queued requests and cancel all future incoming
                // requests until file's shutdown is complete.
                irp->IoStatus.Status = STATUS_CANCELLED;// STATUS_SUCCESS;
                irp->IoStatus.Information = 0;
                IoCompleteRequest(
                    irp,
                    (CCHAR)(NT_SUCCESS(irp->IoStatus.Status) ?
                        IO_DISK_INCREMENT : IO_NO_INCREMENT)
                );
                // reloop and cancel all requests and empty the loop's queue
                continue;
            }

            // TODO read and write shm
            PUCHAR shmBuffer = (PUCHAR)device_extension->g_pSharedSection;
            PCONTEXT_REQUEST shmRequest = (PCONTEXT_REQUEST)device_extension->g_pSharedSection;
            LARGE_INTEGER timeout = { 0,0 };
            // Force 60 sec timeout
//            timeout.QuadPart = -(60ll * 10 * 1000 * 1000);
            timeout.QuadPart = 100; // 1000000000; // = 1 second delay // 10000000000; // = 10 seconds // 50 ms: 50000000;
            LARGE_INTEGER timeout2 = { 0,0 };
            timeout2.QuadPart = 100; // 100000; // 100000000;// 50000000;
            NTSTATUS WaitStatus;
            switch (io_stack->MajorFunction)
            {
            case IRP_MJ_READ:
                /*
                if ((io_stack->Parameters.Read.ByteOffset.QuadPart +
                    io_stack->Parameters.Read.Length) >
                    device_extension->file_size.QuadPart)
                {
                    irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                    irp->IoStatus.Information = 0;
                    break;
                }
                */

                // prepare and set the request in shared memory
                RtlZeroMemory(shmRequest, SHM_HEADER_SIZE);
                shmRequest->MajorFunction = IRP_MJ_READ;
                shmRequest->ByteOffset.HighPart = shmRequest->ByteOffset.LowPart = 0;
                shmRequest->ByteOffset.QuadPart = io_stack->Parameters.Read.ByteOffset.QuadPart;
                shmRequest->Length = io_stack->Parameters.Read.Length;
                shmRequest->reply = DRIVER_SIGNATURE;

                // delay for proper synchronisation of shared memory buffer and everything else in user mode and kernel mode
                KeDelayExecutionThread(KernelMode, FALSE, &timeout2);
                // set the data event for the proxy server to unblock and process the request
                KeSetEvent(device_extension->KeDriverRequestDataSetObj, 100, TRUE);
                //KePulseEvent(device_extension->KeDriverRequestDataSetObj, 1, TRUE);

                // we cannot use indefinite wait, so we use spin lock loop which is breakable by user's command
                while (TRUE)
                {
                    // cancel if user set the termination flag
                    if (device_extension->terminate_thread)
                    {
                        irp->IoStatus.Status = STATUS_CANCELLED;// STATUS_SUCCESS;
                        irp->IoStatus.Information = 0;
                        KeSetEvent(device_extension->KeRequestCompleteObj, 100, TRUE);
                        goto Completed;
                    }

                    // cancel if user set the file closing flag
                    if (device_extension->fileClosingNow)
                    {
                        irp->IoStatus.Status = STATUS_CANCELLED;// STATUS_SUCCESS;
                        irp->IoStatus.Information = 0;
                        KeSetEvent(device_extension->KeRequestCompleteObj, 100, TRUE);
                        goto Completed;
                    }

                    // wait for driver event to occur
                    WaitStatus = KeWaitForSingleObject(device_extension->KeProxyIdleObj, UserRequest, UserMode, FALSE, &timeout);// &timeout);
                    if (WaitStatus == STATUS_SUCCESS)
                    {
                        KeResetEvent(device_extension->KeProxyIdleObj);
                        // success, the driver set the event flag
                        break;
                    }
                    else if (WaitStatus == STATUS_TIMEOUT)
                    {
                        continue;
                    }
                    else
                    {
                        continue;
                    }
                }

                // delay for proper synchronisation of shared memory buffer and everything else in user mode and kernel mode
                KeDelayExecutionThread(KernelMode, FALSE, &timeout2);

                // the data has been read from the virtual disk file by the proxy app into the shared memory buffer, so write it to the window's provided system buffer.
                // set the systembuffer and configuration
                system_buffer = (PUCHAR)MmGetSystemAddressForMdlSafe(irp->MdlAddress, HighPagePriority);// NormalPagePriority);
                RtlCopyBytes(system_buffer, shmBuffer + SHM_HEADER_SIZE, io_stack->Parameters.Read.Length);

                /*
                if (shmRequest->reply != USERMODEAPP_SIGNATURE)
                {
                    DbgPrint("FileDiskThread-IRP_MJ_READ- proxy reply signature not matched at device %u, bug.\r\n", device_extension->device_number);
                    DbgPrint("signature: %u \r\n", shmRequest->reply);
                }
                */
                // finally update current device's current Irp status
                irp->IoStatus.Status = STATUS_SUCCESS;
                irp->IoStatus.Information = io_stack->Parameters.Read.Length;
                KeSetEvent(device_extension->KeRequestCompleteObj, 100, TRUE);
                //KePulseEvent(device_extension->KeRequestCompleteObj, 100, TRUE);

                // completed.
                break;

            case IRP_MJ_WRITE:
                /*
                if ((io_stack->Parameters.Write.ByteOffset.QuadPart +
                    io_stack->Parameters.Write.Length) >
                    device_extension->file_size.QuadPart)
                {
                    irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                    irp->IoStatus.Information = 0;
                    break;
                }
                */
                // prepare and set the request in shared memory
                RtlZeroMemory(shmRequest, SHM_HEADER_SIZE);
                shmRequest->MajorFunction = IRP_MJ_WRITE;
                shmRequest->ByteOffset.HighPart = shmRequest->ByteOffset.LowPart = 0;
                shmRequest->ByteOffset.QuadPart = io_stack->Parameters.Write.ByteOffset.QuadPart;
                shmRequest->Length = io_stack->Parameters.Write.Length;
                shmRequest->reply = DRIVER_SIGNATURE;
                KeDelayExecutionThread(KernelMode, FALSE, &timeout2);

                system_buffer = (PUCHAR)MmGetSystemAddressForMdlSafe(irp->MdlAddress, HighPagePriority | MdlMappingNoWrite);// NormalPagePriority);
                RtlCopyBytes(shmBuffer + SHM_HEADER_SIZE, system_buffer, io_stack->Parameters.Write.Length);
                //MmUnlockPages(irp->MdlAddress);
  //              MmUnmapLockedPages(system_buffer, irp->MdlAddress);
  //              IoFreeMdl(irp->MdlAddress);


                // delay for proper synchronisation of shared memory buffer and everything else in user mode and kernel mode
                KeDelayExecutionThread(KernelMode, FALSE, &timeout2);
                // set the data event for the proxy server to unblock and process the request
                KeSetEvent(device_extension->KeDriverRequestDataSetObj, 100, TRUE);
                //KePulseEvent(device_extension->KeDriverRequestDataSetObj, 1, TRUE);

                // we cannot use indefinite wait, so we use spin lock loop which is breakable by user's command
                while (TRUE)
                {
                    // cancel if user set the termination flag
                    if (device_extension->terminate_thread)
                    {
                        irp->IoStatus.Status = STATUS_CANCELLED;// STATUS_SUCCESS;
                        irp->IoStatus.Information = 0;
                        KeSetEvent(device_extension->KeRequestCompleteObj, 100, TRUE);
                        goto Completed;
                    }

                    // cancel if user set the file closing flag
                    if (device_extension->fileClosingNow)
                    {
                        irp->IoStatus.Status = STATUS_CANCELLED;// STATUS_SUCCESS;
                        irp->IoStatus.Information = 0;
                        KeSetEvent(device_extension->KeRequestCompleteObj, 100, TRUE);
                        goto Completed;
                    }

                    // wait for driver event to occur
                    WaitStatus = KeWaitForSingleObject(device_extension->KeProxyIdleObj, UserRequest, UserMode, FALSE, &timeout);// &timeout);
                    if (WaitStatus == STATUS_SUCCESS)
                    {
                        KeResetEvent(device_extension->KeProxyIdleObj);
                        // success, the driver set the event flag
                        break;
                    }
                    else if (WaitStatus == STATUS_TIMEOUT)
                    {
                        continue;
                    }
                    else
                    {
                        continue;
                    }
                }

                // delay for proper synchronisation of shared memory buffer and everything else in user mode and kernel mode
                KeDelayExecutionThread(KernelMode, FALSE, &timeout2);

                // the data has been written by the proxy app into the backend file, so configure the final configuration and complete the request.
                /*
                if (shmRequest->reply != USERMODEAPP_SIGNATURE)
                {
                    DbgPrint("FileDiskThread-IRP_MJ_WRITE- proxy reply signature not matched at device %u, bug.\r\n", device_extension->device_number);
                    DbgPrint("signature: %u \r\n", shmRequest->reply);
                }
                */
                // finally update current device's current Irp status
                irp->IoStatus.Status = STATUS_SUCCESS;
                irp->IoStatus.Information = io_stack->Parameters.Write.Length;
                KeSetEvent(device_extension->KeRequestCompleteObj, 100, TRUE);
                //KePulseEvent(device_extension->KeRequestCompleteObj, 100, TRUE);

                // completed
                break;
            default:
                irp->IoStatus.Status = STATUS_DRIVER_INTERNAL_ERROR;
            }

        Completed:

            //MmUnmapLockedPages(irp->MdlAddress->MappedSystemVa, irp->MdlAddress);
            //IoFreeMdl(irp->MdlAddress);
            //MmFreePagesFromMdl(irp->MdlAddress);

            IoCompleteRequest(
                irp,
                (CCHAR)(NT_SUCCESS(irp->IoStatus.Status) ?
                    IO_DISK_INCREMENT : IO_NO_INCREMENT)
            );
        }
        
    }
}

NTSTATUS
FileDiskOpenFile(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
)
{
    PDEVICE_EXTENSION               device_extension;
    POPEN_FILE_INFORMATION          open_file_information;
    NTSTATUS                        status;

    PAGED_CODE();

    ASSERT(DeviceObject != NULL);
    ASSERT(Irp != NULL);

    DbgPrint("IOCTL_REGISTER_FILE->FileDiskOpenFile method running.\r\n");

    device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    open_file_information = (POPEN_FILE_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

    /*
    // get semaphore from the proxy application and set it up
    device_extension->shmSemaphoreSync = open_file_information->shmSemaphoreSync;
    status = ObReferenceObjectByHandle(open_file_information->shmSemaphoreSync, SEMAPHORE_ALL_ACCESS, ExSemaphoreObjectType, UserMode, &device_extension->pshmSemaphoreSyncObj, NULL);
    if (status != STATUS_SUCCESS)
    {
        DbgPrint("IOCTL_REGISTER_FILE->FileDiskOpenFile->shmSemaphoreSync->ObReferenceObjectByHandle method failure. aborted.\r\n");
        return STATUS_INVALID_PARAMETER;
    }
    */

    //set file's attributes in the context
    device_extension->read_only = open_file_information->ReadOnly;
    device_extension->file_size.HighPart = open_file_information->FileSize.HighPart;
    device_extension->file_size.LowPart = open_file_information->FileSize.LowPart;
    device_extension->file_size.QuadPart = open_file_information->FileSize.QuadPart;
    device_extension->file_size.u.HighPart = open_file_information->FileSize.u.HighPart;
    device_extension->file_size.u.LowPart = open_file_information->FileSize.u.LowPart;

    device_extension->file_name.Length = open_file_information->FileNameLength;
    device_extension->file_name.MaximumLength = open_file_information->FileNameLength;
    device_extension->file_name.Buffer = ExAllocatePoolWithTag(NonPagedPool, open_file_information->FileNameLength, FILE_DISK_POOL_TAG);

    if (device_extension->file_name.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(
        device_extension->file_name.Buffer,
        open_file_information->FileName,
        open_file_information->FileNameLength
    );

    //device_extension->file_size.QuadPart = open_file_information->FileSize.QuadPart;

    // TODO

    // now finally fill the user's buffer with configuration and reply.
    //open_file_information->g_hSection = device_extension->g_hSection;
    //open_file_information->g_pSectionObj = device_extension->g_pSectionObj;
    //open_file_information->g_pSharedSection = device_extension->g_pSharedSection;
    //open_file_information->pDriverRequestDataSetObj = device_extension->pDriverRequestDataSetObj;
    //open_file_information->DriverRequestDataSet = device_extension->DriverRequestDataSet;
    //open_file_information->pProxyIdleObj = device_extension->pProxyIdleObj;
    //open_file_information->ProxyIdle = device_extension->ProxyIdle;
    open_file_information->DriverReply = TRUE;
    open_file_information->DeviceNumber = device_extension->device_number;

    // finally set status
    device_extension->fileClosingNow = FALSE;
    device_extension->media_in_device = TRUE;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = sizeof(OPEN_FILE_INFORMATION);

    DbgPrint("IOCTL_REGISTER_FILE->FileDiskOpenFile method completed.\r\n");

    return STATUS_SUCCESS;
}

NTSTATUS
FileDiskCloseFile(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
)
{
    PDEVICE_EXTENSION device_extension;

    PAGED_CODE();

    ASSERT(DeviceObject != NULL);
    ASSERT(Irp != NULL);

    PLIST_ENTRY         request;
    PIRP                irp;
    PIO_STACK_LOCATION  io_stack;

    DbgPrint("IOCTL_DEREGISTER_FILE->FileDiskCloseFile method running.\r\n");

    device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    //TODO tushar
    device_extension->fileClosingNow = TRUE;
    // TODO tushar
    FileDiskClearQueue((PVOID)DeviceObject);

    ExFreePool(device_extension->file_name.Buffer);

    //ZwClose(device_extension->file_handle);

    device_extension->media_in_device = FALSE;

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    DbgPrint("IOCTL_DEREGISTER_FILE->FileDiskCloseFile method completed.\r\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FileDiskAdjustPrivilege(
    IN ULONG    Privilege,
    IN BOOLEAN  Enable
)
{
    NTSTATUS            status;
    HANDLE              token_handle;
    TOKEN_PRIVILEGES    token_privileges;

    PAGED_CODE();

    status = ZwOpenProcessToken(
        NtCurrentProcess(),
        TOKEN_ALL_ACCESS,
        &token_handle
    );

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    token_privileges.PrivilegeCount = 1;
    token_privileges.Privileges[0].Luid = RtlConvertUlongToLuid(Privilege);
    token_privileges.Privileges[0].Attributes = Enable ? SE_PRIVILEGE_ENABLED : 0;

    status = ZwAdjustPrivilegesToken(
        token_handle,
        FALSE,
        &token_privileges,
        sizeof(token_privileges),
        NULL,
        NULL
    );

    ZwClose(token_handle);

    return status;
}

NTSTATUS SetSecurityAllAccess(HANDLE h, PVOID obj)
{
    NTSTATUS status;

    status = ObReferenceObjectByHandle(h, STANDARD_RIGHTS_ALL | GENERIC_ALL, NULL, KernelMode, obj, 0);
    if (status != STATUS_SUCCESS)
    {
        DbgPrint("SetSecurityAllAccess-ObReferenceObjectByHandle failure!\r\n");
        return status;
    }
    DbgPrint("SetSecurityAllAccess-ObReferenceObjectByHandle completed!\r\n");

    // ---
    PACL pACL = NULL;
    PSECURITY_DESCRIPTOR pSecurityDescriptor = { 0 };
    status = CreateStandardSCAndACL(&pSecurityDescriptor, &pACL);
    if (status != STATUS_SUCCESS)
    {
        DbgPrint("SetSecurityAllAccess-CreateStandardSCAndACL failure! Status: %p\n", status);
        ObDereferenceObject(obj);
        return status;
    }

    status = GrantAccess(h, pACL);
    if (status != STATUS_SUCCESS)
    {
        DbgPrint("SetSecurityAllAccess-GrantAccess failure! Status: %p\n", status);
        ExFreePool(pACL);
        ExFreePool(pSecurityDescriptor);
        ObDereferenceObject(obj);
        return status;
    }

    DbgPrint("SetSecurityAllAccess method completed!\r\n");

    ExFreePool(pACL);
    ExFreePool(pSecurityDescriptor);
    return status;
}

NTSTATUS CreateSharedEventsKe(PDEVICE_OBJECT device_object)
{
    PDEVICE_EXTENSION device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;
    WCHAR             symbolicNameBuffer[256];
//    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS status;
    PKEVENT eventHandle;

    DbgPrint("CreateSharedEvents method started!\r\n");

    // event 1
    UNICODE_STRING uEventName = { 0 };
    uEventName.Length = 0;
    uEventName.MaximumLength = 256;
    uEventName.Buffer = (PWSTR)symbolicNameBuffer;
    RtlUnicodeStringPrintf(&uEventName, DEVICE_OBJECT_SHM_EVENT_REQUEST_DATA L"%u", device_extension->device_number);
    InitializeObjectAttributes(&ObjectAttributes, &uEventName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    eventHandle = IoCreateSynchronizationEvent(&uEventName, &device_extension->DriverRequestDataSet);
//    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);

    if (eventHandle == NULL)
    {
        DbgPrint("CreateSharedEvents - DEVICE_OBJECT_SHM_EVENT_REQUEST_DATA Event fail!\r\n");
        return STATUS_UNSUCCESSFUL;
    }

    // set security event 1
    status = SetSecurityAllAccess(device_extension->DriverRequestDataSet, &device_extension->pDriverRequestDataSetObj);
    if (status != STATUS_SUCCESS)
    {
        ZwClose(device_extension->DriverRequestDataSet);
        return STATUS_UNSUCCESSFUL;
    }
    device_extension->KeDriverRequestDataSetObj = eventHandle;



    // event 2
    uEventName.Length = 0;
    uEventName.MaximumLength = 256;
    uEventName.Buffer = (PWSTR)symbolicNameBuffer;
    RtlUnicodeStringPrintf(&uEventName, DEVICE_OBJECT_SHM_EVENT_PROXY_IDLE L"%u", device_extension->device_number);
    InitializeObjectAttributes(&ObjectAttributes, &uEventName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    eventHandle = IoCreateSynchronizationEvent(&uEventName, &device_extension->ProxyIdle);
//    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);

    if (eventHandle == NULL)
    {
        DbgPrint("CreateSharedEvents - DEVICE_OBJECT_SHM_EVENT_PROXY_IDLE Event fail!\r\n");
        ZwClose(device_extension->DriverRequestDataSet);
        return STATUS_UNSUCCESSFUL;
    }

    // set security event 2
    status = SetSecurityAllAccess(device_extension->ProxyIdle, &device_extension->pProxyIdleObj);
    if (status != STATUS_SUCCESS)
    {
        ZwClose(device_extension->DriverRequestDataSet);
        ZwClose(device_extension->ProxyIdle);
        return STATUS_UNSUCCESSFUL;
    }
    device_extension->KeProxyIdleObj = eventHandle;



    // event 3
    uEventName.Length = 0;
    uEventName.MaximumLength = 256;
    uEventName.Buffer = (PWSTR)symbolicNameBuffer;
    RtlUnicodeStringPrintf(&uEventName, DEVICE_OBJECT_SHM_REQUESTCOMPLETE L"%u", device_extension->device_number);
    InitializeObjectAttributes(&ObjectAttributes, &uEventName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    eventHandle = IoCreateSynchronizationEvent(&uEventName, &device_extension->RequestComplete);
//    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);

    if (eventHandle == NULL)
    {
        DbgPrint("CreateSharedEvents - DEVICE_OBJECT_SHM_REQUESTCOMPLETE Event fail!\r\n");
        ZwClose(device_extension->DriverRequestDataSet);
        ZwClose(device_extension->ProxyIdle);
        return STATUS_UNSUCCESSFUL;
    }

    // set security event 3
    status = SetSecurityAllAccess(device_extension->RequestComplete, &device_extension->pRequestCompleteObj);
    if (status != STATUS_SUCCESS)
    {
        ZwClose(device_extension->DriverRequestDataSet);
        ZwClose(device_extension->ProxyIdle);
        ZwClose(device_extension->RequestComplete);
        return STATUS_UNSUCCESSFUL;
    }
    device_extension->KeRequestCompleteObj = eventHandle;

    // reset initialize
    KeResetEvent(device_extension->pDriverRequestDataSetObj);
    KeResetEvent(device_extension->pProxyIdleObj);
    KeResetEvent(device_extension->pRequestCompleteObj);

    DbgPrint("CreateSharedEvents method completed!\r\n");
    return status;
}

/*
NTSTATUS CreateSharedEvents(PDEVICE_OBJECT device_object)
{
    PDEVICE_EXTENSION device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;
    WCHAR             symbolicNameBuffer[256];
    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS status;

    DbgPrint("CreateSharedEvents method started!\r\n");

    // event 1
    UNICODE_STRING uEventName = { 0 };
    uEventName.Length = 0;
    uEventName.MaximumLength = 256;
    uEventName.Buffer = (PWSTR)symbolicNameBuffer;
    RtlUnicodeStringPrintf(&uEventName, DEVICE_OBJECT_SHM_EVENT_REQUEST_DATA L"%u", device_extension->device_number);
    InitializeObjectAttributes(&ObjectAttributes, &uEventName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwCreateEvent(&device_extension->DriverRequestDataSet, EVENT_ALL_ACCESS, &ObjectAttributes, SynchronizationEvent, FALSE);
    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);
    
    if (status != STATUS_SUCCESS)
    {
        DbgPrint("CreateSharedEvents - DEVICE_OBJECT_SHM_EVENT_REQUEST_DATA Event fail!\r\n");
        return status;
    }

    // set security event 1
    status = SetSecurityAllAccess(device_extension->DriverRequestDataSet, &device_extension->pDriverRequestDataSetObj);
    if (status != STATUS_SUCCESS)
    {
        ZwClose(device_extension->DriverRequestDataSet);
        return status;
    }



    // event 2
    uEventName.Length = 0;
    uEventName.MaximumLength = 256;
    uEventName.Buffer = (PWSTR)symbolicNameBuffer;
    RtlUnicodeStringPrintf(&uEventName, DEVICE_OBJECT_SHM_EVENT_PROXY_IDLE L"%u", device_extension->device_number);
    InitializeObjectAttributes(&ObjectAttributes, &uEventName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwCreateEvent(&device_extension->ProxyIdle, EVENT_ALL_ACCESS, &ObjectAttributes, SynchronizationEvent, FALSE);
    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);

    if (status != STATUS_SUCCESS)
    {
        DbgPrint("CreateSharedEvents - DEVICE_OBJECT_SHM_EVENT_PROXY_IDLE Event fail!\r\n");
        ZwClose(device_extension->DriverRequestDataSet);
        return status;
    }

    // set security event 2
    status = SetSecurityAllAccess(device_extension->ProxyIdle, &device_extension->pProxyIdleObj);
    if (status != STATUS_SUCCESS)
    {
        ZwClose(device_extension->DriverRequestDataSet);
        ZwClose(device_extension->ProxyIdle);
        return status;
    }


    // event 3
    uEventName.Length = 0;
    uEventName.MaximumLength = 256;
    uEventName.Buffer = (PWSTR)symbolicNameBuffer;
    RtlUnicodeStringPrintf(&uEventName, DEVICE_OBJECT_SHM_REQUESTCOMPLETE L"%u", device_extension->device_number);
    InitializeObjectAttributes(&ObjectAttributes, &uEventName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwCreateEvent(&device_extension->RequestComplete, EVENT_ALL_ACCESS, &ObjectAttributes, SynchronizationEvent, FALSE);
    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);

    if (status != STATUS_SUCCESS)
    {
        DbgPrint("CreateSharedEvents - DEVICE_OBJECT_SHM_REQUESTCOMPLETE Event fail!\r\n");
        ZwClose(device_extension->DriverRequestDataSet);
        ZwClose(device_extension->ProxyIdle);
        return status;
    }

    // set security event 2
    status = SetSecurityAllAccess(device_extension->RequestComplete, &device_extension->pRequestCompleteObj);
    if (status != STATUS_SUCCESS)
    {
        ZwClose(device_extension->DriverRequestDataSet);
        ZwClose(device_extension->ProxyIdle);
        ZwClose(device_extension->RequestComplete);
        return status;
    }


    DbgPrint("CreateSharedEvents method completed!\r\n");
    return status;
}
*/

void DeleteSharedEvents(PDEVICE_OBJECT device_object)
{
    PDEVICE_EXTENSION device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;
    DbgPrint("Driver-->DeviceObject->DeleteSharedEvents routine started - removing device shared events %u\r\n", device_extension->device_number);
    
    //KeClearEvent(device_extension->KeDriverRequestDataSetObj);
    //KeClearEvent(device_extension->KeProxyIdleObj);
    //KeClearEvent(device_extension->KeRequestCompleteObj);
//    ObDereferenceObject(device_extension->KeDriverRequestDataSetObj);
    ObDereferenceObject(device_extension->pDriverRequestDataSetObj);
    ZwClose(device_extension->DriverRequestDataSet);
  //  ObDereferenceObject(device_extension->KeProxyIdleObj);
    ObDereferenceObject(device_extension->pProxyIdleObj);
    ZwClose(device_extension->ProxyIdle);
    //ObDereferenceObject(device_extension->KeRequestCompleteObj);
    ObDereferenceObject(device_extension->pRequestCompleteObj);
    ZwClose(device_extension->RequestComplete);
    DbgPrint("Driver-->DeviceObject->DeleteSharedEvents routine completed!\r\n");

    // TODO - delete all device's objects and handles

}

NTSTATUS CreateSharedMemory(PDEVICE_OBJECT device_object)
{
    NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;
    PDEVICE_EXTENSION device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;
    WCHAR             symbolicNameBuffer[256];
    RtlZeroBytes(symbolicNameBuffer, sizeof(wchar_t) * 2);

    DbgPrint("CreateSharedMemory method started!\r\n");

    UNICODE_STRING uSectionName = { 0 };
    uSectionName.Length = 0;
    uSectionName.MaximumLength = 256;
    uSectionName.Buffer = (PWSTR)symbolicNameBuffer;

    //RtlInitUnicodeString(&uSectionName, gc_wszSharedSectionName);
    RtlUnicodeStringPrintf(&uSectionName, DEVICE_OBJECT_SHM_NAME L"%u", device_extension->device_number);

    OBJECT_ATTRIBUTES objAttributes = { 0 };
    InitializeObjectAttributes(&objAttributes, &uSectionName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    LARGE_INTEGER lMaxSize = { 0 };
    lMaxSize.HighPart = 0;
    lMaxSize.LowPart = DEVICE_OBJECT_SHM_SIZE_BYTES;// 1024 * 10;
    ntStatus = ZwCreateSection(&device_extension->g_hSection, SECTION_ALL_ACCESS, &objAttributes, &lMaxSize, PAGE_READWRITE, SEC_COMMIT, NULL);
    if (ntStatus != STATUS_SUCCESS)
    {
        DbgPrint("ZwCreateSection fail! Status: %p\n", ntStatus);
        return ntStatus;
    }
    DbgPrint("ZwCreateSection completed!\n");

    // set security shared memory all access
    ntStatus = SetSecurityAllAccess(device_extension->g_hSection, &device_extension->g_pSectionObj);
    if (ntStatus != STATUS_SUCCESS)
    {
        ZwClose(device_extension->g_hSection);
        return ntStatus;
    }

    SIZE_T ulViewSize = 0;
    ntStatus = ZwMapViewOfSection(device_extension->g_hSection, NtCurrentProcess(), &device_extension->g_pSharedSection, 0, lMaxSize.LowPart, NULL, &ulViewSize, ViewShare, 0, PAGE_READWRITE | PAGE_NOCACHE);
    DbgPrint("View size %u", ulViewSize);
    if (ntStatus != STATUS_SUCCESS)
    {
        DbgPrint("ZwMapViewOfSection fail! Status: %p\n", ntStatus);
        ObDereferenceObject(device_extension->g_pSectionObj);
        ZwClose(device_extension->g_hSection);
        return ntStatus;
    }
    DbgPrint("ZwMapViewOfSection completed!\n");

    //PCHAR TestString = "Message from kernel";
    //memcpy(g_pSharedSection, TestString, 19);
    //ReadSharedMemory();
    DbgPrint("CreateSharedMemory method completed!\r\n");

    return ntStatus;
}

void ReleaseSharedMemory(PDEVICE_OBJECT device_object)
{
    PDEVICE_EXTENSION device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;
    DbgPrint("Driver-->DeviceObject->ReleaseSharedMemory routine started - removing device SHM %u\r\n", device_extension->device_number);

    if (device_extension->g_pSharedSection)
        ZwUnmapViewOfSection(NtCurrentProcess(), device_extension->g_pSharedSection);

    if (device_extension->g_pSectionObj)
        ObDereferenceObject(device_extension->g_pSectionObj);

    if (device_extension->g_hSection)
        ZwClose(device_extension->g_hSection);

    DbgPrint("Driver-->DeviceObject->ReleaseSharedMemory routine completed!\r\n");

    return TRUE;
}

/*
NTSTATUS connectReqServerPipeline(PDEVICE_OBJECT device_object)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PDEVICE_EXTENSION device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;
    WCHAR symbolicNameBuffer[256];// = ExAllocatePoolWithTag(PagedPool, 1024, FILE_DISK_POOL_TAG);
    //RtlZeroBytes((void*)symbolicNameBuffer, 512 * 2);

    if (device_extension->hReqServerPipe != NULL)
    {
        disconnectReqServerPipeline(device_object);
    }
    
    UNICODE_STRING link = { 0 };
    link.Length = 0;
    link.MaximumLength = 256;
    link.Buffer = (PWSTR)symbolicNameBuffer;
    RtlUnicodeStringPrintf(&link, REQUESTPIPE_NAME_DRIVER L"%u", device_extension->device_number);
    OBJECT_ATTRIBUTES object_attributes;
    IO_STATUS_BLOCK iostatus;

    InitializeObjectAttributes(
        &object_attributes,
        &link,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);

    status = ZwCreateFile(
        &device_extension->hReqServerPipe,
        //FILE_WRITE_DATA | FILE_READ_DATA, // or FILE_READ_DATA
        SYNCHRONIZE | FILE_WRITE_DATA | FILE_READ_DATA, // or FILE_READ_DATA
        &object_attributes,
        &iostatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,
        NULL,
        0);

    //ExFreePoolWithTag(symbolicNameBuffer, FILE_DISK_POOL_TAG);

    if (status != STATUS_SUCCESS)
    {
//        DbgPrint("connectReqServerPipeline method failed at device number %u.\r\n", device_extension->device_number);
  //      printStatusFile(L"\\DosDevices\\C:\\connectReqServerPipeline-failure.txt", "connectReqServerPipeline pipe connect function failure.");
        return status;
    }
    //printStatusFile(L"\\DosDevices\\C:\\connectReqServerPipeline-success.txt", "connectReqServerPipeline pipe connect function success.");
    return status;
}

NTSTATUS disconnectReqServerPipeline(PDEVICE_OBJECT device_object)
{
    PDEVICE_EXTENSION device_extension = (PDEVICE_EXTENSION)device_object->DeviceExtension;
    NTSTATUS status = ZwClose(device_extension->hReqServerPipe);
    device_extension->hReqServerPipe = NULL;
    return status;
}
*/

void printStatusFile(char* path, char* status)
{
    UNICODE_STRING     uniName;
    UNICODE_STRING     uniText;
    OBJECT_ATTRIBUTES  objAttr;

    RtlInitUnicodeString(&uniText, status);
    RtlInitUnicodeString(&uniName, path);  // or L"\\SystemRoot\\example.txt"
    InitializeObjectAttributes(&objAttr, &uniName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL, NULL);

    HANDLE   handle;
    NTSTATUS ntstatus;
    IO_STATUS_BLOCK    ioStatusBlock;

    // Do not try to perform any file operations at higher IRQL levels.
    // Instead, you may use a work item or a system worker thread to perform file operations.

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;

    ntstatus = ZwCreateFile(&handle,
        GENERIC_WRITE,
        &objAttr, &ioStatusBlock, NULL,
        FILE_ATTRIBUTE_NORMAL,
        0,
        FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL, 0);

#define  BUFFER_SIZE 1024
    CHAR     buffer[BUFFER_SIZE];
    size_t  cb;

    if (NT_SUCCESS(ntstatus)) {
        ntstatus = RtlStringCbPrintfA(buffer, sizeof(buffer), status);// , 0x0);
        if (NT_SUCCESS(ntstatus)) {
            ntstatus = RtlStringCbLengthA(buffer, sizeof(buffer), &cb);
            if (NT_SUCCESS(ntstatus)) {
                ntstatus = ZwWriteFile(handle, NULL, NULL, NULL, &ioStatusBlock,
                    buffer, cb, NULL, NULL);
            }
        }
        ZwClose(handle);
    }
    //RtlFreeUnicodeString(&uniName);
    //RtlFreeUnicodeString(&uniText);

}
#pragma code_seg() // end "PAGE"
