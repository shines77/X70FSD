
#include "X70FsdData.h"
#include "X70FsdWrite.h"
#include "X70FsdRead.h"
#include "X70FsdCreate.h"
#include "X70FsdCloseCleanup.h"
#include "X70FsdSupport.h"
#include "X70FsdFileInfo.h"
#include "X70FsdInterface.h"

//�����ļ��汾
UCHAR Version[VERSION_INFO] = "1,0,0,1";

UCHAR FileBegin[FILEBEGIN] = "X70FSD";

UCHAR Flag[OVERFLAG] = "Xiao70";

int hash_idx = -1;

symmetric_key *headskey = NULL;

FAST_MUTEX EncryptMutex;

LARGE_INTEGER X70FsdLarge0 = { 0x00000000,0x00000000 };
LARGE_INTEGER X70FsdLarge1 = { 0x00000001,0x00000000 };

LARGE_INTEGER  Li0 = { 0, 0 };
LARGE_INTEGER  Li1 = { 1, 0 };

CACHE_MANAGER_CALLBACKS  G_CacheMgrCallbacks = {
    /* AcquireForLazyWrite */
    X70FsdCMCAcquireForLazyWrite,
    /* ReleaseFromLazyWrite */
    X70FsdCMCReleaseFromLazyWrite,
    /* AcquireForReadAhead */
    X70FsdCMCAcquireForReadAhead,
    /* ReleaseFromReadAhead */
    X70FsdCMCReleaseFromReadAhead
};

NPAGED_LOOKASIDE_LIST  G_IrpContextLookasideList;
NPAGED_LOOKASIDE_LIST  G_IoContextLookasideList;
NPAGED_LOOKASIDE_LIST  G_FcbLookasideList;
NPAGED_LOOKASIDE_LIST  G_EResourceLookasideList;
NPAGED_LOOKASIDE_LIST  G_CcbLookasideList;

PAGED_LOOKASIDE_LIST  G_FcbHashTableLookasideList;

KSPIN_LOCK GeneralSpinLock;

// ��ϣ��
HASH_ENTRY  FcbTable[NUMHASH] = { 0 };
ERESOURCE   FcbTableResource;

USHORT gOsServicePackMajor = 0;
ULONG gOsMajorVersion = 0;
ULONG gOsMinorVersion = 0;

DYNAMIC_FUNCTION_POINTERS gDynamicFunctions = { 0 };

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VOID UnloadDriver()
{
    ExDeleteNPagedLookasideList(&G_IoContextLookasideList);
    ExDeleteNPagedLookasideList(&G_IrpContextLookasideList);
    ExDeleteNPagedLookasideList(&G_FcbLookasideList);
    ExDeleteNPagedLookasideList(&G_CcbLookasideList);
    ExDeleteNPagedLookasideList(&G_EResourceLookasideList);

    ExDeletePagedLookasideList(&G_FcbHashTableLookasideList);

    ExDeleteResourceLite(&FcbTableResource);

    unregister_hash(&md5_desc);

    if (headskey != NULL) { ExFreePool(headskey); }
    if (Servkey.Buffer != NULL) { ExFreePool(Servkey.Buffer); }
}

NTSTATUS InitDriverEntry(__in PDRIVER_OBJECT DriverObject,
    __in PUNICODE_STRING RegistryPath)
{
    ULONG i;
    NTSTATUS Status;
    try {
        // ���ϵͳ�汾
        GetCurrentVersion();

        if (IS_WINDOWS2000()) {
            try_return(Status = STATUS_UNSUCCESSFUL);
        }

        if (register_hash(&md5_desc) == -1) {
            DbgPrint("Error registering MD5.\n");
            try_return(Status = STATUS_UNSUCCESSFUL);
        }

        hash_idx = find_hash_id(3);

        if (hash_idx == -1) {
            DbgPrint("Error find_hash_id MD5.\n");
            try_return(Status = STATUS_UNSUCCESSFUL);
        }

        headskey = FsRtlAllocatePoolWithTag(NonPagedPool, sizeof(symmetric_key), 'skey');
        RtlZeroMemory(headskey, sizeof(symmetric_key));

        if (aes_setup(TestKey, sizeof(TestKey), 0, headskey) != CRYPT_OK) {
            ExFreePool(headskey);
            return STATUS_UNSUCCESSFUL;
        }

        ExInitializeNPagedLookasideList(&G_IoContextLookasideList, NULL, NULL, 0, sizeof(LAYERFSD_IO_CONTEXT), 'IoC', 0);
        ExInitializeNPagedLookasideList(&G_IrpContextLookasideList, NULL, NULL, 0, sizeof(IRP_CONTEXT), 'TrC', 0);
        ExInitializeNPagedLookasideList(&G_FcbLookasideList, NULL, NULL, 0, sizeof(FCB), 'FCB', 0);
        ExInitializeNPagedLookasideList(&G_CcbLookasideList, NULL, NULL, 0, sizeof(CCB), 'CCB', 0);
        ExInitializeNPagedLookasideList(&G_EResourceLookasideList, NULL, NULL, 0, sizeof(ERESOURCE), 'Res', 0);

        ExInitializePagedLookasideList(&G_FcbHashTableLookasideList, NULL, NULL, 0, sizeof(HASH_ENTRY), 'FHT', 0);

        // ��ʼ����
        KeInitializeSpinLock(&GeneralSpinLock);
        ExInitializeFastMutex(&EncryptMutex);

        ExInitializeResourceLite(&FcbTableResource);

        // ��ʼ��FcbTable
        for (i = 0; i < NUMHASH; i++) {
            // ��ʼ��Ϊh0
            InitializeListHead(&FcbTable[i].ListEntry);
        }

        GetFltRoutineAddress();
try_exit:
        NOTHING;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // �����쳣ֱ�ӷ���
        Status = GetExceptionCode();
    }
    return Status;
}

PERESOURCE
X70FsdAllocateResource(
    )
{
    PERESOURCE Resource = NULL;
    Resource = (PERESOURCE)ExAllocateFromNPagedLookasideList(&G_EResourceLookasideList);

    ExInitializeResourceLite(Resource);
    return Resource;
}

// VACB???
BOOLEAN InSameVACB(IN ULONGLONG LowAddress, IN ULONGLONG HighAddress)
{
    return ((LowAddress >> VACB_OFFSET_SHIFT) == (HighAddress >> VACB_OFFSET_SHIFT));
}

VOID GetFltRoutineAddress()
{
    UNICODE_STRING RoutineString = { 0 };
    gDynamicFunctions.CheckOplockEx = FltGetRoutineAddress("FltCheckOplockEx");
    gDynamicFunctions.OplockBreakH = FltGetRoutineAddress("FltOplockBreakH");

    RtlInitUnicodeString(&RoutineString, L"MmDoesFileHaveUserWritableReferences");
    gDynamicFunctions.pMmDoesFileHaveUserWritableReferences = MmGetSystemRoutineAddress(&RoutineString);

    RtlInitUnicodeString(&RoutineString, L"FsRtlChangeBackingFileObject");
    gDynamicFunctions.pFsRtlChangeBackingFileObject = MmGetSystemRoutineAddress(&RoutineString);
}

BOOLEAN
X70FsdIsIrpTopLevel(
    IN PFLT_CALLBACK_DATA Data
    )
{
    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)Data);
        return TRUE;
    }
    else {
        return FALSE;
    }
}

NTSTATUS X70FsdSyncMoreProcessingCompRoutine(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PIRP  Irp,
    IN PVOID  Context
    )
{
    PKEVENT event = (PKEVENT)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (event) {
        KeSetEvent(event, 0, FALSE);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
IoCompletionRoutine(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PIRP  Irp,
    IN PVOID  Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);

    *Irp->UserIosb = Irp->IoStatus;

    if (Irp->UserEvent)
        KeSetEvent(Irp->UserEvent, IO_NO_INCREMENT, 0);

    if (Irp->MdlAddress) {
        IoFreeMdl(Irp->MdlAddress);
        Irp->MdlAddress = NULL;
    }

    IoFreeIrp(Irp);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

PFCB X70FsdCreateFcb()
{
    PFCB Fcb = NULL;

    // ����һ��FCB
    Fcb = (PFCB)ExAllocateFromNPagedLookasideList(&G_FcbLookasideList);

    if (Fcb != NULL) {
        RtlZeroMemory(Fcb, sizeof(FCB));
    }
    return Fcb;
}

PCCB X70FsdCreateCcb()
{
    PCCB Ccb = NULL;

    // ����һ��FCB
    Ccb = (PCCB)ExAllocateFromNPagedLookasideList(&G_CcbLookasideList);

    if (Ccb != NULL) {
        RtlZeroMemory(Ccb, sizeof(CCB));
        Ccb->FileAccess = FILE_PASS_ACCESS;
    }
    return Ccb;
}

// ��FCB����������������һ��FcbTable ����������Ϣ
BOOLEAN InsertFcbList(PUCHAR HashValue, PFCB *Fcb)
{
    ULONG j = 0;

    PHASH_ENTRY pHashEntry = NULL;

    BOOLEAN AcquireResource = FALSE;
    try {
        j = (*HashValue) % NUMHASH;

        // �����ڴ�
        pHashEntry = (PHASH_ENTRY)ExAllocateFromPagedLookasideList(&G_FcbHashTableLookasideList); //����һ��FCB

        if (pHashEntry == NULL) {
            return FALSE;
        }

        RtlZeroMemory(pHashEntry, sizeof(HASH_ENTRY));
        // ��������
        pHashEntry->Fcb = *Fcb;

        RtlCopyMemory(pHashEntry->HashValue, HashValue, MD5_LENGTH);

        pHashEntry->Type = HASHTABLE_TYPE_FCB;

        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&FcbTableResource, TRUE);
        AcquireResource = TRUE;

        InsertTailList(&FcbTable[j].ListEntry, &pHashEntry->ListEntry);
    }
    finally {
        if (AcquireResource) {
            ExReleaseResourceLite(&FcbTableResource);
            KeLeaveCriticalRegion();
        }
    }
    return TRUE;;
}

BOOLEAN RemoveFcbList(PUCHAR HashValue, PFCB *Fcb)
{
    ULONG j = 0;

    KIRQL OldIrql;

    PLIST_ENTRY pListEntry;
    BOOLEAN AcquireResource = FALSE;
    BOOLEAN ret = FALSE;
    try {
        j = (*HashValue) % NUMHASH;

        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&FcbTableResource, TRUE);
        AcquireResource = TRUE;

        // �յı�
        if (IsListEmpty(&FcbTable[j].ListEntry)) {
            ret = TRUE;
        }
        else {
            // ���ǿձ���������в���

            //�������� ���ҵ��˽�����ӷ�����һ���µ����ݵ�������
            for (pListEntry = FcbTable[j].ListEntry.Flink;
                pListEntry != &FcbTable[j].ListEntry;
                pListEntry = pListEntry->Flink) {
                PHASH_ENTRY hashEntry = CONTAINING_RECORD(pListEntry, HASH_ENTRY, ListEntry);

                if (RtlCompareMemory(hashEntry->HashValue, HashValue, MD5_LENGTH) == MD5_LENGTH) {
                    // ɾ�������
                    RemoveEntryList(&hashEntry->ListEntry);

                    ExFreeToPagedLookasideList(&G_FcbHashTableLookasideList, hashEntry);
                    ret = TRUE;
                    break;
                }
            }
        }
    }
    finally {
        if (AcquireResource) {
            ExReleaseResourceLite(&FcbTableResource);
            KeLeaveCriticalRegion();
        }
    }
    return ret;
}

//��Hash����в���
BOOLEAN FindExistFcb(PUCHAR HashValue, PFCB * pFcb)
{
    ULONG j = 0;

    KIRQL OldIrql;

    PLIST_ENTRY pListEntry;
    BOOLEAN AcquireResource = FALSE;
    BOOLEAN ret = FALSE;
    try {
        j = (*HashValue) % NUMHASH;

        KeEnterCriticalRegion();
        ExAcquireResourceSharedLite(&FcbTableResource, TRUE);
        AcquireResource = TRUE;

        // �յı�
        if (IsListEmpty(&FcbTable[j].ListEntry)) {
            ret = FALSE;
        }
        else {
            // ���ǿձ���������в���

            // �������� ���ҵ��˽�����ӷ�����һ���µ����ݵ�������
            for (pListEntry = FcbTable[j].ListEntry.Flink;
                 pListEntry != &FcbTable[j].ListEntry;
                 pListEntry = pListEntry->Flink) {
                PHASH_ENTRY hashEntry = CONTAINING_RECORD(pListEntry, HASH_ENTRY, ListEntry);

                if (RtlCompareMemory(hashEntry->HashValue, HashValue, MD5_LENGTH) == MD5_LENGTH) {
                    *pFcb = hashEntry->Fcb;
                    ret = TRUE;
                    break;
                }
            }
        }
    }
    finally {
        if (AcquireResource) {
            ExReleaseResourceLite(&FcbTableResource);
            KeLeaveCriticalRegion();
        }
    }
    return ret;
}

BOOLEAN UpdateHashValue(PUCHAR OldHashValue, PUCHAR NewHashValue, PFCB * pFcb)
{
    ULONG j = 0, i = 0;

    PLIST_ENTRY pListEntry;
    PHASH_ENTRY pNewHashEntry = NULL;
    BOOLEAN AcquireResource = FALSE;
    BOOLEAN ret = FALSE;
    try
    {
        j = (*OldHashValue) % NUMHASH;
        i = (*NewHashValue) % NUMHASH;

        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&FcbTableResource, TRUE);
        AcquireResource = TRUE;

        // �յı�
        if (IsListEmpty(&FcbTable[j].ListEntry)) {
            ret = FALSE;
        }
        else {
            // ���ǿձ���������в���

            // �������� ���ҵ��˽�����ӷ�����һ���µ����ݵ�������
            for (pListEntry = FcbTable[j].ListEntry.Flink;
                 pListEntry != &FcbTable[j].ListEntry;
                 pListEntry = pListEntry->Flink) {
                PHASH_ENTRY hashEntry = CONTAINING_RECORD(pListEntry, HASH_ENTRY, ListEntry);

                // ����hash
                if (RtlCompareMemory(hashEntry->HashValue, OldHashValue, MD5_LENGTH) == MD5_LENGTH) {
                    // ɾ�������
                    RemoveEntryList(&hashEntry->ListEntry);
                    ExFreeToPagedLookasideList(&G_FcbHashTableLookasideList, hashEntry);

                    // �����µ�

                    // �����ڴ�, ����һ��FCB
                    pNewHashEntry = (PHASH_ENTRY)ExAllocateFromPagedLookasideList(&G_FcbHashTableLookasideList);

                    RtlZeroMemory(pNewHashEntry, sizeof(HASH_ENTRY));
                    RtlCopyMemory((*pFcb)->HashValue, NewHashValue, MD5_LENGTH);

                    // ��������
                    pNewHashEntry->Fcb = *pFcb;
                    RtlCopyMemory(pNewHashEntry->HashValue, NewHashValue, MD5_LENGTH);
                    pNewHashEntry->Type = HASHTABLE_TYPE_FCB;

                    InsertTailList(&FcbTable[i].ListEntry, &pNewHashEntry->ListEntry);

                    ret = TRUE;
                    break;
                }
            }
        }
    }
    finally {
        if (AcquireResource) {
            ExReleaseResourceLite(&FcbTableResource);
            KeLeaveCriticalRegion();
        }
    }
    return ret;
}

NTSTATUS X70FsdCloseGetFileBasicInfo(PFILE_OBJECT FileObject, PIRP_CONTEXT IrpContext, PFILE_BASIC_INFORMATION fbi)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFLT_CALLBACK_DATA RetNewCallbackData = NULL;

    try {
        Status = FltAllocateCallbackData(IrpContext->FltObjects.Instance, FileObject, &RetNewCallbackData);

        if (NT_SUCCESS(Status)) {
            RetNewCallbackData->Iopb->MajorFunction = IRP_MJ_QUERY_INFORMATION;
            RetNewCallbackData->Iopb->MinorFunction = 0;
            RetNewCallbackData->Iopb->Parameters.QueryFileInformation.FileInformationClass = FileBasicInformation;
            RetNewCallbackData->Iopb->Parameters.QueryFileInformation.Length = sizeof(FILE_BASIC_INFORMATION);
            RetNewCallbackData->Iopb->Parameters.QueryFileInformation.InfoBuffer = fbi;
            RetNewCallbackData->Iopb->TargetFileObject = FileObject;

            FltPerformSynchronousIo(RetNewCallbackData);
            Status = RetNewCallbackData->IoStatus.Status;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
        DbgPrint("X70FsdCloseGetFileBasicInfo Exception %x \n", Status);
    }
    if (!NT_SUCCESS(Status)) {
        DbgPrint("X70FsdCloseGetFileBasicInfo False %x \n", Status);
    }
    if (RetNewCallbackData != NULL) {
        FltFreeCallbackData(RetNewCallbackData);
    }

    return Status;
}

NTSTATUS X70FsdCloseSetFileBasicInfo(PFILE_OBJECT FileObject, PIRP_CONTEXT IrpContext, PFILE_BASIC_INFORMATION fbi)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFLT_CALLBACK_DATA RetNewCallbackData = NULL;

    try {
        Status = FltAllocateCallbackData(IrpContext->FltObjects.Instance, FileObject, &RetNewCallbackData);

        if (NT_SUCCESS(Status)) {
            RetNewCallbackData->Iopb->MajorFunction = IRP_MJ_SET_INFORMATION;
            RetNewCallbackData->Iopb->MinorFunction = 0;
            RetNewCallbackData->Iopb->Parameters.SetFileInformation.FileInformationClass = FileBasicInformation;
            RetNewCallbackData->Iopb->Parameters.SetFileInformation.Length = sizeof(FILE_BASIC_INFORMATION);
            RetNewCallbackData->Iopb->Parameters.SetFileInformation.InfoBuffer = fbi;
            RetNewCallbackData->Iopb->Parameters.SetFileInformation.AdvanceOnly = FALSE;
            RetNewCallbackData->Iopb->Parameters.SetFileInformation.ParentOfTarget = NULL;
            RetNewCallbackData->Iopb->TargetFileObject = FileObject;

            FltPerformSynchronousIo(RetNewCallbackData);

            Status = RetNewCallbackData->IoStatus.Status;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
        DbgPrint("X70FsdCloseSetFileBasicInfo Exception %x \n", Status);
    }
    if (!NT_SUCCESS(Status)) {
        DbgPrint("X70FsdCloseSetFileBasicInfo False %x \n", Status);
    }
    if (RetNewCallbackData != NULL) {
        FltFreeCallbackData(RetNewCallbackData);
    }

    return Status;
}

//�ͷ�FCB
BOOLEAN X70FsdFreeFcb(PFCB Fcb, PIRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_SUCCESS;

    RemoveFcbList(Fcb->HashValue, NULL);

    if (Fcb->CcFileObject != NULL) {
        if (!BooleanFlagOn(Fcb->FcbState, SCB_STATE_DELETE_ON_CLOSE)) {
            if (BooleanFlagOn(Fcb->FcbState, SCB_STATE_FILE_CHANGED)) {

                FILE_BASIC_INFORMATION fbi = { 0 };
                BOOLEAN bSetBasicInfo = FALSE;

                Status = X70FsdCloseGetFileBasicInfo(Fcb->CcFileObject, IrpContext, &fbi);

                if (NT_SUCCESS(Status)) {
                    bSetBasicInfo = TRUE;
                }

                Status = FltClose(Fcb->CcFileHandle);

                if (bSetBasicInfo) {
                    Status = X70FsdCloseSetFileBasicInfo(Fcb->CcFileObject, IrpContext, &fbi);
                }
            }
            else {
                Status = FltClose(Fcb->CcFileHandle);
            }
        }
        ObDereferenceObject(Fcb->CcFileObject);
        Fcb->CcFileObject = NULL;
    }

    if (Fcb->Header.PagingIoResource != NULL) {
        ExDeleteResourceLite(Fcb->Header.PagingIoResource);
        ExFreeToNPagedLookasideList(&G_EResourceLookasideList, Fcb->Header.PagingIoResource);
        Fcb->Header.PagingIoResource = NULL;
    }
    if (Fcb->Header.Resource != NULL) {
        ExDeleteResourceLite(Fcb->Header.Resource);
        ExFreeToNPagedLookasideList(&G_EResourceLookasideList, Fcb->Header.Resource);
        Fcb->Header.Resource = NULL;
    }
    if (Fcb->EncryptResource != NULL) {
        ExDeleteResourceLite(Fcb->EncryptResource);
        ExFreeToNPagedLookasideList(&G_EResourceLookasideList, Fcb->EncryptResource);
        Fcb->EncryptResource = NULL;
    }
    if (Fcb->OutstandingAsyncEvent != NULL) {
        ExFreePool(Fcb->OutstandingAsyncEvent);
        Fcb->OutstandingAsyncEvent = NULL;
    }
    if (Fcb->FileFullName.Buffer != NULL) {
        ExFreePool(Fcb->FileFullName.Buffer);
        Fcb->FileFullName.Buffer = NULL;
    }
    if (FlagOn(Fcb->Header.Flags, FSRTL_FLAG_ADVANCED_HEADER)) {
        FsRtlTeardownPerStreamContexts(&Fcb->Header);
    }

    FltUninitializeOplock(&Fcb->Oplock);

    if (IS_FLT_FILE_LOCK()) {
        FltUninitializeFileLock(Fcb->FileLock);
    }
    else {
        FsRtlUninitializeFileLock(Fcb->FileLock);
    }

    ExFreeToNPagedLookasideList(&G_FcbLookasideList, Fcb);
    return TRUE;
}

VOID NetFileSetCacheProperty(PFILE_OBJECT FileObject, ACCESS_MASK DesiredAccess)
{
    PFCB Fcb = FileObject->FsContext;
    PCCB Ccb = FileObject->FsContext2;

    CREATE_ACCESS_TYPE CreateAccess = CREATE_ACCESS_INVALID;

    if (Fcb->CacheType == CACHE_DISABLE) {
        return;
    }

    if (FlagOn(DesiredAccess, FILE_READ_DATA) &&
        !FlagOn(DesiredAccess, FILE_WRITE_DATA) || (FlagOn(DesiredAccess, FILE_APPEND_DATA))) {
        CreateAccess = CREATE_ACCESS_READ;
        if (CcIsFileCached(FileObject) && Fcb->CacheType == CACHE_READWRITE) {
            Fcb->CacheType = CACHE_READ;
        }
    }
    else if (!FlagOn(DesiredAccess, FILE_READ_DATA) &&
        FlagOn(DesiredAccess, FILE_WRITE_DATA) || (FlagOn(DesiredAccess, FILE_APPEND_DATA))) {
        CreateAccess = CREATE_ACCESS_WRITE;
        Fcb->CacheType = CACHE_READ;
    }
    else if (FlagOn(DesiredAccess, FILE_READ_DATA) &&
        FlagOn(DesiredAccess, FILE_WRITE_DATA) || (FlagOn(DesiredAccess, FILE_APPEND_DATA))) {
        CreateAccess = CREATE_ACCESS_READWRITE;
    }

    return;
}

NTSTATUS CreateFcbAndCcb(__inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PIRP_CONTEXT IrpContext,
    __in PUCHAR HashValue)
{
    PFCB Fcb = NULL;
    PCCB Ccb = NULL;
    NTSTATUS status;
    HANDLE FileHandle;
    OBJECT_ATTRIBUTES   ob;
    IO_STATUS_BLOCK IoStatus;
    PFILE_OBJECT FileObject, SwapFileObject;

    BOOLEAN AdvancedHeader = FALSE;

    try {
        FileObject = FltObjects->FileObject;

        Fcb = X70FsdCreateFcb();

        if (Fcb == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Ccb = X70FsdCreateCcb();

        if (Ccb == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Fcb->Header.NodeTypeCode = LAYER_NTC_FCB;
        Fcb->Header.NodeByteSize = sizeof(FCB);

        Fcb->Header.PagingIoResource = X70FsdAllocateResource();
        Fcb->Header.Resource = X70FsdAllocateResource();
        Fcb->EncryptResource = X70FsdAllocateResource();

        KeInitializeEvent(&Fcb->UninitializeCompleteEvent.Event, NotificationEvent, TRUE);

        if (Fcb->Header.PagingIoResource == NULL || Fcb->Header.Resource == NULL || Fcb->EncryptResource == NULL) {
            try_return(status = STATUS_INSUFFICIENT_RESOURCES);
        }

        ExInitializeFastMutex(&Fcb->AdvancedFcbHeaderMutex);
        FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->AdvancedFcbHeaderMutex);
        AdvancedHeader = TRUE;

        if (IrpContext->CreateInfo.DecrementHeader) {
            IrpContext->CreateInfo.FileSize.QuadPart -= FILE_HEADER_LENGTH;
            IrpContext->CreateInfo.FileAllocationSize.QuadPart -= FILE_HEADER_LENGTH;
        }

        Fcb->Header.FileSize.QuadPart = IrpContext->CreateInfo.FileSize.QuadPart;
        Fcb->Header.ValidDataLength.QuadPart = IrpContext->CreateInfo.FileSize.QuadPart;

        if (IrpContext->CreateInfo.FileSize.QuadPart > IrpContext->CreateInfo.FileAllocationSize.QuadPart) {
            // �ش�С
            ULONG ClusterSize = IrpContext->SectorSize * IrpContext->SectorsPerAllocationUnit;

            LARGE_INTEGER TempLI;

            TempLI.QuadPart = Fcb->Header.FileSize.QuadPart;
            TempLI.QuadPart += ClusterSize;
            TempLI.HighPart += (ULONG)((LONGLONG)ClusterSize >> 32);

            if (TempLI.LowPart == 0) {
                TempLI.HighPart -= 1;
            }

            Fcb->Header.AllocationSize.LowPart = ((ULONG)Fcb->Header.FileSize.LowPart + (ClusterSize - 1)) & (~(ClusterSize - 1));
            Fcb->Header.AllocationSize.HighPart = TempLI.HighPart;
        }
        else {
            Fcb->Header.AllocationSize.QuadPart = IrpContext->CreateInfo.FileAllocationSize.QuadPart;
        }

        if (IrpContext->CreateInfo.RealSize) {
            if (IrpContext->CreateInfo.RealFileSize.QuadPart > Fcb->Header.AllocationSize.QuadPart) {
                IrpContext->CreateInfo.RealFileSize.QuadPart = IrpContext->CreateInfo.FileSize.QuadPart;
            }
            else {
                Fcb->Header.FileSize.QuadPart = IrpContext->CreateInfo.RealFileSize.QuadPart;
                Fcb->Header.ValidDataLength.QuadPart = IrpContext->CreateInfo.RealFileSize.QuadPart;
                Fcb->ValidDataToDisk.QuadPart = IrpContext->CreateInfo.FileSize.QuadPart;
            }
        }

        FltInitializeOplock(&Fcb->Oplock);
        Fcb->Header.IsFastIoPossible = FastIoIsQuestionable;

        if (IrpContext->CreateInfo.IsWriteHeader) {
            SetFlag(Fcb->FcbState, SCB_STATE_FILEHEADER_WRITED);
        }

        if (IrpContext->CreateInfo.Network) {
            // ???
            //SetFlag(Fcb->FcbState,SCB_STATE_DISABLE_LOCAL_BUFFERING);
            //Fcb->Header.IsFastIoPossible = FastIoIsQuestionable;
        }

        Fcb->IsEnFile = IrpContext->CreateInfo.IsEnFile;

        if (Fcb->IsEnFile && FlagOn(Fcb->FcbState, SCB_STATE_FILEHEADER_WRITED)) {
            Fcb->FileHeaderLength = FILE_HEADER_LENGTH;
        }

        if (IS_FLT_FILE_LOCK()) {
            Fcb->FileLock = FltAllocateFileLock(NULL, NULL);
        }
        else {
            Fcb->FileLock = FsRtlAllocateFileLock(NULL, NULL);
        }

        Fcb->CacheType = CACHE_ALLOW;

        Fcb->FileType = IrpContext->CreateInfo.ProcType;

        Fcb->FileFullName.Length = IrpContext->CreateInfo.nameInfo->Name.Length;
        Fcb->FileFullName.MaximumLength = IrpContext->CreateInfo.nameInfo->Name.MaximumLength;
        Fcb->FileFullName.Buffer = FsRtlAllocatePoolWithTag(NonPagedPool, Fcb->FileFullName.MaximumLength, 'ffn');
        RtlCopyMemory(Fcb->FileFullName.Buffer, IrpContext->CreateInfo.nameInfo->Name.Buffer, Fcb->FileFullName.MaximumLength);

        aes_ecb_encrypt(Fcb->FileKey, IrpContext->CreateInfo.FileKey, headskey);

        if (aes_setup(Fcb->FileKey, sizeof(Fcb->FileKey), 0, &Fcb->CryptionKey) != CRYPT_OK) {
            try_return(status = STATUS_UNSUCCESSFUL);
        }

        if (!IrpContext->CreateInfo.Network) {
            ULONG Options = FILE_NON_DIRECTORY_FILE;

#ifdef USE_CACHE_READWRITE
            SetFlag(Options, FILE_WRITE_THROUGH); //����ֱ��д���ļ���
#endif
            InitializeObjectAttributes(&ob, &Fcb->FileFullName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

            status = FltCreateFile(FltObjects->Filter,
                FltObjects->Instance,
                &Fcb->CcFileHandle,
                FILE_SPECIAL_ACCESS,
                &ob,
                &IoStatus,
                NULL,
                FILE_ATTRIBUTE_NORMAL,
                0,
                FILE_OPEN,
                Options,
                NULL,
                0,
                0
                );

            if (!NT_SUCCESS(status)) {
                try_return(status);
            }

            status = ObReferenceObjectByHandle(Fcb->CcFileHandle, 0, *IoFileObjectType, KernelMode, &Fcb->CcFileObject, NULL);

            if (!NT_SUCCESS(status)) {
                FltClose(Fcb->CcFileHandle);
                try_return(status);
            }
        }
        else {
            Fcb->CcFileObject = NULL;
        }

        if (InsertFcbList(HashValue, &Fcb)) {
            RtlCopyMemory(Fcb->HashValue, HashValue, MD5_LENGTH);

            Ccb->StreamFileInfo.StreamHandle = IrpContext->CreateInfo.StreamHandle;
            Ccb->StreamFileInfo.StreamObject = IrpContext->CreateInfo.StreamObject;
            Ccb->StreamFileInfo.FO_Resource = X70FsdAllocateResource();

            if (IrpContext->CreateInfo.Network) {
                SetFlag(Ccb->CcbState, CCB_FLAG_NETWORK_FILE);
            }

            Ccb->FileAccess = IrpContext->CreateInfo.FileAccess;
            RtlCopyMemory(Ccb->ProcessGuid, IrpContext->CreateInfo.ProcessGuid, GUID_SIZE);

            ExInitializeFastMutex(&Ccb->StreamFileInfo.FileObjectMutex);
        }
        else {
            try_return(status = STATUS_INSUFFICIENT_RESOURCES);
        }

        IrpContext->CreateInfo.Fcb = Fcb;
        IrpContext->CreateInfo.Ccb = Ccb;

        try_return(status = STATUS_SUCCESS);

try_exit:
        NOTHING;
    }
    finally {
        if (AbnormalTermination()) {
            status = STATUS_UNSUCCESSFUL;
        }
        if (!NT_SUCCESS(status)) {

            if (Fcb != NULL) {
                X70FsdFreeFcb(Fcb, IrpContext);
                FileObject->FsContext = NULL;
            }
            if (Ccb != NULL) {
                if (Ccb->StreamFileInfo.FO_Resource != NULL) {
                    ExDeleteResourceLite(Ccb->StreamFileInfo.FO_Resource);
                    ExFreeToNPagedLookasideList(&G_EResourceLookasideList, Ccb->StreamFileInfo.FO_Resource);
                    Ccb->StreamFileInfo.FO_Resource = NULL;
                }
                ExFreeToNPagedLookasideList(&G_CcbLookasideList, Ccb);
                FileObject->FsContext2 = NULL;
            }
        }
    }
    return status;
}

// ����ļ���Ϣ
NTSTATUS
MyGetFileStandardInfo(
    __in  PFLT_CALLBACK_DATA Data,
    __in  PCFLT_RELATED_OBJECTS FltObjects,
    __in    PFILE_OBJECT FileObject,
    __in  PLARGE_INTEGER FileAllocationSize,
    __in  PLARGE_INTEGER FileSize,
    __in  PBOOLEAN bDirectory
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    FILE_STANDARD_INFORMATION sFileStandardInfo;

    // ���²�Call
    status = FltQueryInformationFile(FltObjects->Instance,
        FileObject,
        &sFileStandardInfo,
        sizeof(FILE_STANDARD_INFORMATION),
        FileStandardInformation,
        NULL
        );
    if (NT_SUCCESS(status)) {
        if (NULL != FileSize)
            *FileSize = sFileStandardInfo.EndOfFile;
        if (NULL != FileAllocationSize)
            *FileAllocationSize = sFileStandardInfo.AllocationSize;
        if (NULL != bDirectory)
            *bDirectory = sFileStandardInfo.Directory;
    }
    return status;
}

BOOLEAN HashFilePath(PUNICODE_STRING pFileFullPath, PUCHAR HashValue)
{
    ULONG len = MD5_LENGTH;
    BOOLEAN Ret = FALSE;
    try {
        if ((hash_memory(hash_idx, (PUCHAR)pFileFullPath->Buffer, pFileFullPath->Length, HashValue, &len)) != CRYPT_OK) {
            Ret = FALSE;
        }
        else {
            Ret = TRUE;
        }
    }
    except(EXCEPTION_EXECUTE_HANDLER) {
        Ret = FALSE;
    }
    return Ret;
}

NTSTATUS ModifyFileHeader(PCFLT_RELATED_OBJECTS FltObjects, PFILE_OBJECT FileObject, PLARGE_INTEGER pRealFileSize,
                          PUCHAR ProcessGuid, PUNICODE_STRING pFileFullName, MODIFY_TYPE ModType)
{
    NTSTATUS Status = STATUS_SUCCESS;
    LARGE_INTEGER ByteOffset;

    ByteOffset.QuadPart = 0;

    switch (ModType) {
        case FILE_MODIFY_SIZE:
            {
                LARGE_INTEGER RealFileSize;

                RtlCopyMemory(&RealFileSize, pRealFileSize, sizeof(LARGE_INTEGER));
                ByteOffset.QuadPart = FIELD_OFFSET(FILE_HEADER_CRYPTION, RealFileSize);

                Status = FltWriteFile(
                    FltObjects->Instance,
                    FileObject,
                    &ByteOffset,
                    sizeof(LONGLONG),
                    &RealFileSize.QuadPart,
                    /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                    NULL,
                    NULL,
                    NULL
                    );
            }
            break;
        default:
            break;
    }

    return Status;
}

NTSTATUS CreatedFileWriteHeader(PIRP_CONTEXT IrpContext)
{
    PFILE_HEADER_CRYPTION pFileHeader = IrpContext->CreateInfo.pFileHeader;
    LARGE_INTEGER ByteOffset;
    NTSTATUS Status;

    ByteOffset.QuadPart = 0;

    if (!IrpContext->CreateInfo.IsEnFile) {
        // ��Ҫ���´����ļ�ͷ��Ϣ��
        RtlCopyMemory(pFileHeader->FileBegin, FileBegin, sizeof(FileBegin));
        RtlCopyMemory(pFileHeader->Flag, Flag, sizeof(Flag));
        RtlCopyMemory(pFileHeader->RealFileSize, &IrpContext->CreateInfo.FileSize, sizeof(LARGE_INTEGER));
    }
    Status = FltWriteFile(
        IrpContext->FltObjects.Instance,
        IrpContext->CreateInfo.StreamObject,
        &ByteOffset,
        FILE_HEADER_LENGTH,
        pFileHeader,
        FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, // �ǻ���Ĵ�
        NULL,
        NULL,
        NULL
        );
    return Status;
}

// ���ļ�д�����ͷ
NTSTATUS WriteFileHeader(PCFLT_RELATED_OBJECTS FltObjects, PFILE_OBJECT FileObject,
                         PLARGE_INTEGER RealFileSize, PUCHAR ProcessGuid, PUNICODE_STRING pFileFullName)
{
    NTSTATUS Status;
    PFILE_HEADER_CRYPTION pFileHeader = NULL;
    LARGE_INTEGER ByteOffset;
    ULONG i, len = CHKSUM_SIZE;

    ByteOffset.QuadPart = 0;

    try {
        pFileHeader = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, FILE_HEADER_LENGTH, 'wh');  //�ļ�ͷ�Ѿ���������С�����ˣ����Կ϶��򿪿��Գɹ�

        if (pFileHeader == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(pFileHeader, FILE_HEADER_LENGTH);
        RtlCopyMemory(pFileHeader->FileBegin, FileBegin, sizeof(FileBegin));
        RtlCopyMemory(pFileHeader->Flag, Flag, sizeof(Flag));
        RtlCopyMemory(pFileHeader->VersionInfo, Version, sizeof(Version));

        if (RealFileSize != NULL) {
            RtlCopyMemory(pFileHeader->RealFileSize, RealFileSize, sizeof(LARGE_INTEGER));
        }

        // д����ϣ���Լ��������Ϣ
        // .......
        // ����LicensingInfo��MD5

        if ((hash_memory(hash_idx, pFileHeader->LicensingInfo, LICENSING_SIZE,
                         pFileHeader->LicensingChkSum, &len)) != CRYPT_OK) {
            try_return(Status = STATUS_UNSUCCESSFUL);
        }

        // д�����ļ�
        Status = FltWriteFile(
            FltObjects->Instance,
            FileObject,
            &ByteOffset,
            FILE_HEADER_LENGTH,
            pFileHeader,
            /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, // �ǻ���Ĵ�
            NULL,
            NULL,
            NULL
            );

try_exit:
        NOTHING;
    }
    finally {
        if (pFileHeader != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, pFileHeader, 'wh');
        }
        if (AbnormalTermination()) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    return Status;
}

NTSTATUS CreatedFileHeaderInfo(PIRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject;
    PFILE_HEADER_CRYPTION pFileHeader = NULL;
    BOOLEAN IsEnFile;
    LARGE_INTEGER ByteOffset;
    ByteOffset.QuadPart = 0;

    FileObject = IrpContext->CreateInfo.StreamObject;

    IrpContext->CreateInfo.pFileHeader = FltAllocatePoolAlignedWithTag(IrpContext->FltObjects.Instance, PagedPool, FILE_HEADER_LENGTH, 'rh');

    if (IrpContext->CreateInfo.pFileHeader == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (IrpContext->CreateInfo.FileSize.QuadPart >= FILE_HEADER_LENGTH) {
        RtlZeroMemory(IrpContext->CreateInfo.pFileHeader, FILE_HEADER_LENGTH);

        //��ȡ�����ļ�
        Status = FltReadFile(
            IrpContext->FltObjects.Instance,
            FileObject,
            &ByteOffset,
            FILE_HEADER_LENGTH,
            IrpContext->CreateInfo.pFileHeader,
            FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
            NULL,
            NULL,
            NULL
            );

        if (NT_SUCCESS(Status)) {
            if ((RtlCompareMemory(IrpContext->CreateInfo.pFileHeader->FileBegin, FileBegin, sizeof(FileBegin)) == sizeof(FileBegin))
                && (RtlCompareMemory(IrpContext->CreateInfo.pFileHeader->Flag, Flag, sizeof(Flag)) == sizeof(Flag))) {
                IrpContext->CreateInfo.IsEnFile = TRUE;
                IrpContext->CreateInfo.DecrementHeader = TRUE;
                IrpContext->CreateInfo.IsWriteHeader = TRUE;
            }
        }
    }

    if (NT_SUCCESS(Status)) {
        // ֪ͨӦ�ò�ȥ�жϣ�����Ҳ�������Զ��ж���
        if (R3FileAccessNotify(IrpContext)) {
            Status = STATUS_SUCCESS;
        }
        else {
            Status = STATUS_UNSUCCESSFUL;
        }
    }

    if (IrpContext->CreateInfo.pFileHeader != NULL) {
        FltFreePoolAlignedWithTag(IrpContext->FltObjects.Instance, IrpContext->CreateInfo.pFileHeader, 'rh');
    }
    return Status;
}

// ��һ���ļ���
NTSTATUS GetFileStreamRealSize(PIRP_CONTEXT IrpContext,
    PCFLT_RELATED_OBJECTS FltObjects,
    PWCHAR StreamName,
    ULONG StreamNameLength,
    PBOOLEAN IsEnFile)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ob;
    HANDLE StreamHandle;
    IO_STATUS_BLOCK IoStatus;
    UNICODE_STRING StreamString = { 0 };
    PFILE_OBJECT StreamObject = NULL;

    PFILE_HEADER_CRYPTION pFileHeader = NULL;
    LARGE_INTEGER ByteOffset;

    UNICODE_STRING VolumeName = { 0 };
    PFILE_NAME_INFORMATION fni = NULL;
    ULONG LengthReturned = 0;
    ULONG Length = MAX_PATH;

    PFCB Fcb;
    PCCB Ccb;
    PFILE_OBJECT FileObject = FltObjects->FileObject;
    ByteOffset.QuadPart = 0;

    Fcb = (PFCB)FileObject->FsContext;
    Ccb = FileObject->FsContext2;
    try {
        // ��������ȫ��
        Status = FltGetVolumeName(FltObjects->Volume, NULL, &LengthReturned);

        if (STATUS_BUFFER_TOO_SMALL == Status) {
            VolumeName.Buffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, LengthReturned, 'von');

            if (VolumeName.Buffer == NULL) {
                try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
            }

            RtlZeroMemory(VolumeName.Buffer, LengthReturned);
            VolumeName.MaximumLength = (USHORT)LengthReturned;
            VolumeName.Length = (USHORT)LengthReturned;

            Status = FltGetVolumeName(FltObjects->Volume, &VolumeName, &LengthReturned);
        }

        if (!NT_SUCCESS(Status)) {
            try_return(Status);
        }

        // ��ȡ�ļ���
        Length = MAX_PATH;
        fni = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, Length, 'fni');

        if (fni == NULL) {
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }

        RtlZeroMemory(fni, Length);

        Status = FltQueryInformationFile(
            FltObjects->Instance,
            Ccb->StreamFileInfo.StreamObject,
            fni,
            Length,
            FileNameInformation,
            &LengthReturned
            );

        if (Status == STATUS_BUFFER_OVERFLOW) {

            Length = fni->FileNameLength + sizeof(FILE_NAME_INFORMATION);
            // �������ô�С�ٷ�һ��
            FltFreePoolAlignedWithTag(FltObjects->Instance, fni, 'fni');

            fni = NULL;
            fni = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, Length, 'fni');
            if (fni == NULL) {
                try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
            }

            RtlZeroMemory(fni, Length);

            Status = FltQueryInformationFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                fni,
                Length,
                FileNameInformation,
                &LengthReturned
                );
        }

        if (!NT_SUCCESS(Status)) {
            try_return(Status);
        }

        Length = fni->FileNameLength + VolumeName.Length + StreamNameLength + sizeof(WCHAR);
        StreamString.Buffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, Length, 'fsf');
        if (StreamString.Buffer == NULL) {
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES;);
        }

        RtlZeroMemory(StreamString.Buffer, Length);

        StreamString.Length = (USHORT)(Length - sizeof(WCHAR));
        StreamString.MaximumLength = (USHORT)Length;

        RtlCopyMemory(StreamString.Buffer, VolumeName.Buffer, VolumeName.Length);
        RtlCopyMemory(Add2Ptr(StreamString.Buffer, VolumeName.Length), fni->FileName, fni->FileNameLength);
        RtlCopyMemory(Add2Ptr(StreamString.Buffer, VolumeName.Length + fni->FileNameLength), StreamName, StreamNameLength);

        InitializeObjectAttributes(&ob, &StreamString, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

        Status = FltCreateFile(FltObjects->Filter,
            FltObjects->Instance,
            &StreamHandle,
            FILE_READ_DATA,
            &ob,
            &IoStatus,
            NULL,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_VALID_FLAGS,
            FILE_OPEN,
            FILE_NON_DIRECTORY_FILE,
            NULL,
            0,
            IO_IGNORE_SHARE_ACCESS_CHECK
            );

        if (!NT_SUCCESS(Status)) {
            try_return(Status);
        }

        Status = ObReferenceObjectByHandle(StreamHandle,
            0,
            *IoFileObjectType,
            KernelMode,
            &StreamObject,
            NULL);

        if (!NT_SUCCESS(Status)) {
            FltClose(StreamHandle);
            try_return(Status);
        }

        // �ļ�ͷ�Ѿ���������С�����ˣ����Կ϶��򿪿��Գɹ�
        pFileHeader = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, FILE_HEADER_LENGTH, 'rh');

        if (pFileHeader == NULL) {
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }

        RtlZeroMemory(pFileHeader, FILE_HEADER_LENGTH);

        // ��ȡ�����ļ�
        Status = FltReadFile(
            FltObjects->Instance,
            StreamObject,
            &ByteOffset,
            FILE_HEADER_LENGTH,
            pFileHeader,
            /*FLTFL_IO_OPERATION_NON_CACHED |*/FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, // �ǻ���Ĵ�
            NULL,
            NULL,
            NULL
            );

        if (NT_SUCCESS(Status)) {
            if ((RtlCompareMemory(pFileHeader->FileBegin, FileBegin, sizeof(FileBegin)) == sizeof(FileBegin))
                && (RtlCompareMemory(pFileHeader->Flag, Flag, sizeof(Flag)) == sizeof(Flag))) {
                *IsEnFile = TRUE;

                RtlCopyMemory(&IrpContext->CreateInfo.RealFileSize, pFileHeader->RealFileSize, sizeof(LARGE_INTEGER));
                IrpContext->CreateInfo.RealSize = TRUE;
            }
            else {
                *IsEnFile = FALSE;
            }
        }
try_exit:
        NOTHING;
    }
    finally {
        if (pFileHeader != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, pFileHeader, 'rh');
        }
        if (VolumeName.Buffer != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, VolumeName.Buffer, 'von');
        }
        if (fni != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, fni, 'fni');
        }
        if (StreamString.Buffer != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, StreamString.Buffer, 'fsf');
        }
        if (StreamObject != NULL) {
            FltClose(StreamHandle);
            ObDereferenceObject(StreamObject);
        }
    }
    return Status;
}

// �ж��ǲ������ǹ�����FCB
BOOLEAN IsMyFakeFcb(PFILE_OBJECT FileObject)
{
    PFCB Fcb;
    if (FileObject == NULL || FileObject->FsContext == NULL) {
        return FALSE;
    }
    Fcb = FileObject->FsContext;

    if (Fcb->Header.NodeTypeCode == LAYER_NTC_FCB &&
        Fcb->Header.NodeByteSize == sizeof(FCB)) {
        return TRUE;
    }
    return FALSE;
}

// ���IRP_MN_COMPLETE������
FLT_PREOP_CALLBACK_STATUS
X70FsdCompleteMdl(
    __inout PFLT_CALLBACK_DATA Data,
    __in PCFLT_RELATED_OBJECTS FltObjects,
    __in PIRP_CONTEXT IrpContext
    )
{
    PFILE_OBJECT FileObject;
    PIO_STACK_LOCATION IrpSp;
    PFLT_IO_PARAMETER_BLOCK Iopb = Data->Iopb;

    PAGED_CODE();

    if (FltObjects != NULL) {
        FileObject = FltObjects->FileObject;
    }
    else {
        FileObject = Iopb->TargetFileObject;
    }

    FLT_ASSERT(FileObject != NULL);

    switch (Iopb->MajorFunction) {
        case IRP_MJ_READ:
            // �ͷ�mdl
            CcMdlReadComplete(FileObject, Iopb->Parameters.Read.MdlAddress);
            break;

        case IRP_MJ_WRITE:
            // ͬ����
            FLT_ASSERT(CanFsdWait(Data));
            CcMdlWriteComplete(FileObject, &Iopb->Parameters.Write.ByteOffset, Iopb->Parameters.Write.MdlAddress);
            break;

        default:
            FLT_ASSERTMSG("Illegal Mdl Complete, About to bugcheck ", FALSE);
            // ֱ������
            X70FsdBugCheck(Iopb->MajorFunction, 0, 0);
            break;
    }

    //
    // Mdl is now deallocated.
    //
    Iopb->Parameters.Read.MdlAddress = NULL;
    Data->IoStatus.Status = STATUS_SUCCESS;

    X70FsdCompleteRequest(&IrpContext, &Data, STATUS_SUCCESS, FALSE);

    return FLT_PREOP_COMPLETE;
}

//
// �ӳ�д��Ԥ��
//
BOOLEAN
X70FsdCMCAcquireForLazyWrite(
    IN PVOID Context,
    IN BOOLEAN Wait
    )
{
    BOOLEAN AcquiredFile = FALSE;
    ULONG Index = (ULONG)Context & 1;
    PFCB Fcb = (PFCB)Context;

    if (!ExAcquireResourceSharedLite(Fcb->Header.PagingIoResource, Wait)) {
        return FALSE;
    }

    Fcb->LazyWriteThread[Index] = PsGetCurrentThread();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

VOID
X70FsdCMCReleaseFromLazyWrite(
    IN PVOID Context
    )
{
    ULONG Index = (ULONG)Context & 1;
    PFCB Fcb = (PFCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    Fcb->LazyWriteThread[Index] = NULL;
    ExReleaseResource(Fcb->Header.PagingIoResource);
}

BOOLEAN
X70FsdCMCAcquireForReadAhead(
    IN PVOID Context,
    IN BOOLEAN Wait
    )
{
    PFCB Fcb = (PFCB)Context;

    if (!ExAcquireResourceSharedLite(((PFCB)Fcb)->Header.Resource, Wait)) {
        return FALSE;
    }

    FLT_ASSERT(IoGetTopLevelIrp() == NULL);
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

    return TRUE;
}

VOID
X70FsdCMCReleaseFromReadAhead(
    IN PVOID Context
    )
{
    PFCB Fcb = (PFCB)Context;

    FLT_ASSERT(IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    IoSetTopLevelIrp(NULL);

    ExReleaseResourceLite(((PFCB)Fcb)->Header.Resource);
}

VOID X70FsdRaiseStatus(PIRP_CONTEXT IrpContext, NTSTATUS Status)
{
    if (IrpContext != NULL) {
        IrpContext->ExceptionStatus = Status;
    }
    ExRaiseStatus(Status);
}

/////////////////////////////////

PVOID
X70FsdMapUserBuffer(
    __inout PFLT_CALLBACK_DATA Data
    )
{
    NTSTATUS Status;

    PMDL MdlAddress;
    PVOID Buffer;

    PMDL  *MdlAddressPointer;
    PVOID  *BufferPointer;
    PULONG  Length;
    LOCK_OPERATION  DesiredAccess;

    PVOID SystemBuffer = NULL;
    PFLT_IO_PARAMETER_BLOCK Iopb = Data->Iopb;
    PAGED_CODE();

    Status = FltDecodeParameters(Data, &MdlAddressPointer, &BufferPointer, &Length, &DesiredAccess);

    if (!NT_SUCCESS(Status)) {
        X70FsdRaiseStatus(NULL, Status);
    }

    MdlAddress = *MdlAddressPointer;
    Buffer = *BufferPointer;

    if (MdlAddress == NULL) {
        return Buffer;
    }
    else {
        SystemBuffer = MmGetSystemAddressForMdlSafe(MdlAddress, NormalPagePriority);

        if (SystemBuffer == NULL) {
            X70FsdRaiseStatus(NULL, STATUS_INSUFFICIENT_RESOURCES);
        }

        //MmMapLockedPages(MdlAddress,KernelMode);
        return SystemBuffer;
    }
    return NULL;
}

// ���¼��ӵ�������
BOOLEAN
X70FsdWaitForIoAtEof(
    IN PFSRTL_ADVANCED_FCB_HEADER Header,
    IN OUT PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN PEOF_WAIT_BLOCK EofWaitBlock  //
    )

{
    PAGED_CODE();

    FLT_ASSERT(Header->FileSize.QuadPart >= Header->ValidDataLength.QuadPart);

    //
    // Initialize the event and queue our block
    //
    KeInitializeEvent(&EofWaitBlock->Event, NotificationEvent, FALSE);
    InsertTailList(&Header->FilterContexts, &EofWaitBlock->EofWaitLinks); //�ѵȴ��¼��ӵ�fcb�ȴ�������

    //
    // Free the mutex and wait
    //
    ExReleaseFastMutex(Header->FastMutex);

    // �ȴ�����¼�
    KeWaitForSingleObject(&EofWaitBlock->Event,
        Executive,
        KernelMode,
        FALSE,
        (PLARGE_INTEGER)NULL);

    //
    // Now, resynchronize and get on with it.
    //
    ExAcquireFastMutex(Header->FastMutex);

    //
    // Now we have to check again, and actually catch the case
    // where we are no longer extending!
    //
    if ((FileOffset->QuadPart >= 0) &&
       ((FileOffset->QuadPart + Length) <= Header->ValidDataLength.QuadPart)) {

        X70FsdFinishIoAtEof(Header);
        return FALSE;
    }

    return TRUE;
}

// ������ļ�eof����io����
VOID
X70FsdFinishIoAtEof(
    IN PFSRTL_ADVANCED_FCB_HEADER Header
    )
{
    PEOF_WAIT_BLOCK EofWaitBlock;

    PAGED_CODE();

    // �������Ŷӵ��¼������ź�
    if (!IsListEmpty(&Header->FilterContexts)) {

        EofWaitBlock = (PEOF_WAIT_BLOCK)RemoveHeadList(&Header->FilterContexts);
        KeSetEvent(&EofWaitBlock->Event, 0, FALSE);

        //
        // Otherwise, show there is no active extender now.
        //
    }
    else {
        ClearFlag(Header->Flags, FSRTL_FLAG_EOF_ADVANCE_ACTIVE);
    }
}

// ���ö������
PTOP_LEVEL_CONTEXT
X70FsdSetTopLevelIrp(
    IN PTOP_LEVEL_CONTEXT TopLevelContext,
    IN BOOLEAN ForceTopLevel,   // read ʱ��Ϊ true, true
    IN BOOLEAN SetTopLevel
    )
{
    PTOP_LEVEL_CONTEXT CurrentTopLevelContext;
    ULONG_PTR StackBottom;
    ULONG_PTR StackTop;
    BOOLEAN TopLevelRequest = TRUE;
    BOOLEAN TopLevelX70Fsd = TRUE;

    BOOLEAN ValidCurrentTopLevel = FALSE;

    //
    // ��ȡ��ǰ�̱߳��ش洢��ֵ���������һ����ֵ���߲���һ����Ч��NTFS���������Ļ���Ч
    // Fsrtlֵ��ָ����ʱ�����Ƕ��������
    //

    // ���ﷵ�ص���ethread-> TopLevelIrp      : Uint4B
    CurrentTopLevelContext = (PTOP_LEVEL_CONTEXT)IoGetTopLevelIrp();

    //
    // ����Ƿ�����һ����Ч��ntfs�����������
    //

    // ��ö�ջ���ޣ����ޣ������ҿ��� wrk, �����ǻ��д��.
    IoGetStackLimits(&StackTop, &StackBottom);

    if (((ULONG_PTR)CurrentTopLevelContext <= StackBottom - sizeof(TOP_LEVEL_CONTEXT)) &&
        ((ULONG_PTR)CurrentTopLevelContext >= StackTop) &&
        !FlagOn((ULONG_PTR)CurrentTopLevelContext, 0x3) &&
        (CurrentTopLevelContext->X70Fsd == 0x70)) {
        ValidCurrentTopLevel = TRUE;
    }

    // �����ǿ����������ڶ�����ʱ����
    if (ForceTopLevel) {
        TopLevelRequest = SetTopLevel;

        //
        // If the value is NULL then we are top level everything.
        //
    }
    else if (CurrentTopLevelContext == NULL) {
        NOTHING;
    }
    else if ((ULONG)CurrentTopLevelContext <= FSRTL_MAX_TOP_LEVEL_IRP_FLAG) {
        TopLevelRequest = FALSE;
    }
    else if (ValidCurrentTopLevel) {
        // ��Ч�Ķ�������Ͳ�����ȥ������
        TopLevelRequest = FALSE;
        TopLevelX70Fsd = FALSE;
    }

    // ��������ڶ����NTFSȻ���ʼ�������ߵĽṹ������洢���̱߳��ش洢

    if (TopLevelX70Fsd) {
        TopLevelContext->X70Fsd = 0x70;                                     // ���ǵı�־
        TopLevelContext->SavedTopLevelIrp = (PIRP)CurrentTopLevelContext;   // ���涥���������
        TopLevelContext->TopLevelIrpContext = NULL;
        TopLevelContext->TopLevelRequest = TopLevelRequest;

        if (ValidCurrentTopLevel) {
            TopLevelContext->ValidSavedTopLevel = TRUE;
        }
        else {
            TopLevelContext->ValidSavedTopLevel = FALSE;
        }

        IoSetTopLevelIrp((PIRP)TopLevelContext);
        // PsGetCurrentThread()->TopLevelIrp = (ULONG_PTR)Irp;

        return TopLevelContext;
    }

    return CurrentTopLevelContext;
}

// ����������
PIRP_CONTEXT
X70FsdCreateIrpContext(
    IN PFLT_CALLBACK_DATA Data,
    IN PCFLT_RELATED_OBJECTS FltObjects,
    IN BOOLEAN Wait
    )
{
    PIRP_CONTEXT IrpContext = NULL;
    PTOP_LEVEL_CONTEXT CurrentTopLevelContext;
    PFILE_OBJECT FileObject = FltObjects->FileObject;

    // ����ռ�
    IrpContext = ExAllocateFromNPagedLookasideList(&G_IrpContextLookasideList);

    if (IrpContext != NULL) {
        RtlZeroMemory(IrpContext, sizeof(IRP_CONTEXT));

        IrpContext->NodeTypeCode = LAYER_NTC_FCB;
        IrpContext->NodeByteSize = sizeof(IRP_CONTEXT);
        IrpContext->OriginatingData = Data;
        IrpContext->ProcessId = PsGetCurrentProcessId();

        // ͬ����
        if (Wait) {
            SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
        }

        // Write-Through ��־
        if (FlagOn(FileObject->Flags, FO_WRITE_THROUGH)) {
            SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH);
        }

        IrpContext->MajorFunction = Data->Iopb->MajorFunction;
        IrpContext->MinorFunction = Data->Iopb->MinorFunction;

        RtlCopyMemory(&IrpContext->FltObjects, FltObjects, FltObjects->Size);

        if ((PFLT_CALLBACK_DATA)IoGetTopLevelIrp() != Data) {
            SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_RECURSIVE_CALL);
        }
    }
    return IrpContext;
}

VOID
X70FsdDeleteIrpContext(
    IN OUT PIRP_CONTEXT * pIrpContext
    )
{
    PFCB Fcb;
    PIRP_CONTEXT IrpContext;

    FLT_ASSERT(pIrpContext != NULL);
    IrpContext = *pIrpContext;

    if (!FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_DONT_DELETE)) {
        // �ǻ�����첽�����ڲ����ڲ��Լ�������
        if (!FlagOn(IrpContext->Flags, IRP_CONTEXT_STACK_IO_CONTEXT)
            && (IrpContext->X70FsdIoContext != NULL)) {

            ExFreeToNPagedLookasideList(&G_IoContextLookasideList, IrpContext->X70FsdIoContext);
            IrpContext->X70FsdIoContext = NULL;
        }

        // �ͷ��ڴ�
        if (IrpContext != NULL) {
            ExFreeToNPagedLookasideList(&G_IrpContextLookasideList, IrpContext);
        }
        IrpContext = NULL;
    }
}

VOID
X70FsdOplockComplete(
    IN PFLT_CALLBACK_DATA Data,
    IN PVOID Context
    )
{
    PAGED_CODE();

    if (Data->IoStatus.Status == STATUS_SUCCESS) {
        X70FsdAddToWorkque(Data, (PIRP_CONTEXT)Context);
    }
    else {
        X70FsdCompleteRequest(((PIRP_CONTEXT *)&Context), &Data, Data->IoStatus.Status, FALSE);
    }
}

NTSTATUS
X70FsdPostRequest(
    __inout PFLT_CALLBACK_DATA Data,
    __in    PIRP_CONTEXT IrpContext
    )
{
    X70FsdPrePostIrp(Data, IrpContext);
    X70FsdAddToWorkque(Data, IrpContext);

    return STATUS_PENDING;
}

//
// ���������д�����׼��
// FltQueueDeferredIoWorkItem ���������ṩ�Ĺ�������, �����޸�.
//
VOID
X70FsdPrePostIrp(
    IN PFLT_CALLBACK_DATA Data,
    IN PVOID Context
    )
{
    PIRP_CONTEXT IrpContext;
    PFCB Fcb;
    NTSTATUS Status = STATUS_SUCCESS;
    PFLT_IO_PARAMETER_BLOCK Iopb;
    IrpContext = (PIRP_CONTEXT)Context;

    if (Data == NULL) {
        return;
    }

    if ((IrpContext->X70FsdIoContext != NULL) &&
        FlagOn(IrpContext->Flags, IRP_CONTEXT_STACK_IO_CONTEXT)) {

        ClearFlag(IrpContext->Flags, IRP_CONTEXT_STACK_IO_CONTEXT);
        IrpContext->X70FsdIoContext = NULL;
    }

    if (ARGUMENT_PRESENT(Data)) {
        if (IrpContext->MajorFunction == IRP_MJ_READ || IrpContext->MajorFunction == IRP_MJ_WRITE) {
            //
            // �����û�buffer
            //
            if (!FlagOn(IrpContext->MinorFunction, IRP_MN_MDL)) {
                Status = FltLockUserBuffer(Data);
            }
        }
        else if (IrpContext->MajorFunction == IRP_MJ_DIRECTORY_CONTROL
            && IrpContext->MinorFunction == IRP_MN_QUERY_DIRECTORY) {
            Status = FltLockUserBuffer(Data);
        }
        else if (IrpContext->MajorFunction == IRP_MJ_QUERY_EA) {
            Status = FltLockUserBuffer(Data);
        }
        else if (IrpContext->MajorFunction == IRP_MJ_SET_EA) {
            Status = FltLockUserBuffer(Data);
        }

        // ԭ����ʱ�������Ǳ��irp pending
        if (!NT_SUCCESS(Status)) {
            X70FsdRaiseStatus(IrpContext, Status);
        }
    }
}

// �����û����� no use
VOID
X70FsdLockUserBuffer(
    IN PIRP_CONTEXT IrpContext,
    IN OUT PFLT_CALLBACK_DATA Data,
    IN LOCK_OPERATION Operation,
    IN ULONG BufferLength
    )
{
    NTSTATUS Status;
    PMDL Mdl = NULL;
    PFLT_IO_PARAMETER_BLOCK CONST Iopb = Data->Iopb;

    PMDL MdlAddress;
    PVOID Buffer;

    PMDL *MdlAddressPointer;
    PVOID *BufferPointer;
    PULONG Length;
    LOCK_OPERATION DesiredAccess;

    Status = FltDecodeParameters(Data, &MdlAddressPointer, &BufferPointer, &Length, &DesiredAccess);

    if (!NT_SUCCESS(Status)) {
        X70FsdRaiseStatus(IrpContext, Status);
    }

    MdlAddress = *MdlAddressPointer;
    Buffer = *BufferPointer;

    if (MdlAddress == NULL) {
        *MdlAddressPointer = IoAllocateMdl(Buffer, BufferLength, FALSE, FALSE, NULL);

        if (*MdlAddressPointer == NULL) {
            X70FsdRaiseStatus(IrpContext, STATUS_INSUFFICIENT_RESOURCES);
        }

        try {
            MmProbeAndLockPages(*MdlAddressPointer, Data->RequestorMode, Operation);
        }
        except(EXCEPTION_EXECUTE_HANDLER) {
            Status = GetExceptionCode();

            IoFreeMdl(Mdl);
            *MdlAddressPointer = NULL;

            if (!FsRtlIsNtstatusExpected(Status))
                Status = STATUS_INVALID_USER_BUFFER;

            X70FsdRaiseStatus(IrpContext, Status);
        }

        // �������Լ���mdl
        IrpContext->AllocateMdl = *MdlAddressPointer;
    }
}

// ���ӹ�����
VOID
X70FsdAddToWorkque(
    IN PFLT_CALLBACK_DATA Data,
    IN PIRP_CONTEXT IrpContext
    )
{
    PFLT_IO_PARAMETER_BLOCK CONST Iopb = IrpContext->OriginatingData->Iopb;

    IrpContext->WorkItem = IoAllocateWorkItem(Iopb->TargetFileObject->DeviceObject);
    IoQueueWorkItem(IrpContext->WorkItem, X70FsdFspDispatchWorkItem, DelayedWorkQueue, (PVOID)IrpContext);
}

VOID X70FsdFspDispatchWorkItem(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PVOID  Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    X70FsdFspDispatch(Context);
}

// �����߳�����ַ�
VOID X70FsdFspDispatch(
    PVOID  Context
    )
{
    TOP_LEVEL_CONTEXT TopLevelContext;
    PTOP_LEVEL_CONTEXT ThreadTopLevelContext;

    PFLT_CALLBACK_DATA Data;
    PIRP_CONTEXT IrpContext;
    PFLT_IO_PARAMETER_BLOCK  Iopb;
    ULONG LogFileFullCount = 0;

    BOOLEAN Retry;

    IrpContext = (PIRP_CONTEXT)Context;
    Data = IrpContext->OriginatingData;
    if (Data != NULL) {
        Iopb = Data->Iopb;
    }

    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_IN_FSP);

    while (TRUE) {
        FsRtlEnterFileSystem();
        Retry = FALSE;

        if (FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_RECURSIVE_CALL)) {
            IoSetTopLevelIrp((PIRP)FSRTL_FSP_TOP_LEVEL_IRP);
        }
        else {
            IoSetTopLevelIrp((PIRP)Data);
        }

        try {
            IrpContext->ExceptionStatus = 0;

            if (Data != NULL) {
                switch (IrpContext->MajorFunction) {
                case IRP_MJ_CREATE:
                    // ������û�е�����ɼǵü���
                    (VOID)X70FsdCommonCreate(Data, NULL, IrpContext);
                    break;

                case IRP_MJ_CLOSE:
                    X70FsdBugCheck(0, 0, 0);
                    break;

                case IRP_MJ_READ:
                    (VOID)X70FsdCommonRead(Data, NULL, IrpContext);
                    break;

                case IRP_MJ_WRITE:
                    (VOID)X70FsdCommonWrite(Data, NULL, IrpContext);
                    break;

                case IRP_MJ_QUERY_INFORMATION:
                    (VOID)X70FsdCommonQueryInformation(Data, NULL, IrpContext);
                    break;

                case IRP_MJ_SET_INFORMATION:
                    (VOID)X70FsdCommonSetInformation(Data, NULL, IrpContext);
                    break;

                case IRP_MJ_QUERY_EA:
                    //(VOID) X70FsdCommonQueryEa(  Data, NULL, IrpContext );
                    break;

                case IRP_MJ_SET_EA:
                    //(VOID) X70FsdCommonSetEa(  Data, NULL, IrpContext );
                    break;

                case IRP_MJ_FLUSH_BUFFERS:
                    (VOID)X70FsdCommonFlushBuffers(Data, NULL, IrpContext);
                    break;

                case IRP_MJ_CLEANUP:
                    (VOID)X70FsdCommonCleanup(Data, NULL, IrpContext);
                    break;

                case IRP_MJ_LOCK_CONTROL:
                    (VOID)X70FsdCommonLockControl(Data, NULL, IrpContext);
                    break;

                case IRP_MJ_QUERY_SECURITY:
                    //(VOID) X70FsdCommonQuerySecurityInfo( IrpContext, Irp );
                    break;

                case IRP_MJ_SET_SECURITY:
                    //(VOID) X70FsdCommonSetSecurityInfo( IrpContext, Irp );
                    break;

                default:
                    // ɾ���������
                    X70FsdCompleteRequest(&IrpContext, &Data, STATUS_INVALID_DEVICE_REQUEST, TRUE);
                    break;
                }
            }
            else {
                // ɾ���������
                X70FsdCompleteRequest(&IrpContext, NULL, STATUS_SUCCESS, TRUE);
            }

        } except(EXCEPTION_EXECUTE_HANDLER) {
            // �쳣������Ҫ�����޸�
            NTSTATUS ExceptionCode = GetExceptionCode();
            if (ExceptionCode == STATUS_CANT_WAIT) {
                // �����쳣������
                Retry = TRUE;
            }
            else {
                (VOID)X70FsdProcessException(&IrpContext, &Data, GetExceptionCode());
            }
        }

        IoSetTopLevelIrp(NULL);
        FsRtlExitFileSystem();

        if (!Retry) {
            break;
        }
    }
}

VOID
X70FsdProcessException(
    IN OUT PIRP_CONTEXT *IrpContext OPTIONAL,
    IN OUT PFLT_CALLBACK_DATA *Data  OPTIONAL,
    IN NTSTATUS Status
    )
{
    BOOLEAN Pending = FALSE;
    try {
        if (ARGUMENT_PRESENT(IrpContext) && ARGUMENT_PRESENT(*IrpContext)) {
            (*IrpContext)->ExceptionStatus = Status;

            if ((*IrpContext)->WorkItem != NULL) {
                IoFreeWorkItem((*IrpContext)->WorkItem);
                (*IrpContext)->WorkItem = NULL;
            }

            // �ӳٵ����ÿ���ɾ��irp��������Ȼ��ɾ��������
            if ((*IrpContext)->AllocateMdl != NULL) {
                IoFreeMdl((*IrpContext)->AllocateMdl);
                (*IrpContext)->AllocateMdl = NULL;
            }

            if (FlagOn((*IrpContext)->Flags, IRP_CONTEXT_FLAG_IN_FSP)) {
                Pending = TRUE;
            }
            if (Pending) {
                // �ӳ�����ɾ��������
                ClearFlag((*IrpContext)->Flags, IRP_CONTEXT_FLAG_DONT_DELETE);
            }
            X70FsdDeleteIrpContext(IrpContext);
        }

        if (ARGUMENT_PRESENT(Data) && ARGUMENT_PRESENT(*Data)) {
            if (NT_ERROR(Status) &&
                FlagOn((*Data)->Iopb->IrpFlags, IRP_INPUT_OPERATION)) {
                (*Data)->IoStatus.Information = 0;
            }

            (*Data)->IoStatus.Status = Status;

            if (Pending) {
                FltCompletePendedPreOperation(*Data, FLT_PREOP_COMPLETE, NULL);
            }
        }
    }
    finally {
        if (ARGUMENT_PRESENT(Data)) {
            (*Data)->IoStatus.Status = Status;
        }
    }
}

// �쳣������
ULONG
X70FsdExceptionFilter(
    IN PIRP_CONTEXT IrpContext,
    IN PEXCEPTION_POINTERS ExceptionPointer
    )
{
    NTSTATUS ExceptionCode;

    ExceptionCode = ExceptionPointer->ExceptionRecord->ExceptionCode;

    //
    // If the exception is STATUS_IN_PAGE_ERROR, get the I/O error code
    // from the exception record.
    //
    if (ExceptionCode == STATUS_IN_PAGE_ERROR) {
        if (ExceptionPointer->ExceptionRecord->NumberParameters >= 3) {
            ExceptionCode = (NTSTATUS)ExceptionPointer->ExceptionRecord->ExceptionInformation[2];
        }
    }

    //
    // If there is not an irp context, we must have had insufficient resources.
    //
    if (!ARGUMENT_PRESENT(IrpContext)) {
        if (!FsRtlIsNtstatusExpected(ExceptionCode)) {
            X70FsdBugCheck((ULONG_PTR)ExceptionPointer->ExceptionRecord,
                (ULONG_PTR)ExceptionPointer->ContextRecord,
                (ULONG_PTR)ExceptionPointer->ExceptionRecord->ExceptionAddress);
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }

    //
    // For the purposes of processing this exception, let's mark this
    // request as being able to wait and disable  write through if we
    // aren't posting it.
    //
    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

    if ((ExceptionCode != STATUS_CANT_WAIT) &&
        (ExceptionCode != STATUS_VERIFY_REQUIRED)) {
        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_DISABLE_WRITE_THROUGH);
    }

    if (IrpContext->ExceptionStatus == 0) {
        if (FsRtlIsNtstatusExpected(ExceptionCode)) {
            IrpContext->ExceptionStatus = ExceptionCode;
            return EXCEPTION_EXECUTE_HANDLER;
        }
        else {
            X70FsdBugCheck((ULONG_PTR)ExceptionPointer->ExceptionRecord,
                (ULONG_PTR)ExceptionPointer->ContextRecord,
                (ULONG_PTR)ExceptionPointer->ExceptionRecord->ExceptionAddress);
        }
    }
    else {
        FLT_ASSERT(IrpContext->ExceptionStatus == ExceptionCode);
        FLT_ASSERT(FsRtlIsNtstatusExpected(ExceptionCode));
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

VOID
X70FsdCompleteRequest(
    IN OUT PIRP_CONTEXT *IrpContext OPTIONAL,
    IN OUT PFLT_CALLBACK_DATA *Data  OPTIONAL,
    IN NTSTATUS Status,
    IN BOOLEAN Pending
    )
{
    if (ARGUMENT_PRESENT(IrpContext) &&
        ARGUMENT_PRESENT(*IrpContext)) {
        (*IrpContext)->ExceptionStatus = Status;

        if ((*IrpContext)->WorkItem != NULL) {
            IoFreeWorkItem((*IrpContext)->WorkItem);
            (*IrpContext)->WorkItem = NULL;
        }
        // �ӳٵ����ÿ���ɾ��irp��������Ȼ��ɾ��������
        if (FlagOn((*IrpContext)->Flags, IRP_CONTEXT_FLAG_IN_FSP)) {
            Pending = TRUE;
        }
        // �ӳ�����ɾ��������
        if (Pending) {
            ClearFlag((*IrpContext)->Flags, IRP_CONTEXT_FLAG_DONT_DELETE);
        }

        if ((*IrpContext)->AllocateMdl != NULL) {
            IoFreeMdl((*IrpContext)->AllocateMdl);
            (*IrpContext)->AllocateMdl = NULL;
        }

        X70FsdDeleteIrpContext(IrpContext);
    }

    if (ARGUMENT_PRESENT(Data) && ARGUMENT_PRESENT(*Data)) {
        if (NT_ERROR(Status) &&
            FlagOn((*Data)->Iopb->IrpFlags, IRP_INPUT_OPERATION)) {
            (*Data)->IoStatus.Information = 0;
        }

        (*Data)->IoStatus.Status = Status;

        if (Pending) {
            FltCompletePendedPreOperation(*Data, FLT_PREOP_COMPLETE, NULL);
        }
    }
}

// ��������������
BOOLEAN
X70FsdZeroData(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb,
    IN PFILE_OBJECT FileObject,
    IN LONGLONG StartingZero,
    IN LONGLONG ByteCount,
    IN ULONG SectorSize
    )
{
    LARGE_INTEGER ZeroStart = { 0,0 };
    LARGE_INTEGER BeyondZeroEnd = { 0,0 };

    BOOLEAN Finished;

    PAGED_CODE();

    ZeroStart.QuadPart = ((ULONGLONG)StartingZero + (SectorSize - 1)) & ~((ULONGLONG)SectorSize - 1);

    //
    // Detect overflow if we were asked to zero in the last sector of the file,
    // which must be "zeroed" already (or we're in trouble).
    //
    if (StartingZero != 0 && ZeroStart.QuadPart == 0) {
        return TRUE;
    }

    //
    // Note that BeyondZeroEnd can take the value 4gb.
    //
    BeyondZeroEnd.QuadPart = ((ULONGLONG)StartingZero + ByteCount + (SectorSize - 1))
        & (~((LONGLONG)SectorSize - 1));

    //
    // If we were called to just zero part of a sector we are in trouble.
    //
    if (ZeroStart.QuadPart == BeyondZeroEnd.QuadPart) {
        return TRUE;
    }

    Finished = CcZeroData(FileObject,
        &ZeroStart,
        &BeyondZeroEnd,
        BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT));

    return Finished;
}

// �ļ���С�ı��ʱ������滻����д�ȵ�
NTSTATUS X70FsdOverWriteFile(
    PFILE_OBJECT FileObject,
    PFCB Fcb,
    LARGE_INTEGER AllocationSize
    )
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    BOOLEAN AcquiredPagingResource = FALSE;

    // �޸�ԭ���Ĵ�С
    try {
        // �ļ����ض�������ԭ���Ļ��������ļ���С
        if (MmCanFileBeTruncated(&Fcb->SectionObjectPointers, &Li0)) {
            // �����������
            SetFlag(Fcb->FcbState, FCB_STATE_NOTIFY_RESIZE_STREAM);

            // ������������close
            CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);

            ClearFlag(Fcb->FcbState, FCB_STATE_NOTIFY_RESIZE_STREAM);

            // ���»������ļ���С
            ExAcquireResourceExclusiveLite(Fcb->Header.PagingIoResource, TRUE);
            AcquiredPagingResource = TRUE;

            Fcb->Header.FileSize.QuadPart = 0;
            Fcb->Header.ValidDataLength.QuadPart = 0;
            Fcb->Header.AllocationSize.QuadPart = AllocationSize.QuadPart;

            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);

            ExReleaseResourceLite(Fcb->Header.PagingIoResource);
            AcquiredPagingResource = FALSE;

            Status = STATUS_SUCCESS;
        }
        else {
            Status = STATUS_USER_MAPPED_FILE;
        }
    }
    finally {
        if (AcquiredPagingResource) {
            ExReleaseResourceLite(Fcb->Header.PagingIoResource);
        }
    }
    return Status;
}

VOID
X70FsdVerifyOperationIsLegal(
    IN PIRP_CONTEXT IrpContext
    )
{
    PFLT_CALLBACK_DATA Data;
    PFILE_OBJECT FileObject;

    Data = IrpContext->OriginatingData;
    if (Data == NULL) {
        return;
    }

    FileObject = Data->Iopb->TargetFileObject;
    if (FileObject == NULL) {
        return;
    }

    if (FlagOn(FileObject->Flags, FO_CLEANUP_COMPLETE)) {
        PFLT_IO_PARAMETER_BLOCK Iopb = Data->Iopb;
        if ((FlagOn(Iopb->IrpFlags, IRP_PAGING_IO)) ||
            (Iopb->MajorFunction == IRP_MJ_CLOSE) ||
            (Iopb->MajorFunction == IRP_MJ_SET_INFORMATION) ||
            (Iopb->MajorFunction == IRP_MJ_QUERY_INFORMATION) ||
            (((Iopb->MajorFunction == IRP_MJ_READ) ||
            (Iopb->MajorFunction == IRP_MJ_WRITE)) &&
            FlagOn(Iopb->MinorFunction, IRP_MN_COMPLETE))) {
            NOTHING;
        }
        else {
            X70FsdRaiseStatus(IrpContext, STATUS_FILE_CLOSED);
        }
    }
}

// �����ռ��Ȩ��
BOOLEAN X70FsdAcquireExclusiveFcb(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb
    )
{
RetryFcbExclusive:
    if (ExAcquireResourceExclusiveLite(Fcb->Header.Resource, BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
        if ((Fcb->OutstandingAsyncWrites != 0) &&
            ((IrpContext->MajorFunction != IRP_MJ_WRITE) ||
            !FlagOn(IrpContext->OriginatingData->Iopb->IrpFlags, IRP_NOCACHE) ||
            (ExGetSharedWaiterCount(Fcb->Header.Resource) != 0) ||
            (ExGetExclusiveWaiterCount(Fcb->Header.Resource) != 0))) {

            KeWaitForSingleObject(Fcb->OutstandingAsyncEvent,
                Executive,
                KernelMode,
                FALSE,
                (PLARGE_INTEGER)NULL);

            X70FsdReleaseFcb(IrpContext, Fcb);

            goto RetryFcbExclusive;
        }

        try {
            X70FsdVerifyOperationIsLegal(IrpContext);
        }
        finally {
            if (AbnormalTermination()) {
                X70FsdReleaseFcb(IrpContext, Fcb);
            }
        }

        return TRUE;
    }
    else {
        return FALSE;
    }
}

BOOLEAN X70FsdAcquireSharedFcbWaitForEx(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb
    )
{
RetryFcbSharedWaitEx:
    if (ExAcquireSharedWaitForExclusive(Fcb->Header.Resource, FALSE)) {
        if ((Fcb->OutstandingAsyncWrites != 0) &&
            (IrpContext->MajorFunction != IRP_MJ_WRITE)) {
            // ͬ��
            KeWaitForSingleObject(Fcb->OutstandingAsyncEvent,
                Executive,
                KernelMode,
                FALSE,
                (PLARGE_INTEGER)NULL);

            X70FsdReleaseFcb(IrpContext, Fcb);

            goto RetryFcbSharedWaitEx;
        }

        try {
            X70FsdVerifyOperationIsLegal(IrpContext);
        }
        finally {
            if (AbnormalTermination()) {
                X70FsdReleaseFcb(IrpContext, Fcb);
            }
        }
        return TRUE;

    }
    else {
        return FALSE;
    }
}

BOOLEAN X70FsdAcquireSharedFcb(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb
    )
{
RetryFcbShared:
    if (ExAcquireResourceSharedLite(Fcb->Header.Resource, BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
        if ((Fcb->OutstandingAsyncWrites != 0) &&
            ((IrpContext->MajorFunction != IRP_MJ_WRITE) ||
            !FlagOn(IrpContext->OriginatingData->Iopb->IrpFlags, IRP_NOCACHE) ||
            (ExGetSharedWaiterCount(Fcb->Header.Resource) != 0) ||
            (ExGetExclusiveWaiterCount(Fcb->Header.Resource) != 0))) {

            KeWaitForSingleObject(Fcb->OutstandingAsyncEvent,
                Executive,
                KernelMode,
                FALSE,
                (PLARGE_INTEGER)NULL);

            X70FsdReleaseFcb(IrpContext, Fcb);

            goto RetryFcbShared;
        }

        try {
            X70FsdVerifyOperationIsLegal(IrpContext);
        }
        finally {
            if (AbnormalTermination()) {
                X70FsdReleaseFcb(IrpContext, Fcb);
            }
        }
        return TRUE;
    }
    else {
        return FALSE;
    }
}

VOID
X70FsdPopUpFileCorrupt(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb
    )
{
    PKTHREAD Thread;

    if (IoIsSystemThread(IrpContext->OriginatingData->Thread)) {
        Thread = NULL;
    }
    else {
        Thread = IrpContext->OriginatingData->Thread;
    }

    IoRaiseInformationalHardError(STATUS_FILE_CORRUPT_ERROR,
        &Fcb->FileFullName,
        Thread);
}

// ���µĵ��ļ��ķ����С
VOID
X70FsdLookupFileAllocationSize(
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb,
    IN PCCB Ccb
    )
{
    NTSTATUS Status;

    FILE_STANDARD_INFORMATION fsi = { 0 };
    PFLT_CALLBACK_DATA RetNewCallbackData = NULL;
    PFLT_RELATED_OBJECTS FltObjects = &IrpContext->FltObjects;

    Status = FltAllocateCallbackData(FltObjects->Instance, Ccb->StreamFileInfo.StreamObject, &RetNewCallbackData);

    if (NT_SUCCESS(Status)) {
        RetNewCallbackData->Iopb->MajorFunction = IRP_MJ_QUERY_INFORMATION;
        RetNewCallbackData->Iopb->Parameters.QueryFileInformation.FileInformationClass = FileStandardInformation;
        RetNewCallbackData->Iopb->Parameters.QueryFileInformation.InfoBuffer = &fsi;
        RetNewCallbackData->Iopb->Parameters.QueryFileInformation.Length = sizeof(FILE_STANDARD_INFORMATION);

        FltPerformSynchronousIo(RetNewCallbackData);
        Status = RetNewCallbackData->IoStatus.Status;
    }
    if (NT_SUCCESS(Status)) {

        Fcb->Header.AllocationSize.QuadPart = fsi.AllocationSize.QuadPart - Fcb->FileHeaderLength;
        if (Fcb->Header.FileSize.QuadPart > Fcb->Header.AllocationSize.QuadPart) {
            ULONG ClusterSize;
            LARGE_INTEGER TempLI;
            PVOLUME_CONTEXT volCtx = NULL;

            Status = FltGetVolumeContext(FltObjects->Filter,
                FltObjects->Volume,
                &volCtx);
            if (!NT_SUCCESS(Status)) {
                X70FsdRaiseStatus(IrpContext, Status);
            }

            // �ش�С
            ClusterSize = volCtx->SectorSize * volCtx->SectorsPerAllocationUnit;

            if (volCtx != NULL) {
                FltReleaseContext(volCtx);
                volCtx = NULL;
            }

            // ռ�ô�С
            TempLI.QuadPart = Fcb->Header.FileSize.QuadPart;
            TempLI.QuadPart += ClusterSize;
            TempLI.HighPart += (ULONG)((LONGLONG)ClusterSize >> 32);

            // ����Ҫ��λ
            if (TempLI.LowPart == 0) {
                TempLI.HighPart -= 1;
            }

            Fcb->Header.AllocationSize.LowPart = ((ULONG)Fcb->Header.FileSize.LowPart + (ClusterSize - 1)) & (~(ClusterSize - 1));
            Fcb->Header.AllocationSize.HighPart = TempLI.HighPart;
        }

        if (RetNewCallbackData != NULL) {
            FltFreeCallbackData(RetNewCallbackData);
        }
    }
    else {
        if (RetNewCallbackData != NULL) {
            FltFreeCallbackData(RetNewCallbackData);
        }

        X70FsdRaiseStatus(IrpContext, Status);
    }

    if (Fcb->Header.FileSize.QuadPart > Fcb->Header.AllocationSize.QuadPart) {
        X70FsdPopUpFileCorrupt(IrpContext, Fcb);
        X70FsdRaiseStatus(IrpContext, STATUS_FILE_CORRUPT_ERROR);
    }
}

FLT_PREOP_CALLBACK_STATUS
MyFltProcessFileLock(
    __in PFILE_LOCK FileLock,
    __in PFLT_CALLBACK_DATA  CallbackData,
    __in_opt PVOID Context
    )

{
    FLT_PREOP_CALLBACK_STATUS FltStatus;
    PFLT_IO_PARAMETER_BLOCK Iopb = CallbackData->Iopb;

    IO_STATUS_BLOCK Iosb;
    NTSTATUS        Status;

    BOOLEAN ExclusiveLock;
    BOOLEAN FailImmediately;

    Iosb.Information = 0;

    FLT_ASSERT(Iopb->MajorFunction == IRP_MJ_LOCK_CONTROL);

    ExclusiveLock = Iopb->Parameters.LockControl.ExclusiveLock;
    FailImmediately = Iopb->Parameters.LockControl.FailImmediately;

    switch (Iopb->MinorFunction) {
    case IRP_MN_LOCK:
        (VOID)FsRtlFastLock(FileLock,
            Iopb->TargetFileObject,
            &Iopb->Parameters.LockControl.ByteOffset,
            Iopb->Parameters.LockControl.Length,
            Iopb->Parameters.LockControl.ProcessId,
            Iopb->Parameters.LockControl.Key,
            FailImmediately,
            ExclusiveLock,
            &Iosb,
            Context,
            FALSE);
        break;

    case IRP_MN_UNLOCK_SINGLE:
        Iosb.Status = FsRtlFastUnlockSingle(FileLock,
            Iopb->TargetFileObject,
            &Iopb->Parameters.LockControl.ByteOffset,
            Iopb->Parameters.LockControl.Length,
            Iopb->Parameters.LockControl.ProcessId,
            Iopb->Parameters.LockControl.Key,
            Context,
            FALSE);
        break;

    case IRP_MN_UNLOCK_ALL:
        Iosb.Status = FsRtlFastUnlockAll(FileLock,
            Iopb->TargetFileObject,
            Iopb->Parameters.LockControl.ProcessId,
            Context);
        break;

    case IRP_MN_UNLOCK_ALL_BY_KEY:
        Iosb.Status = FsRtlFastUnlockAllByKey(FileLock,
            Iopb->TargetFileObject,
            Iopb->Parameters.LockControl.ProcessId,
            Iopb->Parameters.LockControl.Key,
            Context);
        break;

    default:
        Iosb.Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    CallbackData->IoStatus = Iosb;
    Status = Iosb.Status;

    if (Status == STATUS_PENDING) {
        FltStatus = FLT_PREOP_PENDING;
    }
    else {
        FltStatus = FLT_PREOP_COMPLETE;
    }
    return FltStatus;
}

BOOLEAN
MyFltCheckLockForReadAccess(
    __in PFILE_LOCK FileLock,
    __in PFLT_CALLBACK_DATA  CallbackData
    )
{
    BOOLEAN Result;

    PFLT_IO_PARAMETER_BLOCK  Iopb;

    LARGE_INTEGER  StartingByte;
    LARGE_INTEGER  Length;
    ULONG          Key;
    PFILE_OBJECT   FileObject;
    PVOID          ProcessId;
    LARGE_INTEGER  BeyondLastByte;

    if (FileLock->LockInformation == NULL) {
        return TRUE;
    }

    Iopb = CallbackData->Iopb;

    StartingByte = Iopb->Parameters.Read.ByteOffset;
    Length.QuadPart = (ULONGLONG)Iopb->Parameters.Read.Length;

    BeyondLastByte.QuadPart = (ULONGLONG)StartingByte.QuadPart + Length.LowPart;

    Key = Iopb->Parameters.Read.Key;
    FileObject = Iopb->TargetFileObject;
    ProcessId = FltGetRequestorProcess(CallbackData);

    Result = FsRtlFastCheckLockForRead(FileLock,
        &StartingByte,
        &Length,
        Key,
        FileObject,
        ProcessId);

    return Result;
}

BOOLEAN
MyFltCheckLockForWriteAccess(
    __in PFILE_LOCK FileLock,
    __in PFLT_CALLBACK_DATA  CallbackData
    )
{
    BOOLEAN Result;

    PFLT_IO_PARAMETER_BLOCK  Iopb;

    LARGE_INTEGER   StartingByte;
    LARGE_INTEGER   Length;
    ULONG           Key;
    PFILE_OBJECT    FileObject;
    PVOID           ProcessId;
    LARGE_INTEGER   BeyondLastByte;

    if (FileLock->LockInformation == NULL) {
        return TRUE;
    }

    Iopb = CallbackData->Iopb;

    StartingByte = Iopb->Parameters.Write.ByteOffset;
    Length.QuadPart = (ULONGLONG)Iopb->Parameters.Write.Length;

    BeyondLastByte.QuadPart = (ULONGLONG)StartingByte.QuadPart + Length.LowPart;

    Key = Iopb->Parameters.Write.Key;
    FileObject = Iopb->TargetFileObject;
    ProcessId = FltGetRequestorProcess(CallbackData);

    Result = FsRtlFastCheckLockForWrite(FileLock,
        &StartingByte,
        &Length,
        Key,
        FileObject,
        ProcessId);

    return Result;
}

NTSTATUS ExtendingValidDataSetFile(PCFLT_RELATED_OBJECTS FltObjects, PFCB Fcb, PCCB Ccb)
{
    NTSTATUS Status;
    // �����ļ��Ĵ�С
    FILE_VALID_DATA_LENGTH_INFORMATION fvi;
    ULONG RetryCount = 0;

    fvi.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart + Fcb->FileHeaderLength;

    // ʧ�����ظ�3��
    while (RetryCount <= 3) {
        Status = FltSetInformationFile(
            FltObjects->Instance,
            Ccb->StreamFileInfo.StreamObject,
            &fvi,
            sizeof(FILE_VALID_DATA_LENGTH_INFORMATION),
            FileValidDataLengthInformation
            );
        if (NT_SUCCESS(Status) || Status == STATUS_INVALID_PARAMETER) {
            Status = STATUS_SUCCESS;
            break;
        }
        RetryCount++;
    }
    return Status;
}

NTSTATUS CleanupSetFile(PCFLT_RELATED_OBJECTS FltObjects, PFCB Fcb, PCCB Ccb)
{
    NTSTATUS Status;
    // �����ļ��Ĵ�С
    FILE_END_OF_FILE_INFORMATION fei;
    ULONG RetryCount = 0;

    fei.EndOfFile.QuadPart = Fcb->Header.FileSize.QuadPart + Fcb->FileHeaderLength;

    if (FlagOn(Fcb->FcbState, SCB_STATE_FILEHEADER_WRITED)) {
        LARGE_INTEGER TempLI;
        ULONG UnitSize = CRYPT_UNIT;

        TempLI.QuadPart = fei.EndOfFile.QuadPart;
        TempLI.QuadPart += UnitSize;
        TempLI.HighPart += (ULONG)((LONGLONG)UnitSize >> 32);

        if (TempLI.LowPart == 0) {
            TempLI.HighPart -= 1;
        }

        fei.EndOfFile.LowPart = ((ULONG)fei.EndOfFile.LowPart + (UnitSize - 1)) & (~(UnitSize - 1));
        fei.EndOfFile.HighPart = TempLI.HighPart;
        Fcb->ValidDataToDisk.QuadPart = fei.EndOfFile.QuadPart;

        RetryCount = 0;
        while (RetryCount < 3) {
            Status = ModifyFileHeader(FltObjects,
                BooleanFlagOn(Ccb->CcbState, CCB_FLAG_NETWORK_FILE) ? Ccb->StreamFileInfo.StreamObject : Fcb->CcFileObject,
                &Fcb->Header.FileSize,
                Ccb->ProcessGuid,
                &Fcb->FileFullName,
                FILE_MODIFY_SIZE);
            if (NT_SUCCESS(Status)) {
                break;
            }
            RetryCount++;
        }
        if (!NT_SUCCESS(Status)) {
            DbgPrint("ModifyFileHeader false\n");
        }
    }

    RetryCount = 0;
    while (RetryCount < 3) {
        Status = FltSetInformationFile(
            FltObjects->Instance,
            Ccb->StreamFileInfo.StreamObject,
            &fei,
            sizeof(FILE_END_OF_FILE_INFORMATION),
            FileEndOfFileInformation
            );
        if (NT_SUCCESS(Status)) {
            break;
        }
        RetryCount++;
    }

    if (!NT_SUCCESS(Status)) {
        DbgPrint("FltSetInformationFile false\n");
    }
    return Status;
}

NTSTATUS ExtendingSetFile(PCFLT_RELATED_OBJECTS FltObjects, PFCB Fcb, PCCB Ccb)
{
    NTSTATUS Status;
    // �����ļ��Ĵ�С
    FILE_END_OF_FILE_INFORMATION fei;

    ULONG RetryCount = 0;
    fei.EndOfFile.QuadPart = Fcb->Header.FileSize.QuadPart + Fcb->FileHeaderLength;

    if (FlagOn(Fcb->FcbState, SCB_STATE_FILEHEADER_WRITED)) {
        LARGE_INTEGER TempLI;
        ULONG UnitSize = CRYPT_UNIT;

        // ռ�ô�С
        TempLI.QuadPart = fei.EndOfFile.QuadPart;
        TempLI.QuadPart += UnitSize;
        TempLI.HighPart += (ULONG)((LONGLONG)UnitSize >> 32);

        // ����Ҫ��λ
        if (TempLI.LowPart == 0) {
            TempLI.HighPart -= 1;
        }

        fei.EndOfFile.LowPart = ((ULONG)fei.EndOfFile.LowPart + (UnitSize - 1)) & (~(UnitSize - 1));
        fei.EndOfFile.HighPart = TempLI.HighPart;
        Fcb->ValidDataToDisk.QuadPart = fei.EndOfFile.QuadPart;
    }

    // ʧ�����ظ�3��
    while (RetryCount <= 3) {
        Status = FltSetInformationFile(
            FltObjects->Instance,
            Ccb->StreamFileInfo.StreamObject,
            &fei,
            sizeof(FILE_END_OF_FILE_INFORMATION),
            FileEndOfFileInformation
            );
        if (NT_SUCCESS(Status)) {
            break;
        }
        RetryCount++;
    }
    return Status;
}

// ֱ��ת���ļ���ɲ����ܼ����ļ�
NTSTATUS TransformFileToDisEncrypt(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PFCB Fcb, PCCB Ccb)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ob;
    HANDLE TmpHandle;
    IO_STATUS_BLOCK IoStatus;
    UNICODE_STRING TmpFileString = { 0 };
    PFILE_OBJECT TmpObject = NULL;

    PVOID pFileBuffer = NULL;
    LARGE_INTEGER ByteOffset;
    LARGE_INTEGER OrgByteOffset;

    UNICODE_STRING VolumeName = { 0 };
    PFILE_NAME_INFORMATION fni = NULL;
    ULONG LengthReturned = 0;
    ULONG Length = MAX_PATH;
    ULONG i;
    BOOLEAN ScbAcquired = FALSE;
    BOOLEAN PagingIoResourceAcquired = FALSE;
    BOOLEAN PagingIo = FALSE;
    BOOLEAN EncryptResourceAcquired = FALSE;

    WCHAR TmpName[5] = L".tmp";
    ULONG TmpNameLength = sizeof(TmpName) - sizeof(WCHAR);

    /* PagingIo = BooleanFlagOn(Iopb->IrpFlags , IRP_PAGING_IO); */
    ByteOffset.QuadPart = 0;

    if (!Fcb->IsEnFile) {
        return STATUS_SUCCESS;
    }

    try {
        ExAcquireResourceExclusiveLite(Fcb->EncryptResource, TRUE);
        EncryptResourceAcquired = TRUE;

        if (FlagOn(Fcb->FcbState, SCB_STATE_SHADOW_CLOSE)) {
            try_return(Status = STATUS_FILE_DELETED);
        }
        if (!Fcb->IsEnFile) {
            try_return(Status = STATUS_SUCCESS);
        }

        Length = Fcb->FileFullName.Length + TmpNameLength + sizeof(WCHAR);
        TmpFileString.Buffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, Length, 'tfs');
        if (TmpFileString.Buffer == NULL) {
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES;);
        }

        RtlZeroMemory(TmpFileString.Buffer, Length);

        TmpFileString.Length = (USHORT)(Length - sizeof(WCHAR));
        TmpFileString.MaximumLength = (USHORT)Length;

        RtlCopyMemory(TmpFileString.Buffer, Fcb->FileFullName.Buffer, Fcb->FileFullName.Length);
        RtlCopyMemory(Add2Ptr(TmpFileString.Buffer, Fcb->FileFullName.Length), TmpName, TmpNameLength);
        InitializeObjectAttributes(&ob, &TmpFileString, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

        Status = FltCreateFile(FltObjects->Filter,
            FltObjects->Instance,
            &TmpHandle,
            FILE_READ_DATA | FILE_WRITE_DATA | DELETE,
            &ob,
            &IoStatus,
            NULL,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
            FILE_SHARE_VALID_FLAGS,
            FILE_OVERWRITE_IF,
            FILE_DELETE_ON_CLOSE | FILE_NON_DIRECTORY_FILE,
            NULL,
            0,
            IO_IGNORE_SHARE_ACCESS_CHECK
            );

        if (!NT_SUCCESS(Status)) {
            try_return(Status);
        }

        Status = ObReferenceObjectByHandle(TmpHandle,
            0,
            *IoFileObjectType,
            KernelMode,
            &TmpObject,
            NULL);

        if (!NT_SUCCESS(Status)) {
            FltClose(TmpHandle);
            try_return(Status);
        }

        // ���ȴ���һ������buf
        // �ļ�ͷ�Ѿ���������С�����ˣ����Կ϶��򿪿��Գɹ�
        pFileBuffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, FILE_HEADER_LENGTH, 'rh');

        if (pFileBuffer == NULL) {
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }

        // ���ļ����������浽tmp�ļ�����
        OrgByteOffset.QuadPart = FILE_HEADER_LENGTH;
        ByteOffset.QuadPart = 0;
        while (OrgByteOffset.QuadPart < (Fcb->Header.AllocationSize.QuadPart + FILE_HEADER_LENGTH)) {
            RtlZeroMemory(pFileBuffer, FILE_HEADER_LENGTH);

            Status = FltReadFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                &OrgByteOffset,
                FILE_HEADER_LENGTH,
                pFileBuffer,
                /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                NULL,
                NULL,
                NULL
                );
            if (!NT_SUCCESS(Status)) {
                break;
            }
            //for(i = 0 ; i < FILE_HEADER_LENGTH/CRYPT_UNIT ; i++)
            //{
            //  aes_ecb_decrypt(Add2Ptr(pFileBuffer,i*CRYPT_UNIT),Add2Ptr(pFileBuffer,i*CRYPT_UNIT),Fcb->CryptionKey);
            //}
            Status = FltWriteFile(
                FltObjects->Instance,
                TmpObject,
                &ByteOffset,
                FILE_HEADER_LENGTH,
                pFileBuffer,
                /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                NULL,
                NULL,
                NULL
                );
            if (!NT_SUCCESS(Status)) {
                break;
            }
            OrgByteOffset.QuadPart += FILE_HEADER_LENGTH;
            ByteOffset.QuadPart += FILE_HEADER_LENGTH;

        }
        // �����ļ��Ĵ�С�ȵ���Ϣ
        if (!NT_SUCCESS(Status) && Status != STATUS_END_OF_FILE) {
            try_return(Status);
        }

        // �Ѽ����ļ�д��ȥ
        ByteOffset.QuadPart = 0;
        while (ByteOffset.QuadPart < Fcb->Header.AllocationSize.QuadPart) {
            RtlZeroMemory(pFileBuffer, FILE_HEADER_LENGTH);

            Status = FltReadFile(
                FltObjects->Instance,
                TmpObject,
                &ByteOffset,
                FILE_HEADER_LENGTH,
                pFileBuffer,
                /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                NULL,
                NULL,
                NULL
                );
            if (!NT_SUCCESS(Status)) {
                break;
            }

            Status = FltWriteFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                &ByteOffset,
                FILE_HEADER_LENGTH,
                pFileBuffer,
                /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                NULL,
                NULL,
                NULL
                );
            if (!NT_SUCCESS(Status)) {
                break;
            }
            ByteOffset.QuadPart += FILE_HEADER_LENGTH;
        }
        // �����ļ��Ĵ�С
        if (NT_SUCCESS(Status) || Status == STATUS_END_OF_FILE) {
            FILE_END_OF_FILE_INFORMATION fei;
            FILE_ALLOCATION_INFORMATION fai;

            fei.EndOfFile = Fcb->Header.FileSize;

            Status = FltSetInformationFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                &fei,
                sizeof(FILE_END_OF_FILE_INFORMATION),
                FileEndOfFileInformation
                );
            if (!NT_SUCCESS(Status)) {
                try_return(Status);
            }

            fai.AllocationSize = Fcb->Header.AllocationSize;

            Status = FltSetInformationFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                &fai,
                sizeof(FILE_ALLOCATION_INFORMATION),
                FileAllocationInformation
                );
            if (!NT_SUCCESS(Status)) {
                try_return(Status);
            }
        }
try_exit:
        NOTHING;
        if (NT_SUCCESS(Status)) {
            Fcb->IsEnFile = FALSE;
            Fcb->FileHeaderLength = 0;

            ClearFlag(Fcb->FcbState, SCB_STATE_FILEHEADER_WRITED);
            SetFlag(Fcb->FcbState, SCB_STATE_DISCRYPTED_TYPE);
        }
    }
    finally {

        if (ScbAcquired) {
            ExReleaseResourceLite(Fcb->Header.Resource);
        }

        if (PagingIoResourceAcquired) {
            ExReleaseResourceLite(Fcb->Header.PagingIoResource);
        }

        if (EncryptResourceAcquired) {
            ExReleaseResourceLite(Fcb->EncryptResource);
        }

        if (pFileBuffer != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, pFileBuffer, 'rh');
        }
        if (VolumeName.Buffer != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, VolumeName.Buffer, 'von');
        }
        if (fni != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, fni, 'fni');
        }
        if (TmpFileString.Buffer != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, TmpFileString.Buffer, 'fsf');
        }
        if (TmpObject != NULL) {
            FltClose(TmpHandle);
            ObDereferenceObject(TmpObject);
        }
    }

    return Status;
}

// ֱ��ת���ļ���ɼ����ļ�
NTSTATUS TransformFileToEncrypted(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects, PFCB Fcb, PCCB Ccb)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ob;
    HANDLE TmpHandle;
    IO_STATUS_BLOCK IoStatus;
    UNICODE_STRING TmpFileString = { 0 };
    PFILE_OBJECT TmpObject = NULL;

    PVOID pFileBuffer = NULL;
    LARGE_INTEGER ByteOffset;
    LARGE_INTEGER OrgByteOffset;

    UNICODE_STRING VolumeName = { 0 };
    PFILE_NAME_INFORMATION fni = NULL;
    ULONG LengthReturned = 0;
    ULONG Length = MAX_PATH;
    ULONG i;
    BOOLEAN ScbAcquired = FALSE;
    BOOLEAN PagingIoResourceAcquired = FALSE;
    BOOLEAN PagingIo = FALSE;
    BOOLEAN EncryptResourceAcquired = FALSE;

    WCHAR TmpName[5] = L".tmp";
    ULONG TmpNameLength = sizeof(TmpName) - sizeof(WCHAR);

    /* PagingIo = BooleanFlagOn(Iopb->IrpFlags , IRP_PAGING_IO); */
    ByteOffset.QuadPart = 0;

    try {
        ExAcquireResourceExclusiveLite(Fcb->EncryptResource, TRUE);
        EncryptResourceAcquired = TRUE;

        if (FlagOn(Fcb->FcbState, SCB_STATE_SHADOW_CLOSE)) {
            try_return(Status = STATUS_FILE_DELETED);
        }
        if (Fcb->IsEnFile || BooleanFlagOn(Fcb->FcbState, SCB_STATE_DISCRYPTED_TYPE)) {
            try_return(Status = STATUS_SUCCESS);
        }

        // 0��С���ļ�ֱ��д��ȥ����ͷ�Ϳ�����
        if (Fcb->Header.FileSize.QuadPart == 0) {
            // д����ͷ
            Status = WriteFileHeader(FltObjects,
                BooleanFlagOn(Ccb->CcbState, CCB_FLAG_NETWORK_FILE) ? Ccb->StreamFileInfo.StreamObject : Fcb->CcFileObject,
                &Fcb->Header.FileSize, Ccb->ProcessGuid, &Fcb->FileFullName);

            try_return(Status);
        }

        Length = Fcb->FileFullName.Length + TmpNameLength + sizeof(WCHAR);
        TmpFileString.Buffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, Length, 'tfs');

        if (TmpFileString.Buffer == NULL) {
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES;);
        }

        RtlZeroMemory(TmpFileString.Buffer, Length);

        TmpFileString.Length = (USHORT)(Length - sizeof(WCHAR));
        TmpFileString.MaximumLength = (USHORT)Length;

        RtlCopyMemory(TmpFileString.Buffer, Fcb->FileFullName.Buffer, Fcb->FileFullName.Length);
        RtlCopyMemory(Add2Ptr(TmpFileString.Buffer, Fcb->FileFullName.Length), TmpName, TmpNameLength);
        InitializeObjectAttributes(&ob, &TmpFileString, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

        Status = FltCreateFile(FltObjects->Filter,
            FltObjects->Instance,
            &TmpHandle,
            FILE_READ_DATA | FILE_WRITE_DATA | DELETE,
            &ob,
            &IoStatus,
            NULL,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
            FILE_SHARE_VALID_FLAGS,
            FILE_OVERWRITE_IF,
            FILE_DELETE_ON_CLOSE | FILE_NON_DIRECTORY_FILE, //�ò����˹ر���
            NULL,
            0,
            IO_IGNORE_SHARE_ACCESS_CHECK
            );

        if (!NT_SUCCESS(Status)) {
            try_return(Status);
        }

        Status = ObReferenceObjectByHandle(TmpHandle,
            0,
            *IoFileObjectType,
            KernelMode,
            &TmpObject,
            NULL);

        if (!NT_SUCCESS(Status)) {
            FltClose(TmpHandle);
            try_return(Status);
        }

        Status = WriteFileHeader(FltObjects, TmpObject, &Fcb->Header.FileSize, Ccb->ProcessGuid, &Fcb->FileFullName);

        if (!NT_SUCCESS(Status)) {
            try_return(Status);
        }

        // ���ȴ���һ������buf

        // �ļ�ͷ�Ѿ���������С������, ���Կ϶��򿪿��Գɹ�.
        pFileBuffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, PagedPool, FILE_HEADER_LENGTH, 'rh');

        if (pFileBuffer == NULL) {
            try_return(Status = STATUS_INSUFFICIENT_RESOURCES);
        }

        // ���ļ����������浽tmp�ļ�����
        OrgByteOffset.QuadPart = 0;
        ByteOffset.QuadPart += FILE_HEADER_LENGTH;
        while (OrgByteOffset.QuadPart < Fcb->Header.AllocationSize.QuadPart) {
            RtlZeroMemory(pFileBuffer, FILE_HEADER_LENGTH);

            Status = FltReadFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                &OrgByteOffset,
                FILE_HEADER_LENGTH,
                pFileBuffer,
                /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                NULL,
                NULL,
                NULL
                );

            if (!NT_SUCCESS(Status)) {
                break;
            }
            for (i = 0; i < FILE_HEADER_LENGTH / CRYPT_UNIT; i++) {
                aes_ecb_encrypt(Add2Ptr(pFileBuffer, i*CRYPT_UNIT), Add2Ptr(pFileBuffer, i*CRYPT_UNIT), &Fcb->CryptionKey);
            }
            Status = FltWriteFile(
                FltObjects->Instance,
                TmpObject,
                &ByteOffset,
                FILE_HEADER_LENGTH,
                pFileBuffer,
                /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                NULL,
                NULL,
                NULL
                );
            if (!NT_SUCCESS(Status)) {
                break;
            }
            OrgByteOffset.QuadPart += FILE_HEADER_LENGTH;
            ByteOffset.QuadPart += FILE_HEADER_LENGTH;

        }
        // �����ļ��Ĵ�С�ȵ���Ϣ
        if (!NT_SUCCESS(Status) && Status != STATUS_END_OF_FILE) {
            try_return(Status);
        }

        // �Ѽ����ļ�д��ȥ
        ByteOffset.QuadPart = 0;
        while (ByteOffset.QuadPart < (Fcb->Header.AllocationSize.QuadPart + FILE_HEADER_LENGTH)) {
            RtlZeroMemory(pFileBuffer, FILE_HEADER_LENGTH);

            Status = FltReadFile(
                FltObjects->Instance,
                TmpObject,
                &ByteOffset,
                FILE_HEADER_LENGTH,
                pFileBuffer,
                /*FLTFL_IO_OPERATION_NON_CACHED | */FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                NULL,
                NULL,
                NULL
                );

            if (!NT_SUCCESS(Status)) {
                break;
            }

            Status = FltWriteFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                &ByteOffset,
                FILE_HEADER_LENGTH,
                pFileBuffer,
                /*FLTFL_IO_OPERATION_NON_CACHED |*/  FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET, //�ǻ���Ĵ�
                NULL,
                NULL,
                NULL
                );
            if (!NT_SUCCESS(Status)) {
                break;
            }
            ByteOffset.QuadPart += FILE_HEADER_LENGTH;
        }

        // �����ļ��Ĵ�С
        if (NT_SUCCESS(Status) || Status == STATUS_END_OF_FILE) {
            FILE_END_OF_FILE_INFORMATION fei;
            FILE_ALLOCATION_INFORMATION fai;
            LARGE_INTEGER TempLI;
            ULONG UnitSize = CRYPT_UNIT;

            fei.EndOfFile = Fcb->Header.FileSize;
            fei.EndOfFile.QuadPart += FILE_HEADER_LENGTH;

            TempLI.QuadPart = fei.EndOfFile.QuadPart;//ռ�ô�С
            TempLI.QuadPart += UnitSize;
            TempLI.HighPart += (ULONG)((LONGLONG)UnitSize >> 32);

            // ����Ҫ��λ
            if (TempLI.LowPart == 0) {
                TempLI.HighPart -= 1;
            }

            fei.EndOfFile.LowPart = ((ULONG)fei.EndOfFile.LowPart + (UnitSize - 1)) & (~(UnitSize - 1));
            fei.EndOfFile.HighPart = TempLI.HighPart;
            Fcb->ValidDataToDisk.QuadPart = fei.EndOfFile.QuadPart;

            Status = FltSetInformationFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                &fei,
                sizeof(FILE_END_OF_FILE_INFORMATION),
                FileEndOfFileInformation
                );
            if (!NT_SUCCESS(Status)) {
                try_return(Status);
            }

            fai.AllocationSize = Fcb->ValidDataToDisk;

            Status = FltSetInformationFile(
                FltObjects->Instance,
                Ccb->StreamFileInfo.StreamObject,
                &fai,
                sizeof(FILE_ALLOCATION_INFORMATION),
                FileAllocationInformation
                );
            if (!NT_SUCCESS(Status)) {
                try_return(Status);
            }
        }
try_exit:
        NOTHING;
        if (NT_SUCCESS(Status) && !BooleanFlagOn(Fcb->FcbState, SCB_STATE_DISCRYPTED_TYPE)) {
            Fcb->IsEnFile = TRUE;
            Fcb->FileHeaderLength = FILE_HEADER_LENGTH;
            SetFlag(Fcb->FcbState, SCB_STATE_FILEHEADER_WRITED);
        }
    }
    finally {
        if (ScbAcquired) {
            ExReleaseResourceLite(Fcb->Header.Resource);
        }

        if (PagingIoResourceAcquired) {
            ExReleaseResourceLite(Fcb->Header.PagingIoResource);
        }

        if (EncryptResourceAcquired) {
            ExReleaseResourceLite(Fcb->EncryptResource);
        }

        if (pFileBuffer != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, pFileBuffer, 'rh');
        }
        if (VolumeName.Buffer != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, VolumeName.Buffer, 'von');
        }
        if (fni != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, fni, 'fni');
        }
        if (TmpFileString.Buffer != NULL) {
            FltFreePoolAlignedWithTag(FltObjects->Instance, TmpFileString.Buffer, 'fsf');
        }
        if (TmpObject != NULL) {
            FltClose(TmpHandle);
            ObDereferenceObject(TmpObject);
        }
    }

    return Status;
}
