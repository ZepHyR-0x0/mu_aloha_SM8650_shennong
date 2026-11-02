#ifndef WCD_USB_LIB_H_
#define WCD_USB_LIB_H_
#include <Uefi.h>
typedef UINT32 (EFIAPI *WCD_I2C_TRANSFER)(UINT64, VOID *, UINT32, UINT32, VOID *, UINT32, UINT32 *, UINT32);
typedef struct {
  UINT64 Handle;
  VOID *Config;
  WCD_I2C_TRANSFER Read;
  WCD_I2C_TRANSFER Write;
} WCD_USB_IO;
EFI_STATUS WcdReadRegister (IN WCD_USB_IO *Io, IN UINT16 Register, OUT UINT8 *Value);
EFI_STATUS WcdConfigureUsb (IN WCD_USB_IO *Io);
EFI_STATUS WcdResetUsb (IN WCD_USB_IO *Io);
#endif
