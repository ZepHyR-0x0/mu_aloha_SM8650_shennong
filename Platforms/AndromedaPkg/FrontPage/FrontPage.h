/** @file
  FrontPage routines to handle the callbacks and browser calls

Copyright (c) 2004 - 2012, Intel Corporation. All rights reserved.<BR>
Copyright (c), Microsoft Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef _FRONT_PAGE_H_
#define _FRONT_PAGE_H_

#include <Protocol/FormBrowser2.h>
#include <Protocol/SimpleWindowManager.h>
#include <Protocol/HiiConfigAccess.h>
#include "FrontPageVfr.h"  // all shared VFR / C constants here.
#include <DfciSystemSettingTypes.h>
#include <Protocol/DfciSettingAccess.h>
#include <Library/HiiLib.h>
#include <Protocol/MsFrontPageAuthTokenProtocol.h>
#include <Protocol/DfciAuthentication.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/HiiConfigRouting.h>

//
// These are the VFR compiler generated data representing our VFR data.
//
extern UINT8  FrontPageVfrBin[];

extern UINTN  mCallbackKey;
EFI_STATUS FpPrepareReboot (EFI_QUESTION_ID QuestionId);
BOOLEAN FpRebootPending (VOID);
VOID FpRebootErrorText (CHAR16 *Buffer, UINTN Bytes, EFI_STATUS Status);
EFI_STATUS FpRebootSelected (VOID);
UINT8 FpGetUsbUiMode (VOID);
BOOLEAN FpGetRamAllocated (VOID);
EFI_STATUS FpQueueUsbUiMode (UINT8 UiMode);
EFI_STATUS FpQueueRamDelete (VOID);

#define FRONT_PAGE_CALLBACK_DATA_SIGNATURE  SIGNATURE_32 ('F', 'P', 'C', 'B')

typedef struct {
  UINTN                             Signature;

  //
  // HII relative handles
  //
  EFI_HII_HANDLE                    HiiHandle;
  EFI_HANDLE                        DriverHandle;
  EFI_STRING_ID                     *LanguageToken;

  //
  // Produced protocols
  //
  EFI_HII_CONFIG_ACCESS_PROTOCOL    ConfigAccess;
} FRONT_PAGE_CALLBACK_DATA;

extern FRONT_PAGE_CALLBACK_DATA  mFrontPagePrivate;
extern EFI_GUID                  gMuFrontPageConfigFormSetGuid;

/**
  Initialize HII information for the FrontPage


  @param InitializeHiiData    TRUE if HII elements need to be initialized.

  @retval  EFI_SUCCESS        The operation is successful.
  @retval  EFI_DEVICE_ERROR   If the dynamic opcode creation failed.

**/
EFI_STATUS
InitializeFrontPage (
  BOOLEAN  InitializeHiiData
  );

/**
  Draws the Front Page Title Bar.

  @retval  EFI_SUCCESS        Success.
**/
EFI_STATUS
RenderTitlebar (
  VOID
  );

/**
  Draws the FrontPage Master Frame and its Top-Level menu.

  @retval  EFI_SUCCESS        Success.
**/
EFI_STATUS
RenderMasterFrame (
  VOID
  );

/**
  Acquire an Auth Token and save it in a protocol
**/
EFI_STATUS
GetAuthToken (
  CHAR16  *PasswordBuffer
  );

/**
  Dump read-only storage and I2C diagnostics to the log ring.
**/
VOID
FpDiagDump (
  VOID
  );

VOID FpHealthReport (VOID);
VOID FpFileBrowser (MS_SIMPLE_WINDOW_MANAGER_PROTOCOL *Swm, EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop);
VOID FpPowerStatus (MS_SIMPLE_WINDOW_MANAGER_PROTOCOL *Swm, EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop);
VOID FpTestPage (MS_SIMPLE_WINDOW_MANAGER_PROTOCOL *Swm, EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop);

/** Deprecated heartbeat hook; returns zero and performs no I2C write. */
UINT32
FpDiagI2cReassert (
  VOID
  );

EFI_STATUS FpQueueUsbMode (EFI_QUESTION_ID QuestionId);
VOID FpGetRamSizeText (CHAR16 *Text, UINTN Bytes);
VOID FpGetUsbModeText (CHAR16 *Text, UINTN Bytes);
EFI_STATUS FpRouteRamSize (CONST CHAR16 *Text);
EFI_STATUS FpQueueRamSize (CONST CHAR16 *Text);

#endif // _FRONT_PAGE_H_
