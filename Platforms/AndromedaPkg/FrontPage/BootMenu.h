/** @file

  Pre-GUI boot menu shown by FrontPage before the form stack is initialized.

  Copyright (C) Microsoft Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef FP_BOOT_MENU_H
#define FP_BOOT_MENU_H

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleWindowManager.h>

/**
  Display the pre-GUI boot menu and act on the selection.

  Returns when the user picks "Enter GUI" or when the 10 second timeout
  expires, so the caller can continue starting the FrontPage form stack.
  Shell / boot option / reboot are handled here; they either never return or
  come back to the menu.

  @param[in]  Swm          Simple Window Manager (drawing goes through it).
  @param[in]  Gop          Graphics output used as the HII font target.
  @param[in]  ImageHandle  This image, for BltWindow/StringToWindow.

  @retval EFI_SUCCESS     Continue into the GUI.
  @retval EFI_NOT_FOUND   No keypad input; the menu was skipped.
**/
EFI_STATUS
EFIAPI
FpBootMenuShow (
  IN MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *Swm,
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *Gop,
  IN EFI_HANDLE                          ImageHandle
  );

#endif // FP_BOOT_MENU_H
