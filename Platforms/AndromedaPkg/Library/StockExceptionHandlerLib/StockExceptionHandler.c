// Copyright (c) 2026, ProjectMu shennong port.
// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/DebugSupport.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

STATIC volatile BOOLEAN mFaultPrinting;

STATIC VOID
FaultSay (CONST CHAR8 *Format, ...)
{
  CHAR8 Buffer[192];
  VA_LIST Args;
  UINTN Length;
  VA_START (Args, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Args);
  VA_END (Args);
  SerialPortWrite ((UINT8 *)Buffer, Length);
}

// The stock bring-up must retain ESR/FAR before trying to resolve images or
// follow fault-time SP/FP. Corrupt stacks cannot safely be used for backtraces.
// Use only the vector's saved context and the existing serial/RAM log sink.
VOID
EFIAPI __attribute__((noinline))
ShennongStockExceptionDump (EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT Context)
{
  EFI_SYSTEM_CONTEXT_AARCH64 *Cpu = Context.SystemContextAArch64;
  if (mFaultPrinting) {
    // The serial sink itself may have faulted. Do not try it again on recursion.
    CpuDeadLoop ();
    return;
  }
  mFaultPrinting = TRUE;
  if (Cpu == NULL) {
    FaultSay ("\r\n[USBFAULT b319] type=%u missing context; halt\r\n", (UINT32)Type);
    CpuDeadLoop ();
    return;
  }
  FaultSay ("\r\n[USBFAULT b319] type=%u ELR=%lx ESR=%lx FAR=%lx\r\n",
    (UINT32)Type, Cpu->ELR, Cpu->ESR, Cpu->FAR);
  FaultSay ("[USBFAULT b319] LR=%lx SP=%lx FP=%lx SPSR=%lx\r\n",
    Cpu->LR, Cpu->SP, Cpu->FP, Cpu->SPSR);
  FaultSay ("[USBFAULT b319] X0=%lx X1=%lx X2=%lx X3=%lx\r\n",
    Cpu->X0, Cpu->X1, Cpu->X2, Cpu->X3);
  FaultSay ("[USBFAULT b319] X8=%lx X9=%lx X10=%lx X11=%lx\r\n",
    Cpu->X8, Cpu->X9, Cpu->X10, Cpu->X11);
  FaultSay ("[USBFAULT b319] X19=%lx X20=%lx X21=%lx X22=%lx\r\n",
    Cpu->X19, Cpu->X20, Cpu->X21, Cpu->X22);
  FaultSay ("[USBFAULT b319] X23=%lx X24=%lx X25=%lx; halt\r\n",
    Cpu->X23, Cpu->X24, Cpu->X25);
  CpuDeadLoop ();
}
