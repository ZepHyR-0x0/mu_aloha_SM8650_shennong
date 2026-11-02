/** @file

  Implements the UEFI Front Page (Settings Menu).

  Copyright (C) Microsoft Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <UefiSecureBoot.h>
#include "FrontPage.h"
#include "String.h"
#include "FrontPageUi.h"
#include "FrontPageConfigAccess.h"

#include <IndustryStandard/SmBios.h>

#include <Guid/GlobalVariable.h>
#include <Guid/MsBootMenuGuid.h>
#include <Guid/MdeModuleHii.h>
#include <Guid/DebugImageInfoTable.h>
#include <Guid/MsNVBootReason.h>
#include <Guid/DfciMenuGuid.h>
#include <Guid/HwhMenuGuid.h>
#include <Guid/ImageAuthentication.h>

#include <Pi/PiFirmwareFile.h>

#include <Protocol/GraphicsOutput.h>
#include "QrCode.h"
#include "BootLog.h"
#include "BootMenu.h"
#include "RemoteControl.h"
#include <Protocol/Smbios.h>
#include <Protocol/OnScreenKeyboard.h>
#include <Protocol/SimpleWindowManager.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/FirmwareManagement.h>
#include <Protocol/VariablePolicy.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/SerialPortLib.h>
#include <Library/FrameBufferSerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/DxeServicesLib.h>
#include <PiDxe.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BmpSupportLib.h>
#include <Library/BootGraphicsProviderLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/GraphicsConsoleHelperLib.h>
#include <Library/MsUiThemeLib.h>
#include <Library/ResetUtilityLib.h>
#include <Library/MsColorTableLib.h>
#include <Library/MsNVBootReasonLib.h>
#include <Library/SecureBootVariableLib.h>
#include <Library/MuSecureBootKeySelectorLib.h>
#include <Library/SecureBootKeyStoreLib.h>
#include <Library/SwmDialogsLib.h>

#include <MsDisplayEngine.h>
#include <UIToolKit/SimpleUIToolKit.h>
#include <Library/IoLib.h>
#include <Protocol/EFIUsbDevice.h>
#include <Protocol/UsbBringup.h>
#include <Protocol/FirmwareInput.h>
#include <Configuration/BootDevices.h>

//
// PORT: breadcrumbs.
//
// DEBUG() is compiled out under MDEPKG_NDEBUG, so when FrontPage returns
// EFI_UNSUPPORTED there is no way to tell which of its three protocol lookups
// failed. All three providers (FontDxe, SimpleWindowManagerDxe,
// OnScreenKeyboardDxe) are in the FV, so the interesting question is which one
// did not actually publish. These lines go straight to the framebuffer console.
//
STATIC
VOID
FpPortSay (
  IN CONST CHAR8  *Text
  )
{
  SerialPortInitialize ();
  SerialPortWrite ((UINT8 *)Text, AsciiStrLen (Text));
}

//
// The unified screen log path (FrameBufferSerialPortLib) stays silent unless
// this sets verbose. The boot menu and GUI are drawn through SWM, so the few
// seconds before UefiMain are deliberately blank unless volume-up was held.
//
STATIC
VOID
FpDetectVerbose (
  VOID
  )
{
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *In    = NULL;
  EFI_DEVICE_PATH_PROTOCOL           *Path;
  EFI_HANDLE                          Handle = NULL;
  EFI_KEY_DATA                        Key;
  EFI_GUID                            InputGuid = FIRMWARE_INPUT_PROTOCOL_GUID;
  FIRMWARE_INPUT_PROTOCOL             *FirmwareInput = NULL;

  // Preserve the early DXE selection, without draining the keypad again.
  if (FbConGetVerbose ()) {
    return;
  }
  // The DXE sampler owns boot-key detection and preserves other buffered keys.
  // Do not drain its input wrapper a second time when FrontPage starts.
  if (!EFI_ERROR (gBS->LocateProtocol (&InputGuid, NULL, (VOID **)&FirmwareInput)) &&
      FirmwareInput != NULL) {
    return;
  }

  //
  // gST->ConsoleInHandle is SerialTextConsoleDxe, whose SimpleTextIn never
  // reports a key. The real volume keys come from the prebuilt ButtonsDxe,
  // which installs SimpleTextInputEx on the keypad device path.
  //
  Path = (EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath;
  if (EFI_ERROR (gBS->LocateDevicePath (
                    &gEfiSimpleTextInputExProtocolGuid,
                    &Path,
                    &Handle
                    )) ||
      (Handle == NULL) ||
      EFI_ERROR (gBS->HandleProtocol (
                    Handle,
                    &gEfiSimpleTextInputExProtocolGuid,
                    (VOID **)&In
                    )) ||
      (In == NULL))
  {
    return;
  }

  // Fallback for profiles without VerboseKeyDxe. Sample buffered input once;
  // the normal profile already samples asynchronously during DXE.
  if (In->ReadKeyStrokeEx (In, &Key) == EFI_SUCCESS && Key.Key.ScanCode == SCAN_UP) {
    FbConSetVerbose (TRUE);
  }
}

//
// Defined much further down, but the DIAG menu entry calls it, and that entry
// lives above the definition.
//
STATIC VOID FpApplyUsbMode (VOID);

#define FP_OSK_WIDTH_PERCENT  100           // Full screen width, transformed key hit areas.

UINTN       mCallbackKey;
EFI_HANDLE  mImageHandle;

// Protocols.
//
EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;
EFI_HII_FONT_PROTOCOL         *mFont;

// UI Elements.
//
UINT32   mTitleBarWidth, mTitleBarHeight;
UINT32   mMasterFrameWidth, mMasterFrameHeight;
ListBox  *mTopMenu;
BOOLEAN  mShowFullMenu = FALSE;     // By default we won't show the full FrontPage menu (requires validation if there's a system password).

// Master Frame - Form Notifications.
//
UINT32                           mCurrentFormIndex;
EFI_EVENT                        mMasterFrameNotifyEvent;
DISPLAY_ENGINE_SHARED_STATE      mDisplayEngineState;
BOOLEAN                          mTerminateFrontPage = FALSE;
BOOLEAN                          mResetRequired;
FRONT_PAGE_AUTH_TOKEN_PROTOCOL   *mFrontPageAuthTokenProtocol = NULL;
DFCI_AUTHENTICATION_PROTOCOL     *mAuthProtocol               = NULL;
EFI_HII_CONFIG_ROUTING_PROTOCOL  *mHiiConfigRouting;
DFCI_SETTING_ACCESS_PROTOCOL     *mSettingAccess;
DFCI_AUTH_TOKEN                  mAuthToken;
SECURE_BOOT_PAYLOAD_INFO         *mSecureBootKeys     = NULL;
UINT8                            mSecureBootKeysCount = 0;

extern EFI_HII_HANDLE  gStringPackHandle;
extern EFI_GUID        gMsEventMasterFrameNotifyGroupGuid;

//
// Boot video resolution and text mode.
//
UINT32  mBootHorizontalResolution = 0;
UINT32  mBootVerticalResolution   = 0;

EFI_FORM_BROWSER2_PROTOCOL         *mFormBrowser2;
MS_ONSCREEN_KEYBOARD_PROTOCOL      *mOSKProtocol;
MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *mSWMProtocol;
EDKII_VARIABLE_POLICY_PROTOCOL     *mVariablePolicyProtocol;

// Map Top Menu entries to HII Form IDs.
//
#define UNUSED_INDEX  (UINT16)-1
struct {
  UINT16           FullMenuIndex;       // Master Frame full menu index.
  UINT16           LimitedMenuIndex;    // Master Frame limited menu index.
  EFI_STRING_ID    MenuString;          // Master Frame menu string.
  EFI_GUID         FormSetGUID;         // HII FormSet GUID.
  EFI_FORM_ID      FormId;              // HII Form ID.
} mFormMap[] =
{
  //    Index (Full)  Index (Limited)     String                                      Formset Guid                       Form ID
  // -------------------------------------------------------------------------------------------------------------------------------------------------------------------
  { 0, 0,            STRING_TOKEN (STR_MF_MENU_OP_PCINFO),    FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_PCINFO   },                           // PC info
  // Security, Boot configuration, Management (DFCI) and Hardware Health are
  // hidden from this bring-up GUI. Their formset GUIDs remain available.
  // { UNUSED_INDEX, UNUSED_INDEX, STRING_TOKEN (STR_MF_MENU_OP_SECURITY),  FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_SECURITY },
  // { UNUSED_INDEX, UNUSED_INDEX, STRING_TOKEN (STR_MF_MENU_OP_BOOTORDER), MS_BOOT_MENU_FORMSET_GUID,      MS_BOOT_ORDER_FORM_ID       },
  // { UNUSED_INDEX, UNUSED_INDEX, STRING_TOKEN (STR_MF_MENU_OP_DFCI),      DFCI_MENU_FORMSET_GUID,         DFCI_MENU_FORM_ID           },
  // { UNUSED_INDEX, UNUSED_INDEX, STRING_TOKEN (STR_MF_MENU_OP_HWH),       HWH_MENU_FORMSET_GUID,          HWH_MENU_FORM_ID            },
  { 1, 1,            STRING_TOKEN (STR_MF_MENU_OP_LOGAUTO),   FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_LOGAUTO  },                            // QR Log
  { 2, 2,            STRING_TOKEN (STR_MF_MENU_OP_LOGREAD),   FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_LOGREAD },                            // QR Log (last)
  { 3, 3,            STRING_TOKEN (STR_MF_MENU_OP_REFRESH_LOG), FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_REFRESH_LOG },
  { 4, 4,            STRING_TOKEN (STR_MF_MENU_OP_DIAG),      FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_USB     },                           // USB Function
  { 5, 5,            STRING_TOKEN (STR_MF_MENU_OP_FILES),     FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_FILES },
  { 6, 6,            STRING_TOKEN (STR_MF_MENU_OP_POWER_STATUS), FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_POWER_STATUS },
  { 7, 7,            STRING_TOKEN (STR_MF_MENU_OP_RETURN_BOOT), FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_RETURN_BOOT },
  { 8, 8,            STRING_TOKEN (STR_MF_MENU_OP_EXIT),      FRONT_PAGE_CONFIG_FORMSET_GUID, FRONT_PAGE_FORM_ID_EXIT     }                            // Reboot
};

// Frontpage form set GUID
//
EFI_GUID  gMuFrontPageConfigFormSetGuid = FRONT_PAGE_CONFIG_FORMSET_GUID;

#pragma pack(1)

///
/// HII specific Vendor Device Path definition.
///
typedef struct {
  VENDOR_DEVICE_PATH          VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HII_VENDOR_DEVICE_PATH;

#pragma pack()

FRONT_PAGE_CALLBACK_DATA  mFrontPagePrivate = {
  FRONT_PAGE_CALLBACK_DATA_SIGNATURE,
  NULL,
  NULL,
  NULL,
  {
    ExtractConfig,        // Lives in FrontPageConfigAccess.c
    RouteConfig,          // Lives in FrontPageConfigAccess.c
    UiCallback            // Lives in FrontPageUi.c
  }
};

HII_VENDOR_DEVICE_PATH  mFrontPageHiiVendorDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    FRONT_PAGE_CONFIG_FORMSET_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};

EFI_STATUS
GetAndDisplayBitmap (
  EFI_GUID  *FileGuid,
  UINTN     XCoord,
  BOOLEAN   XCoordAdj
  );

/**

  Acquire the string associated with the Index from smbios structure and return it.
  The caller is responsible for free the string buffer.

  @param    OptionalStrStart  The start position to search the string
  @param    Index             The index of the string to extract
  @param    String            The string that is extracted

  @retval   EFI_SUCCESS       The function returns EFI_SUCCESS if found the string.
  @retval   EFI_NOT_FOUND     If the target string is not found.

**/
STATIC
EFI_STATUS
GetOptionalStringByIndex (
  IN  CHAR8   *OptionalStrStart,
  UINT8       Index,
  OUT CHAR16  **String
  )
{
  UINTN  StrSize;

  if (Index == 0) {
    *String = AllocateZeroPool (sizeof (CHAR16));
    return EFI_SUCCESS;
  }

  StrSize = 0;
  do {
    Index--;
    OptionalStrStart += StrSize;
    StrSize           = AsciiStrSize (OptionalStrStart);
  } while (OptionalStrStart[StrSize] != 0 && Index != 0);

  if ((Index != 0) || (StrSize == 1)) {
    //
    // Meet the end of strings set but Index is non-zero, or
    // Find an empty string
    //
    FreePool (*String);
    *String = NULL;
    return EFI_NOT_FOUND;
  } else {
    *String = AllocatePool (StrSize * sizeof (CHAR16));
    AsciiStrToUnicodeStrS (OptionalStrStart, *String, StrSize);
  }

  return EFI_SUCCESS;
}

/**
  Updates HII display strings based on associated EFI variable state.

@param[in]  HiiHandle       - Hii database handle.

@retval     EFI_SUCCESS     Updated display strings.

**/
STATIC
EFI_STATUS
UpdateDisplayStrings (
  IN EFI_HII_HANDLE  HiiHandle
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;
  CHAR16      *NewString;

  EFI_SMBIOS_PROTOCOL      *SmbiosProtocol;
  EFI_SMBIOS_HANDLE        SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  EFI_SMBIOS_TABLE_HEADER  *Record;
  SMBIOS_TYPE              Type;
  SMBIOS_TABLE_TYPE1       *Type1Record;
  SMBIOS_TABLE_TYPE3       *Type3Record;

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&SmbiosProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Could not locate SMBIOS protocol.  %r\n", Status));
    return Status;
  }

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;      // Reset handle
  Type         = SMBIOS_TYPE_SYSTEM_INFORMATION; // Smbios type1
  Status       = SmbiosProtocol->GetNext (SmbiosProtocol, &SmbiosHandle, &Type, &Record, NULL);
  if (!EFI_ERROR (Status)) {
    Type1Record = (SMBIOS_TABLE_TYPE1 *)Record;

    Status = GetOptionalStringByIndex ((CHAR8 *)((UINT8 *)Type1Record + Type1Record->Hdr.Length), Type1Record->SerialNumber, &NewString);
    if (!EFI_ERROR (Status)) {
      HiiSetString (HiiHandle, STRING_TOKEN (STR_INF_VIEW_PC_SERIALNUM_VALUE), NewString, NULL);
      FreePool (NewString);
    }

    Status = GetOptionalStringByIndex ((CHAR8 *)((UINT8 *)Type1Record + Type1Record->Hdr.Length), Type1Record->ProductName, &NewString);
    if (!EFI_ERROR (Status)) {
      HiiSetString (HiiHandle, STRING_TOKEN (STR_INF_VIEW_PC_MODEL_VALUE), NewString, NULL);
      FreePool (NewString);
    }

    NewString = AllocatePool ((GUID_STRING_LENGTH + 1) * sizeof (CHAR16));
    if (NewString != NULL) {
      SetMem (NewString, (GUID_STRING_LENGTH + 1) * sizeof (CHAR16), 0);
      UnicodeSPrint (
        NewString,
        (GUID_STRING_LENGTH * sizeof (CHAR16)),
        L"%g",
        (EFI_GUID *)&Type1Record->Uuid
        );
      HiiSetString (HiiHandle, STRING_TOKEN (STR_INF_VIEW_PC_UUID_VALUE), NewString, NULL);
      FreePool (NewString);
    }
  }

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;    // Reset handle
  Type         = SMBIOS_TYPE_SYSTEM_ENCLOSURE; // Smbios type3
  Status       = SmbiosProtocol->GetNext (SmbiosProtocol, &SmbiosHandle, &Type, &Record, NULL);
  if (!EFI_ERROR (Status)) {
    Type3Record = (SMBIOS_TABLE_TYPE3 *)Record;
    Status      = GetOptionalStringByIndex ((CHAR8 *)((UINT8 *)Type3Record + Type3Record->Hdr.Length), Type3Record->AssetTag, &NewString);
    if (!EFI_ERROR (Status)) {
      HiiSetString (HiiHandle, STRING_TOKEN (STR_INF_VIEW_PC_ASSET_TAG_VALUE), NewString, NULL);
      FreePool (NewString);
    }
  }

  return Status;
}

/**
Function to populate the PC INFO firmware version form with the current fw versions
found using FMP.

**/
VOID
UpdateFormWithFirmwareVersions (
  IN EFI_HII_HANDLE  HiiHandle
  )
{
  EFI_STATUS          Status;
  VOID                *StartOpCodeHandle;
  VOID                *EndOpCodeHandle = NULL;
  EFI_IFR_GUID_LABEL  *StartLabel;
  EFI_IFR_GUID_LABEL  *EndLabel;
  EFI_STRING_ID       StringId;
  EFI_STRING_ID       StringId1;

  EFI_FIRMWARE_MANAGEMENT_PROTOCOL  **FmpList;
  UINTN                             FmpCount;
  UINTN                             Index;
  EFI_FIRMWARE_MANAGEMENT_PROTOCOL  *Fmp;
  UINTN                             DescriptorSize;
  EFI_FIRMWARE_IMAGE_DESCRIPTOR     *FmpImageInfoBuf;
  UINT8                             FmpImageInfoCount;
  UINT32                            FmpImageInfoDescriptorVer;
  UINTN                             ImageInfoSize;
  UINT32                            PackageVersion;
  CHAR16                            *PackageVersionName;

  FmpImageInfoBuf    = NULL;
  Fmp                = NULL;
  FmpList            = NULL;
  PackageVersionName = NULL;

  do {
    //
    // Init OpCode Handle and Allocate space for creation of UpdateData Buffer
    //
    StartOpCodeHandle = HiiAllocateOpCodeHandle ();
    if (StartOpCodeHandle == NULL) {
      ASSERT (StartOpCodeHandle != NULL);
      break;
    }

    EndOpCodeHandle = HiiAllocateOpCodeHandle ();
    if (EndOpCodeHandle == NULL) {
      ASSERT (EndOpCodeHandle != NULL);
      break;
    }

    //
    // Create Hii Extend Label OpCode as the start opcode
    //
    StartLabel               = (EFI_IFR_GUID_LABEL *)HiiCreateGuidOpCode (StartOpCodeHandle, &gEfiIfrTianoGuid, NULL, sizeof (EFI_IFR_GUID_LABEL));
    StartLabel->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;

    //
    // Create Hii Extend Label OpCode as the end opcode
    //
    EndLabel               = (EFI_IFR_GUID_LABEL *)HiiCreateGuidOpCode (EndOpCodeHandle, &gEfiIfrTianoGuid, NULL, sizeof (EFI_IFR_GUID_LABEL));
    EndLabel->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;

    StartLabel->Number = LABEL_PCINFO_FW_VERSION_TAG_START;
    EndLabel->Number   = LABEL_PCINFO_FW_VERSION_TAG_END;

    //
    // Get all FMP instances and then use the descriptor to get string name and version
    //
    Status = EfiLocateProtocolBuffer (&gEfiFirmwareManagementProtocolGuid, &FmpCount, (VOID *)&FmpList);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "EfiLocateProtocolBuffer(gEfiFirmwareManagementProtocolGuid) returned error.  %r \n", Status));
      break;
    }

    for (Index = 0; Index < FmpCount; Index++) {
      Fmp = (EFI_FIRMWARE_MANAGEMENT_PROTOCOL *)(FmpList[Index]);
      // get the GetImageInfo for the FMP

      ImageInfoSize = 0;
      //
      // get necessary descriptor size
      // this should return TOO SMALL
      Status = Fmp->GetImageInfo (
                      Fmp,                        // FMP Pointer
                      &ImageInfoSize,             // Buffer Size (in this case 0)
                      NULL,                       // NULL so we can get size
                      &FmpImageInfoDescriptorVer, // DescriptorVersion
                      &FmpImageInfoCount,         // DescriptorCount
                      &DescriptorSize,            // DescriptorSize
                      &PackageVersion,            // PackageVersion
                      &PackageVersionName         // PackageVersionName
                      );

      if (Status != EFI_BUFFER_TOO_SMALL) {
        DEBUG ((DEBUG_ERROR, "%a - Unexpected Failure in GetImageInfo.  Status = %r\n", __FUNCTION__, Status));
        continue;
      }

      FmpImageInfoBuf = NULL;
      FmpImageInfoBuf = AllocateZeroPool (ImageInfoSize);
      if (FmpImageInfoBuf == NULL) {
        DEBUG ((DEBUG_ERROR, "%a - Failed to get memory for descriptors.\n", __FUNCTION__));
        continue;
      }

      PackageVersionName = NULL;
      Status             = Fmp->GetImageInfo (
                                  Fmp,
                                  &ImageInfoSize,             // ImageInfoSize
                                  FmpImageInfoBuf,            // ImageInfo
                                  &FmpImageInfoDescriptorVer, // DescriptorVersion
                                  &FmpImageInfoCount,         // DescriptorCount
                                  &DescriptorSize,            // DescriptorSize
                                  &PackageVersion,            // PackageVersion
                                  &PackageVersionName         // PackageVersionName
                                  );

      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a - Failure in GetImageInfo.  Status = %r\n", __FUNCTION__, Status));
        goto FmpCleanUp;
      }

      if (FmpImageInfoCount == 0) {
        DEBUG ((DEBUG_INFO, "%a - No Image Info descriptors.\n", __FUNCTION__));
        goto FmpCleanUp;
      }

      if (FmpImageInfoCount > 1) {
        DEBUG ((DEBUG_INFO, "%a - Found %d descriptors.  For FrontPage we only show the 1st descriptor.\n", __FUNCTION__, FmpImageInfoCount));
      }

      if (FmpImageInfoBuf->ImageIdName != NULL) {
        if ((StringId = HiiSetString (HiiHandle, 0, FmpImageInfoBuf->ImageIdName, NULL)) == 0) {
          DEBUG ((DEBUG_ERROR, "%a - Failed to set string for fmp ImageIdName: %s. \n", __FUNCTION__, FmpImageInfoBuf->ImageIdName));
          goto FmpCleanUp;
        }
      } else {
        DEBUG ((DEBUG_ERROR, "%a - FMP ImageIdName is null\n", __FUNCTION__));
      }

      if (FmpImageInfoBuf->VersionName != NULL) {
        if ((StringId1 = HiiSetString (HiiHandle, 0, FmpImageInfoBuf->VersionName, NULL)) == 0) {
          DEBUG ((DEBUG_ERROR, "%a - Failed to set string for fmp VersionName: %s. \n", __FUNCTION__, FmpImageInfoBuf->VersionName));
          goto FmpCleanUp;
        }
      } else {
        DEBUG ((DEBUG_ERROR, "%a - FMP VersionName is null\n", __FUNCTION__));
      }

      // Create a Subtitle OpCode to group the Firmware version "key-value" pair that follows.
      //
      HiiCreateSubTitleOpCode (
        StartOpCodeHandle,
        STRING_TOKEN (STR_NULL_STRING),
        STRING_TOKEN (STR_NULL_STRING),
        EFI_IFR_FLAGS_HORIZONTAL,
        0
        );

      HiiCreateTextOpCode (
        StartOpCodeHandle,
        StringId,
        STRING_TOKEN (STR_NULL_STRING),
        STRING_TOKEN (STR_NULL_STRING)
        );

      HiiCreateTextOpCode (
        StartOpCodeHandle,
        StringId1,
        STRING_TOKEN (STR_NULL_STRING),
        STRING_TOKEN (STR_NULL_STRING)
        );

      // Create empty 3rd text Opcode add an additional column to the display grid, thus moving the firmware version to the left (better alignment).
      //
      HiiCreateTextOpCode (
        StartOpCodeHandle,
        STRING_TOKEN (STR_NULL_STRING),
        STRING_TOKEN (STR_NULL_STRING),
        STRING_TOKEN (STR_NULL_STRING)
        );

FmpCleanUp:
      // clean up -
      FreePool (FmpImageInfoBuf);
      FmpImageInfoBuf = NULL;
      if (PackageVersionName != NULL) {
        FreePool (PackageVersionName);
        PackageVersionName = NULL;
      }
    } // for loop for all fmp handles

    // Free up the FmpList of pointers
    if (FmpList != NULL) {
      FreePool (FmpList);
    }

    Status = HiiUpdateForm (
               HiiHandle,                      // HII handle
               &gMuFrontPageConfigFormSetGuid, // Formset GUID
               FRONT_PAGE_FORM_ID_PCINFO,      // Form ID
               StartOpCodeHandle,              // Label for where to insert opcodes
               EndOpCodeHandle                 // Replace data
               );
  } while (FALSE);

  if (StartOpCodeHandle != NULL) {
    HiiFreeOpCodeHandle (StartOpCodeHandle);
  }

  if (EndOpCodeHandle != NULL) {
    HiiFreeOpCodeHandle (EndOpCodeHandle);
  }
}

/**
  Initialize HII information for the FrontPage


  @param InitializeHiiData    TRUE if HII elements need to be initialized.

  @retval  EFI_SUCCESS        The operation is successful.
  @retval  EFI_DEVICE_ERROR   If the dynamic opcode creation failed.

**/
EFI_STATUS
InitializeFrontPage (
  BOOLEAN  InitializeHiiData
  )
{
  EFI_STATUS      Status = EFI_SUCCESS;
  EFI_HII_HANDLE  HiiHandle;

  if (InitializeHiiData) {
    mCallbackKey = 0;

    //
    // Locate Hii relative protocols
    //
    Status = gBS->LocateProtocol (&gEfiFormBrowser2ProtocolGuid, NULL, (VOID **)&mFormBrowser2);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = gBS->LocateProtocol (&gEfiHiiConfigRoutingProtocolGuid, NULL, (VOID **)&mHiiConfigRouting);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = gBS->LocateProtocol (&gEdkiiVariablePolicyProtocolGuid, NULL, (VOID **)&mVariablePolicyProtocol);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // Install Device Path Protocol and Config Access protocol to driver handle
    //
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &mFrontPagePrivate.DriverHandle,
                    &gEfiDevicePathProtocolGuid,
                    &mFrontPageHiiVendorDevicePath,
                    &gEfiHiiConfigAccessProtocolGuid,
                    &mFrontPagePrivate.ConfigAccess,
                    NULL
                    );

    ASSERT_EFI_ERROR (Status);

    //
    // Publish our HII data
    //
    mFrontPagePrivate.HiiHandle = HiiAddPackages (
                                    &gMuFrontPageConfigFormSetGuid,
                                    mFrontPagePrivate.DriverHandle,
                                    FrontPageVfrBin,
                                    FrontPageStrings,
                                    NULL
                                    );
    if (mFrontPagePrivate.HiiHandle == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  HiiHandle = mFrontPagePrivate.HiiHandle;

  // Update PC information display strings from EFI variables.
  //
  UpdateDisplayStrings (HiiHandle);
  UpdateFormWithFirmwareVersions (HiiHandle);
  UpdateSecureBootStatusStrings (FALSE);

  return Status;
}

/**
  Uninitialize HII information for the FrontPage


  @param InitializeHiiData    TRUE if HII elements need to be initialized.

  @retval  EFI_SUCCESS        The operation is successful.
  @retval  EFI_DEVICE_ERROR   If the dynamic opcode creation failed.

**/
EFI_STATUS
UninitializeFrontPage (
  VOID
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;

  // Dispose the auth token we acquired for the front page.
  if (mAuthProtocol != NULL) {
    Status = mAuthProtocol->DisposeAuthToken (mAuthProtocol, &mAuthToken);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, " Dispose Auth Token Failed %r\n", Status));
    }
  }

  if (NULL != mFrontPageAuthTokenProtocol) {
    Status = gBS->UninstallMultipleProtocolInterfaces (
                    mImageHandle,
                    &gMsFrontPageAuthTokenProtocolGuid,
                    mFrontPageAuthTokenProtocol,
                    NULL
                    );
    FreePool (mFrontPageAuthTokenProtocol);
    mFrontPageAuthTokenProtocol = NULL;
    ASSERT_EFI_ERROR (Status);
  }

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  mFrontPagePrivate.DriverHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mFrontPageHiiVendorDevicePath,
                  &gEfiHiiConfigAccessProtocolGuid,
                  &mFrontPagePrivate.ConfigAccess,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);
  //
  // Remove our published HII data
  //
  HiiRemovePackages (mFrontPagePrivate.HiiHandle);
  if (mFrontPagePrivate.LanguageToken != NULL) {
    FreePool (mFrontPagePrivate.LanguageToken);
    mFrontPagePrivate.LanguageToken = (EFI_STRING_ID *)NULL;
  }

  gBS->CloseEvent (mMasterFrameNotifyEvent);

  return Status;
}

/**
  Call the browser and display the front page

  @return   Status code that will be returned by
            EFI_FORM_BROWSER2_PROTOCOL.SendForm ().

**/

STATIC
VOID
QrLogTrace (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Buffer[128];
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  SerialPortWrite ((UINT8 *)Buffer, Length);
}

STATIC
VOID
QrLogDrainInput (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *SimpleTextInEx
  )
{
  EFI_STATUS    Status;
  EFI_KEY_DATA  KeyData;

  for (;;) {
    Status = SimpleTextInEx->ReadKeyStrokeEx (SimpleTextInEx, &KeyData);
    if (Status != EFI_SUCCESS) {
      break;
    }
  }
}

/**
  Non-blocking Enter test, for loops that must keep working while watching for a key.

  Packets advance on a timer, so the key is checked between slices and the
  loop carries on if it is absent.

  Enter is the same predicate the UI toolkit uses to select a control - the keypad
  driver maps the power key to it, so this fires on the power button.

  @retval TRUE   Enter was pending, and has been consumed.
  @retval FALSE  Nothing, or only other keys - those are discarded.

**/
STATIC
BOOLEAN
QrLogEnterPressed (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *SimpleTextInEx
  )
{
  EFI_KEY_DATA  KeyData;

  if (SimpleTextInEx == NULL) {
    return FALSE;
  }

  while (SimpleTextInEx->ReadKeyStrokeEx (SimpleTextInEx, &KeyData) == EFI_SUCCESS) {
    if (KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC EFI_FONT_DISPLAY_INFO  *mQrProgressInfo = NULL;
STATIC EFI_IMAGE_OUTPUT       *mQrProgressOut  = NULL;

/**
  Show "current / total" while the QR stream runs.

  The symbol is square and centred in the content area, so there is a wide band
  of unused space above it on a 1440x3200 panel - that is where the counter goes.
  It is drawn after the code, never over it.
**/
STATIC
VOID
QrLogShowProgress (
  IN UINTN  Current,
  IN UINTN  Total
  )
{
  EFI_STATUS  Status;
  CHAR16      Text[64];
  UINT16      FontSize;
  UINTN       X;
  UINTN       Y;

  if (mSWMProtocol == NULL) {
    return;
  }

  if (mQrProgressInfo == NULL) {
    mQrProgressInfo = AllocateZeroPool (sizeof (*mQrProgressInfo));
    if (mQrProgressInfo == NULL) {
      return;
    }
  }

  //
  // StringToImage rejects a NULL *Blt with EFI_INVALID_PARAMETER when
  // EFI_HII_DIRECT_TO_SCREEN is set - it needs a real surface to draw into.
  // This is the same buffer RenderTitlebar allocates before its own call.
  //
  if (mQrProgressOut == NULL) {
    mQrProgressOut = AllocateZeroPool (sizeof (*mQrProgressOut));
    if (mQrProgressOut == NULL) {
      return;
    }

    mQrProgressOut->Width=(UINT16)(mBootHorizontalResolution-mMasterFrameWidth-80);
    mQrProgressOut->Height=(UINT16)(2*(MsUiGetMediumFontHeight()+12));
    mQrProgressOut->Image.Bitmap=AllocateZeroPool((UINTN)mQrProgressOut->Width*mQrProgressOut->Height*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
    if (mQrProgressOut->Image.Bitmap==NULL) { FreePool(mQrProgressOut);mQrProgressOut=NULL;return; }
  }

  //
  // FontSize must be one of the sizes the theme actually provides. 32 was
  // rejected outright, which is why nothing appeared on screen.
  //
  FontSize = MsUiGetMediumFontHeight ();

  X = mMasterFrameWidth + 40;
  Y = mTitleBarHeight + 40;

  ZeroMem (mQrProgressInfo, sizeof (*mQrProgressInfo));
  mQrProgressInfo->FontInfoMask       = EFI_FONT_INFO_ANY_FONT;
  mQrProgressInfo->FontInfo.FontSize  = FontSize;
  mQrProgressInfo->FontInfo.FontStyle = EFI_HII_FONT_STYLE_NORMAL;
  CopyMem (
    &mQrProgressInfo->ForegroundColor,
    &gMsColorTable.TitleBarTextColor,
    sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
    );
  CopyMem (
    &mQrProgressInfo->BackgroundColor,
    &gMsColorTable.MasterFrameBackgroundColor,
    sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
    );

  // Render to a bitmap first: DIRECT_TO_SCREEN depends on the currently active
  // GOP surface, which is not necessarily the FrontPage content window.
  UINTN Pixel;
  for (Pixel=0;Pixel<(UINTN)mQrProgressOut->Width*mQrProgressOut->Height;Pixel++)
    mQrProgressOut->Image.Bitmap[Pixel]=mQrProgressInfo->BackgroundColor;
  UnicodeSPrint(Text,sizeof(Text),L"Log transfer: %u / %u (%u%%)",Current,Total,Total==0?0:Current*100/Total);

  Status = mSWMProtocol->StringToWindow (
                           mSWMProtocol,
                           mImageHandle,
                           EFI_HII_OUT_FLAG_CLIP | EFI_HII_OUT_FLAG_CLIP_CLEAN_X |
                           EFI_HII_OUT_FLAG_CLIP_CLEAN_Y | EFI_HII_IGNORE_LINE_BREAK,
                           Text,
                           mQrProgressInfo,
                           &mQrProgressOut,
                           0,
                           0,
                           NULL,
                           NULL,
                           NULL
                           );
  if (EFI_ERROR (Status)) {
    QrLogTrace ("[QRA] progress counter -> %r\r\n", Status);
  }

  UnicodeSPrint (Text, sizeof (Text), L"Press Power to stop transfer");

  Status = mSWMProtocol->StringToWindow (
                           mSWMProtocol,
                           mImageHandle,
                           EFI_HII_OUT_FLAG_CLIP | EFI_HII_OUT_FLAG_CLIP_CLEAN_X |
                           EFI_HII_OUT_FLAG_CLIP_CLEAN_Y | EFI_HII_IGNORE_LINE_BREAK,
                           Text,
                           mQrProgressInfo,
                           &mQrProgressOut,
                           0,
                           FontSize + 12,
                           NULL,
                           NULL,
                           NULL
                           );
  if (EFI_ERROR (Status)) {
    QrLogTrace ("[QRA] progress hint -> %r\r\n", Status);
  }
  (VOID)mSWMProtocol->BltWindow(mSWMProtocol,mImageHandle,mQrProgressOut->Image.Bitmap,
    EfiBltBufferToVideo,0,0,X,Y,mQrProgressOut->Width,mQrProgressOut->Height,
    (UINTN)mQrProgressOut->Width*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
}

/**
  Wipe the symbol off the content area.

  SWM has no "destroy window" call - a client owns exactly one window for its
  lifetime - so the code is removed the same way it was drawn: a full-area fill
  through BltWindow. Going through SWM instead of mGop->Blt matters, because SWM
  keeps its own copy of the window and would repaint the stale pixels on the next
  compose, which is what left the code on screen after the stream stopped.
**/
STATIC
VOID
QrLogClearArea (
  VOID
  )
{
  if (mSWMProtocol == NULL) {
    return;
  }

  mSWMProtocol->BltWindow (
                  mSWMProtocol,
                  mImageHandle,
                  &gMsColorTable.MasterFrameBackgroundColor,
                  EfiBltVideoFill,
                  0,
                  0,
                  mMasterFrameWidth,
                  mTitleBarHeight,
                  mBootHorizontalResolution - mMasterFrameWidth,
                  mBootVerticalResolution - mTitleBarHeight,
                  0
                  );

  //
  // The counter lives in the same window, so the fill above takes it with it.
  // Free the surface we allocated for the text; the next stream allocates a
  // fresh one.
  //
  if (mQrProgressOut != NULL) {
    if (mQrProgressOut->Image.Bitmap!=NULL) FreePool(mQrProgressOut->Image.Bitmap);
    FreePool (mQrProgressOut);
    mQrProgressOut = NULL;
  }
}

/**
  Draw the LOGAUTO page for a scanner that is already running:
  every packet carries its own index, and the first one also carries the total, so the
  receiver knows where it is without any interaction on the device.

  Packet layout, raw bytes:

    packet 0   0x00, total (1..255), payload
    packet n   n (1..total-1), payload

  The payload is the boot log verbatim, including any byte value: the encoder is
  byte mode and the receiver reads decoded bytes rather than text, so a 0x00 inside
  the log does not truncate anything.

  Packets advance after 4 s for the first one and 250 ms for the rest, and the
  sequence ends on its own.
**/
STATIC
VOID
ShowLogQrAuto (
  BOOLEAN Stored
  )
{
  EFI_STATUS                         Status;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *SimpleTextInEx;
  CHAR8                              *LogBuffer;
  CHAR8                              *LogData;
  UINTN                              LogLength;
  UINTN                              Prefix;
  BOOLEAN                            CurrentSnapshot;
  UINTN                              Payload0;
  UINTN                              Rest;
  UINTN                              Total;
  UINTN                              Index;
  UINTN                              Offset;
  UINTN                              Chunk;
  UINT8                              Packet[QR_MAX_BYTES];
  BOOLEAN                            Aborted;

  if (mSWMProtocol == NULL) {
    return;
  }

  SerialPortInitialize ();
  FirmwareSuppressPower(500);

  //
  // Same keypad console source the LOG page reads. It now does two things: end the
  // stream early when Enter is pressed, and discard whatever else was pressed.
  //
  SimpleTextInEx = NULL;
  if (gST->ConsoleInHandle != NULL) {
    gBS->HandleProtocol (
           gST->ConsoleInHandle,
           &gEfiSimpleTextInputExProtocolGuid,
           (VOID **)&SimpleTextInEx
           );
  }

  // Reserve display metadata outside the 256KiB snapshot buffer.
  LogBuffer = AllocateZeroPool (BOOT_LOG_MAX_BYTES + 192);
  if (LogBuffer == NULL) {
    return;
  }

  LogLength = BOOT_LOG_MAX_BYTES;
  LogData = LogBuffer + 192;
  CurrentSnapshot = FALSE;
  if (!Stored) (VOID)BootLogSave ("gui-logauto");
  Status = Stored ? BootLogReadStored (LogData, &LogLength, TRUE) :
    BootLogRead (LogData, BOOT_LOG_MAX_BYTES, &LogLength);
  // The first successful save has no previous boot. Verify/read its current
  // slot so persistence can be tested immediately; never hide CRC/I/O errors.
  if (Stored && Status == EFI_NOT_FOUND) {
    LogLength = BOOT_LOG_MAX_BYTES;
    Status = BootLogReadStored (LogData, &LogLength, FALSE);
    CurrentSnapshot = (BOOLEAN)!EFI_ERROR (Status);
  }
  if (EFI_ERROR (Status) || (LogLength == 0)) {
    LogLength = AsciiSPrint (LogBuffer, BOOT_LOG_MAX_BYTES + 192,
      "QR LOG b327: %a -> %r\n%a\n",
      Stored ? (Status == EFI_NOT_FOUND ? "no Mu snapshot saved in logfs yet" : "logfs verify/mount/read failed") : "RAM log unavailable",
      Status, Stored ? "Use QR Log for the current RAM log and LOGFS failure stage." : "No RAM log bytes available.");
  } else {
    Prefix = Stored ? AsciiSPrint (LogBuffer, 192, "LOGREAD b327: %a; size + header/payload CRC verified.\n",
      CurrentSnapshot ? "no previous boot; current saved snapshot" : "previous saved snapshot") : 0;
    CopyMem (LogBuffer + Prefix, LogData, LogLength);
    LogLength += Prefix;
  }

  //
  // Packet 0 spends two bytes on its header, the rest one; QR_MAX_BYTES is the
  // encoder ceiling for the whole packet.
  //
  Payload0 = MIN (LogLength, QR_MAX_BYTES - 2);
  Rest     = LogLength - Payload0;
  Total    = 1 + (Rest + (QR_MAX_BYTES - 1) - 1) / (QR_MAX_BYTES - 1);

  if (Total > 255) {
    Total     = 255;
    LogLength = Payload0 + 254 * (QR_MAX_BYTES - 1);
  }

  QrLogTrace ("[QRA] %u bytes -> %u packets (Enter ends it)\r\n", LogLength, Total);

  Aborted = FALSE;
  QrLogClearArea ();
  for (Index = 0, Offset = 0; Index < Total; Index++) {
    if (Index == 0) {
      Chunk     = MIN (QR_MAX_BYTES - 2, LogLength - Offset);
      Packet[0] = 0;
      Packet[1] = (UINT8)Total;
      CopyMem (Packet + 2, LogBuffer + Offset, Chunk);
      Chunk    += 2;
    } else {
      Chunk     = MIN (QR_MAX_BYTES - 1, LogLength - Offset);
      Packet[0] = (UINT8)Index;
      CopyMem (Packet + 1, LogBuffer + Offset, Chunk);
      Chunk    += 1;
    }

    Offset += (Chunk - ((Index == 0) ? 2 : 1));

    Status = QrPaintBytes (
               mSWMProtocol,
               mImageHandle,
               mMasterFrameWidth,
            mTitleBarHeight+2*(MsUiGetMediumFontHeight()+12)+80,
               mBootHorizontalResolution - mMasterFrameWidth,
            mBootVerticalResolution-mTitleBarHeight-2*(MsUiGetMediumFontHeight()+12)-80,
               Packet,
               Chunk,
               0
               );
    if (EFI_ERROR (Status)) {
      QrLogTrace ("[QRA] packet %u paint failed: %r\r\n", Index, Status);
      break;
    }

    //
    // Drawn after the code so it sits above the white field, never under it.
    //
    QrLogShowProgress (Index + 1, Total);

    //
    // The first page stays up for four seconds, the rest for 250 ms.
    //
    // The camera needs a moment to lock focus and settle the exposure before it
    // will decode anything at all, so packet 0 is given the long window. Once
    // the phone is pointed and focused, 250 ms per page is what the receiver
    // keeps up with in practice - 150 ms was tried and it dropped frames, 400 ms
    // worked but made a 115 page transfer take nearly a minute.
    //
    // The wait is sliced at 50 ms so Enter can cut the stream short. A single Stall
    // would leave no chance to press anything, which is what made a 255 packet
    // transfer impossible to stop once it started.
    //
    {
      UINTN  Remaining = (Index == 0) ? (4 * 1000 * 1000) : (250 * 1000);

      while (Remaining > 0) {
        UINTN  Step = MIN ((UINTN)(50 * 1000), Remaining);

        gBS->Stall (Step);
        Remaining -= Step;

        if (QrLogEnterPressed (SimpleTextInEx)) {
          Aborted = TRUE;
          break;
        }
      }
    }

    if (Aborted) {
      QrLogTrace ("[QRA] ended by Enter at packet %u of %u\r\n", Index, Total);
      break;
    }
  }

  FreePool (LogBuffer);

  //
  // Take the code off the screen before anything else redraws. This has to go
  // through SWM: mGop->Blt below writes the framebuffer directly, and SWM would
  // then compose its stale copy of the window back over it, leaving the last
  // page visible after the stream stopped.
  //
  QrLogClearArea ();
  FirmwareSuppressPower(500);

  //
  // Nothing consumed input during the stream; drop whatever was pressed while
  // watching so the menu does not act on it.
  //
  if (SimpleTextInEx != NULL) {
    QrLogDrainInput (SimpleTextInEx);
  }

  RenderTitlebar ();
  RenderMasterFrame ();
}

EFI_STATUS
CallFrontPage (
  UINT32  FormIndex
  )
{
  EFI_STATUS                  Status = EFI_SUCCESS;
  UINT16                      Count, Index = 0;
  EFI_BROWSER_ACTION_REQUEST  ActionRequest;

  #define MAX_FORMSET_HANDLES  5
  EFI_HII_HANDLE  Handles[MAX_FORMSET_HANDLES];
  UINTN           HandleCount;

  // Locate Boot Menu form - this should already be registered.
  //
  EFI_GUID        BootMenu    = MS_BOOT_MENU_FORMSET_GUID;
  EFI_HII_HANDLE  *BootHandle = HiiGetHiiHandles (&BootMenu);
  EFI_HII_HANDLE  *DfciHandle = HiiGetHiiHandles (&gDfciMenuFormsetGuid);
  EFI_HII_HANDLE  *HwhHandle  =  HiiGetHiiHandles (&gHwhMenuFormsetGuid);

  Handles[0]  = mFrontPagePrivate.HiiHandle;
  HandleCount = 1;

  if (BootHandle != NULL) {
    Handles[HandleCount++] = BootHandle[0];
    FreePool (BootHandle);
  }

  if (DfciHandle != NULL) {
    Handles[HandleCount++] = DfciHandle[0];
    FreePool (DfciHandle);
  }

  if (HwhHandle != NULL) {
    Handles[HandleCount++] = HwhHandle[0];
    FreePool (HwhHandle);
  }

  DEBUG ((DEBUG_INFO, "MAX_FORMSET_HANDLES=%d, CurrentFormsetHandles=%d\n", MAX_FORMSET_HANDLES, HandleCount));
  ASSERT (HandleCount < MAX_FORMSET_HANDLES);

  ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE;

  // Search through the form mapping table to find the form set GUID and ID corresponding to the selected index.
  //
  for (Count = 0; Count < (sizeof (mFormMap) / sizeof (mFormMap[0])); Count++) {
    Index = ((FALSE == mShowFullMenu) ? mFormMap[Count].LimitedMenuIndex : mFormMap[Count].FullMenuIndex);

    if (Index == FormIndex) {
      break;
    }
  }

  // If we didn't find it, exit with an error.
  //
  if (Index != FormIndex) {
    Status = EFI_NOT_FOUND;
    goto Exit;
  }

  //
  // The LOG entry is not an HII form: draw it here and go back to the menu.
  //
  if (mFormMap[Count].FormId == FRONT_PAGE_FORM_ID_FILES ||
      mFormMap[Count].FormId == FRONT_PAGE_FORM_ID_POWER_STATUS) {
    if (mOSKProtocol != NULL) {
      mOSKProtocol->ShowKeyboard (mOSKProtocol, FALSE);
      mOSKProtocol->ShowKeyboardIcon (mOSKProtocol, FALSE);
    }
    if (mFormMap[Count].FormId == FRONT_PAGE_FORM_ID_FILES) FpFileBrowser (mSWMProtocol, mGop);
    else FpPowerStatus (mSWMProtocol, mGop);
    mTerminateFrontPage = FALSE;
    mCurrentFormIndex = 0;
    Status = EFI_SUCCESS;
    goto Exit;
  }

  if (mFormMap[Count].FormId == FRONT_PAGE_FORM_ID_LOGAUTO ||
      mFormMap[Count].FormId == FRONT_PAGE_FORM_ID_LOGREAD) {
    ShowLogQrAuto (mFormMap[Count].FormId == FRONT_PAGE_FORM_ID_LOGREAD);

    //
    // This path bypasses the form browser. Clear the loop flag and leave a
    // real form selected so the menu remains usable after LOGAUTO closes.
    //
    mTerminateFrontPage = FALSE;
    mCurrentFormIndex   = 0;
    Status              = EFI_SUCCESS;
    goto Exit;
  }

  if (mFormMap[Count].FormId == FRONT_PAGE_FORM_ID_REFRESH_LOG) {
    SWM_MB_RESULT Result;
    CHAR16 Message[192];
    EFI_STATUS Saved=BootLogSave ("gui-refresh-log");
    UnicodeSPrint(Message,sizeof(Message),EFI_ERROR(Saved)?
      L"Log save failed: %r. QR Log shows the LOGFS failure stage.":L"Current log saved to logfs (%r).",Saved);
    SwmDialogsMessageBox(L"Refresh Log",Message,L"",SWM_MB_OK,0,&Result);
    mTerminateFrontPage=FALSE;mCurrentFormIndex=0;Status=EFI_SUCCESS;goto Exit;
  }

  if (mFormMap[Count].FormId == FRONT_PAGE_FORM_ID_RETURN_BOOT) {
    if (mOSKProtocol != NULL) { mOSKProtocol->ShowKeyboard (mOSKProtocol, FALSE);mOSKProtocol->ShowKeyboardIcon (mOSKProtocol, FALSE); }
    if (mSWMProtocol != NULL) mSWMProtocol->EnableMousePointer (mSWMProtocol, FALSE);
    if (gST != NULL && gST->ConOut != NULL) gST->ConOut->EnableCursor (gST->ConOut, FALSE);
    FpBootMenuShow (mSWMProtocol, mGop, gImageHandle, FALSE);RenderTitlebar ();RenderMasterFrame ();
    mTerminateFrontPage = FALSE;mCurrentFormIndex = 0;Status = EFI_SUCCESS;goto Exit;
  }

  // Call the browser to display the selected form.
  //
  if (mFormMap[Count].FormId==FRONT_PAGE_FORM_ID_USB) {
    CHAR16 ModeText[128];
    FpGetUsbModeText(ModeText,sizeof(ModeText));
    HiiSetString(mFrontPagePrivate.HiiHandle,STRING_TOKEN(STR_USB_CURRENT_MODE),ModeText,NULL);
  }
  Status = mFormBrowser2->SendForm (
                            mFormBrowser2,
                            Handles,
                            HandleCount,
                            &mFormMap[Count].FormSetGUID,
                            mFormMap[Count].FormId,
                            (EFI_SCREEN_DESCRIPTOR *)NULL,
                            &ActionRequest
                            );

  // SendForm consumes callback EXIT internally and exports only NONE/RESET
  // (SetupBrowserDxe/Setup.c). Dispatch our pending confirmed target directly.
  // Falling through would return to BDS and launch the local LinuxLoader menu.
  FpApplyUsbMode ();
  if (FpRebootPending ()) {
    SWM_MB_RESULT Result;
    EFI_STATUS ResetStatus = FpRebootSelected ();
    QrLogTrace ("[REBOOT b327] returned -> %r\r\n", ResetStatus);
    mTerminateFrontPage = FALSE;
    mResetRequired = FALSE;
    CHAR16 Message[256];
    FpRebootErrorText(Message,sizeof(Message),ResetStatus);
    SwmDialogsMessageBox (L"Restart", Message, L"", SWM_MB_OK, 0, &Result);
  } else {
    if (ActionRequest == EFI_BROWSER_ACTION_REQUEST_EXIT) mTerminateFrontPage = TRUE;
    if (ActionRequest == EFI_BROWSER_ACTION_REQUEST_RESET) mResetRequired = TRUE;
  }

Exit:

  return Status;
}

/**
  IsDfciEnabledForDisplay

  The DFCI dialog will not be present when not enabled.  When DFCI is
  enabled, then enable the DFCI tab of OemFrontPage.

  returns FALSE - do not display DFCI page
  returns TRUE  - display DFCI page
*/
BOOLEAN
IsDfciEnabledForDisplay (
  VOID
  )
{
  EFI_STATUS  Status;
  VOID        *dummy;   // There is no protocol interface - just the existence that is is published
  BOOLEAN     DfciEnabled = FALSE;

  if (FeaturePcdGet (PcdDfciEnabled)) {
    Status      = gBS->LocateProtocol (&gDfciMenuFormsetGuid, NULL, (VOID **)&dummy);
    DfciEnabled = !EFI_ERROR (Status);
  }

  return DfciEnabled;
}

/**
  IsHwhEnabledForDisplay

  The HWH dialog will not be present if the formset protocol has not been published.

  returns FALSE - do not display HWH page
  returns TRUE  - display HWH page
*/
BOOLEAN
IsHwhEnabledForDisplay (
  VOID
  )
{
  EFI_STATUS  Status;
  VOID        *dummy;

  Status = gBS->LocateProtocol (&gHwhMenuFormsetGuid, NULL, (VOID **)&dummy);

  return !EFI_ERROR (Status);
}

/**
  RemoveMenuFromList -
    Updates mFormMap so that the menu item specified by MenuId is omitted.
    The item in question will have FullMenuIndex and LimitedMenuIndex set
    to UNUSED_INDEX, and the other indexes in the list are adjusted accordingly.
*/
VOID
RemoveMenuFromList (
  UINT16  MenuId
  )
{
  BOOLEAN  FullMenuRemoved    = FALSE;
  BOOLEAN  LimitedMenuRemoved = FALSE;
  UINTN    Count;

  UINT16  MenuOptionCount = (sizeof (mFormMap) / sizeof (mFormMap[0]));

  for (Count = 0; Count < MenuOptionCount; Count++) {
    if (mFormMap[Count].MenuString == MenuId) {
      if (mFormMap[Count].FullMenuIndex != UNUSED_INDEX) {
        FullMenuRemoved               = TRUE;
        mFormMap[Count].FullMenuIndex = UNUSED_INDEX;
      }

      if (mFormMap[Count].LimitedMenuIndex != UNUSED_INDEX) {
        LimitedMenuRemoved               = TRUE;
        mFormMap[Count].LimitedMenuIndex = UNUSED_INDEX;
      }
    }

    if (FullMenuRemoved && (mFormMap[Count].FullMenuIndex != UNUSED_INDEX)) {
      mFormMap[Count].FullMenuIndex -= 1;
    }

    if (LimitedMenuRemoved && (mFormMap[Count].LimitedMenuIndex != UNUSED_INDEX)) {
      mFormMap[Count].LimitedMenuIndex -= 1;
    }
  }
}

/**
  Creates the top-level menu in the Master Frame for selecting amongst the various HII forms.

  NOTE: Selectable menu options is dependent on whether there is a System firmware password and on whether the user knows it.


  @param OrigX          Menu's origin (x-axis).
  @param OrigY          Menu's origin (y-axis).
  @param CellWidth      Menu's width.
  @param CellHeight     Menu's height.
  @param CellTextXOffset   Menu entry text indentation.

  @retval  EFI_SUCCESS        The operation is successful.
  @retval  EFI_DEVICE_ERROR   Failed to create the menu.

**/
STATIC
ListBox *
CreateTopMenu (
  UINT32  OrigX,
  UINT32  OrigY,
  UINT32  CellWidth,
  UINT32  CellHeight,
  UINT32  CellTextXOffset
  )
{
  EFI_FONT_INFO  FontInfo;

  // Check whether there is a system password set.  If so, prompt the user for it before deciding the top-level menu list.
  // If the user doesn't know the password, they can dismiss the dialog and will see a limited-functionality menu.
  //
  if (GetAuthToken (NULL) != EFI_SUCCESS) {
    if (TRUE == ChallengeUserPassword (PcdGet8 (PcdMaxPasswordAttempts))) {
      mShowFullMenu = TRUE;
    }
  } else {
    // If no password is set, show the full menu.
    //
    // If no password is set we still need to make sure the token is valid
    if (mAuthToken != DFCI_AUTH_TOKEN_INVALID) {
      mShowFullMenu = TRUE;
    }
  }

  if (!mShowFullMenu) {
    PcdSetBoolS (PcdSetupUiReducedFunction, TRUE);
  }

  // Create a listbox with menu options.  The contents of the menu depend on whether a system password is
  // set and whether the user entered the password correctly or not.  If the user cancels the password dialog
  // then only a limited menu is available.
  //
  //
  UINT16           Count, Index;
  UINT16           MenuOptionCount = (sizeof (mFormMap) / sizeof (mFormMap[0]));
  UIT_LB_CELLDATA  *MenuOptions    = AllocateZeroPool ((MenuOptionCount + 1) * sizeof (UIT_LB_CELLDATA));  // NOTE: the list relies on a zero-initialized list terminator (hence +1).

  ASSERT (NULL != MenuOptions);
  if (NULL == MenuOptions) {
    return NULL;
  }

  //
  // If Dfci is Enabled, always display the DfciMenu.
  // If Dfci is Disabled, only display the Dfci menu if Dfci Enrolled
  //
  if (!IsDfciEnabledForDisplay ()) {
    RemoveMenuFromList (STRING_TOKEN (STR_MF_MENU_OP_DFCI));
  }

  if (!IsHwhEnabledForDisplay ()) {
    RemoveMenuFromList (STRING_TOKEN (STR_MF_MENU_OP_HWH));
  }

  for (Count = 0; Count < MenuOptionCount; Count++) {
    Index = ((FALSE == mShowFullMenu) ? mFormMap[Count].LimitedMenuIndex : mFormMap[Count].FullMenuIndex);

    if ((UNUSED_INDEX != Index) && (Index < MenuOptionCount)) {
      MenuOptions[Index].CellText = HiiGetString (mFrontPagePrivate.HiiHandle, mFormMap[Count].MenuString, NULL);
    }
  }

  // Create the ListBox that encapsulates the top-level menu.
  //
  FontInfo.FontSize  = FP_MFRAME_MENU_TEXT_FONT_HEIGHT;
  FontInfo.FontStyle = EFI_HII_FONT_STYLE_NORMAL;

  ListBox  *TopMenu = new_ListBox (
                        OrigX,
                        OrigY,
                        CellWidth,
                        CellHeight,
                        0,
                        &FontInfo,
                        CellTextXOffset,
                        &gMsColorTable.MasterFrameCellNormalColor,
                        &gMsColorTable.MasterFrameCellHoverColor,
                        &gMsColorTable.MasterFrameCellSelectColor,
                        &gMsColorTable.MasterFrameCellGrayoutColor,
                        MenuOptions,
                        NULL
                        );

  // Free HII string buffer.
  //
  if (NULL != MenuOptions) {
    FreePool (MenuOptions);
  }

  return TopMenu;
}

/**
  Draws the Front Page Title Bar.


  @param None.

  @retval  EFI_SUCCESS        Success.

**/
EFI_STATUS
RenderTitlebar (
  VOID
  )
{
  EFI_STATUS                 Status = EFI_SUCCESS;
  EFI_FONT_DISPLAY_INFO      StringInfo;
  EFI_IMAGE_OUTPUT           *pBltBuffer;
  EFI_LOADED_IMAGE_PROTOCOL  *ImageInfo;
  CHAR8                      Parameter = '\0';
  EFI_GUID                   *IconFile = NULL;
  UINTN                      DataSize;
  UINT8                      *RebootReason;

  // Draw the titlebar background.
  //
  mGop->Blt (
          mGop,
          &gMsColorTable.TitleBarBackgroundColor,
          EfiBltVideoFill,
          0,
          0,
          0,
          0,
          mTitleBarWidth,
          mTitleBarHeight,
          mTitleBarWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
          );

  GetAndDisplayBitmap (PcdGetPtr (PcdFrontPageLogoFile), (mMasterFrameWidth  * FP_TBAR_MSLOGO_X_PERCENT) / 100, FALSE);   // 2nd param is x coordinate

  Status = gBS->HandleProtocol (mImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&ImageInfo);
  ASSERT_EFI_ERROR (Status);

  if ((ImageInfo->LoadOptionsSize == 0) ||
      (ImageInfo->LoadOptions == NULL))
  {
    DataSize = 0;
    Status   = GetRebootReason (NULL, &DataSize);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      DEBUG ((DEBUG_ERROR, "%a error getting RebootReason size. Code = %r\n", __FUNCTION__, Status));
    } else {
      RebootReason = AllocatePool (DataSize);
      Status       = GetRebootReason (RebootReason, &DataSize);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a error reading RebootReason. Code = %r\n", __FUNCTION__, Status));
        Parameter = 'B';
      } else {
        Parameter = RebootReason[0];
        Status    = ClearRebootReason ();
        if (EFI_ERROR (Status)) {
          DEBUG ((DEBUG_ERROR, "%a error clearing RebootReason. Code = %r\n", __FUNCTION__, Status));
        }
      }

      FreePool (RebootReason);
    }
  } else {
    Parameter = *((CHAR8 *)ImageInfo->LoadOptions);
  }

  DEBUG ((DEBUG_ERROR, "%a Parameter = %c - LoadOption=%p\n", __FUNCTION__, Parameter, ImageInfo->LoadOptions));

  switch (Parameter) {
    case 'V':   // VOL+
      IconFile = PcdGetPtr (PcdVolumeUpIndicatorFile);
      break;
    case 'B':  // BOOTFAIL
      IconFile = PcdGetPtr (PcdBootFailIndicatorFile);
      break;
    case 'O':  // OSIndication
      IconFile = PcdGetPtr (PcdFirmwareSettingsIndicatorFile);
      break;
    default:
      IconFile = NULL;
  }

  if (NULL != IconFile) {
    GetAndDisplayBitmap (IconFile, (mTitleBarWidth * FP_TBAR_ENTRY_INDICATOR_X_PERCENT) / 100, TRUE);
  }

  // Prepare string blitting buffer.
  //
  pBltBuffer = (EFI_IMAGE_OUTPUT *)AllocateZeroPool (sizeof (EFI_IMAGE_OUTPUT));

  ASSERT (pBltBuffer != NULL);
  if (NULL == pBltBuffer) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  pBltBuffer->Width        = (UINT16)mBootHorizontalResolution;
  pBltBuffer->Height       = (UINT16)mBootVerticalResolution;
  pBltBuffer->Image.Screen = mGop;

  // Select a font (size & style) and font colors.
  //
  StringInfo.FontInfoMask       = EFI_FONT_INFO_ANY_FONT;
  StringInfo.FontInfo.FontSize  = FP_TBAR_TEXT_FONT_HEIGHT;
  StringInfo.FontInfo.FontStyle = EFI_HII_FONT_STYLE_NORMAL;

  CopyMem (&StringInfo.ForegroundColor, &gMsColorTable.TitleBarTextColor, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  CopyMem (&StringInfo.BackgroundColor, &gMsColorTable.TitleBarBackgroundColor, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));

  // Determine the size the TitleBar text string will occupy on the screen.
  //
  UINT32    MaxDescent;
  SWM_RECT  StringRect;

  GetTextStringBitmapSize (
    HiiGetString (mFrontPagePrivate.HiiHandle, STRING_TOKEN (STR_FRONT_PAGE_TITLE), NULL),
    &StringInfo.FontInfo,
    FALSE,
    EFI_HII_OUT_FLAG_CLIP |
    EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
    EFI_HII_IGNORE_LINE_BREAK,
    &StringRect,
    &MaxDescent
    );

  // Render the string to the screen, vertically centered.
  //
  mSWMProtocol->StringToWindow (
                  mSWMProtocol,
                  mImageHandle,
                  EFI_HII_OUT_FLAG_CLIP |
                  EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
                  EFI_HII_IGNORE_LINE_BREAK | EFI_HII_DIRECT_TO_SCREEN,
                  HiiGetString (mFrontPagePrivate.HiiHandle, STRING_TOKEN (STR_FRONT_PAGE_TITLE), NULL),
                  &StringInfo,
                  &pBltBuffer,
                  ((mMasterFrameWidth  * FP_TBAR_TEXT_X_PERCENT) / 100),                                     // Based on Master Frame width - so the logo bitmap aligns with the text in the menu.
                  ((mTitleBarHeight / 2) - ((StringRect.Bottom - StringRect.Top + 1) / 2)),                  // Vertically center.
                  NULL,
                  NULL,
                  NULL
                  );

Exit:

  if (NULL != pBltBuffer) {
    FreePool (pBltBuffer);
  }

  return Status;
}

/**
  Draws the FrontPage Master Frame and its Top-Level menu.


  @param None.

  @retval  EFI_SUCCESS        Success.

**/
EFI_STATUS
RenderMasterFrame (
  VOID
  )
{
  EFI_STATUS  Status    = EFI_SUCCESS;
  VOID        *pContext = NULL;

  // Verify that the top-level menu was created.
  //
  ASSERT (NULL != mTopMenu);
  if (NULL == mTopMenu) {
    Status = EFI_INVALID_PARAMETER;
    goto Exit;
  }

  // Draw the master frame background.
  //
  mGop->Blt (
          mGop,
          &gMsColorTable.MasterFrameBackgroundColor,
          EfiBltVideoFill,
          0,
          0,
          0,
          mTitleBarHeight,
          mMasterFrameWidth,
          mMasterFrameHeight,
          0
          );

  // Draw divider line.
  //
  mGop->Blt (
          mGop,
          &gMsColorTable.TitleBarBackgroundColor,
          EfiBltVideoFill,
          0,
          0,
          (mMasterFrameWidth - FP_MFRAME_DIVIDER_LINE_WIDTH_PIXELS),
          mTitleBarHeight,
          FP_MFRAME_DIVIDER_LINE_WIDTH_PIXELS,
          mMasterFrameHeight,
          0
          );

  // Draw the top-level menu.
  //
  mTopMenu->Base.Draw (
                   mTopMenu,
                   FALSE,
                   NULL,
                   &pContext
                   );

Exit:

  return Status;
}

/**
  Determines whether there are any pending messages for the user
  and presents them if there are.

  @retval     EFI_SUCCESS

**/
EFI_STATUS
NotifyUserOfAlerts (
  VOID
  )
{
  EFI_STATUS     Status = EFI_SUCCESS, VarStatus;
  CHAR16         *SbViolationVarName = SFP_SB_VIOLATION_SIGNAL_VAR_NAME, *SbViolationMessage;
  BOOLEAN        SecViolation = FALSE;
  UINTN          DataSize;
  SWM_MB_RESULT  SwmResult = 0;

  // Check for SecureBoot notifications.
  //
  DataSize  = sizeof (SecViolation);
  VarStatus = gRT->GetVariable (
                     SbViolationVarName,
                     &gOemBootNVVarGuid,
                     NULL,
                     &DataSize,
                     (UINT8 *)&SecViolation
                     );

  // Inform the user if there was a SecureBoot violation.
  //
  if (SecViolation) {
    DEBUG ((DEBUG_INFO, "FrontPage::%a - SecureBoot violation detected! Warning user...\n", __FUNCTION__));
    SbViolationMessage = (CHAR16 *)HiiGetString (gStringPackHandle, STRING_TOKEN (STR_SB_VIOLATION_WARNING), NULL);
    Status             = SwmDialogsMessageBox (
                           (CHAR16 *)HiiGetString (gStringPackHandle, STRING_TOKEN (STR_SB_VIOLATION_TITLE), NULL), // Dialog titlebar text.
                           SbViolationMessage,                                                                      // Dialog body text.
                           L"",                                                                                     // Dialog caption text.
                           SWM_MB_OK,                                                                               // Show Ok button only.
                           0,                                                                                       // No timeout
                           &SwmResult
                           );                           // Return result.
  }

  // If the variable was found successfully, let's delete it so
  // that we don't continue popping up the message.
  //
  if (!EFI_ERROR (Status) && !EFI_ERROR (VarStatus)) {
    Status = gRT->SetVariable (
                    SbViolationVarName,
                    &gOemBootNVVarGuid,
                    0,
                    0,
                    NULL
                    );
  }

  return Status;
} // NotifyUserOfAlerts()

/**
  Master Frame callback (signalled by Display Engine) for receiving user input data (i.e., key, touch, mouse, etc.).


  @param    None.

  @retval   None.

**/

#ifdef SHENNONG_TOUCH_GESTURES
STATIC UIT_TOUCH_GESTURE mMenuTap;
#endif

STATIC OBJECT_STATE
FpTopMenuInput (SWM_INPUT_STATE *Input, VOID **Context)
{
#ifdef SHENNONG_TOUCH_GESTURES
  UINTN I, Target = 0;
  UINT64 X, Y;
  UIT_TAP_RESULT Tap;
  SWM_INPUT_STATE Press;
  if (Context != NULL) *Context = NULL;
  if (Input->InputType != SWM_INPUT_TYPE_TOUCH) {
    ZeroMem (&mMenuTap, sizeof (mMenuTap));
    return mTopMenu->Base.Draw (mTopMenu, TRUE, Input, Context);
  }
  X = Input->State.TouchState.CurrentX;
  Y = Input->State.TouchState.CurrentY;
  for (I = 0; I < mTopMenu->m_NumberOfCells; I++) {
    SWM_RECT *B = &mTopMenu->m_pCells[I].CellBounds;
    if (X >= B->Left && X <= B->Right && Y >= B->Top && Y <= B->Bottom) {
      Target = I + 1;
      break;
    }
  }
  Tap = UitTouchTap (&mMenuTap, SWM_IS_FINGER_DOWN (Input->State.TouchState), Target, X, Y,
                    GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000000,
                    MsUiScaleByTheme (24));
  if (Target != 0 && ((!mMenuTap.Cancelled && mMenuTap.Down) || Tap == UitTapFocus)) {
    mTopMenu->m_SelectedCell = (UINT32)(Target - 1);
    mTopMenu->m_HighlightedCell = (UINT32)(Target - 1);
    mTopMenu->m_State = NORMAL;
    mTopMenu->Base.Draw (mTopMenu, TRUE, NULL, NULL);
  }
  if (Tap != UitTapConfirm) return NORMAL;
  Press = *Input;
  Press.State.TouchState.ActiveButtons |= 1;
  mTopMenu->Base.Draw (mTopMenu, TRUE, &Press, NULL);
  return mTopMenu->Base.Draw (mTopMenu, TRUE, Input, Context);
#else
  return mTopMenu->Base.Draw (mTopMenu, TRUE, Input, Context);
#endif
}

VOID
EFIAPI
MasterFrameNotifyCallback (
  IN  EFI_EVENT  Event,
  IN  VOID       *Context
  )
{
  UINT32           SelectedIndex      = 0;
  VOID             *pSelectionContext = NULL;
  OBJECT_STATE     MenuState          = NORMAL;
  SWM_INPUT_STATE  *pInputState       = &mDisplayEngineState.InputState;
  LB_RETURN_DATA   ReturnData;

  // If we just need to redraw, do that and exit.
  //
  if (REDRAW == mDisplayEngineState.NotificationType) {
    mTopMenu->Base.Draw (
                     mTopMenu,
                     mDisplayEngineState.ShowTopMenuHighlight,
                     NULL, // Painting must not replay an old touch into the control.
                     &pSelectionContext
                     );

    goto Exit;
  }

  // We'll only handle user input from this point onwards.
  //
  if (USERINPUT != mDisplayEngineState.NotificationType) {
    goto Exit;
  }

  // If we are receiving touch/mouse data from the display engine and it's a finger/button down event, process it.
  //
  if ((SWM_INPUT_TYPE_TOUCH == pInputState->InputType /* && (pInputState->State.TouchState.ActiveButtons & 0x1) */) ||
      (SWM_INPUT_TYPE_KEY   == pInputState->InputType))
  {
    // Draw the top-level menu in the master frame.
    //
    MenuState = FpTopMenuInput (pInputState, &pSelectionContext);

    // If nothing was selected (user may simply have moved the highlighted cell), there's no action to take.
    //
    if (SELECT != MenuState) {
      return;
    }

    // Get the currently selected top-level menu entry (may be none).
    //
    mTopMenu->GetSelectedCellIndex (
                mTopMenu,
                &ReturnData
                );

    SelectedIndex = ReturnData.SelectedCell;

    if (SelectedIndex != mCurrentFormIndex) {
      // Update the current form ID to the new one.
      //
      mCurrentFormIndex = SelectedIndex;

      // Signal the form (browser) to close so the new form will be displayed.
      //
      mDisplayEngineState.CloseFormRequest = TRUE;
      mTerminateFrontPage                  = FALSE;
    }
  }

Exit:
  mDisplayEngineState.NotificationType = NONE;

  return;
}

STATIC
EFI_STATUS
InitializeFrontPageUI (
  VOID
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;

  // Establish initial FrontPage TitleBar and Master Frame dimensions based on the current screen size.
  //
  mTitleBarWidth     = mBootHorizontalResolution;
  mTitleBarHeight    = ((mBootVerticalResolution   * FP_TBAR_HEIGHT_PERCENT)  / 100);
  mMasterFrameWidth  = ((mBootHorizontalResolution * FP_MFRAME_WIDTH_PERCENT) / 100);
  mMasterFrameHeight = (mBootVerticalResolution - mTitleBarHeight);

  DEBUG ((
    DEBUG_INFO,
    "INFO [FP]: FP Dimensions: %d, %d, %d, %d, %d, %d\r\n", \
    mBootHorizontalResolution,
    mBootVerticalResolution,
    mTitleBarWidth,
    mTitleBarHeight,
    mMasterFrameWidth,
    mMasterFrameHeight
    ));

  // Compute Master Frame origin and menu text indentation.
  //
  UINT32  MasterFrameMenuOrigX = 0;
  UINT32  MasterFrameMenuOrigY = mTitleBarHeight;
  UINT32  CellTextXOffset      = ((mMasterFrameWidth * FP_MFRAME_MENU_TEXT_OFFSET_PERCENT) / 100);

  // Determine whether there are any events that require user notification.
  // NOTE: This should come before CreateTopMenu() because it needs to happen before the
  //       Admin Password prompt.
  //
  NotifyUserOfAlerts ();

  // Create the top-level menu in the Master Frame.
  //
  mTopMenu = CreateTopMenu (
               MasterFrameMenuOrigX,
               MasterFrameMenuOrigY,
               (mMasterFrameWidth - FP_MFRAME_DIVIDER_LINE_WIDTH_PIXELS),
               ((mMasterFrameHeight * FP_MFRAME_MENU_CELL_HEIGHT_PERCENT) / 100),
               CellTextXOffset
               );

  ASSERT (NULL != mTopMenu);
  if (NULL == mTopMenu) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  // Render the TitleBar at the top of the screen.
  //
  RenderTitlebar ();

  // Render the Master Frame and its Top-Level menu contents.
  //
  RenderMasterFrame ();

  // Create the Master Frame notification event.  This event is signalled by the display engine to note that
  // there is a user input event outside the form area to consider.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MasterFrameNotifyCallback,
                  NULL,
                  &gMsEventMasterFrameNotifyGroupGuid,
                  &mMasterFrameNotifyEvent
                  );

  if (EFI_SUCCESS != Status) {
    DEBUG ((DEBUG_ERROR, "ERROR [FP]: Failed to create master frame notification event.  Status = %r\r\n", Status));
    goto Exit;
  }

  // Set shared pointer to user input context structure in a PCD so it can be shared.
  //
  PcdSet64S (PcdCurrentPointerState, (UINT64)(UINTN)&mDisplayEngineState);

Exit:

  return Status;
}

VOID
ProcessBootNext (
  VOID
  )
{
  EFI_STATUS                    Status;
  UINT16                        *BootNext;
  UINTN                         DataSize;
  CHAR16                        BootNextVariableName[sizeof ("Boot####")];
  EFI_BOOT_MANAGER_LOAD_OPTION  LoadOption;
  UINT64                        OsIndication;

  DEBUG ((DEBUG_INFO, "%a entry\n", __FUNCTION__));
  //
  // Cache and remove the "BootNext" NV variable.
  //
  GetEfiGlobalVariable2 (EFI_BOOT_NEXT_VARIABLE_NAME, (VOID **)&BootNext, &DataSize);
  if (DataSize != sizeof (UINT16)) {
    if (BootNext != NULL) {
      FreePool (BootNext);
    }

    BootNext = NULL;
  }

  Status = gRT->SetVariable (
                  EFI_BOOT_NEXT_VARIABLE_NAME,
                  &gEfiGlobalVariableGuid,
                  0,
                  0,
                  NULL
                  );
  //
  // Deleting NV variable shouldn't fail unless it doesn't exist.
  //
  ASSERT (Status == EFI_SUCCESS || Status == EFI_NOT_FOUND);

  if (NULL != BootNext) {
    UnicodeSPrint (BootNextVariableName, sizeof (BootNextVariableName), L"Boot%04x", *BootNext);
    DEBUG ((DEBUG_INFO, "Acting on BootNext %4.4x\n", *BootNext));
    FreePool (BootNext);
    Status = EfiBootManagerVariableToLoadOption (BootNextVariableName, &LoadOption);
    if (!EFI_ERROR (Status)) {
      EfiBootManagerBoot (&LoadOption);
      EfiBootManagerFreeLoadOption (&LoadOption);
    }

    // Reboot to front page

    OsIndication = EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
    Status       = gRT->SetVariable (
                          L"OsIndications",
                          &gEfiGlobalVariableGuid,
                          EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                          sizeof (UINT64),
                          &OsIndication
                          );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Unable to set OsIndications\n"));
    }

    DEBUG ((DEBUG_INFO, "%a Resetting system\n", __FUNCTION__));
    gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
  }
}

/**
  This function is the main entry of the platform setup entry.
  The function will present the main menu of the system setup,
  this is the platform reference part and can be customize.
**/

// The test protocol owns the controller and its direct descriptor/event pump.
// Do not reconnect the vendor stack or force RUN_STOP behind that owner's back.
STATIC UINT32 mUsbPending = MAX_UINT32;
STATIC UINT32 mRamSizePending=MAX_UINT32;
VOID FpGetRamSizeText (CHAR16 *Text, UINTN Bytes)
{
  EFI_GUID Guid=USB_BRINGUP_PROTOCOL_GUID;USB_BRINGUP_PROTOCOL *Usb=NULL;
  UINT32 MiB=512;BOOLEAN Allocated=FALSE;
  if (!EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,(VOID **)&Usb)) && Usb!=NULL && Usb->Revision>=5 && Usb->GetRamSize!=NULL)
    (VOID)Usb->GetRamSize(Usb,&MiB,&Allocated);
  if (mRamSizePending!=MAX_UINT32) MiB=mRamSizePending;
  UnicodeSPrint(Text,Bytes,L"%u",MiB);
}
EFI_STATUS FpQueueRamSize (CONST CHAR16 *Text)
{
  UINT32 MiB=0;UINTN I;EFI_STATUS Status=EFI_INVALID_PARAMETER;
  if (Text==NULL) return EFI_INVALID_PARAMETER;
  for (I=0;I<5 && Text[I]!=0;I++) {
    if (Text[I]<'0' || Text[I]>'9' || I==4) goto Done;
    MiB=MiB*10+(Text[I]-'0');
  }
  if (I>=2 && I<=4 && MiB>=64 && MiB<=2048) { mRamSizePending=MiB;Status=EFI_SUCCESS; }
Done:
  return Status;
}
EFI_STATUS FpRouteRamSize (CONST CHAR16 *Text)
{
  // CHANGED already saved the confirmed edit. A later browser submission may
  // contain the old extracted size and must not replace that request.
  return mRamSizePending!=MAX_UINT32?EFI_SUCCESS:FpQueueRamSize(Text);
}
VOID FpGetUsbModeText (CHAR16 *Text, UINTN Bytes)
{
  EFI_GUID Guid=USB_BRINGUP_PROTOCOL_GUID;
  USB_BRINGUP_PROTOCOL *Usb=NULL;
  UINT32 Mode=USB_MODE_DISABLED;
  BOOLEAN Configured=FALSE;
  CONST CHAR16 *Name;
  if (EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,(VOID **)&Usb)) ||
      Usb==NULL || Usb->Revision<5 || Usb->GetMode==NULL ||
      EFI_ERROR(Usb->GetMode(Usb,&Mode,&Configured))) {
    UnicodeSPrint(Text,Bytes,L"Current mode: unavailable");return;
  }
  switch (Mode) {
    case USB_MODE_NATIVE: Name=L"USB link";break;
    case USB_MODE_STORAGE: Name=L"logfs (read only)";break;
    case USB_MODE_CDC: Name=L"CDC serial";break;
    case USB_MODE_RAMDISK: Name=L"RAM disk";break;
    default: Name=L"Disabled";break;
  }
  if (Mode==USB_MODE_DISABLED) UnicodeSPrint(Text,Bytes,L"Current mode: %s",Name);
  else UnicodeSPrint(Text,Bytes,L"Current mode: %s (%s)",Name,
    Configured?L"connected":L"waiting for host");
}
EFI_STATUS FpQueueUsbMode (EFI_QUESTION_ID QuestionId)
{
  EFI_GUID Guid = USB_BRINGUP_PROTOCOL_GUID;
  USB_BRINGUP_PROTOCOL *Usb = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (&Guid, NULL, (VOID **)&Usb);
  if (EFI_ERROR (Status)) return Status;
  if (Usb == NULL || Usb->Revision < 3 || Usb->SelectMode == NULL) return EFI_UNSUPPORTED;
  if ((QuestionId==FRONT_PAGE_ACTION_USB_RAM || QuestionId==FRONT_PAGE_ACTION_USB_RAM_OFF) && Usb->Revision<4)
    return EFI_UNSUPPORTED;
  switch (QuestionId) {
    case FRONT_PAGE_ACTION_USB_NATIVE: mUsbPending=USB_MODE_NATIVE;break;
    case FRONT_PAGE_ACTION_USB_STORAGE: mUsbPending=USB_MODE_STORAGE;break;
    case FRONT_PAGE_ACTION_USB_CDC: mUsbPending=USB_MODE_CDC;break;
    case FRONT_PAGE_ACTION_USB_DISABLE: mUsbPending=USB_MODE_DISABLED;break;
    case FRONT_PAGE_ACTION_USB_REFRESH: mUsbPending=USB_MODE_REFRESH;break;
    case FRONT_PAGE_ACTION_USB_RAM: mUsbPending=USB_MODE_RAMDISK;break;
    case FRONT_PAGE_ACTION_USB_RAM_OFF: mUsbPending=USB_MODE_RAM_RELEASE;break;
    default:return EFI_INVALID_PARAMETER;
  }
  QrLogTrace ("[USBMODE b339] action=%x pending=%u driver-rev=%lu\r\n", QuestionId, mUsbPending, Usb->Revision);
  return EFI_SUCCESS;
}
UINT8 FpGetUsbUiMode (VOID)
{
  EFI_GUID Guid = USB_BRINGUP_PROTOCOL_GUID;
  USB_BRINGUP_PROTOCOL *Usb = NULL;
  UINT32 Mode = USB_MODE_DISABLED;
  BOOLEAN Configured = FALSE;
  if (!EFI_ERROR (gBS->LocateProtocol (&Guid, NULL, (VOID **)&Usb)) &&
      Usb != NULL && Usb->Revision >= 5 && Usb->GetMode != NULL)
  {
    (VOID)Usb->GetMode (Usb, &Mode, &Configured);
  }

  switch (Mode) {
    case USB_MODE_STORAGE: return 1;
    case USB_MODE_CDC:     return 2;
    case USB_MODE_RAMDISK: return 3;
    default:               return 0;
  }
}
BOOLEAN FpGetRamAllocated (VOID)
{
  EFI_GUID Guid = USB_BRINGUP_PROTOCOL_GUID;
  USB_BRINGUP_PROTOCOL *Usb = NULL;
  UINT32 MiB = 0;
  BOOLEAN Allocated = FALSE;
  if (!EFI_ERROR (gBS->LocateProtocol (&Guid, NULL, (VOID **)&Usb)) &&
      Usb != NULL && Usb->Revision >= 5 && Usb->GetRamSize != NULL)
  {
    (VOID)Usb->GetRamSize (Usb, &MiB, &Allocated);
  }

  return Allocated;
}
EFI_STATUS FpQueueUsbUiMode (UINT8 UiMode)
{
  UINT32 Mapped = USB_MODE_DISABLED;
  switch (UiMode) {
    case 0: mUsbPending = Mapped = USB_MODE_DISABLED; break;
    case 1: mUsbPending = Mapped = USB_MODE_STORAGE; break;
    case 2: mUsbPending = Mapped = USB_MODE_CDC;     break;
    case 3: mUsbPending = Mapped = USB_MODE_RAMDISK; break;
    default: return EFI_INVALID_PARAMETER;
  }

  QrLogTrace ("[USBMODE b336] queue ui=%u pending=%u\r\n", UiMode, Mapped);
  return EFI_SUCCESS;
}
EFI_STATUS FpQueueRamDelete (VOID)
{
  // RAM_RELEASE makes the driver stop the active RAM device (DISABLED) before
  // freeing pages; if the RAM disk is not currently owned by USB it only frees.
  mUsbPending = USB_MODE_RAM_RELEASE;
  return EFI_SUCCESS;
}
STATIC VOID FpApplyUsbMode (VOID)
{
  EFI_GUID Guid = USB_BRINGUP_PROTOCOL_GUID;
  USB_BRINGUP_PROTOCOL *Usb = NULL;
  EFI_STATUS Status;
  SWM_MB_RESULT Result;
  UINT32 Mode=mUsbPending;
  if (Mode==MAX_UINT32 && mRamSizePending==MAX_UINT32) return;
  mUsbPending=MAX_UINT32;
  Status=gBS->LocateProtocol (&Guid,NULL,(VOID **)&Usb);
  if (mRamSizePending!=MAX_UINT32) {
    UINT32 MiB=mRamSizePending;mRamSizePending=MAX_UINT32;
    if (!EFI_ERROR(Status)) Status=(Usb!=NULL && Usb->Revision>=5 && Usb->SetRamSize!=NULL)?Usb->SetRamSize(Usb,MiB):EFI_UNSUPPORTED;
    QrLogTrace("[USBRAM b328] requested %u MiB -> %r\r\n",MiB,Status);
    if (EFI_ERROR(Status)) SwmDialogsMessageBox(L"RAM disk",L"Size was not changed. Release the existing RAM disk first.",L"",SWM_MB_OK,0,&Result);
    mTerminateFrontPage=FALSE;mResetRequired=FALSE;
    if (Mode==MAX_UINT32 || EFI_ERROR(Status)) return;
  }
    if (!EFI_ERROR (Status)) Status=(Usb!=NULL && Usb->Revision>=3 && Usb->SelectMode!=NULL)?Usb->SelectMode(Usb,Mode):EFI_UNSUPPORTED;
    else QrLogTrace ("[USBMODE b336] locate protocol -> %r; UI mode not applied\r\n", Status);
  QrLogTrace ("[USBMODE b327] GUI mode=%u -> %r\r\n",Mode,Status);
  mTerminateFrontPage=FALSE;mResetRequired=FALSE;
  if (EFI_ERROR(Status) && Status!=EFI_ALREADY_STARTED)
    SwmDialogsMessageBox (L"USB",L"USB mode could not start. See QR Log.",L"",SWM_MB_OK,0,&Result);
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status  = EFI_SUCCESS;
  UINT32      OSKMode = 0;

  FpDetectVerbose ();

  FpPortSay ("[FP b340] entry; Boot Menu, asynchronous Volume Up detection\n");

  // Keep the automatic boot path short and do not replay I2C/USB tests while
  // the window manager is being created. LOGAUTO saves then reads the RAM ring.
  FpPortSay ("[FP b341] default USB CDC; power diagnostics available on demand\n");

  //
  // Late dispatch: the log shows '[DISP] dispatch complete' twice AFTER BDS
  // asks for this UI, so anything that depends on those drivers only publishes
  // here. Re-check EFI_USBFN_IO_PROTOCOL at this point.
  //
  {
    EFI_GUID  UsbfnIo = { 0x32d2963a, 0xfe5d, 0x4f30, { 0xb6, 0x33, 0x6e, 0x5d, 0xc5, 0x58, 0x03, 0xcc } };
    VOID      *UsbfnInstance = NULL;

    if (EFI_ERROR (gBS->LocateProtocol (&UsbfnIo, NULL, &UsbfnInstance))) {
      FpPortSay ("[FP] UsbfnIo MISSING\n");
    } else {
      FpPortSay ("[FP] UsbfnIo present\n");
    }
  }

  //
  // Print our own load base and size, so a crash report of the form
  // 'IMG = <base> + <offset>' can be attributed. The known crash while entering
  // HardwareHealth a second time reports IMG = 0xD3F14000 + 0x1E78, and RVA
  // 0x1E78 lands in BmGetNextLoadOptionDevicePath + 0x498 in this image's map -
  // but that is only conclusive if the base matches.
  //
  {
    EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage = NULL;
    CHAR8                      Line[80];
    UINTN                      LineLen;

    if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage))) {
      LineLen = AsciiSPrint (
                  Line,
                  sizeof (Line),
                  "[FP] base=0x%lx size=0x%lx\n",
                  (UINT64)(UINTN)LoadedImage->ImageBase,
                  (UINT64)LoadedImage->ImageSize
                  );
      SerialPortInitialize ();
      SerialPortWrite ((UINT8 *)Line, LineLen);
    }
  }

  //
  // The DWC3 register dump and the start attempt moved into FpTryStartUsb, and
  // that is now reached from the DIAG menu rather than from here. StartEx hangs,
  // and calling it on the normal boot path left the machine stuck before the UI
  // came up, which also made LOGAUTO unreachable - and LOGAUTO is how the log
  // gets off the device at all.
  //
  // Delete BootNext if entry to BootManager.
  Status = gRT->SetVariable (
                  L"BootNext",
                  &gEfiGlobalVariableGuid,
                  0,
                  0,
                  NULL
                  );

  // Save image handle for later.
  //
  mImageHandle = ImageHandle;

  // Disable the watchdog timer
  //
  gBS->SetWatchdogTimer (0, 0, 0, (CHAR16 *)NULL);

  mResetRequired = FALSE;

  Status = gBS->LocateProtocol (
                  &gDfciSettingAccessProtocolGuid,
                  NULL,
                  (VOID **)&mSettingAccess
                  );
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    DEBUG ((DEBUG_ERROR, "%a Couldn't locate system setting access protocol\n", __FUNCTION__));
  }

  Status = GetPlatformKeyStore (&mSecureBootKeys, &mSecureBootKeysCount);
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    DEBUG ((DEBUG_ERROR, "%a Couldn't fetch platform key store %r!\n", __FUNCTION__, Status));
  }

  // Force-connect all controllers.
  //
  EfiBootManagerConnectAll ();

  // Set console mode: *not* VGA, no splashscreen logo.
  // Insure Gop is in Big Display mode prior to accessing GOP.
  SetGraphicsConsoleMode (GCM_NATIVE_RES);

  //
  // After the console is ready, get current video resolution
  // and text mode before launching setup at first time.
  //
  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&mGop
                  );

  if (EFI_ERROR (Status)) {
    mGop = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)NULL;
    FpPortSay ("[FP] FAIL no GOP\n");
    goto Exit;
  }

  // Determine if the Font Protocol is available
  //
  Status = gBS->LocateProtocol (
                  &gEfiHiiFontProtocolGuid,
                  NULL,
                  (VOID **)&mFont
                  );

  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    mFont  = (EFI_HII_FONT_PROTOCOL *)NULL;
    Status = EFI_UNSUPPORTED;
    FpPortSay ("[FP] FAIL no HII Font protocol\n");
    DEBUG ((DEBUG_ERROR, "ERROR [FP]: Failed to find Font protocol (%r).\r\n", Status));
    goto Exit;
  }

  // Locate the Simple Window Manager protocol.
  //
  Status = gBS->LocateProtocol (
                  &gMsSWMProtocolGuid,
                  NULL,
                  (VOID **)&mSWMProtocol
                  );

  if (EFI_ERROR (Status)) {
    mSWMProtocol = NULL;
    Status       = EFI_UNSUPPORTED;
    FpPortSay ("[FP] FAIL no SWM (window manager) protocol\n");
    DEBUG ((DEBUG_ERROR, "ERROR [FP]: Failed to find the window manager protocol (%r).\r\n", Status));
    goto Exit;
  }

  // Locate the on-screen keyboard (OSK) protocol.
  //
  Status = gBS->LocateProtocol (
                  &gMsOSKProtocolGuid,
                  NULL,
                  (VOID **)&mOSKProtocol
                  );

  if (EFI_ERROR (Status)) {
    Status       = EFI_UNSUPPORTED;
    mOSKProtocol = (MS_ONSCREEN_KEYBOARD_PROTOCOL *)NULL;
    FpPortSay ("[FP] FAIL no OSK (on-screen keyboard) protocol\n");
    DEBUG ((DEBUG_ERROR, "ERROR [FP]: Failed to find the on-screen keyboard protocol (%r).\r\n", Status));
    goto Exit;
  }

  // Set default on-screen keyboard size and position.  Disable icon auto-activation (set by BDS) since
  // we'll display the OSK ourselves when appropriate.
  //

  // Disable OSK icon auto-activation and self-refresh, and ensure keyboard is disabled.
  //
  mOSKProtocol->GetKeyboardMode (mOSKProtocol, &OSKMode);
  OSKMode &= ~(OSK_MODE_AUTOENABLEICON | OSK_MODE_SELF_REFRESH);
  mOSKProtocol->ShowKeyboard (mOSKProtocol, FALSE);
  mOSKProtocol->ShowKeyboardIcon (mOSKProtocol, FALSE);
  mOSKProtocol->SetKeyboardMode (mOSKProtocol, OSKMode);

  // Full width at the bottom; SetKeyboardSize transforms both drawing and hits.
  //
  mOSKProtocol->SetKeyboardSize (mOSKProtocol, FP_OSK_WIDTH_PERCENT);
  mOSKProtocol->SetKeyboardPosition (mOSKProtocol, BottomCenter, Docked);

  // First entry auto-opens the GUI after five seconds unless the user acts.
  FpRemoteRegister(ImageHandle);
  Status=FpBootMenuShow(mSWMProtocol,mGop,ImageHandle,TRUE);
  if (EFI_ERROR(Status)) goto Exit;

  if (mGop != NULL) {
    //
    // Get current video resolution and text mode.
    //
    mBootHorizontalResolution = mGop->Mode->Info->HorizontalResolution;
    mBootVerticalResolution   = mGop->Mode->Info->VerticalResolution;
  }

  // Hide the text cursor, but do not ClearScreen: the Boot Menu already owns
  // the whole framebuffer and clearing it produced the visible black frame on
  // the BootMenu -> GUI transition. SWM and DisplayEngine paint every region
  // they own immediately afterwards.
  //
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  // Initialize the Simple UI ToolKit.
  //
  Status = InitializeUIToolKit (ImageHandle);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [FP]: Failed to initialize the UI toolkit (%r).\r\n", Status));
    goto Exit;
  }

  // Register Front Page strings with the HII database.
  //
  InitializeStringSupport ();

  // Initialize HII data (ex: register strings, etc.).
  //
  InitializeFrontPage (TRUE);

  // Initialize the FrontPage User Interface.
  //
  Status = InitializeFrontPageUI ();

  if (EFI_SUCCESS != Status) {
    DEBUG ((DEBUG_ERROR, "ERROR [FP]: Failed to initialize the FrontPage user interface.  Status = %r\r\n", Status));
    goto Exit;
  }

  // Set the default form ID to show on the canvas.
  //
  mCurrentFormIndex = 0;
  Status            = EFI_SUCCESS;
  {
    EFI_STATUS Saved = BootLogSave ("gui-ready-tz-summary");
    QrLogTrace ("[LOGSAVE b327] GUI checkpoint -> %r (logfs only)\r\n", Saved);
  }
  // Start CDC before entering the form loop, so no GUI action is needed.
  mUsbPending = USB_MODE_CDC;
  FpApplyUsbMode ();

  // Display the specified FrontPage form.
  //
  do {
    // By default, we'll terminate FrontPage after processing the next Form unless the flag is reset.
    //
    mTerminateFrontPage = TRUE;

    CallFrontPage (mCurrentFormIndex);
  } while (FALSE == mTerminateFrontPage);

  if (mResetRequired) {
    ResetSystemWithSubtype (EfiResetCold, &gFrontPageResetGuid);
  }

  ProcessBootNext ();

  // Clean-up
  //
  UninitializeFrontPage ();

Exit:
  FpRemoteUnregister();
  return Status;
}

EFI_STATUS
GetAndDisplayBitmap (
  EFI_GUID  *FileGuid,
  UINTN     XCoord,
  BOOLEAN   XCoordAdj
  )
{
  EFI_STATUS                     Status;
  UINT8                          *BMPData    = NULL;
  UINTN                          BMPDataSize = 0;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *BltBuffer  = NULL;
  UINTN                          BltBufferSize;
  UINTN                          BitmapHeight;
  UINTN                          BitmapWidth;

  // Get the specified image from FV.
  //
  Status = GetSectionFromAnyFv (
             FileGuid,
             EFI_SECTION_RAW,
             0,
             (VOID **)&BMPData,
             &BMPDataSize
             );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [DE]: Failed to find bitmap file (GUID=%g) (%r).\r\n", FileGuid, Status));
    return Status;
  }

  // Convert the bitmap from BMP format to a GOP framebuffer-compatible form.
  //
  Status = TranslateBmpToGopBlt (
             BMPData,
             BMPDataSize,
             &BltBuffer,
             &BltBufferSize,
             &BitmapHeight,
             &BitmapWidth
             );
  if (EFI_ERROR (Status)) {
    FreePool (BMPData);
    DEBUG ((DEBUG_ERROR, "ERROR [DE]: Failed to convert bitmap file to GOP format (%r).\r\n", Status));
    return Status;
  }

  if (XCoordAdj == TRUE) {
    XCoord -= BitmapWidth;
  }

  mGop->Blt (
          mGop,
          BltBuffer,
          EfiBltBufferToVideo,
          0,
          0,
          XCoord,       // Upper Right corner
          ((mTitleBarHeight / 2) - (BitmapHeight / 2)),
          BitmapWidth,
          BitmapHeight,
          0
          );

  FreePool (BMPData);
  FreePool (BltBuffer);
  return Status;
}

/**
Acquire an Auth Token and save it in a protocol
**/
EFI_STATUS
GetAuthToken (
  CHAR16  *PasswordBuffer
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (
                  &gDfciAuthenticationProtocolGuid,
                  NULL,
                  (VOID **)&mAuthProtocol
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a - Failed to locate MsAuthProtocol. Can't use check auth. %r\n", __FUNCTION__, Status));
    mAuthProtocol = NULL;
    return Status;
  }

  if (PasswordBuffer != NULL) {
    Status = mAuthProtocol->AuthWithPW (mAuthProtocol, PasswordBuffer, StrLen (PasswordBuffer), &mAuthToken);
    DEBUG ((DEBUG_INFO, "%a Auth Token Acquired %x- %r\n", __FUNCTION__, mAuthToken, Status));
  } else {
    Status = mAuthProtocol->AuthWithPW (mAuthProtocol, NULL, 0, &mAuthToken);
    DEBUG ((DEBUG_INFO, "%a Auth Token Acquired with NULL Password %x - %r\n", __FUNCTION__, mAuthToken, Status));
  }

  if (!EFI_ERROR (Status) && (mAuthToken != DFCI_AUTH_TOKEN_INVALID)) {
    mFrontPageAuthTokenProtocol = (FRONT_PAGE_AUTH_TOKEN_PROTOCOL *)AllocateZeroPool (sizeof (mFrontPageAuthTokenProtocol));

    //
    // Regardless of the auth token value we install the protocol.
    // When system password is set, if user enters a invalid password, then the frontpage access will be restricted.
    // When there is no system password set, if auth token with a null request is returned invalid; we
    // still allow only a restricted access of the menu. The protocol with invalid auth token will not be used.
    //
    mFrontPageAuthTokenProtocol->AuthToken = (UINTN)mAuthToken;
    Status                                 = gBS->InstallMultipleProtocolInterfaces (
                                                    &mImageHandle,
                                                    &gMsFrontPageAuthTokenProtocolGuid,
                                                    mFrontPageAuthTokenProtocol,
                                                    NULL
                                                    );

    if (Status == EFI_SUCCESS) {
      DEBUG ((DEBUG_INFO, "%a FrontPageAuthTokenProtocol was successfully installed %r\n", __FUNCTION__, Status));
    }
  }

  return Status;
}
