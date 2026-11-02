#ifndef USB_BRINGUP_PROTOCOL_H_
#define USB_BRINGUP_PROTOCOL_H_

#include <Uefi.h>

#define USB_BRINGUP_PROTOCOL_GUID \
  { 0x7746d19b, 0x1b2e, 0x4a53, { 0x8d, 0x76, 0x39, 0x6a, 0x95, 0x20, 0xc7, 0xe4 } }

#define USB_MODE_DISABLED 0U
#define USB_MODE_NATIVE 1U
#define USB_MODE_STORAGE 2U
#define USB_MODE_CDC 3U
#define USB_MODE_REFRESH 4U
#define USB_MODE_RAMDISK 5U
#define USB_MODE_RAM_RELEASE 6U
#define USB_RAM_DISK_DEVICE_GUID \
  {0xb3275120,0x6f0a,0x45de,{0x81,0x2c,0x54,0x41,0x4d,0x52,0x41,0x4d}}

typedef struct _USB_BRINGUP_PROTOCOL USB_BRINGUP_PROTOCOL;
typedef EFI_STATUS (EFIAPI *USB_REMOTE_EXECUTOR)(VOID *Context, CONST CHAR8 *Command,
                                               CHAR8 *Output, UINTN Capacity, UINTN *Bytes);
struct _USB_BRINGUP_PROTOCOL {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *TestOnce)(IN USB_BRINGUP_PROTOCOL *This);
  VOID *Context;
  // Revision 2: choose full speed before the first manual DMA attempt.
  EFI_STATUS (EFIAPI *TestFullSpeedOnce)(IN USB_BRINGUP_PROTOCOL *This);
  // Revision 3: explicit disconnect/reconfigure; logfs is a read-only snapshot.
  // Revision 4: separate volatile 512MiB RAM disk, no UFS write capability.
  EFI_STATUS (EFIAPI *SelectMode)(IN USB_BRINGUP_PROTOCOL *This, UINT32 Mode);
  EFI_STATUS (EFIAPI *GetMode)(IN USB_BRINGUP_PROTOCOL *This, UINT32 *Mode, BOOLEAN *Configured);
  // Revision 5: RAM-only capacity selection and a scoped CDC Shell console.
  // A different size requires releasing the previous disk first.
  EFI_STATUS (EFIAPI *SetRamSize)(IN USB_BRINGUP_PROTOCOL *This, UINT32 MiB);
  EFI_STATUS (EFIAPI *GetRamSize)(IN USB_BRINGUP_PROTOCOL *This, UINT32 *MiB, BOOLEAN *Allocated);
  EFI_STATUS (EFIAPI *SetShellConsole)(IN USB_BRINGUP_PROTOCOL *This, BOOLEAN Enable);
  // Revision 6: application-owned command execution, outside the USB event pump.
  EFI_STATUS (EFIAPI *SetRemoteExecutor)(IN USB_BRINGUP_PROTOCOL *This, USB_REMOTE_EXECUTOR Execute, VOID *Context);
};

#endif
