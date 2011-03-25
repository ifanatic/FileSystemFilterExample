//#include <ntddk.h>

// #include <ntifs.h>

#include "sfmSysFreezer.h"


//*****************    GLOBAL VARIABLES    ******************

PDRIVER_OBJECT 	gsfmSysFreezerDriverObject = NULL;
PDEVICE_OBJECT	gGuiDevice = NULL;
PDEVICE_OBJECT	gDriveHookDevices[26];

ULONG		 	gCurrentDriveSet = 0;

FAST_IO_DISPATCH gFastIoDispatch =
{
    sizeof(FAST_IO_DISPATCH),
    FsFilterFastIoCheckIfPossible,
    FsFilterFastIoRead,
    FsFilterFastIoWrite,
    FsFilterFastIoQueryBasicInfo,
    FsFilterFastIoQueryStandardInfo,
    FsFilterFastIoLock,
    FsFilterFastIoUnlockSingle,
    FsFilterFastIoUnlockAll,
    FsFilterFastIoUnlockAllByKey,
    FsFilterFastIoDeviceControl,
    NULL,
    NULL,
    FsFilterFastIoDetachDevice,
    FsFilterFastIoQueryNetworkOpenInfo,
    NULL,
    FsFilterFastIoMdlRead,
    FsFilterFastIoMdlReadComplete,
    FsFilterFastIoPrepareMdlWrite,
    FsFilterFastIoMdlWriteComplete,
    FsFilterFastIoReadCompressed,
    FsFilterFastIoWriteCompressed,
    FsFilterFastIoMdlReadCompleteCompressed,
    FsFilterFastIoMdlWriteCompleteCompressed,
    FsFilterFastIoQueryOpen,
    NULL,
    NULL,
    NULL,
};

//

NTSTATUS DriverEntry(IN PDRIVER_OBJECT pDriverobject, IN PUNICODE_STRING pUnicodeString);

NTSTATUS SSFPassThrough(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);


BOOLEAN HookDrive(IN ULONG drive, IN PDRIVER_OBJECT pDriverObject);
VOID UnhookDrive(IN ULONG drive);
ULONG HookDriveSet(IN ULONG driveSet, IN PDRIVER_OBJECT driverObject);

//*******************    IMPLEMENTS    **********************

NTSTATUS DriverEntry(IN PDRIVER_OBJECT pDriverObject, IN PUNICODE_STRING pUnicodeString)
{
	UNICODE_STRING	ntDeviceName;
	UNICODE_STRING	dosDeviceName;
	ULONG			i;
	NTSTATUS		status = STATUS_SUCCESS;
	
	gsfmSysFreezerDriverObject = pDriverObject;
	
	#ifdef DBG
	//pDriverObject->DriverUnload = DriverUnload;
	#endif
	
	for(i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
	{
		pDriverObject->MajorFunction[i] = SSFPassThrough;
	}
	
	pDriverObject->FastIoDispatch = &gFastIoDispatch;
	
	// Создадим объект устройства для общения с нашим драйвером
	
	RtlInitUnicodeString(&ntDeviceName, NT_DEVICE_NAME);
	RtlInitUnicodeString(&dosDeviceName, DOS_DEVICE_NAME);
	
	status = IoCreateDevice(pDriverObject,
							sizeof(SYS_FREEZER_DEVICE_EXTENSTION),
							&ntDeviceName,
							FILE_DEVICE_UNKNOWN,
							0,
							FALSE,
							&gGuiDevice);
	if(!NT_SUCCESS(status))
	{
		DBG_PRINT_ERR(L"Cannot create gui device");
		return status;
	}
	
	((PSYS_FREEZER_DEVICE_EXTENSION)gGuiDevice->DeviceExtension)->Type = GUIINTERFACE;
	
	status = IoCreateSymbolicLink(&dosDeviceName, &ntDeviceName);
	
	if(!NT_SUCCESS(status))
	{
		DBG_PRINT_ERR(L"cannot create symbolic link");
		
		IoDeleteDevice(&gGuiDevice);
		
		return status;
	}
	
	for(i = 0; i < 26; ++i)
	{
		gDriveHookDevices[i] = NULL;
	}
	
	HookDriveSet(4, gsfmSysFreezerDriverObject);	
	
	return STATUS_SUCCESS;
}

BOOLEAN HookDrive(IN ULONG drive, IN PDRIVER_OBJECT driverObject)
{
	IO_STATUS_BLOCK 	ioStatus;
	HANDLE				ntFileHandle;
	OBJECT_ATTRIBUTES	objectAttributes;
	PDEVICE_OBJECT      fileSysDevice;
    PDEVICE_OBJECT      hookDevice;
    UNICODE_STRING      fileNameUnicodeString;
    ULONG               fileFsAttributesSize;
    WCHAR               fileName[] = L"\\DosDevices\\A:\\";
    NTSTATUS            ntStatus;
    ULONG               i;
    PFILE_OBJECT        fileObject;
    PSYS_FREEZER_DEVICE_EXTENSION	devExt;
    
    if(drive >= 26)
    {
    	return FALSE;
    }
    
    if(gDriveHookDevices[drive] == NULL)
    {
		fileName[12] = (CHAR)('A' + drive);
		
		RtlInitUnicodeString(&fileNameUnicodeString, fileName);
		
		InitializeObjectAttributes(&objectAttributes,
									&fileNameUnicodeString,
									OBJ_CASE_INSENSITIVE,
									NULL, 
									NULL);
									
		ntStatus = ZwCreateFile(&ntFileHandle,
								SYNCHRONIZE | FILE_ANY_ACCESS,
								&objectAttributes,
								&ioStatus,
								NULL,
								0, 
								FILE_SHARE_READ | FILE_SHARE_WRITE,
								FILE_OPEN,
								FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE,
								NULL,
								0);
								
		if(!NT_SUCCESS(ntStatus))
		{
			DBG_PRINT_ERR(L"Could open drive");
			return FALSE;
		}
		
		DBG_PRINT_INFO(L"opened root directory");
		
		ntStatus = ObReferenceObjectByHandle(ntFileHandle, 
											FILE_READ_DATA,
											NULL,
											KernelMode,
											&fileObject,
											NULL);
		if(!NT_SUCCESS(ntStatus))
		{
			DBG_PRINT_ERR(L"Could not get object by handle");
			
			ZwClose(ntFileHandle);
			
			return FALSE;
		}
		
		fileSysDevice = IoGetRelatedDeviceObject(fileObject);
		
		if(!fileSysDevice)
		{
			DBG_PRINT_ERR(L"Could not get related device object");
			
			ObDereferenceObject(fileObject);
			
			ZwClose(ntFileHandle);
			
			return FALSE;
		}
		
		for(i = 0; i < 26; ++i)
		{
			if(gDriveHookDevices[i] == fileSysDevice)
			{
				ObDereferenceObject(fileObject);

				ZwClose(ntFileHandle);

				gDriveHookDevices[drive] = fileSysDevice;

				return TRUE;				
			}
		}
		
		ntStatus = IoCreateDevice(driverObject,
								sizeof(SYS_FREEZER_DEVICE_EXTENSTION),
								NULL,
								fileSysDevice->DeviceType,
								0,
								FALSE,
								&hookDevice);
		if(!NT_SUCCESS(ntStatus))
		{
			DBG_PRINT_ERR(L"Failed: to create associated device");
			
			ObDereferenceObject(fileObject);
			
			ZwClose(ntFileHandle);
			
			return FALSE;
		}
		
		hookDevice->Flags &= ~DO_DEVICE_INITIALIZING;
		
		devExt = hookDevice->DeviceExtension;
		devExt->AttachedToDeviceObject = fileSysDevice;
		devExt->DriveLetter = (CHAR)('A' + drive);
		devExt->Hooked		= TRUE;
		devExt->Type		= STANDART;
		
		ntStatus = IoAttachDeviceByPointer(hookDevice, fileSysDevice);
		
		if(!NT_SUCCESS(ntStatus))
		{
			DBG_PRINT_ERR(L"Failed: attaching to device");
			
			//IoDeleteDevice(hookDevice);
			
			ObDereferenceObject(fileObject);
			
			ZwClose(ntFileHandle);
			
			return FALSE;
		}		
		else
		{
			DBG_PRINT_INFO(L"Successfully connected to file system device");
		}
		
		ObDereferenceObject(fileObject);
		
		ZwClose(ntFileHandle);
		
		gDriveHookDevices[drive] = hookDevice;
	}
	else
	{
		devExt = gDriveHookDevices[drive]->DeviceExtension;
		devExt->Hooked = TRUE;
	}
	
	return TRUE;
}

VOID UnhookDrive(IN ULONG drive)
{
	PSYS_FREEZER_DEVICE_EXTENSION devExt;
	
	if(gDriveHookDevices[drive])
	{
		devExt = gDriveHookDevices[drive]->DeviceExtension;
		devExt->Hooked = FALSE;
	}
}

ULONG HookDriveSet(IN ULONG driveSet, IN PDRIVER_OBJECT driverObject)
{
	//TODO Обратить внимание!!!!!!!!!!!
	
	PSYS_FREEZER_DEVICE_EXTENSION devExt;
	ULONG drive, i;
	ULONG result = 0;
	ULONG mask = 1;
	
	for(drive = 0; drive < 26; ++drive)
	{
		if(((driveSet >> drive) & mask) && !((gCurrentDriveSet >> drive) & mask))
		{
			if(HookDrive(drive, driverObject))
			{
				for(i = 0; i < 26; ++i)
				{
					if(gDriveHookDevices[drive] == gDriveHookDevices[i])
					{
						result = result | (1 << drive);
					}
				}
			}
		}
		else if(!((driveSet >> drive) & mask) && ((gCurrentDriveSet >> drive) & mask))
		{
			UnhookDrive(drive);
		}
	}
	
	gCurrentDriveSet = result;
	return result;
}

NTSTATUS SSFPassThrough(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
	PIO_STACK_LOCATION stack;
	PSYS_FREEZER_DEVICE_EXTENSION devExt;
	
	stack = IoGetCurrentIrpStackLocation(pIrp);
	devExt = (PSYS_FREEZER_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
	
	switch(stack->MajorFunction)
	{
	case IRP_MJ_CREATE:			
		DbgPrint("sfmSysFreezer: IRP_MJ_CREATE - file: %c:%wZ\n", devExt->DriveLetter, &stack->FileObject->FileName);
	break;
	
	case IRP_MJ_CLOSE:
		DbgPrint("sfmSysFreezer: IRP_MJ_CLOSE - file: %c:%wZ\n", devExt->DriveLetter, &stack->FileObject->FileName);
	break;
	
	case IRP_MJ_READ:
		DbgPrint("sfmSysFreezer: IRP_MJ_READ - file: %c:%wZ\n", devExt->DriveLetter, &stack->FileObject->FileName);
	break;
	
	case IRP_MJ_WRITE:
		DbgPrint("sfmSysFreezer: IRP_MJ_WRITE - file: %c:%wZ\n", devExt->DriveLetter, &stack->FileObject->FileName);
	break;
	}
	
	IoSkipCurrentIrpStackLocation(pIrp);
	
	return IoCallDriver(devExt->AttachedToDeviceObject, pIrp);
}


