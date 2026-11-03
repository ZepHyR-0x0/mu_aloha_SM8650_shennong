/** Bounded pulse/pattern/mode diagnostics. SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/PmicHapticsLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/ShellParameters.h>
STATIC CONST CHAR16 *mModes[] = { L"native", L"direct", L"autores" };

STATIC VOID Help (VOID)
{
  Print (L"HapticTest.efi status | modes | matrix\n"
         L"HapticTest.efi [pulse] <ms> <mV> [amplitude] [native|direct|autores]\n"
         L"HapticTest.efi pattern <ms> <mV> <amplitude> <count> <gap-ms> [mode]\n"
         L"HapticTest.efi level <ms> <percent> [mode] (default direct, normal 1600 mV)\n"
         L"ms=50..1000; percent=0..100; mV=1400..1750 in 50 mV steps; amplitude=0..255\n"
         L"count=1..3; gap=50..1000 ms. matrix compares three 500 ms modes.\n"
         L"native: native duty, existing autores; direct: existing duty, autores off;\n"
         L"autores: existing duty, autores on. Direct PLAY source=1, TLRA=0602.\n"
         L"status reads registers; cold native cache may be initialized by disabled Stop.\n");
}

STATIC EFI_STATUS Mode (CHAR16 *Name, PMIC_HAPTICS_MODE *Result)
{
  for (UINTN I=0; I<ARRAY_SIZE(mModes); I++) {
    if (StrCmp(Name,mModes[I])==0) { *Result=(PMIC_HAPTICS_MODE)I; return EFI_SUCCESS; }
  }
  return EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS Number (CHAR16 *Text, UINTN *Value)
{
  CHAR16 *End=NULL;
  return !RETURN_ERROR(StrDecimalToUintnS(Text,&End,Value)) && End!=NULL && *End==0
    ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS Run (UINTN Ms, UINTN Mv, UINTN Amplitude, UINTN Count, UINTN Gap, PMIC_HAPTICS_MODE Selected, UINTN Percent)
{
  EFI_STATUS Status=PmicHapticsConfigureMode(Selected);
  if (Percent!=MAX_UINTN) {
    Mv=1600;Amplitude=255*(0x7fff*Percent/100)/0x7fff;
    if (!EFI_ERROR(Status)) Status=PmicHapticsConfigureIntensity((UINT32)Percent);
  } else if (!EFI_ERROR(Status)) Status=PmicHapticsConfigure((UINT32)Mv,(UINT8)Amplitude);
  if (EFI_ERROR(Status)) return Status;
  for (UINTN I=0; I<Count; I++) {
    CHAR8 Text[180];
    UINTN N=AsciiSPrint(Text,sizeof(Text),"[HAPTIC TRIAL] mode=%u pulse=%u/%u duration_ms=%u vmax_mv=%u amplitude=%u gap_ms=%u\r\n",
      (UINT32)Selected,(UINT32)I+1,(UINT32)Count,(UINT32)Ms,(UINT32)Mv,(UINT32)Amplitude,(UINT32)Gap);
    SerialPortWrite((UINT8 *)Text,N);
    Print(L"[HAPTIC] mode=%s pulse=%u/%u %u ms %u mV amplitude=%u\n",mModes[Selected],(UINT32)I+1,(UINT32)Count,(UINT32)Ms,(UINT32)Mv,(UINT32)Amplitude);
    Status=PmicHapticsPulse(Ms*1000);
    N=AsciiSPrint(Text,sizeof(Text),"[HAPTIC TRIAL] mode=%u pulse=%u result=%r pending=%u\r\n",(UINT32)Selected,(UINT32)I+1,Status,PmicHapticsIsEnabled());
    SerialPortWrite((UINT8 *)Text,N);
    Print(L"[HAPTIC] pulse/stop -> %r; stop pending=%u\n",Status,PmicHapticsIsEnabled());
    if (EFI_ERROR(Status)) return Status;
    if (I+1<Count) gBS->Stall(Gap*1000);
  }
  return Status;
}

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_SHELL_PARAMETERS_PROTOCOL *Args=NULL;
  UINTN Ms=500,Mv=1600,Amplitude=255,Count=1,Gap=250,First=1,Numeric;
  PMIC_HAPTICS_MODE Selected=PmicHapticsNativeDuty;
  EFI_STATUS Status;
  (VOID)SystemTable;
  if (EFI_ERROR(gBS->HandleProtocol(ImageHandle,&gEfiShellParametersProtocolGuid,(VOID **)&Args)) || Args==NULL) return EFI_NOT_FOUND;
  if (Args->Argc>1 && (StrCmp(Args->Argv[1],L"help")==0 || StrCmp(Args->Argv[1],L"modes")==0)) { Help(); return Args->Argc==2?EFI_SUCCESS:EFI_INVALID_PARAMETER; }
  if (Args->Argc>1 && StrCmp(Args->Argv[1],L"status")==0) {
    if (Args->Argc!=2) return EFI_INVALID_PARAMETER;
    Status=PmicHapticsDiagnostics();
    Print(L"[HAPTIC] status -> %r (register snapshot in CDC logcat)\n",Status);
    return Status;
  }
  if (Args->Argc>1 && StrCmp(Args->Argv[1],L"matrix")==0) {
    EFI_STATUS Overall=EFI_SUCCESS;
    if (Args->Argc!=2) return EFI_INVALID_PARAMETER;
    for (UINTN I=0; I<PmicHapticsModeCount; I++) {
      Status=Run(500,1600,255,1,250,(PMIC_HAPTICS_MODE)I,MAX_UINTN);
      if (EFI_ERROR(Status)) Overall=Status;
      if (PmicHapticsIsEnabled()) break;
      if (I+1<PmicHapticsModeCount) gBS->Stall(500000);
    }
    return Overall;
  }
  if (Args->Argc>1 && StrCmp(Args->Argv[1],L"level")==0) {
    UINTN Percent=0;
    Selected=PmicHapticsDirect;
    if ((Args->Argc!=4 && Args->Argc!=5) || EFI_ERROR(Number(Args->Argv[2],&Ms)) || EFI_ERROR(Number(Args->Argv[3],&Percent))) return EFI_INVALID_PARAMETER;
    if (Args->Argc==5 && EFI_ERROR(Mode(Args->Argv[4],&Selected))) return EFI_INVALID_PARAMETER;
    if (Ms<50 || Ms>1000 || Percent>100) return EFI_INVALID_PARAMETER;
    return Run(Ms,1600,255,1,250,Selected,Percent);
  }
  if (Args->Argc>1) {
    BOOLEAN Pattern=StrCmp(Args->Argv[1],L"pattern")==0;
    if (Pattern || StrCmp(Args->Argv[1],L"pulse")==0) First=2;
    Numeric=Args->Argc-First;
    if (Numeric>0 && !EFI_ERROR(Mode(Args->Argv[Args->Argc-1],&Selected))) Numeric--;
    if ((!Pattern && (Numeric<2 || Numeric>3)) || (Pattern && Numeric!=5)) { Help(); return EFI_INVALID_PARAMETER; }
    UINTN *Values[]={&Ms,&Mv,&Amplitude,&Count,&Gap};
    for (UINTN I=0; I<Numeric; I++) if (EFI_ERROR(Number(Args->Argv[First+I],Values[I]))) return EFI_INVALID_PARAMETER;
  }
  if (Ms<50 || Ms>1000 || Mv<1400 || Mv>1750 || Mv%50!=0 || Amplitude>255 || Count<1 || Count>3 || Gap<50 || Gap>1000) return EFI_INVALID_PARAMETER;
  return Run(Ms,Mv,Amplitude,Count,Gap,Selected,MAX_UINTN);
}
