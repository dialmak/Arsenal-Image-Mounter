
/// iodisp.c
/// Routines called from worker thread at PASSIVE_LEVEL to complete work items
/// queued form miniport dispatch routines.
/// 
/// Copyright (c) 2012-2015, Arsenal Consulting, Inc. (d/b/a Arsenal Recon) <http://www.ArsenalRecon.com>
/// This source code and API are available under the terms of the Affero General Public
/// License v3.
///
/// Please see LICENSE.txt for full license terms, including the availability of
/// proprietary exceptions.
/// Questions, comments, or requests for clarification: http://ArsenalRecon.com/contact/
///

//#define _MP_H_skip_includes

#include "phdskmnt.h"

#include "legacycompat.h"

//#pragma warning(push)
//#pragma warning(disable : 4204)                       /* Prevent C4204 messages from stortrce.h. */
//#include <stortrce.h>
//#pragma warning(pop)
//
//#include "trace.h"
//#include "iodisp.tmh"

/**************************************************************************************************/
/*                                                                                                */
/* Globals, forward definitions, etc.                                                             */
/*                                                                                                */
/**************************************************************************************************/

VOID
ImScsiCleanupLU(
__in pHW_LU_EXTENSION     pLUExt,
__inout __deref PKIRQL         LowestAssumedIrql
)
{
    pHW_HBA_EXT             pHBAExt = pLUExt->pHBAExt;
    pHW_LU_EXTENSION *      ppLUExt = NULL;
    PLIST_ENTRY             list_ptr;
    pMP_WorkRtnParms        free_worker_params = NULL;
    KLOCK_QUEUE_HANDLE      LockHandle;

    KdPrint(("PhDskMnt::ImScsiCleanupLU: Removing device: %d:%d:%d pLUExt=%p\n",
        pLUExt->DeviceNumber.PathId,
        pLUExt->DeviceNumber.TargetId,
        pLUExt->DeviceNumber.Lun,
        pLUExt));

    free_worker_params = (pMP_WorkRtnParms)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(MP_WorkRtnParms), MP_TAG_GENERAL);

    if (free_worker_params == NULL)
    {
        DbgPrint("PhDskMnt::ImScsiCleanupLU: Memory allocation error.\n");
        return;
    }

    RtlZeroMemory(free_worker_params, sizeof(MP_WorkRtnParms));
    free_worker_params->pHBAExt = pHBAExt;
    free_worker_params->pLUExt = pLUExt;
    
    ImScsiAcquireLock(                   // Serialize the linked list of LUN extensions.              
        &pHBAExt->LUListLock, &LockHandle, *LowestAssumedIrql);

    ppLUExt = (pHW_LU_EXTENSION*)StoragePortGetLogicalUnit(
        pHBAExt,
        pLUExt->DeviceNumber.PathId,
        pLUExt->DeviceNumber.TargetId,
        pLUExt->DeviceNumber.Lun
        );

    if (ppLUExt != NULL)
        *ppLUExt = NULL;

    for (list_ptr = pHBAExt->LUList.Flink;
        list_ptr != &pHBAExt->LUList;
        list_ptr = list_ptr->Flink
        )
    {
        pHW_LU_EXTENSION object;
        object = CONTAINING_RECORD(list_ptr, HW_LU_EXTENSION, List);

        if (object == pLUExt)
        {
            KLOCK_QUEUE_HANDLE inner_lock_handle;
            KIRQL inner_assumed_irql = DISPATCH_LEVEL;

            list_ptr->Blink->Flink = list_ptr->Flink;
            list_ptr->Flink->Blink = list_ptr->Blink;

            // If a worker thread has started, we are now in that
            // thread context and will terminate that thread after
            // this function has run to completion. Instruct the
            // global worker thread to wait for this thread. It
            // will then free our LUExt in a place where it is
            // guaranteed to be unused.
            //
            // If a worker thread has not started, we are now in
            // the context of global worker thread. Instruct it
            // to free LUExt as next request, which will happen
            // after this function has run to completion.
            KdPrint(("PhDskMnt::ImScsiCleanupLU: Setting request to wait for LU worker thread.\n"));

            ImScsiAcquireLock(&pMPDrvInfoGlobal->RequestListLock,
                &inner_lock_handle, inner_assumed_irql);

            InsertTailList(&pMPDrvInfoGlobal->RequestList,
                &free_worker_params->RequestListEntry);

            ImScsiReleaseLock(&inner_lock_handle, &inner_assumed_irql);

            KeSetEvent(&pMPDrvInfoGlobal->RequestEvent, (KPRIORITY)0, FALSE);

            break;
        }
    }

    ImScsiReleaseLock(&LockHandle, LowestAssumedIrql);

    /// Cleanup all file handles, object name buffers,
    /// proxy refs etc.
    if (pLUExt->UseProxy)
    {
        ImScsiCloseProxy(&pLUExt->Proxy);
    }

    if (pLUExt->LastIoBuffer != NULL)
    {
        ExFreePoolWithTag(pLUExt->LastIoBuffer, MP_TAG_GENERAL);
        pLUExt->LastIoBuffer = NULL;
    }

    if (pLUExt->VMDisk)
    {
        SIZE_T free_size = 0;
        if (pLUExt->ImageBuffer != NULL)
        {
            ZwFreeVirtualMemory(NtCurrentProcess(),
                (PVOID*)&pLUExt->ImageBuffer,
                &free_size, MEM_RELEASE);
        }

        pLUExt->ImageBuffer = NULL;
    }
    else
    {
        if (pLUExt->FileObject != NULL)
        {
            ObDereferenceObject(pLUExt->FileObject);
            pLUExt->FileObject = NULL;
        }

        pLUExt->FileObject = NULL;

        if (pLUExt->ImageFile != NULL)
        {
            ZwClose(pLUExt->ImageFile);
        }

        pLUExt->ImageFile = NULL;
    }
    
    if (pLUExt->ObjectName.Buffer != NULL)
    {
        ExFreePoolWithTag(pLUExt->ObjectName.Buffer, MP_TAG_GENERAL);
        pLUExt->ObjectName.Buffer = NULL;
        pLUExt->ObjectName.Length = 0;
        pLUExt->ObjectName.MaximumLength = 0;
    }

    KdPrint(("PhDskMnt::ImScsiCleanupLU: Done.\n"));
}

VOID
ImScsiCreateLU(
__in pHW_HBA_EXT             pHBAExt,
__in PSCSI_REQUEST_BLOCK     pSrb,
__in PETHREAD                pReqThread,
__inout __deref PKIRQL    LowestAssumedIrql
)
{
    PSRB_IMSCSI_CREATE_DATA new_device = (PSRB_IMSCSI_CREATE_DATA)pSrb->DataBuffer;
    PLIST_ENTRY             list_ptr;
    pHW_LU_EXTENSION        pLUExt = NULL;
    NTSTATUS                ntstatus;

    KLOCK_QUEUE_HANDLE      LockHandle;

    KdPrint(("PhDskMnt::ImScsiCreateLU: Initializing new device: %d:%d:%d\n",
        new_device->Fields.DeviceNumber.PathId,
        new_device->Fields.DeviceNumber.TargetId,
        new_device->Fields.DeviceNumber.Lun));

    ImScsiAcquireLock(                   // Serialize the linked list of LUN extensions.              
        &pHBAExt->LUListLock, &LockHandle, *LowestAssumedIrql);

    for (list_ptr = pHBAExt->LUList.Flink;
        list_ptr != &pHBAExt->LUList;
        list_ptr = list_ptr->Flink
        )
    {
        pHW_LU_EXTENSION object;
        object = CONTAINING_RECORD(list_ptr, HW_LU_EXTENSION, List);

        if (object->DeviceNumber.LongNumber ==
            new_device->Fields.DeviceNumber.LongNumber)
        {
            pLUExt = object;
            break;
        }
    }

    if (pLUExt != NULL)
    {
        ImScsiReleaseLock(&LockHandle, LowestAssumedIrql);

        ntstatus = STATUS_OBJECT_NAME_COLLISION;

        goto Done;
    }

    pLUExt = (pHW_LU_EXTENSION)ExAllocatePoolWithTag(NonPagedPool, sizeof(HW_LU_EXTENSION), MP_TAG_GENERAL);

    if (pLUExt == NULL)
    {
        ImScsiReleaseLock(&LockHandle, LowestAssumedIrql);

        ntstatus = STATUS_INSUFFICIENT_RESOURCES;

        goto Done;
    }

    RtlZeroMemory(pLUExt, sizeof(HW_LU_EXTENSION));

    pLUExt->DeviceNumber = new_device->Fields.DeviceNumber;

    KeInitializeEvent(&pLUExt->StopThread, NotificationEvent, FALSE);

    InsertHeadList(&pHBAExt->LUList, &pLUExt->List);

    ImScsiReleaseLock(&LockHandle, LowestAssumedIrql);

    pLUExt->pHBAExt = pHBAExt;

    ntstatus = ImScsiInitializeLU(pLUExt, new_device, pReqThread);
    if (!NT_SUCCESS(ntstatus))
    {
        ImScsiCleanupLU(pLUExt, LowestAssumedIrql);
        goto Done;
    }

Done:
    new_device->SrbIoControl.ReturnCode = ntstatus;

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);

    return;
}

NTSTATUS
ImScsiIoCtlCallCompletion(
PDEVICE_OBJECT DeviceObject,
PIRP Irp,
PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);

    if (!NT_SUCCESS(Irp->IoStatus.Status))
        DbgPrint("PhDskMnt::ImScsiIoCtlCallCompletion: SMB_IMSCSI_CHECK failed: 0x%X\n",
        Irp->IoStatus.Status);
    else
        KdPrint2(("PhDskMnt::ImScsiIoCtlCallCompletion: Finished SMB_IMSCSI_CHECK.\n"));

    ExFreePoolWithTag(Irp->AssociatedIrp.SystemBuffer, MP_TAG_GENERAL);

    ImScsiFreeIrpWithMdls(Irp);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
ImScsiParallelReadWriteImageCompletion(
PDEVICE_OBJECT DeviceObject,
PIRP Irp,
PVOID Context)
{
    __analysis_assume(Context != NULL);

    pMP_WorkRtnParms pWkRtnParms = (pMP_WorkRtnParms)Context;
    PKTHREAD thread = NULL;
    KIRQL lowest_assumed_irql = PASSIVE_LEVEL;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!NT_SUCCESS(Irp->IoStatus.Status))
    {
        switch (Irp->IoStatus.Status)
        {
        case STATUS_INVALID_BUFFER_SIZE:
        {
            DbgPrint("PhDskMnt::ImScsiParallelReadWriteImageCompletion: STATUS_INVALID_BUFFER_SIZE from image I/O. Reporting SCSI_SENSE_ILLEGAL_REQUEST/SCSI_ADSENSE_INVALID_CDB/0x00.\n");
            ScsiSetCheckCondition(
                pWkRtnParms->pSrb,
                SRB_STATUS_ERROR,
                SCSI_SENSE_ILLEGAL_REQUEST,
                SCSI_ADSENSE_INVALID_CDB,
                0);
            break;
        }
        case STATUS_DEVICE_BUSY:
        {
            DbgPrint("PhDskMnt::ImScsiParallelReadWriteImageCompletion: STATUS_DEVICE_BUSY from image I/O. Reporting SRB_STATUS_BUSY/SCSI_SENSE_NOT_READY/SCSI_ADSENSE_LUN_NOT_READY/SCSI_SENSEQ_BECOMING_READY.\n");
            ScsiSetCheckCondition(
                pWkRtnParms->pSrb,
                SRB_STATUS_BUSY,
                SCSI_SENSE_NOT_READY,
                SCSI_ADSENSE_LUN_NOT_READY,
                SCSI_SENSEQ_BECOMING_READY
                );
            break;
        }
        default:
        {
            KdPrint(("PhDskMnt::ImScsiParallelReadWriteImageCompletion: Parallel I/O failed with status %#x\n",
                Irp->IoStatus.Status));

            ScsiSetCheckCondition(
                pWkRtnParms->pSrb,
                SRB_STATUS_ERROR,
                SCSI_SENSE_HARDWARE_ERROR,
                SCSI_ADSENSE_NO_SENSE,
                0);
            break;
        }
        }
    }
    else
    {
        ScsiSetSuccess(pWkRtnParms->pSrb, (ULONG)Irp->IoStatus.Information);

        if (pWkRtnParms->CopyBack)
        {
            RtlCopyMemory(pWkRtnParms->MappedSystemBuffer,
                pWkRtnParms->AllocatedBuffer,
                Irp->IoStatus.Information);
        }
    }

    if (Irp->MdlAddress != pWkRtnParms->pOriginalMdl)
    {
        ImScsiFreeIrpWithMdls(Irp);
    }
    else
    {
        IoFreeIrp(Irp);
    }

    if (pWkRtnParms->AllocatedBuffer != NULL)
    {
        PCDB pCdb = (PCDB)pWkRtnParms->pSrb->Cdb;
        LARGE_INTEGER startingSector;
        KLOCK_QUEUE_HANDLE LockHandle;

        if ((pCdb->AsByte[0] == SCSIOP_READ16) |
            (pCdb->AsByte[0] == SCSIOP_WRITE16))
        {
            REVERSE_BYTES_QUAD(&startingSector, pCdb->CDB16.LogicalBlock);
        }
        else
        {
            REVERSE_BYTES(&startingSector, &pCdb->CDB10.LogicalBlockByte0);
        }

        if (thread == NULL)
        {
            thread = PsGetCurrentThread();
        }

        if (pWkRtnParms->pReqThread == thread)
        {
            lowest_assumed_irql = pWkRtnParms->LowestAssumedIrql;
        }

        ImScsiAcquireLock(&pWkRtnParms->pLUExt->LastIoLock, &LockHandle,
            lowest_assumed_irql);

        if (pWkRtnParms->pLUExt->LastIoBuffer != NULL)
        {
            ExFreePoolWithTag(pWkRtnParms->pLUExt->LastIoBuffer,
                MP_TAG_GENERAL);
        }

        pWkRtnParms->pLUExt->LastIoStartSector = startingSector.QuadPart;
        pWkRtnParms->pLUExt->LastIoLength =
            pWkRtnParms->pSrb->DataTransferLength;
        pWkRtnParms->pLUExt->LastIoBuffer = pWkRtnParms->AllocatedBuffer;

        ImScsiReleaseLock(&LockHandle, &lowest_assumed_irql);
    }

#ifdef USE_SCSIPORT

    if (thread == NULL)
    {
        thread = PsGetCurrentThread();
    }

    if (pWkRtnParms->pReqThread == thread)
    {
        KdPrint2(("PhDskMnt::ImScsiParallelReadWriteImageCompletion sending 'RequestComplete', 'NextRequest' and 'NextLuRequest' to ScsiPort.\n"));

        ScsiPortNotification(RequestComplete, pWkRtnParms->pHBAExt, pWkRtnParms->pSrb);
        ScsiPortNotification(NextRequest, pWkRtnParms->pHBAExt);
        ScsiPortNotification(NextLuRequest, pWkRtnParms->pHBAExt, 0, 0, 0);

        ExFreePoolWithTag(pWkRtnParms, MP_TAG_GENERAL);
    }
    else
    {
        PIRP ioctl_irp = NULL;
        KIRQL known_irql;

        if (pWkRtnParms->AllocatedBuffer != NULL)
        {
            known_irql = lowest_assumed_irql;
        }
        else
        {
            known_irql = KeGetCurrentIrql();
        }
        
        if (known_irql == PASSIVE_LEVEL)
        {
            ioctl_irp = ImScsiBuildCompletionIrp();

            if (ioctl_irp == NULL)
            {
                DbgPrint("PhDskMnt::ImScsiParallelReadWriteImageCompletion: ImScsiBuildCompletionIrp failed.\n");
            }
        }
        else
        {
            KdPrint2(("PhDskMnt::ImScsiParallelReadWriteImageCompletion: IRQL too high to call for Srb completion through SMP_IMSCSI_CHECK. Queuing for timer instead.\n"));
        }

        KdPrint2(("PhDskMnt::ImScsiParallelReadWriteImageCompletion calling for Srb completion.\n"));

        ImScsiCallForCompletion(ioctl_irp, pWkRtnParms, &lowest_assumed_irql);
    }

#endif

#ifdef USE_STORPORT

    KdPrint2(("PhDskMnt::ImScsiParallelReadWriteImageCompletion sending 'RequestComplete' to port StorPort.\n"));
    StorPortNotification(RequestComplete, pWkRtnParms->pHBAExt, pWkRtnParms->pSrb);

    ExFreePoolWithTag(pWkRtnParms, MP_TAG_GENERAL);

#endif

    return STATUS_MORE_PROCESSING_REQUIRED;
}

#ifdef USE_SCSIPORT
PIRP
ImScsiBuildCompletionIrp()
{
    PIRP ioctl_irp;
    PIO_STACK_LOCATION ioctl_stack;
    PSRB_IMSCSI_CHECK completion_srb;

    completion_srb = (PSRB_IMSCSI_CHECK)ExAllocatePoolWithTag(NonPagedPool,
        sizeof(*completion_srb), MP_TAG_GENERAL);

    if (completion_srb == NULL)
    {
        return NULL;
    }

    ImScsiInitializeSrbIoBlock(&completion_srb->SrbIoControl,
        sizeof(*completion_srb), SMP_IMSCSI_CHECK, 0);

    ioctl_irp = IoAllocateIrp(
        pMPDrvInfoGlobal->ControllerObject->StackSize, FALSE);

    if (ioctl_irp == NULL)
    {
        ExFreePoolWithTag(completion_srb, MP_TAG_GENERAL);
        return NULL;
    }

    ioctl_irp->AssociatedIrp.SystemBuffer = completion_srb;

    ioctl_stack = IoGetNextIrpStackLocation(ioctl_irp);
    ioctl_stack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    ioctl_stack->Parameters.DeviceIoControl.InputBufferLength =
        sizeof(*completion_srb);
    ioctl_stack->Parameters.DeviceIoControl.IoControlCode =
        IOCTL_SCSI_MINIPORT;

    IoSetCompletionRoutine(ioctl_irp, ImScsiIoCtlCallCompletion,
        NULL, TRUE, TRUE, TRUE);

    return ioctl_irp;
}

NTSTATUS
ImScsiCallForCompletion(__in __deref PIRP Irp OPTIONAL,
__in __deref pMP_WorkRtnParms pWkRtnParms,
__inout __deref PKIRQL LowestAssumedIrql)
{
    KLOCK_QUEUE_HANDLE lock_handle;

    KdPrint2(("PhDskMnt::ImScsiCallForCompletion: Invoking SMB_IMSCSI_CHECK for work: 0x%p.\n",
        pWkRtnParms));

    ImScsiAcquireLock(&pMPDrvInfoGlobal->ResponseListLock,
        &lock_handle, *LowestAssumedIrql);

    InsertTailList(
        &pMPDrvInfoGlobal->ResponseList,
        &pWkRtnParms->ResponseListEntry);

    ImScsiReleaseLock(&lock_handle, LowestAssumedIrql);

    if (Irp != NULL)
    {
        return IoCallDriver(pMPDrvInfoGlobal->ControllerObject, Irp);
    }
    else
    {
        return STATUS_SUCCESS;
    }
}
#endif // USE_SCSIPORT

VOID
ImScsiParallelReadWriteImage(
__in pMP_WorkRtnParms       pWkRtnParms,
__inout __deref PUCHAR   pResult,
__inout __deref PKIRQL   LowestAssumedIrql
)
{
    PCDB pCdb = (PCDB)pWkRtnParms->pSrb->Cdb;
    PIO_STACK_LOCATION lower_io_stack = NULL;
    PDEVICE_OBJECT lower_device =
        IoGetRelatedDeviceObject(pWkRtnParms->pLUExt->FileObject);
    PIRP lower_irp;
    LARGE_INTEGER starting_sector = { 0 };
    LARGE_INTEGER starting_offset;
    UCHAR function = 0;
    BOOLEAN use_mdl = FALSE;

    if ((pCdb->AsByte[0] == SCSIOP_READ16) |
        (pCdb->AsByte[0] == SCSIOP_WRITE16))
    {
        REVERSE_BYTES_QUAD(&starting_sector, pCdb->CDB16.LogicalBlock);
    }
    else
    {
        REVERSE_BYTES(&starting_sector, &pCdb->CDB10.LogicalBlockByte0);
    }

    starting_offset.QuadPart = (starting_sector.QuadPart <<
        pWkRtnParms->pLUExt->BlockPower) +
        pWkRtnParms->pLUExt->ImageOffset.QuadPart;

    KdPrint2(("PhDskMnt::ImScsiParallelReadWriteImage starting sector: 0x%I64X\n",
        starting_sector));

    if ((pWkRtnParms->pSrb->Cdb[0] == SCSIOP_READ) ||
        (pWkRtnParms->pSrb->Cdb[0] == SCSIOP_READ16))
    {
        function = IRP_MJ_READ;
    }
    else if ((pWkRtnParms->pSrb->Cdb[0] == SCSIOP_WRITE) ||
        (pWkRtnParms->pSrb->Cdb[0] == SCSIOP_WRITE16))
    {
        function = IRP_MJ_WRITE;
    }

#ifdef USE_STORPORT
    // Try to use original MDL if available
    if (PortSupportsGetOriginalMdl &&
        (lower_device->Flags & DO_DIRECT_IO))
    {
        ULONG result = StorPortGetOriginalMdl(pWkRtnParms->pHBAExt,
            pWkRtnParms->pSrb, (PVOID*)&pWkRtnParms->pOriginalMdl);

        if (result == STOR_STATUS_SUCCESS)
        {
            use_mdl = TRUE;
        }
        else if (result == STOR_STATUS_NOT_IMPLEMENTED)
        {
            PortSupportsGetOriginalMdl = FALSE;
        }
    }

#endif
    if (use_mdl)
    {
        lower_irp = IoAllocateIrp(lower_device->StackSize, FALSE);

        if (lower_irp != NULL)
        {
            lower_io_stack = IoGetNextIrpStackLocation(lower_irp);

            lower_io_stack->MajorFunction = function;
            lower_io_stack->Parameters.Read.ByteOffset = starting_offset;
            lower_io_stack->Parameters.Read.Length =
                pWkRtnParms->pSrb->DataTransferLength;

            lower_irp->MdlAddress = pWkRtnParms->pOriginalMdl;
        }
    }
    else
    {
        ULONG storage_status =
            StoragePortGetSystemAddress(pWkRtnParms->pHBAExt,
            pWkRtnParms->pSrb, &pWkRtnParms->MappedSystemBuffer);

        if ((storage_status != STORAGE_STATUS_SUCCESS) ||
            (pWkRtnParms->MappedSystemBuffer == NULL))
        {
            DbgPrint("PhDskMnt::ImScsiParallelReadWriteImage: Memory allocation failed: status=0x%X address=0x%p translated=0x%p\n",
                storage_status,
                pWkRtnParms->pSrb->DataBuffer,
                pWkRtnParms->MappedSystemBuffer);

            ScsiSetCheckCondition(pWkRtnParms->pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR,
                SCSI_ADSENSE_NO_SENSE, 0);

            return;
        }

        pWkRtnParms->AllocatedBuffer =
            ExAllocatePoolWithTag(NonPagedPool,
            pWkRtnParms->pSrb->DataTransferLength, MP_TAG_GENERAL);

        if (pWkRtnParms->AllocatedBuffer == NULL)
        {
            DbgPrint("PhDskMnt::ImScsiParallelReadWriteImage: Memory allocation failed: status=0x%X address=0x%p translated=0x%p\n",
                storage_status,
                pWkRtnParms->pSrb->DataBuffer,
                pWkRtnParms->MappedSystemBuffer);

            ScsiSetCheckCondition(pWkRtnParms->pSrb, SRB_STATUS_ERROR,
                SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);

            return;
        }

        if (function == IRP_MJ_WRITE)
        {
            RtlCopyMemory(pWkRtnParms->AllocatedBuffer,
                pWkRtnParms->MappedSystemBuffer,
                pWkRtnParms->pSrb->DataTransferLength);
        }
        else if (function == IRP_MJ_READ)
        {
            pWkRtnParms->CopyBack = TRUE;
        }

        if (lower_device->Flags & DO_DIRECT_IO)
        {
            lower_irp = IoBuildAsynchronousFsdRequest(function,
                lower_device, pWkRtnParms->AllocatedBuffer,
                pWkRtnParms->pSrb->DataTransferLength,
                &starting_offset, NULL);

            if (lower_irp != NULL)
            {
                lower_io_stack = IoGetNextIrpStackLocation(lower_irp);
            }
        }
        else
        {
            lower_irp = IoAllocateIrp(lower_device->StackSize, FALSE);

            if (lower_irp != NULL)
            {
                lower_io_stack = IoGetNextIrpStackLocation(lower_irp);

                lower_io_stack->MajorFunction = function;
                lower_io_stack->Parameters.Read.ByteOffset =
                    starting_offset;
                lower_io_stack->Parameters.Read.Length =
                    pWkRtnParms->pSrb->DataTransferLength;

                if (lower_device->Flags & DO_BUFFERED_IO)
                {
                    lower_irp->AssociatedIrp.SystemBuffer =
                        pWkRtnParms->AllocatedBuffer;
                }
                else
                {
                    lower_irp->UserBuffer = pWkRtnParms->AllocatedBuffer;
                }
            }
        }
    }

    if (lower_irp == NULL)
    {
        if (pWkRtnParms->AllocatedBuffer != NULL)
        {
            ExFreePoolWithTag(pWkRtnParms->AllocatedBuffer, MP_TAG_GENERAL);
            pWkRtnParms->AllocatedBuffer = NULL;
        }

        DbgPrint("PhDskMnt::ImScsiParallelReadWriteImage: IRP allocation failed: data length=0x%u\n",
            pWkRtnParms->pSrb->DataTransferLength);

        ScsiSetCheckCondition(pWkRtnParms->pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);

        return;
    }

    lower_irp->Tail.Overlay.Thread = NULL;

    if (function == IRP_MJ_READ)
    {
        lower_irp->Flags |= IRP_READ_OPERATION;
    }
    else if (function == IRP_MJ_WRITE)
    {
        lower_irp->Flags |= IRP_WRITE_OPERATION;
        lower_io_stack->Flags |= SL_WRITE_THROUGH;
    }

    lower_irp->Flags |= IRP_NOCACHE;

    lower_io_stack->FileObject = pWkRtnParms->pLUExt->FileObject;

    if ((function == IRP_MJ_WRITE) &&
        (!pWkRtnParms->pLUExt->Modified))
    {
        pWkRtnParms->pLUExt->Modified = TRUE;
    }

    pWkRtnParms->pReqThread = PsGetCurrentThread();
    pWkRtnParms->LowestAssumedIrql = *LowestAssumedIrql;

    IoSetCompletionRoutine(lower_irp, ImScsiParallelReadWriteImageCompletion,
        pWkRtnParms, TRUE, TRUE, TRUE);

    IoCallDriver(lower_device, lower_irp);

    *pResult = ResultQueued;

    return;
}

NTSTATUS
ImScsiReadDevice(
__in pHW_LU_EXTENSION pLUExt,
__in PVOID            Buffer,
__in PLARGE_INTEGER   Offset,
__in PULONG           Length
)
{
    IO_STATUS_BLOCK io_status = { 0 };
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;
    LARGE_INTEGER byteoffset;

    byteoffset.QuadPart = Offset->QuadPart + pLUExt->ImageOffset.QuadPart;

    KdPrint2(("PhDskMnt::ImScsiReadDevice: pLUExt=%p, Buffer=%p, Offset=0x%I64X, EffectiveOffset=0x%I64X, Length=0x%X\n", pLUExt, Buffer, *Offset, byteoffset, *Length));

    if (pLUExt->VMDisk)
    {
#ifdef _WIN64
        ULONG_PTR vm_offset = Offset->QuadPart;
#else
        ULONG_PTR vm_offset = Offset->LowPart;
#endif

        RtlCopyMemory(Buffer,
            pLUExt->ImageBuffer + vm_offset,
            *Length);

        status = STATUS_SUCCESS;
        io_status.Status = status;
        io_status.Information = *Length;
    }
    else if (pLUExt->UseProxy)
        status = ImScsiReadProxy(
        &pLUExt->Proxy,
        &io_status,
        &pLUExt->StopThread,
        Buffer,
        *Length,
        &byteoffset);
    else if (pLUExt->ImageFile != NULL)
        status = NtReadFile(
        pLUExt->ImageFile,
        NULL,
        NULL,
        NULL,
        &io_status,
        Buffer,
        *Length,
        &byteoffset,
        NULL);

    if (status == STATUS_END_OF_FILE)
    {
        KdPrint2(("PhDskMnt::ImScsiReadDevice pLUExt=%p, status=STATUS_END_OF_FILE, Length=0x%X. Returning zeroed buffer with requested length.\n", pLUExt, *Length));
        RtlZeroMemory(Buffer, *Length);
        status = STATUS_SUCCESS;
    }
    else if (NT_SUCCESS(status))
    {
        *Length = (ULONG)io_status.Information;
    }
    else
    {
        *Length = 0;
    }

    KdPrint2(("PhDskMnt::ImScsiReadDevice Result: pLUExt=%p, status=0x%X, Length=0x%X\n", pLUExt, status, *Length));

    return status;
}

NTSTATUS
ImScsiZeroDevice(
    __in pHW_LU_EXTENSION pLUExt,
    __in PLARGE_INTEGER   Offset,
    __in ULONG            Length
    )
{
    IO_STATUS_BLOCK io_status = { 0 };
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;
    LARGE_INTEGER byteoffset;

    byteoffset.QuadPart = Offset->QuadPart + pLUExt->ImageOffset.QuadPart;

    KdPrint2(("PhDskMnt::ImScsiZeroDevice: pLUExt=%p, Offset=0x%I64X, EffectiveOffset=0x%I64X, Length=0x%X\n",
        pLUExt, *Offset, byteoffset, Length));

    pLUExt->Modified = TRUE;

    if (pLUExt->VMDisk)
    {
#ifdef _WIN64
        ULONG_PTR vm_offset = Offset->QuadPart;
#else
        ULONG_PTR vm_offset = Offset->LowPart;
#endif

        RtlZeroMemory(pLUExt->ImageBuffer + vm_offset,
            Length);

        status = STATUS_SUCCESS;
    }
    else if (pLUExt->UseProxy)
    {
        DEVICE_DATA_SET_RANGE range;
        range.StartingOffset = Offset->QuadPart;
        range.LengthInBytes = Length;

        status = ImScsiUnmapOrZeroProxy(
            &pLUExt->Proxy,
            IMDPROXY_REQ_ZERO,
            &io_status,
            &pLUExt->StopThread,
            1,
            &range);
    }
    else if (pLUExt->ImageFile != NULL)
    {
        FILE_ZERO_DATA_INFORMATION zerodata;
        zerodata.FileOffset = *Offset;
        zerodata.BeyondFinalZero.QuadPart = Offset->QuadPart + Length;

        status = ZwFsControlFile(
            pLUExt->ImageFile,
            NULL,
            NULL,
            NULL,
            &io_status,
            FSCTL_SET_ZERO_DATA,
            &zerodata,
            sizeof(zerodata),
            NULL,
            0);
    }

    KdPrint2(("PhDskMnt::ImScsiZeroDevice Result: pLUExt=%p, status=0x%X\n",
        pLUExt, status));

    return status;
}

NTSTATUS
ImScsiWriteDevice(
__in pHW_LU_EXTENSION pLUExt,
__in PVOID            Buffer,
__in PLARGE_INTEGER   Offset,
__in PULONG           Length
)
{
    IO_STATUS_BLOCK io_status = { 0 };
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;
    LARGE_INTEGER byteoffset;

    if (pLUExt->SupportsZero &&
        ImScsiIsBufferZero(Buffer, *Length))
    {
        status = ImScsiZeroDevice(pLUExt, Offset, *Length);

        if (NT_SUCCESS(status))
        {
            KdPrint2(("PhDskMnt::ImScsiWriteDevice: Zero block set at %I64i, bytes: %u.\n",
                Offset->QuadPart, *Length));

            return status;
        }

        KdPrint(("PhDskMnt::ImScsiWriteDevice: Volume does not support "
            "FSCTL_SET_ZERO_DATA: 0x%#X\n", status));

        pLUExt->SupportsZero = FALSE;
    }

    byteoffset.QuadPart = Offset->QuadPart + pLUExt->ImageOffset.QuadPart;

    KdPrint2(("PhDskMnt::ImScsiWriteDevice: pLUExt=%p, Buffer=%p, Offset=0x%I64X, EffectiveOffset=0x%I64X, Length=0x%X\n",
        pLUExt, Buffer, *Offset, byteoffset, *Length));

    pLUExt->Modified = TRUE;

    if (pLUExt->VMDisk)
    {
#ifdef _WIN64
        ULONG_PTR vm_offset = Offset->QuadPart;
#else
        ULONG_PTR vm_offset = Offset->LowPart;
#endif

        RtlCopyMemory(pLUExt->ImageBuffer + vm_offset,
            Buffer,
            *Length);

        status = STATUS_SUCCESS;
        io_status.Status = status;
        io_status.Information = *Length;
    }
    else if (pLUExt->UseProxy)
    {
        status = ImScsiWriteProxy(
            &pLUExt->Proxy,
            &io_status,
            &pLUExt->StopThread,
            Buffer,
            *Length,
            &byteoffset);
    }
    else if (pLUExt->ImageFile != NULL)
    {
        status = NtWriteFile(
            pLUExt->ImageFile,
            NULL,
            NULL,
            NULL,
            &io_status,
            Buffer,
            *Length,
            &byteoffset,
            NULL);
    }

    if (NT_SUCCESS(status))
    {
        *Length = (ULONG)io_status.Information;
    }
    else
    {
        *Length = 0;
    }

    KdPrint2(("PhDskMnt::ImScsiWriteDevice Result: pLUExt=%p, status=0x%X, Length=0x%X\n", pLUExt, status, *Length));

    return status;
}

NTSTATUS
ImScsiExtendLU(
    pHW_HBA_EXT pHBAExt,
    pHW_LU_EXTENSION device_extension,
    PSRB_IMSCSI_EXTEND_DEVICE extend_device_data)
{
    NTSTATUS status;
    FILE_END_OF_FILE_INFORMATION new_size;
    FILE_STANDARD_INFORMATION file_standard_information;

    UNREFERENCED_PARAMETER(pHBAExt);

    new_size.EndOfFile.QuadPart =
        device_extension->DiskSize.QuadPart +
        extend_device_data->ExtendSize.QuadPart;

    KdPrint(("ImScsi: New size of device %i:%i:%i will be %I64i bytes.\n",
        (int)extend_device_data->DeviceNumber.PathId,
        (int)extend_device_data->DeviceNumber.TargetId,
        (int)extend_device_data->DeviceNumber.Lun,
        new_size.EndOfFile.QuadPart));

    if (new_size.EndOfFile.QuadPart <= 0)
    {
        status = STATUS_END_OF_MEDIA;
        goto done;
    }

    if (device_extension->VMDisk)
    {
        PVOID new_image_buffer = NULL;
        SIZE_T free_size = 0;
#ifdef _WIN64
        ULONG_PTR old_size =
            device_extension->DiskSize.QuadPart;
        SIZE_T max_size = new_size.EndOfFile.QuadPart;
#else
        ULONG_PTR old_size =
            device_extension->DiskSize.LowPart;
        SIZE_T max_size = new_size.EndOfFile.LowPart;

        // A vm type disk cannot be extended to a larger size than
        // 2 GB.
        if (new_size.EndOfFile.QuadPart & 0xFFFFFFFF80000000)
        {
            status = STATUS_INVALID_DEVICE_REQUEST;
            goto done;
        }
#endif // _WIN64

        KdPrint(("ImScsi: Allocating %I64u bytes.\n",
            max_size));

        status = ZwAllocateVirtualMemory(NtCurrentProcess(),
            &new_image_buffer,
            0,
            &max_size,
            MEM_COMMIT,
            PAGE_READWRITE);

        if (!NT_SUCCESS(status))
        {
            status = STATUS_NO_MEMORY;
            goto done;
        }

        RtlCopyMemory(new_image_buffer,
            device_extension->ImageBuffer,
            min(old_size, max_size));

        ZwFreeVirtualMemory(NtCurrentProcess(),
            (PVOID*)&device_extension->ImageBuffer,
            &free_size,
            MEM_RELEASE);

        device_extension->ImageBuffer = (PUCHAR)new_image_buffer;
        device_extension->DiskSize = new_size.EndOfFile;

        status = STATUS_SUCCESS;
        goto done;
    }

    // For proxy-type disks the new size is just accepted and
    // that's it.
    if (device_extension->UseProxy)
    {
        device_extension->DiskSize =
            new_size.EndOfFile;

        status = STATUS_SUCCESS;
        goto done;
    }

    // Image file backed disks left to do.

    // For disks with offset, refuse to extend size. Otherwise we
    // could break compatibility with the header data we have
    // skipped and we don't know about.
    if (device_extension->ImageOffset.QuadPart != 0)
    {
        status = STATUS_INVALID_DEVICE_REQUEST;
        goto done;
    }

    IO_STATUS_BLOCK io_status;

    status =
        ZwQueryInformationFile(device_extension->ImageFile,
            &io_status,
            &file_standard_information,
            sizeof file_standard_information,
            FileStandardInformation);

    if (!NT_SUCCESS(status))
    {
        goto done;
    }

    KdPrint(("ImScsi: Current image size is %I64u bytes.\n",
        file_standard_information.EndOfFile.QuadPart));

    if (file_standard_information.EndOfFile.QuadPart >=
        new_size.EndOfFile.QuadPart)
    {
        device_extension->DiskSize = new_size.EndOfFile;

        status = STATUS_SUCCESS;
        goto done;
    }

    // For other, fixed file-backed disks we need to adjust the
    // physical file size.

    KdPrint(("ImScsi: Setting new image size to %I64u bytes.\n",
        new_size.EndOfFile.QuadPart));

    status = ZwSetInformationFile(device_extension->ImageFile,
        &io_status,
        &new_size,
        sizeof new_size,
        FileEndOfFileInformation);

    if (NT_SUCCESS(status))
    {
        device_extension->DiskSize = new_size.EndOfFile;
        goto done;
    }

done:
    KdPrint(("ImScsi: SMP_IMSCSI_EXTEND_DEVICE result: %#x\n", status));

    return status;
}

NTSTATUS
ImScsiInitializeLU(__inout __deref pHW_LU_EXTENSION LUExtension,
__inout __deref PSRB_IMSCSI_CREATE_DATA CreateData,
__in __deref PETHREAD ClientThread)
{
    UNICODE_STRING file_name = { 0 };
    HANDLE thread_handle = NULL;
    NTSTATUS status;
    HANDLE file_handle = NULL;
    PUCHAR image_buffer = NULL;
    PROXY_CONNECTION proxy = { };
    ULONG alignment_requirement;
    BOOLEAN proxy_supports_unmap = FALSE;
    BOOLEAN proxy_supports_zero = FALSE;

    ASSERT(CreateData != NULL);

    KdPrint
        (("PhDskMnt: Got request to create a virtual disk. Request data:\n"
        "DeviceNumber   = %#x\n"
        "DiskSize       = %I64u\n"
        "ImageOffset    = %I64u\n"
        "SectorSize     = %u\n"
        "Flags          = %#x\n"
        "FileNameLength = %u\n"
        "FileName       = '%.*ws'\n",
        CreateData->Fields.DeviceNumber.LongNumber,
        CreateData->Fields.DiskSize.QuadPart,
        CreateData->Fields.ImageOffset.QuadPart,
        CreateData->Fields.BytesPerSector,
        CreateData->Fields.Flags,
        CreateData->Fields.FileNameLength,
        (int)(CreateData->Fields.FileNameLength / sizeof(*CreateData->Fields.FileName)),
        CreateData->Fields.FileName));

    // Auto-select type if not specified.
    if (IMSCSI_TYPE(CreateData->Fields.Flags) == 0)
        if (CreateData->Fields.FileNameLength == 0)
            CreateData->Fields.Flags |= IMSCSI_TYPE_VM;
        else
            CreateData->Fields.Flags |= IMSCSI_TYPE_FILE;

    // Blank filenames only supported for non-zero VM disks.
    if ((CreateData->Fields.FileNameLength == 0) &&
        !(((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_VM) &
        (CreateData->Fields.DiskSize.QuadPart > 0)) |
        ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE) &
        (IMSCSI_FILE_TYPE(CreateData->Fields.Flags) == IMSCSI_FILE_TYPE_AWEALLOC) &
        (CreateData->Fields.DiskSize.QuadPart > 0))))
    {
        KdPrint(("PhDskMnt: Blank filenames only supported for non-zero length "
            "vm type disks.\n"));

        ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
            0,
            0,
            NULL,
            0,
            1000,
            STATUS_INVALID_PARAMETER,
            102,
            STATUS_INVALID_PARAMETER,
            0,
            0,
            NULL,
            L"Blank filenames only supported for non-zero length "
            L"vm type disks."));

        return STATUS_INVALID_PARAMETER;
    }

    if (IMSCSI_BYTE_SWAP(CreateData->Fields.Flags))
    {
        KdPrint(("PhDskMnt: IMSCSI_OPTION_BYTE_SWAP not implemented.\n"));

        return STATUS_NOT_IMPLEMENTED;
    }

    // Cannot create >= 2 GB VM disk in 32 bit version.
#ifndef _WIN64
    if ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_VM) &
        ((CreateData->Fields.DiskSize.QuadPart & 0xFFFFFFFF80000000) !=
        0))
    {
        KdPrint(("PhDskMnt: Cannot create >= 2GB vm disks on 32-bit system.\n"));

        return STATUS_INVALID_PARAMETER;
    }
#endif

    file_name.Length = CreateData->Fields.FileNameLength;
    file_name.MaximumLength = CreateData->Fields.FileNameLength;
    file_name.Buffer = NULL;

    // If a file is to be opened or created, allocate name buffer and open that
    // file...
    if ((CreateData->Fields.FileNameLength > 0) |
        ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE) &
        (IMSCSI_FILE_TYPE(CreateData->Fields.Flags) == IMSCSI_FILE_TYPE_AWEALLOC)))
    {
        IO_STATUS_BLOCK io_status;
        OBJECT_ATTRIBUTES object_attributes;
        UNICODE_STRING real_file_name;
        ACCESS_MASK desired_access = 0;
        ULONG share_access = 0;
        ULONG create_options = 0;

        if (CreateData->Fields.FileNameLength > 0)
        {
            file_name.Buffer = (PWCHAR)ExAllocatePoolWithTag(NonPagedPool,
                file_name.MaximumLength, MP_TAG_GENERAL);

            if (file_name.Buffer == NULL)
            {
                KdPrint(("PhDskMnt: Error allocating buffer for filename.\n"));

                ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                    0,
                    0,
                    NULL,
                    0,
                    1000,
                    STATUS_INSUFFICIENT_RESOURCES,
                    102,
                    STATUS_INSUFFICIENT_RESOURCES,
                    0,
                    0,
                    NULL,
                    L"Memory allocation error."));

                return STATUS_INSUFFICIENT_RESOURCES;
            }

            RtlCopyMemory(file_name.Buffer, CreateData->Fields.FileName,
                CreateData->Fields.FileNameLength);
        }

        // If no device-type specified, check if filename ends with .iso or .nrg.
        // In that case, set device-type automatically to FILE_DEVICE_CDROM
        if ((IMSCSI_DEVICE_TYPE(CreateData->Fields.Flags) == 0) &
            (CreateData->Fields.FileNameLength >= (4 * sizeof(*CreateData->Fields.FileName))))
        {
            LPWSTR name = CreateData->Fields.FileName +
        	(CreateData->Fields.FileNameLength / sizeof(*CreateData->Fields.FileName)) - 4;
            if ((_wcsnicmp(name, L".iso", 4) == 0) ||
        	(_wcsnicmp(name, L".bin", 4) == 0) ||
        	(_wcsnicmp(name, L".nrg", 4) == 0))
        	CreateData->Fields.Flags |= IMSCSI_DEVICE_TYPE_CD | IMSCSI_OPTION_RO;
        }

        if (IMSCSI_DEVICE_TYPE(CreateData->Fields.Flags) ==
            IMSCSI_DEVICE_TYPE_CD)
        {
            CreateData->Fields.Flags |= IMSCSI_OPTION_RO;
        }
        else if (IMSCSI_DEVICE_TYPE(CreateData->Fields.Flags) ==
            IMSCSI_DEVICE_TYPE_FD)
        {
            KdPrint(("PhDskMnt: IMSCSI_DEVICE_TYPE_FD not implemented.\n"));
        }

        KdPrint((
            "PhDskMnt: Done with device type auto-selection by file ext.\n"));

        if (ClientThread != NULL)
        {
            SECURITY_QUALITY_OF_SERVICE security_quality_of_service;
            SECURITY_CLIENT_CONTEXT security_client_context;

            RtlZeroMemory(&security_quality_of_service,
                sizeof(SECURITY_QUALITY_OF_SERVICE));

            security_quality_of_service.Length =
                sizeof(SECURITY_QUALITY_OF_SERVICE);
            security_quality_of_service.ImpersonationLevel =
                SecurityImpersonation;
            security_quality_of_service.ContextTrackingMode =
                SECURITY_STATIC_TRACKING;
            security_quality_of_service.EffectiveOnly = FALSE;

            status =
                SeCreateClientSecurity(
                ClientThread,
                &security_quality_of_service,
                FALSE,
                &security_client_context);

            if (NT_SUCCESS(status))
            {
                KdPrint(("PhDskMnt: Impersonating client thread token.\n"));
                SeImpersonateClient(&security_client_context, NULL);
                SeDeleteClientSecurity(&security_client_context);
            }
            else
                DbgPrint("PhDskMnt: Error impersonating client thread token: %#X\n", status);
        }
        else
            KdPrint(("PhDskMnt: No impersonation information.\n"));

        if ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE) &&
            (IMSCSI_FILE_TYPE(CreateData->Fields.Flags) ==
            IMSCSI_FILE_TYPE_AWEALLOC))
        {
            real_file_name.MaximumLength = sizeof(AWEALLOC_DEVICE_NAME) +
                file_name.Length;

            real_file_name.Buffer = (PWCHAR)
                ExAllocatePoolWithTag(PagedPool,
                real_file_name.MaximumLength,
                MP_TAG_GENERAL);

            if (real_file_name.Buffer == NULL)
            {
                KdPrint(("ImDisk: Out of memory while allocating %#x bytes\n",
                    real_file_name.MaximumLength));

                if (file_name.Buffer != NULL)
                    ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                return STATUS_INSUFFICIENT_RESOURCES;
            }

            real_file_name.Length = 0;

            status =
                RtlAppendUnicodeToString(&real_file_name,
                AWEALLOC_DEVICE_NAME);

            if (NT_SUCCESS(status) && (file_name.Length > 0))
                status =
                RtlAppendUnicodeStringToString(&real_file_name,
                &file_name);

            if (!NT_SUCCESS(status))
            {
                KdPrint(("ImDisk: Internal error: "
                    "RtlAppendUnicodeStringToString failed with "
                    "pre-allocated buffers.\n"));

                if (file_name.Buffer != NULL)
                    ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                ExFreePoolWithTag(real_file_name.Buffer, MP_TAG_GENERAL);
                
                return STATUS_DRIVER_INTERNAL_ERROR;
            }

            InitializeObjectAttributes(&object_attributes,
                &real_file_name,
                OBJ_CASE_INSENSITIVE |
                OBJ_FORCE_ACCESS_CHECK,
                NULL,
                NULL);
        }
        else if ((IMSCSI_TYPE(CreateData->Fields.Flags) ==
            IMSCSI_TYPE_PROXY) &
            ((IMSCSI_PROXY_TYPE(CreateData->Fields.Flags) ==
            IMSCSI_PROXY_TYPE_TCP) |
            (IMSCSI_PROXY_TYPE(CreateData->Fields.Flags) ==
            IMSCSI_PROXY_TYPE_COMM)))
        {
            RtlInitUnicodeString(&real_file_name,
                IMDPROXY_SVC_PIPE_NATIVE_NAME);

            InitializeObjectAttributes(&object_attributes,
                &real_file_name,
                OBJ_CASE_INSENSITIVE,
                NULL,
                NULL);
        }
        else
        {
            real_file_name = file_name;

            InitializeObjectAttributes(&object_attributes,
                &real_file_name,
                OBJ_CASE_INSENSITIVE |
                OBJ_FORCE_ACCESS_CHECK,
                NULL,
                NULL);
        }

        if ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_PROXY) &
            (IMSCSI_PROXY_TYPE(CreateData->Fields.Flags) ==
            IMSCSI_PROXY_TYPE_SHM))
        {
            proxy.connection_type = PROXY_CONNECTION::PROXY_CONNECTION_SHM;

            status =
                ZwOpenSection(&file_handle,
                GENERIC_READ | GENERIC_WRITE,
                &object_attributes);
        }
        else
        {
            desired_access = GENERIC_READ;

            if ((IMSCSI_TYPE(CreateData->Fields.Flags) ==
                IMSCSI_TYPE_PROXY) ||
                ((IMSCSI_TYPE(CreateData->Fields.Flags) != IMSCSI_TYPE_VM) &&
                !IMSCSI_READONLY(CreateData->Fields.Flags)))
                desired_access |= GENERIC_WRITE;

            share_access = FILE_SHARE_READ | FILE_SHARE_DELETE;

            if (IMSCSI_READONLY(CreateData->Fields.Flags) ||
                (IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_VM))
                share_access |= FILE_SHARE_WRITE;

            create_options = FILE_NON_DIRECTORY_FILE |
                FILE_NO_INTERMEDIATE_BUFFERING |
                FILE_SYNCHRONOUS_IO_NONALERT;

            if (IMSCSI_SPARSE_FILE(CreateData->Fields.Flags))
                create_options |= FILE_OPEN_FOR_BACKUP_INTENT;

            if (IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_PROXY)
                create_options |= FILE_SEQUENTIAL_ONLY;
            else
                create_options |= FILE_RANDOM_ACCESS;

            KdPrint(("PhDskMnt::ImScsiCreateLU: Passing DesiredAccess=%#x ShareAccess=%#x CreateOptions=%#x\n",
                desired_access, share_access, create_options));

            status = ZwCreateFile(
                &file_handle,
                desired_access,
                &object_attributes,
                &io_status,
                NULL,
                FILE_ATTRIBUTE_NORMAL,
                share_access,
                FILE_OPEN,
                create_options,
                NULL,
                0);
        }

        // For 32 bit driver running on Windows 2000 and earlier, the above
        // call will fail because OBJ_FORCE_ACCESS_CHECK is not supported. If so,
        // STATUS_INVALID_PARAMETER is returned and we go on without any access
        // checks in that case.
#ifdef NT4_COMPATIBLE
        if (status == STATUS_INVALID_PARAMETER)
        {
            InitializeObjectAttributes(&object_attributes,
                &real_file_name,
                OBJ_CASE_INSENSITIVE,
                NULL,
                NULL);

            if ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_PROXY) &
                (IMSCSI_PROXY_TYPE(CreateData->Fields.Flags) == IMSCSI_PROXY_TYPE_SHM))
            {
                proxy.connection_type = PROXY_CONNECTION_SHM;

                status =
                    ZwOpenSection(&file_handle,
                    GENERIC_READ | GENERIC_WRITE,
                    &object_attributes);
            }
            else
            {
                status = ZwCreateFile(
                    &file_handle,
                    desired_access,
                    &object_attributes,
                    &io_status,
                    NULL,
                    FILE_ATTRIBUTE_NORMAL,
                    share_access,
                    FILE_OPEN,
                    create_options,
                    NULL,
                    0);
            }
        }
#endif

        if (!NT_SUCCESS(status))
        {
            KdPrint(("PhDskMnt: Error opening file '%.*ws'. Status: %#x SpecSize: %i WritableFile: %i DevTypeFile: %i Flags: %#x\n",
                (int)(real_file_name.Length / sizeof(WCHAR)),
                real_file_name.Buffer,
                status,
                CreateData->Fields.DiskSize.QuadPart != 0,
                !IMSCSI_READONLY(CreateData->Fields.Flags),
                IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE,
                CreateData->Fields.Flags));
        }

        // If not found we will create the file if a new non-zero size is
        // specified, read-only virtual disk is not specified and we are
        // creating a type 'file' virtual disk.
        if (((status == STATUS_OBJECT_NAME_NOT_FOUND) |
            (status == STATUS_NO_SUCH_FILE)) &
            (CreateData->Fields.DiskSize.QuadPart != 0) &
            (!IMSCSI_READONLY(CreateData->Fields.Flags)) &
            (IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE))
        {

            status = ZwCreateFile(
                &file_handle,
                GENERIC_READ |
                GENERIC_WRITE,
                &object_attributes,
                &io_status,
                NULL,
                FILE_ATTRIBUTE_NORMAL,
                share_access,
                FILE_OPEN_IF,
                create_options, NULL, 0);

            if (!NT_SUCCESS(status))
            {
                ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                    0,
                    0,
                    NULL,
                    0,
                    1000,
                    status,
                    102,
                    status,
                    0,
                    0,
                    NULL,
                    L"Cannot create image file."));

                KdPrint(("PhDskMnt: Cannot create '%.*ws'. (%#x)\n",
                    (int)(CreateData->Fields.FileNameLength /
                    sizeof(*CreateData->Fields.FileName)),
                    CreateData->Fields.FileName,
                    status));

                if ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE) &&
                    (IMSCSI_FILE_TYPE(CreateData->Fields.Flags) == IMSCSI_FILE_TYPE_AWEALLOC))
                    ExFreePoolWithTag(real_file_name.Buffer, MP_TAG_GENERAL);

                if (file_name.Buffer != NULL)
                    ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                return status;
            }
        }
        else if (!NT_SUCCESS(status))
        {
            ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                0,
                0,
                NULL,
                0,
                1000,
                status,
                102,
                status,
                0,
                0,
                NULL,
                L"Cannot open image file."));

            KdPrint(("PhDskMnt: Cannot open file '%.*ws'. Status: %#x\n",
                (int)(real_file_name.Length / sizeof(WCHAR)),
                real_file_name.Buffer,
                status));

            if ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE) &&
                (IMSCSI_FILE_TYPE(CreateData->Fields.Flags) == IMSCSI_FILE_TYPE_AWEALLOC))
                ExFreePoolWithTag(real_file_name.Buffer, MP_TAG_GENERAL);

            if (file_name.Buffer != NULL)
                ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

            return status;
        }

        KdPrint(("PhDskMnt: File '%.*ws' opened successfully.\n",
            (int)(real_file_name.Length / sizeof(WCHAR)),
            real_file_name.Buffer));

        if ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE) &&
            (IMSCSI_FILE_TYPE(CreateData->Fields.Flags) == IMSCSI_FILE_TYPE_AWEALLOC))
            ExFreePoolWithTag(real_file_name.Buffer, MP_TAG_GENERAL);

        if (IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_PROXY)
        {
            if (IMSCSI_PROXY_TYPE(CreateData->Fields.Flags) == IMSCSI_PROXY_TYPE_SHM)
                status =
                ZwMapViewOfSection(file_handle,
                NtCurrentProcess(),
                (PVOID*)&proxy.shared_memory,
                0,
                0,
                NULL,
                &proxy.shared_memory_size,
                ViewUnmap,
                0,
                PAGE_READWRITE);
            else
                status =
                ObReferenceObjectByHandle(file_handle,
                FILE_READ_ATTRIBUTES |
                FILE_READ_DATA |
                FILE_WRITE_DATA,
                *IoFileObjectType,
                KernelMode,
                (PVOID*)&proxy.device,
                NULL);

            if (!NT_SUCCESS(status))
            {
                ZwClose(file_handle);

                if (file_name.Buffer != NULL)
                    ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                    0,
                    0,
                    NULL,
                    0,
                    1000,
                    status,
                    102,
                    status,
                    0,
                    0,
                    NULL,
                    L"Error referencing proxy device."));

                KdPrint(("PhDskMnt: Error referencing proxy device (%#x).\n",
                    status));

                return status;
            }

            KdPrint(("PhDskMnt: Got reference to proxy object %p.\n",
                proxy.connection_type == PROXY_CONNECTION::PROXY_CONNECTION_DEVICE ?
                (PVOID)proxy.device :
                (PVOID)proxy.shared_memory));

            if (IMSCSI_PROXY_TYPE(CreateData->Fields.Flags) != IMSCSI_PROXY_TYPE_DIRECT)
                status = ImScsiConnectProxy(&proxy,
                &io_status,
                NULL,
                CreateData->Fields.Flags,
                CreateData->Fields.FileName,
                CreateData->Fields.FileNameLength);

            if (!NT_SUCCESS(status))
            {
                ImScsiCloseProxy(&proxy);

                ZwClose(file_handle);

                if (file_name.Buffer != NULL)
                    ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                    0,
                    0,
                    NULL,
                    0,
                    1000,
                    status,
                    102,
                    status,
                    0,
                    0,
                    NULL,
                    L"Error connecting proxy."));

                KdPrint(("PhDskMnt: Error connecting proxy (%#x).\n", status));

                return status;
            }
        }

        // Get the file size of the disk file.
        if (IMSCSI_TYPE(CreateData->Fields.Flags) != IMSCSI_TYPE_PROXY)
        {
            LARGE_INTEGER disk_size;

            status = ImScsiGetDiskSize(
                file_handle,
                &io_status,
                &disk_size);

            if (!NT_SUCCESS(status))
            {
                ZwClose(file_handle);

                if (file_name.Buffer != NULL)
                    ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                    0,
                    0,
                    NULL,
                    0,
                    1000,
                    status,
                    102,
                    status,
                    0,
                    0,
                    NULL,
                    L"Error getting image size."));

                KdPrint
                    (("PhDskMnt: Error getting image size (%#x).\n",
                    status));

                return status;
            }

            // Allocate virtual memory for 'vm' type.
            if (IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_VM)
            {
                SIZE_T max_size;

                // If no size given for VM disk, use size of pre-load image file.
                // This code is somewhat easier for 64 bit architectures.

#ifdef _WIN64
                if (CreateData->Fields.DiskSize.QuadPart == 0)
                {
                    CreateData->Fields.DiskSize.QuadPart =
                        disk_size.QuadPart -
                        CreateData->Fields.ImageOffset.QuadPart;
                }

                max_size = CreateData->Fields.DiskSize.QuadPart;
#else
                if (CreateData->Fields.DiskSize.QuadPart == 0)
                {
                    // Check that file size < 2 GB.
                    if ((disk_size.QuadPart -
                        CreateData->Fields.ImageOffset.QuadPart) &
                        0xFFFFFFFF80000000)
                    {
                        ZwClose(file_handle);

                        if (file_name.Buffer != NULL)
                            ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                        KdPrint(("PhDskMnt: VM disk >= 2GB not supported.\n"));

                        return STATUS_INSUFFICIENT_RESOURCES;
                    }
                    else
                    {
                        CreateData->Fields.DiskSize.QuadPart =
                            disk_size.QuadPart -
                            CreateData->Fields.ImageOffset.QuadPart;
                    }
                }

                max_size = CreateData->Fields.DiskSize.LowPart;
#endif

                status =
                    ZwAllocateVirtualMemory(NtCurrentProcess(),
                    (PVOID*)&image_buffer,
                    0,
                    &max_size,
                    MEM_COMMIT,
                    PAGE_READWRITE);
                if (!NT_SUCCESS(status))
                {
                    ZwClose(file_handle);

                    if (file_name.Buffer != NULL)
                        ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                    ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                        0,
                        0,
                        NULL,
                        0,
                        1000,
                        status,
                        102,
                        status,
                        0,
                        0,
                        NULL,
                        L"Not enough memory for VM disk."));

                    KdPrint(("PhDskMnt: Error allocating vm for image. (%#x)\n",
                        status));

                    return STATUS_NO_MEMORY;
                }

                alignment_requirement = FILE_BYTE_ALIGNMENT;

                // Loading of image file has been moved to be done just before
                // the service loop.
            }
            else
            {
                FILE_ALIGNMENT_INFORMATION file_alignment;

                status = ZwQueryInformationFile(file_handle,
                    &io_status,
                    &file_alignment,
                    sizeof
                    (FILE_ALIGNMENT_INFORMATION),
                    FileAlignmentInformation);

                if (!NT_SUCCESS(status))
                {
                    ZwClose(file_handle);

                    if (file_name.Buffer != NULL)
                        ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                    ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                        0,
                        0,
                        NULL,
                        0,
                        1000,
                        status,
                        102,
                        status,
                        0,
                        0,
                        NULL,
                        L"Error getting alignment information."));

                    KdPrint(("PhDskMnt: Error querying file alignment (%#x).\n",
                        status));

                    return status;
                }

                // If creating a sparse image file
                if (IMSCSI_SPARSE_FILE(CreateData->Fields.Flags))
                {
                    status = ZwFsControlFile(
                        file_handle,
                        NULL,
                        NULL,
                        NULL,
                        &io_status,
                        FSCTL_SET_SPARSE,
                        NULL,
                        0,
                        NULL,
                        0
                        );

                    if (NT_SUCCESS(status))
                        KdPrint(("PhDskMnt::ImScsiInitializeLU: Sparse attribute set on image file.\n"));
                    else
                        DbgPrint("PhDskMnt::ImScsiInitializeLU: Cannot set sparse attribute on image file: 0x%X\n", status);
                }

                if (CreateData->Fields.DiskSize.QuadPart == 0)
                    CreateData->Fields.DiskSize.QuadPart =
                    disk_size.QuadPart -
                    CreateData->Fields.ImageOffset.QuadPart;
                else if ((disk_size.QuadPart <
                    CreateData->Fields.DiskSize.QuadPart +
                    CreateData->Fields.ImageOffset.QuadPart) &
                    (!IMSCSI_READONLY(CreateData->Fields.Flags)))
                {
                    LARGE_INTEGER new_image_size;
                    new_image_size.QuadPart =
                        CreateData->Fields.DiskSize.QuadPart +
                        CreateData->Fields.ImageOffset.QuadPart;

                    // Adjust the file length to the requested virtual disk size.
                    status = ZwSetInformationFile(
                        file_handle,
                        &io_status,
                        &new_image_size,
                        sizeof(FILE_END_OF_FILE_INFORMATION),
                        FileEndOfFileInformation);

                    if (!NT_SUCCESS(status))
                    {
                        ZwClose(file_handle);

                        if (file_name.Buffer != NULL)
                            ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                        ImScsiLogError((
                            pMPDrvInfoGlobal->pDriverObj,
                            0,
                            0,
                            NULL,
                            0,
                            1000,
                            status,
                            102,
                            status,
                            0,
                            0,
                            NULL,
                            L"Error setting file size."));

                        DbgPrint("PhDskMnt: Error setting eof (%#x).\n", status);
                        return status;
                    }
                }

                alignment_requirement = file_alignment.AlignmentRequirement;
            }
        }
        else
            // If proxy is used, get the image file size from the proxy instead.
        {
            IMDPROXY_INFO_RESP proxy_info;

            status = ImScsiQueryInformationProxy(&proxy,
                &io_status,
                NULL,
                &proxy_info,
                sizeof(IMDPROXY_INFO_RESP));

            if (!NT_SUCCESS(status))
            {
                ImScsiCloseProxy(&proxy);
                ZwClose(file_handle);

                if (file_name.Buffer != NULL)
                    ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                    0,
                    0,
                    NULL,
                    0,
                    1000,
                    status,
                    102,
                    status,
                    0,
                    0,
                    NULL,
                    L"Error querying proxy."));

                KdPrint(("PhDskMnt: Error querying proxy (%#x).\n", status));

                return status;
            }

            if (CreateData->Fields.DiskSize.QuadPart == 0)
                CreateData->Fields.DiskSize.QuadPart = proxy_info.file_size;

            if ((proxy_info.req_alignment - 1 > FILE_512_BYTE_ALIGNMENT) |
                (CreateData->Fields.DiskSize.QuadPart == 0))
            {
                ImScsiCloseProxy(&proxy);
                ZwClose(file_handle);

                if (file_name.Buffer != NULL)
                    ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);

                ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                    0,
                    0,
                    NULL,
                    0,
                    1000,
                    status,
                    102,
                    status,
                    0,
                    0,
                    NULL,
                    L"Unsupported sizes."));

                KdPrint(("PhDskMnt: Unsupported sizes. "
                    "Got 0x%I64x size and 0x%I64x alignment.\n",
                    proxy_info.file_size,
                    proxy_info.req_alignment));

                return STATUS_INVALID_PARAMETER;
            }

            alignment_requirement = (ULONG)proxy_info.req_alignment - 1;

            if (proxy_info.flags & IMDPROXY_FLAG_RO)
                CreateData->Fields.Flags |= IMSCSI_OPTION_RO;

            if (proxy_info.flags & IMDPROXY_FLAG_SUPPORTS_UNMAP)
                proxy_supports_unmap = TRUE;

            if (proxy_info.flags & IMDPROXY_FLAG_SUPPORTS_ZERO)
                proxy_supports_zero = TRUE;

            KdPrint(("PhDskMnt: Got from proxy: Siz=0x%08x%08x Flg=%#x Alg=%#x.\n",
                CreateData->Fields.DiskSize.HighPart,
                CreateData->Fields.DiskSize.LowPart,
                (ULONG)proxy_info.flags,
                (ULONG)proxy_info.req_alignment));
        }

        if (CreateData->Fields.DiskSize.QuadPart == 0)
        {
            SIZE_T free_size = 0;

            ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                0,
                0,
                NULL,
                0,
                1000,
                status,
                102,
                status,
                0,
                0,
                NULL,
                L"Disk size equals zero."));

            KdPrint(("PhDskMnt: Fatal error: Disk size equals zero.\n"));

            ImScsiCloseProxy(&proxy);
            if (file_handle != NULL)
                ZwClose(file_handle);
            if (file_name.Buffer != NULL)
                ExFreePoolWithTag(file_name.Buffer, MP_TAG_GENERAL);
            if (image_buffer != NULL)
                ZwFreeVirtualMemory(NtCurrentProcess(),
                (PVOID*)&image_buffer,
                &free_size, MEM_RELEASE);

            return STATUS_INVALID_PARAMETER;
        }
    }
    // Blank vm-disk, just allocate...
    else
    {
        SIZE_T max_size;
#ifdef _WIN64
        max_size = CreateData->Fields.DiskSize.QuadPart;
#else
        max_size = CreateData->Fields.DiskSize.LowPart;
#endif

        image_buffer = NULL;
        status =
            ZwAllocateVirtualMemory(NtCurrentProcess(),
            (PVOID*)&image_buffer,
            0,
            &max_size,
            MEM_COMMIT,
            PAGE_READWRITE);
        if (!NT_SUCCESS(status))
        {
            KdPrint
                (("PhDskMnt: Error allocating virtual memory for vm disk (%#x).\n",
                status));

            ImScsiLogError((pMPDrvInfoGlobal->pDriverObj,
                0,
                0,
                NULL,
                0,
                1000,
                status,
                102,
                status,
                0,
                0,
                NULL,
                L"Not enough free memory for VM disk."));

            return STATUS_NO_MEMORY;
        }

        alignment_requirement = FILE_BYTE_ALIGNMENT;
    }

    KdPrint(("PhDskMnt: Done with file/memory checks.\n"));

    // If no device-type specified and size matches common floppy sizes,
    // auto-select FILE_DEVICE_DISK with FILE_FLOPPY_DISKETTE and
    // FILE_REMOVABLE_MEDIA.
    // If still no device-type specified, specify FILE_DEVICE_DISK with no
    // particular characteristics. This will emulate a hard disk partition.
    if (IMSCSI_DEVICE_TYPE(CreateData->Fields.Flags) == 0)
        CreateData->Fields.Flags |= IMSCSI_DEVICE_TYPE_HD;

    KdPrint(("PhDskMnt: Done with device type selection for floppy sizes.\n"));

    // If some parts of the DISK_GEOMETRY structure are zero, auto-fill with
    // typical values for this type of disk.
    if (IMSCSI_DEVICE_TYPE(CreateData->Fields.Flags) == IMSCSI_DEVICE_TYPE_CD)
    {
        if (CreateData->Fields.BytesPerSector == 0)
            CreateData->Fields.BytesPerSector = DEFAULT_SECTOR_SIZE_CD_ROM;

        CreateData->Fields.Flags |= IMSCSI_OPTION_REMOVABLE | IMSCSI_OPTION_RO;
    }
    else
    {
        if (CreateData->Fields.BytesPerSector == 0)
            CreateData->Fields.BytesPerSector = DEFAULT_SECTOR_SIZE_HDD;
    }

    KdPrint(("PhDskMnt: Done with disk geometry setup.\n"));

    // Now build real DeviceType and DeviceCharacteristics parameters.
    switch (IMSCSI_DEVICE_TYPE(CreateData->Fields.Flags))
    {
    case IMSCSI_DEVICE_TYPE_CD:
        LUExtension->DeviceType = READ_ONLY_DIRECT_ACCESS_DEVICE;
        CreateData->Fields.Flags |= IMSCSI_OPTION_REMOVABLE;
        break;

    case IMSCSI_DEVICE_TYPE_RAW:
        LUExtension->DeviceType = ARRAY_CONTROLLER_DEVICE;
        break;

    default:
        LUExtension->DeviceType = DIRECT_ACCESS_DEVICE;
    }

    if (IMSCSI_READONLY(CreateData->Fields.Flags))
        LUExtension->ReadOnly = TRUE;

    if (IMSCSI_REMOVABLE(CreateData->Fields.Flags))
        LUExtension->RemovableMedia = TRUE;

    if (alignment_requirement > CreateData->Fields.BytesPerSector)
        CreateData->Fields.BytesPerSector = alignment_requirement + 1;

    KdPrint
        (("PhDskMnt: After checks and translations we got this create data:\n"
        "DeviceNumber   = %#x\n"
        "DiskSize       = %I64u\n"
        "ImageOffset    = %I64u\n"
        "SectorSize     = %u\n"
        "Flags          = %#x\n"
        "FileNameLength = %u\n"
        "FileName       = '%.*ws'\n",
        CreateData->Fields.DeviceNumber.LongNumber,
        CreateData->Fields.DiskSize.QuadPart,
        CreateData->Fields.ImageOffset.QuadPart,
        CreateData->Fields.BytesPerSector,
        CreateData->Fields.Flags,
        CreateData->Fields.FileNameLength,
        (int)(CreateData->Fields.FileNameLength / sizeof(*CreateData->Fields.FileName)),
        CreateData->Fields.FileName));

    LUExtension->ObjectName = file_name;

    LUExtension->DiskSize = CreateData->Fields.DiskSize;

    while (CreateData->Fields.BytesPerSector >>= 1)
        LUExtension->BlockPower++;
    if (LUExtension->BlockPower == 0)
        LUExtension->BlockPower = DEFAULT_BLOCK_POWER;
    CreateData->Fields.BytesPerSector = 1UL << LUExtension->BlockPower;

    LUExtension->ImageOffset = CreateData->Fields.ImageOffset;

    // VM disk.
    if (IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_VM)
        LUExtension->VMDisk = TRUE;
    else
        LUExtension->VMDisk = FALSE;

    // AWEAlloc disk.
    if ((IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE) &
        (IMSCSI_FILE_TYPE(CreateData->Fields.Flags) == IMSCSI_FILE_TYPE_AWEALLOC))
        LUExtension->AWEAllocDisk = TRUE;
    else
        LUExtension->AWEAllocDisk = FALSE;

    LUExtension->ImageBuffer = image_buffer;
    LUExtension->ImageFile = file_handle;

    // Use proxy service.
    if (IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_PROXY)
    {
        LUExtension->Proxy = proxy;
        LUExtension->UseProxy = TRUE;
    }
    else
        LUExtension->UseProxy = FALSE;

    // If we are going to fake a disk signature if existing one
    // is all zeroes and device is read-only, prepare that fake
    // disk sig here.
    if ((CreateData->Fields.Flags & IMSCSI_FAKE_DISK_SIG_IF_ZERO) &&
        IMSCSI_READONLY(CreateData->Fields.Flags))
    {
        LUExtension->FakeDiskSignature =
            (RtlRandomEx(&pMPDrvInfoGlobal->RandomSeed) |
                0x80808081UL) & 0xFEFEFEFFUL;
    }

    if ((LUExtension->FileObject == NULL) &&
        (!LUExtension->AWEAllocDisk) &&
        (!LUExtension->VMDisk) &&
        ((!LUExtension->UseProxy) ||
            proxy_supports_unmap))
    {
        LUExtension->SupportsUnmap = TRUE;
    }

    if ((LUExtension->FileObject == NULL) &&
        (!LUExtension->AWEAllocDisk) &&
        (!LUExtension->VMDisk) &&
        ((!LUExtension->UseProxy) ||
            proxy_supports_zero))
    {
        LUExtension->SupportsZero = TRUE;
    }

    KeInitializeSpinLock(&LUExtension->RequestListLock);
    InitializeListHead(&LUExtension->RequestList);
    KeInitializeEvent(&LUExtension->RequestEvent, SynchronizationEvent, FALSE);

    KeInitializeEvent(&LUExtension->Initialized, NotificationEvent, FALSE);

    KeInitializeSpinLock(&LUExtension->LastIoLock);

    KeSetEvent(&LUExtension->Initialized, (KPRIORITY)0, FALSE);

    KdPrint(("PhDskMnt::ImScsiCreateLU: Creating worker thread for pLUExt=0x%p.\n",
        LUExtension));

    // Get FILE_OBJECT if we will need that later
    if ((file_handle != NULL) &&
        (IMSCSI_TYPE(CreateData->Fields.Flags) == IMSCSI_TYPE_FILE) &&
        ((IMSCSI_FILE_TYPE(CreateData->Fields.Flags) == IMSCSI_FILE_TYPE_AWEALLOC) ||
        (IMSCSI_FILE_TYPE(CreateData->Fields.Flags) == IMSCSI_FILE_TYPE_PARALLEL_IO)))
    {
        status = ObReferenceObjectByHandle(file_handle,
            SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_READ_DATA |
            (LUExtension->ReadOnly ?
            0 : FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES),
            *IoFileObjectType,
            KernelMode, (PVOID*)&LUExtension->FileObject, NULL);

        if (!NT_SUCCESS(status))
        {
            LUExtension->FileObject = NULL;

            DbgPrint("PhDskMnt::ImScsiCreateLU: Error referencing image file handle: %#x\n",
                status);
        }
    }

    status = PsCreateSystemThread(
        &thread_handle,
        (ACCESS_MASK)0L,
        NULL,
        NULL,
        NULL,
        ImScsiWorkerThread,
        LUExtension);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("PhDskMnt::ImScsiCreateLU: Cannot create device worker thread. (%#x)\n", status);

        return status;
    }

    KeWaitForSingleObject(
        &LUExtension->Initialized,
        Executive,
        KernelMode,
        FALSE,
        NULL);

    status = ObReferenceObjectByHandle(
        thread_handle,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        *PsThreadType,
        KernelMode,
        (PVOID*)&LUExtension->WorkerThread,
        NULL
        );

    if (!NT_SUCCESS(status))
    {
        LUExtension->WorkerThread = NULL;

        DbgPrint("PhDskMnt::ImScsiCreateLU: Cannot reference device worker thread. (%#x)\n", status);
        KeSetEvent(&LUExtension->StopThread, (KPRIORITY)0, FALSE);
        ZwWaitForSingleObject(thread_handle, FALSE, NULL);
    }
    else
    {
        KdPrint(("PhDskMnt::ImScsiCreateLU: Device created and ready.\n"));
    }

    ZwClose(thread_handle);

    return status;
}


BOOLEAN
ImScsiFillMemoryDisk(pHW_LU_EXTENSION LUExtension)
{
    LARGE_INTEGER byte_offset = LUExtension->ImageOffset;
    IO_STATUS_BLOCK io_status;
    NTSTATUS status;
#ifdef _WIN64
    SIZE_T disk_size = LUExtension->DiskSize.QuadPart;
#else
    SIZE_T disk_size = LUExtension->DiskSize.LowPart;
#endif

    KdPrint(("PhDskMnt: Reading image file into vm disk buffer.\n"));

    status =
        ImScsiSafeReadFile(
        LUExtension->ImageFile,
        &io_status,
        LUExtension->ImageBuffer,
        disk_size,
        &byte_offset);

    ZwClose(LUExtension->ImageFile);
    LUExtension->ImageFile = NULL;

    // Failure to read pre-load image is now considered a fatal error
    if (!NT_SUCCESS(status))
    {
        KdPrint(("PhDskMnt: Failed to read image file (%#x).\n", status));

        return FALSE;
    }

    KdPrint(("PhDskMnt: Image loaded successfully.\n"));

    return TRUE;
}

VOID
ImScsiUnmapDevice(
    __in pHW_HBA_EXT pHBAExt,
    __in pHW_LU_EXTENSION pLUExt,
    __in PSCSI_REQUEST_BLOCK pSrb)
{
    PUNMAP_LIST_HEADER list = (PUNMAP_LIST_HEADER)pSrb->DataBuffer;
    USHORT descrlength = RtlUshortByteSwap(*(PUSHORT)list->BlockDescrDataLength);

    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(pLUExt);

    if ((ULONG)descrlength + FIELD_OFFSET(UNMAP_LIST_HEADER, Descriptors) >
        pSrb->DataTransferLength)
    {
        KdBreakPoint();
        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    USHORT items = descrlength / sizeof(*list->Descriptors);

    NTSTATUS status = STATUS_SUCCESS;

    IO_STATUS_BLOCK io_status;

    if (pLUExt->UseProxy)
    {
        WPoolMem<DEVICE_DATA_SET_RANGE, PagedPool> range(sizeof(DEVICE_DATA_SET_RANGE) * items);

        if (!range)
        {
            ScsiSetError(pSrb, SRB_STATUS_ERROR);
            return;
        }

        for (USHORT i = 0; i < items; i++)
        {
            LONGLONG startingSector = RtlUlonglongByteSwap(*(PULONGLONG)list->Descriptors[i].StartingLba);
            ULONG numBlocks = RtlUlongByteSwap(*(PULONG)list->Descriptors[i].LbaCount);

            range[i].StartingOffset = (startingSector << pLUExt->BlockPower) + pLUExt->ImageOffset.QuadPart;
            range[i].LengthInBytes = (ULONGLONG)numBlocks << pLUExt->BlockPower;

            KdPrint(("PhDskMnt::ImScsiDispatchUnmap: Offset: %I64i, bytes: %I64u\n",
                range[i].StartingOffset, range[i].LengthInBytes));
        }

        status = ImScsiUnmapOrZeroProxy(
            &pLUExt->Proxy,
            IMDPROXY_REQ_UNMAP,
            &io_status,
            &pLUExt->StopThread,
            items,
            range);
    }
    else if (pLUExt->ImageFile != NULL)
    {
        FILE_ZERO_DATA_INFORMATION zerodata;

#if _NT_TARGET_VERSION >= 0x602
        ULONG fltrim_size = FIELD_OFFSET(FILE_LEVEL_TRIM, Ranges) +
            (items * sizeof(FILE_LEVEL_TRIM_RANGE));

        WPoolMem<FILE_LEVEL_TRIM, PagedPool> fltrim;

        if (!pLUExt->NoFileLevelTrim)
        {
            fltrim.Alloc(fltrim_size);

            if (!fltrim)
            {
                ScsiSetError(pSrb, SRB_STATUS_ERROR);
                return;
            }

            fltrim->NumRanges = items;
        }
#endif

        for (int i = 0; i < items; i++)
        {
            LONGLONG startingSector = RtlUlonglongByteSwap(*(PULONGLONG)list->Descriptors[i].StartingLba);
            ULONG numBlocks = RtlUlongByteSwap(*(PULONG)list->Descriptors[i].LbaCount);

            zerodata.FileOffset.QuadPart = (startingSector << pLUExt->BlockPower) + pLUExt->ImageOffset.QuadPart;
            zerodata.BeyondFinalZero.QuadPart = ((LONGLONG)numBlocks << pLUExt->BlockPower) +
                zerodata.FileOffset.QuadPart;

            KdPrint(("PhDskMnt::ImScsiDispatchUnmap: Zero data request from 0x%I64X to 0x%I64X\n",
                zerodata.FileOffset.QuadPart, zerodata.BeyondFinalZero.QuadPart));

#if _NT_TARGET_VERSION >= 0x602
            if (!pLUExt->NoFileLevelTrim)
            {
                fltrim->Ranges[i].Offset = zerodata.FileOffset.QuadPart;
                fltrim->Ranges[i].Length = (LONGLONG)numBlocks << pLUExt->BlockPower;

                KdPrint(("PhDskMnt::ImScsiDispatchUnmap: File level trim request 0x%I64X bytes at 0x%I64X\n",
                    fltrim->Ranges[i].Length, fltrim->Ranges[i].Offset));
            }
#endif

            status = ZwFsControlFile(
                pLUExt->ImageFile,
                NULL,
                NULL,
                NULL,
                &io_status,
                FSCTL_SET_ZERO_DATA,
                &zerodata,
                sizeof(zerodata),
                NULL,
                0);

            KdPrint(("PhDskMnt::ImScsiDispatchUnmap: FSCTL_SET_ZERO_DATA result: 0x%#X\n", status));

            if (!NT_SUCCESS(status))
            {
                goto done;
            }
        }

#if _NT_TARGET_VERSION >= 0x602
        if (!pLUExt->NoFileLevelTrim)
        {
            status = ZwFsControlFile(
                pLUExt->ImageFile,
                NULL,
                NULL,
                NULL,
                &io_status,
                FSCTL_FILE_LEVEL_TRIM,
                fltrim,
                fltrim_size,
                NULL,
                0);

            KdPrint(("PhDskMnt::ImScsiDispatchUnmap: FSCTL_FILE_LEVEL_TRIM result: %#x\n", status));

            if (!NT_SUCCESS(status))
            {
                pLUExt->NoFileLevelTrim = TRUE;
            }
        }
#endif
    }
    else
    {
        status = STATUS_NOT_SUPPORTED;
    }

done:

    KdPrint(("PhDskMnt::ImScsiDispatchUnmap: Result: %#x\n", status));

    ScsiSetSuccess(pSrb, 0);
}

