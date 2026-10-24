/** @file
  FrontPage routines to handle the callbacks and browser calls

Copyright (c) 2004 - 2012, Intel Corporation. All rights reserved.<BR>
Copyright (c), Microsoft Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef _FRONT_PAGE_H_
#define _FRONT_PAGE_H_

#include <Protocol/FormBrowser2.h>
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

#endif // _FRONT_PAGE_H_
