/** @file
  OpenCore driver.

Copyright (c) 2019, vit9696. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <OpenCore.h>
#include <Uefi.h>

#include <Guid/OcLogVariable.h>

#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/OcBootstrap.h>
#include <Protocol/SimpleFileSystem.h>

#include <Library/DebugLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleBootPolicyLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcConfigurationLib.h>
#include <Library/OcCpuLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcStorageLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

STATIC
OC_GLOBAL_CONFIG
mOpenCoreConfiguration;

STATIC
OC_STORAGE_CONTEXT
mOpenCoreStorage;

STATIC
EFI_IMAGE_START
mOcOriginalStartImage;

STATIC
UINT32
mOpenCoreStartImageNest;

STATIC
BOOLEAN
mOpenCoreStartImageIsWindows;

STATIC
RSA_PUBLIC_KEY *
mOpenCoreVaultKey;

STATIC
VOID
OcEfiStartImagePrologue (
  IN BOOLEAN  IsUnknown,
  IN BOOLEAN  IsWindows
  )
{
  ++mOpenCoreStartImageNest;

  if (mOpenCoreStartImageNest == 1) {
    if (IsUnknown) {
      IsWindows = mOpenCoreStartImageIsWindows;
    } else {
      mOpenCoreStartImageIsWindows = IsWindows;
    }

    //
    // Some people make their ACPI tables incompatible with Windows after modding them for macOS.
    // While obviously it is their fault, here we provide a quick and dirty workaround.
    //
    if (!mOpenCoreConfiguration.Acpi.Quirks.IgnoreForWindows || !IsWindows) {
      OcLoadAcpiSupport (&mOpenCoreStorage, &mOpenCoreConfiguration);
    }

    //
    // Do not waste time for kext injection, when we are in Windows.
    //
    if (!IsWindows) {
      OcLoadKernelSupport (&mOpenCoreStorage, &mOpenCoreConfiguration);
    }
  }
}

STATIC
VOID
OcEfiStartImageEpilogue (
  VOID
  )
{
  if (mOpenCoreStartImageNest == 1) {
    if (!mOpenCoreStartImageIsWindows) {
      OcUnloadKernelSupport ();
    }

    mOpenCoreStartImageIsWindows = FALSE;
  }

  --mOpenCoreStartImageNest;
}

STATIC
EFI_STATUS
EFIAPI
OcEfiStartImage (
  IN  EFI_HANDLE                  ImageHandle,
  OUT UINTN                       *ExitDataSize,
  OUT CHAR16                      **ExitData    OPTIONAL
  )
{
  EFI_STATUS   Status;

  //
  // We do not know what OS is that, could be macOS booted from shell.
  //
  OcEfiStartImagePrologue (TRUE, FALSE);

  Status = mOcOriginalStartImage (
    ImageHandle,
    ExitDataSize,
    ExitData
    );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OC: Boot failed - %r\n", Status));
  }

  OcEfiStartImageEpilogue ();

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
OcStartImage (
  IN  OC_BOOT_ENTRY               *Chosen,
  IN  EFI_HANDLE                  ImageHandle,
  OUT UINTN                       *ExitDataSize,
  OUT CHAR16                      **ExitData    OPTIONAL
  )
{
  EFI_STATUS   Status;

  OcEfiStartImagePrologue (FALSE, Chosen->IsWindows);

  Status = mOcOriginalStartImage (
    ImageHandle,
    ExitDataSize,
    ExitData
    );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OC: Boot failed - %r\n", Status));
  }

  OcEfiStartImageEpilogue ();

  return Status;
}

STATIC
VOID
OcStoreLoadPath (
  IN EFI_DEVICE_PATH_PROTOCOL  *LoadPath OPTIONAL
  )
{
  EFI_STATUS  Status;
  CHAR16      *DevicePath;
  CHAR8       OutPath[256];

  if (LoadPath != NULL) {
    DevicePath = ConvertDevicePathToText (LoadPath, FALSE, FALSE);
    if (DevicePath != NULL) {
      AsciiSPrint (OutPath, sizeof (OutPath), "%s", DevicePath);
      FreePool (DevicePath);
    } else {
      LoadPath = NULL;
    }
  }

  if (LoadPath == NULL) {
    AsciiSPrint (OutPath, sizeof (OutPath), "Unknown");
  }

  Status = gRT->SetVariable (
    OC_LOG_VARIABLE_PATH,
    &gOcLogVariableGuid,
    OPEN_CORE_NVRAM_ATTR,
    AsciiStrSize (OutPath),
    OutPath
    );

  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_WARN : DEBUG_INFO,
    "OC: Setting NVRAM %g:%a = %a - %r\n",
    &gOcLogVariableGuid,
    OC_LOG_VARIABLE_PATH,
    OutPath,
    Status
    ));
}

STATIC
EFI_STATUS
OcMiscEarlyInit (
  IN OC_STORAGE_CONTEXT        *Storage,
  IN OC_GLOBAL_CONFIG          *Configuration
  )
{
  EFI_STATUS                Status;
  CHAR8                     *Config;
  UINT32                    ConfigSize;

  Config = OcStorageReadFileUnicode (
    Storage,
    OPEN_CORE_CONFIG_PATH,
    &ConfigSize
    );

  if (Config != NULL) {
    DEBUG ((DEBUG_INFO, "OC: Loaded configuration of %u bytes\n", ConfigSize));

    Status = OcConfigurationInit (Configuration, Config, ConfigSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OC: Failed to parse configuration!\n"));
      CpuDeadLoop ();
      return EFI_UNSUPPORTED; ///< Should be unreachable.
    }

    FreePool (Config);
  } else {
    DEBUG ((DEBUG_ERROR, "OC: Failed to load configuration!\n"));
    CpuDeadLoop ();
    return EFI_UNSUPPORTED; ///< Should be unreachable.
  }

  //
  // Sanity check that the configuration is adequate.
  //
  if (!Storage->HasVault && Configuration->Misc.Security.RequireVault) {
    DEBUG ((DEBUG_ERROR, "OC: Configuration requires vault but no vault provided!\n"));
    CpuDeadLoop ();
    return EFI_SECURITY_VIOLATION; ///< Should be unreachable.
  }

  if (mOpenCoreVaultKey == NULL && Configuration->Misc.Security.RequireSignature) {
    DEBUG ((DEBUG_ERROR, "OC: Configuration requires signed vault but no public key provided!\n"));
    CpuDeadLoop ();
    return EFI_SECURITY_VIOLATION; ///< Should be unreachable.
  }

  OcConfigureLogProtocol (
    Configuration->Misc.Debug.Target,
    Configuration->Misc.Debug.Delay,
    (UINTN) Configuration->Misc.Debug.DisplayLevel,
    (UINTN) Configuration->Misc.Security.HaltLevel
    );

  DEBUG ((
    DEBUG_INFO,
    "OC: OpenCore is now loading (Vault: %d/%d, Sign %d/%d)...\n",
    Storage->HasVault,
    Configuration->Misc.Security.RequireVault,
    mOpenCoreVaultKey != NULL,
    Configuration->Misc.Security.RequireSignature
    ));

  return EFI_SUCCESS;
}

STATIC
VOID
OcMain (
  IN OC_STORAGE_CONTEXT        *Storage,
  IN EFI_DEVICE_PATH_PROTOCOL  *LoadPath OPTIONAL
  )
{
  EFI_STATUS                Status;

  OC_CPU_INFO               CpuInfo;
  EFI_HANDLE                LoadHandle;

  Status = OcMiscEarlyInit (Storage, &mOpenCoreConfiguration);
  if (EFI_ERROR (Status)) {
    return;
  }

  OcCpuScanProcessor (&CpuInfo);

  DEBUG ((DEBUG_INFO, "OC: OcLoadUefiSupport...\n"));
  OcLoadUefiSupport (Storage, &mOpenCoreConfiguration, &CpuInfo);
  DEBUG ((DEBUG_INFO, "OC: OcLoadPlatformSupport...\n"));
  OcLoadPlatformSupport (&mOpenCoreConfiguration, &CpuInfo);
  DEBUG ((DEBUG_INFO, "OC: OcLoadDevPropsSupport...\n"));
  OcLoadDevPropsSupport (&mOpenCoreConfiguration);
  DEBUG ((DEBUG_INFO, "OC: OcLoadNvramSupport...\n"));
  OcLoadNvramSupport (&mOpenCoreConfiguration);

  if (mOpenCoreConfiguration.Misc.Debug.ExposeBootPath) {
    OcStoreLoadPath (LoadPath);
  }

  if (mOpenCoreConfiguration.Misc.Boot.ReinstallProtocol) {
    if (OcAppleBootPolicyInstallProtocol (TRUE) == NULL) {
      DEBUG ((DEBUG_ERROR, "OC: Failed to reinstall boot policy protocol\n"));
    }
  }

  LoadHandle = NULL;
  if (LoadPath != NULL) {
    Status = gBS->LocateDevicePath (
      &gEfiSimpleFileSystemProtocolGuid,
      &LoadPath,
      &LoadHandle
      );
    DEBUG ((DEBUG_INFO, "OC: LoadHandle is %p - %r\n", LoadHandle, Status));
  }

  //
  // This is required to catch UEFI Shell boot if any.
  // We do it as late as possible to let other drivers install their hooks.
  //
  mOcOriginalStartImage = gBS->StartImage;
  gBS->StartImage       = OcEfiStartImage;
  gBS->Hdr.CRC32        = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);

  DEBUG ((DEBUG_INFO, "OC: OpenCore is loaded, showing boot menu...\n"));

  //
  // Do not hide self entry unless asked.
  //
  if (!mOpenCoreConfiguration.Misc.Boot.HideSelf) {
    LoadHandle = NULL;
  }

  Status = OcRunSimpleBootPicker (
    OC_SCAN_DEFAULT_POLICY,
    OC_LOAD_DEFAULT_POLICY,
    mOpenCoreConfiguration.Misc.Boot.Timeout,
    OcStartImage,
    mOpenCoreConfiguration.Misc.Boot.ShowPicker,
    LoadHandle
    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to show boot menu!\n"));
  }
}

STATIC
VOID
EFIAPI
OcBootstrapRerun (
  IN OC_BOOTSTRAP_PROTOCOL            *This,
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem,
  IN EFI_DEVICE_PATH_PROTOCOL         *LoadPath OPTIONAL
  )
{
  EFI_STATUS          Status;

  DEBUG ((DEBUG_INFO, "OC: ReRun executed!\n"));

  ++This->NestedCount;

  if (This->NestedCount == 1) {
    mOpenCoreVaultKey = OcGetVaultKey (This);

    Status = OcStorageInitFromFs (
      &mOpenCoreStorage,
      FileSystem,
      OPEN_CORE_ROOT_PATH,
      mOpenCoreVaultKey
      );

    if (!EFI_ERROR (Status)) {
      OcMain (&mOpenCoreStorage, LoadPath);
      OcStorageFree (&mOpenCoreStorage);
    } else {
      DEBUG ((DEBUG_ERROR, "OC: Failed to open root FS - %r!\n", Status));
      if (Status == EFI_SECURITY_VIOLATION) {
        CpuDeadLoop (); ///< Should not return.
      }
    }
  } else {
    DEBUG ((DEBUG_ERROR, "OC: Nested ReRun is not supported\n"));
  }

  --This->NestedCount;
}

STATIC
OC_BOOTSTRAP_PROTOCOL
mOpenCoreBootStrap = {
  .Revision    = OC_BOOTSTRAP_PROTOCOL_REVISION,
  .NestedCount = 0,
  .VaultKey    = NULL,
  .ReRun       = OcBootstrapRerun
};

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                        Status;
  EFI_LOADED_IMAGE_PROTOCOL         *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *FileSystem;
  EFI_HANDLE                        BootstrapHandle;
  OC_BOOTSTRAP_PROTOCOL             *Bootstrap;
  EFI_DEVICE_PATH_PROTOCOL          *AbsPath;

  DEBUG ((DEBUG_INFO, "OC: Starting OpenCore...\n"));

  //
  // We have just started by bootstrap or manually at EFI/OC/OpenCore.efi.
  // When bootstrap runs us, we only install the protocol.
  // Otherwise we do self start.
  //

  Bootstrap = NULL;
  Status = gBS->LocateProtocol (
    &gOcBootstrapProtocolGuid,
    NULL,
    (VOID **) &Bootstrap
    );

  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OC: Found previous image, aborting\n"));
    return EFI_ALREADY_STARTED;
  }

  LoadedImage = NULL;
  Status = gBS->HandleProtocol (
    ImageHandle,
    &gEfiLoadedImageProtocolGuid,
    (VOID **) &LoadedImage
    );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to locate loaded image - %r\n", Status));
    return EFI_NOT_FOUND;
  }

  BootstrapHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
    &BootstrapHandle,
    &gOcBootstrapProtocolGuid,
    &mOpenCoreBootStrap,
    NULL
    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to install bootstrap protocol - %r\n", Status));
    return Status;
  }

  DebugPrintDevicePath (DEBUG_INFO, "OC: Booter path", LoadedImage->FilePath);

  if (LoadedImage->FilePath == NULL) {
    DEBUG ((DEBUG_INFO, "OC: Booted from bootstrap\n"));
    return EFI_SUCCESS;
  }

  //
  // Obtain the file system device path
  //
  FileSystem = LocateFileSystem (
    LoadedImage->DeviceHandle,
    LoadedImage->FilePath
    );

  AbsPath = AbsoluteDevicePath (LoadedImage->DeviceHandle, LoadedImage->FilePath);

  //
  // Return success in either case to let rerun work afterwards.
  //
  if (FileSystem != NULL) {
    mOpenCoreBootStrap.ReRun (&mOpenCoreBootStrap, FileSystem, AbsPath);
    DEBUG ((DEBUG_ERROR, "OC: Failed to boot\n"));
  } else {
    DEBUG ((DEBUG_ERROR, "OC: Failed to locate file system\n"));
  }

  if (AbsPath != NULL) {
    FreePool (AbsPath);
  }

  return EFI_SUCCESS;
}
