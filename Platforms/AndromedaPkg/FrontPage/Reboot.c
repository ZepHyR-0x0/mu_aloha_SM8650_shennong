/** Reboot targets via the resident stock ResetRuntimeDxe. No UFS/BCB writes. */
#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "FrontPage.h"
#include "BootLog.h"
#include "RebootData.h"

STATIC EFI_GUID mResetFile = {0x3AE17DB7,0x3CC5,0x4B89,{0x93,0x04,0x95,0x49,0x21,0x10,0x57,0xEF}};
// ResetRuntimeDxe +70F8; old Qcom EFIPmicPon.h names another GUID/layout.
STATIC EFI_GUID mPonGuid = {0x9BA45B66,0xEFA4,0x441C,{0xA3,0xE4,0xED,0x22,0x24,0x78,0x6B,0xE2}};
STATIC EFI_GUID mPmicFile={0x5776232E,0x082D,0x4B75,{0x9A,0x0E,0xFE,0x1D,0x13,0xF7,0xA5,0xD9}};
STATIC EFI_GUID mSpmiFile={0x2A7B4BEF,0x80CD,0x49E1,{0xB4,0x73,0x37,0x4B,0xA4,0xD6,0x73,0xFC}};
STATIC EFI_GUID mRebootSpmiGuid={0xFA5F306B,0xF47D,0x4AC4,{0xA4,0x7D,0x88,0x2F,0x82,0x04,0xEC,0x30}};
typedef UINT32 (EFIAPI *FP_SPMI_READ)(VOID *,UINT32,UINT32,UINT32,UINT32,UINT8 *,UINT32,UINT32 *);
typedef UINT32 (EFIAPI *FP_SPMI_WRITE)(VOID *,UINT32,UINT32,UINT32,UINT32,UINT8 *,UINT32);
STATIC struct { UINT64 Revision;FP_SPMI_READ Read;FP_SPMI_WRITE Write; } *mRebootSpmi;
STATIC BOOLEAN mRebootMaskedWriteSuppressed;
STATIC CONST CHAR16 *mRebootPhase=L"stock service verification";
STATIC UINT8 mRebootReason;
STATIC UINT8 *mResetBase;
STATIC EFI_RUNTIME_SERVICES mResetRt;

STATIC EFI_STATUS EFIAPI
FpResetDenyVariable (CHAR16 *Name, EFI_GUID *Guid, UINT32 Attributes, UINTN Size, VOID *Data)
{
  (VOID)Name; (VOID)Guid; (VOID)Attributes; (VOID)Size; (VOID)Data;
  return EFI_WRITE_PROTECTED;
}

STATIC VOID
FpRebootTrace (CONST CHAR8 *Phase, UINT8 Reason, EFI_STATUS Status)
{
  CHAR8 Text[192];
  UINTN Size = AsciiSPrint (Text, sizeof (Text),
    "[REBOOT b327] %a target=%u stock RESET_PARAM(26)/PMIC/SMC -> %r\r\n", Phase, Reason, Status);
  SerialPortWrite ((UINT8 *)Text, Size);
}

STATIC BOOLEAN
FpResetReadable (CONST VOID *Address, UINTN Size)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  UINT64 At = (UINTN)Address;
  if (Address == NULL || Size == 0 || gDS == NULL ||
      EFI_ERROR (gDS->GetMemorySpaceDescriptor (At, &D)) ||
      D.GcdMemoryType != EfiGcdMemoryTypeSystemMemory || (D.Attributes & EFI_MEMORY_RP) != 0 ||
      At < D.BaseAddress || Size > D.Length || At - D.BaseAddress > D.Length - Size) return FALSE;
  return TRUE;
}

STATIC BOOLEAN
FpResetFilePath (EFI_DEVICE_PATH_PROTOCOL *Path, EFI_GUID *File)
{
  UINTN I, Length;
  // Bounded device-path walk; the GUID comes from the FV file node.
  for (I = 0; I < 32 && FpResetReadable (Path, sizeof (*Path)); ++I) {
    Length = DevicePathNodeLength (Path);
    if (Length < sizeof (*Path) || Length > 1024 || !FpResetReadable (Path, Length)) return FALSE;
    if (DevicePathType (Path) == MEDIA_DEVICE_PATH && DevicePathSubType (Path) == MEDIA_PIWG_FW_FILE_DP &&
        Length == sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH) &&
        CompareGuid (&((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Path)->FvFileName, File)) return TRUE;
    if (IsDevicePathEnd (Path)) return FALSE;
    Path = NextDevicePathNode (Path);
  }
  return FALSE;
}

STATIC EFI_STATUS
FpVerifyStockReset (VOID)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, I;
  EFI_LOADED_IMAGE_PROTOCOL *Image;
  EFI_STATUS Status;
  UINT8 *Base;
  VOID *Pon = NULL;
  BOOLEAN Verified = FALSE;
  mResetBase = NULL;
  if (gRT == NULL || gRT->ResetSystem == NULL) return EFI_NOT_READY;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) return Status;
  for (I = 0; I < Count; ++I) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiLoadedImageProtocolGuid, (VOID **)&Image)) ||
        Image == NULL || Image->ImageSize < 0x9000 || !FpResetFilePath (Image->FilePath,&mResetFile)) continue;
    Base = Image->ImageBase;
    if ((UINTN)gRT->ResetSystem != (UINTN)Base + 0x1534 || !FpResetReadable (Base, 0x7000)) continue;
    // Entry, reason-byte load, PMIC byte shift and SMC from this stock PE.
    if (*(UINT32 *)(Base + 0x1534) == 0xA9BD7BFD &&
        *(UINT32 *)(Base + 0x295C) == 0x38606A60 &&
        *(UINT32 *)(Base + 0x2D30) == 0x531F7A62 &&
        *(UINT32 *)(Base + 0x2D60) == 0xD4000003) {
      EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
      if (FpResetReadable (Base + 0x71A8, 8) &&
          *(EFI_RUNTIME_SERVICES **)(Base + 0x71A8) == gRT &&
          !EFI_ERROR (gDS->GetMemorySpaceDescriptor ((UINTN)Base + 0x71A8, &D)) &&
          (D.Attributes & EFI_MEMORY_RO) == 0) {
        Verified = TRUE;
        mResetBase = Base;
      }
    }
    break;
  }
  FreePool (Handles);
  if (!Verified) return EFI_UNSUPPORTED;
  Status = gBS->LocateProtocol (&mPonGuid, NULL, &Pon);
  // Bind the private PON interface to this phone's resident PmicDxe, including
  // the enum mapping SOFT_SPARE3 -> SDAM byte7148. A GUID alone is insufficient.
  if (EFI_ERROR (Status)) return Status;
  if (!FpResetReadable (Pon, 0x30) || *(VOID **)((UINT8 *)Pon + 0x20) == NULL ||
      *(VOID **)((UINT8 *)Pon + 0x28) == NULL) return EFI_NOT_READY;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status;
  Verified=FALSE;
  for (I=0;I<Count;I++) {
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiLoadedImageProtocolGuid,(VOID **)&Image)) ||
        Image==NULL || Image->ImageSize<0x3F000 || !FpResetFilePath(Image->FilePath,&mPmicFile)) continue;
    Base=Image->ImageBase;
    if (!FpResetReadable(Base+0x23CC,0x90) || !FpResetReadable(Base+0x324B0,0x30) ||
        !FpResetReadable(Base+0x2A740,16) || !FpResetReadable(Base+0x2C3F0,10)) continue;
    Verified=(UINTN)Pon==(UINTN)Base+0x324B0 && *(UINT64 *)Pon==0x1000B &&
      ((FP_RESET_PON *)Pon)->Get==(FP_PON_GET)(Base+0x23CC) &&
      ((FP_RESET_PON *)Pon)->Set==(FP_PON_SET)(Base+0x2418) &&
      *(UINT32 *)(Base+0x23CC)==0xB4000122 && *(UINT32 *)(Base+0x2418)==0xA9BF7BFD &&
      *(UINT32 *)(Base+0x23E4)==0x911D0108 &&
      *(UINT32 *)(Base+0x2A74C)==4 && *(UINT16 *)(Base+0x2C3F8)==0x7148;
    break;
  }
  FreePool(Handles);
  return Verified?EFI_SUCCESS:EFI_UNSUPPORTED;
}

STATIC EFI_STATUS FpVerifyRebootSpmi (VOID)
{
  EFI_HANDLE *Handles=NULL;UINTN Count=0,I;EFI_LOADED_IMAGE_PROTOCOL *Image;
  UINT8 *Base;EFI_STATUS S;BOOLEAN Verified=FALSE;
  mRebootSpmi=NULL;mRebootMaskedWriteSuppressed=FALSE;
  S=gBS->LocateProtocol(&mRebootSpmiGuid,NULL,(VOID **)&mRebootSpmi);
  if (EFI_ERROR(S)) return S;
  if (!FpResetReadable(mRebootSpmi,sizeof(*mRebootSpmi))) return EFI_UNSUPPORTED;
  S=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Handles);
  if (EFI_ERROR(S)) return S;
  for (I=0;I<Count;I++) {
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiLoadedImageProtocolGuid,(VOID **)&Image)) ||
        Image==NULL || Image->ImageSize<0xB000 || !FpResetFilePath(Image->FilePath,&mSpmiFile)) continue;
    Base=Image->ImageBase;
    Verified=FpResetReadable(Base+0x7098,32) && (UINTN)mRebootSpmi==(UINTN)Base+0x7098 &&
      mRebootSpmi->Revision==0x10003 && mRebootSpmi->Read==(FP_SPMI_READ)(Base+0x13EC) &&
      mRebootSpmi->Write==(FP_SPMI_WRITE)(Base+0x14AC);
    if (Verified && FpResetReadable(Base+0x154C,8))
      mRebootMaskedWriteSuppressed=*(UINTN *)(Base+0x70B0)==(UINTN)Base+0x154C &&
        *(UINT32 *)(Base+0x154C)==0x2A1F03E0 && *(UINT32 *)(Base+0x1550)==0xD65F03C0;
    break;
  }
  FreePool(Handles);if (!Verified) mRebootSpmi=NULL;
  return Verified?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI FpDirectReasonGet (UINT32 Device,UINT32 Register,UINT8 *Value)
{
  UINT32 Done=0,Rc;
  if (Device!=0 || Register!=3 || Value==NULL || mRebootSpmi==NULL) return EFI_INVALID_PARAMETER;
  *Value=0;Rc=mRebootSpmi->Read(mRebootSpmi,0,0,0,0x7148,Value,1,&Done);
  CHAR8 Text[128];UINTN N=AsciiSPrint(Text,sizeof(Text),"[REBOOT b327] SPMI read sid0:7148 rc=%u done=%u value=%02x\r\n",Rc,Done,*Value);
  SerialPortWrite((UINT8 *)Text,N);
  return Rc==0 && Done==1?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS EFIAPI FpDirectReasonSet (UINT32 Device,UINT32 Register,UINT8 Value,UINT8 Mask)
{
  UINT8 Before;UINT32 Rc;EFI_STATUS S;
  if (Device!=0 || Register!=3 || Mask!=0xFE || mRebootSpmi==NULL) return EFI_INVALID_PARAMETER;
  S=FpDirectReasonGet(Device,Register,&Before);if (EFI_ERROR(S)) return S;
  Value=(UINT8)((Before&1)|(Value&Mask));
  Rc=mRebootSpmi->Write(mRebootSpmi,0,0,0,0x7148,&Value,1);
  CHAR8 Text[128];UINTN N=AsciiSPrint(Text,sizeof(Text),"[REBOOT b327] SPMI write sid0:7148 rc=%u value=%02x mask=FE\r\n",Rc,Value);
  SerialPortWrite((UINT8 *)Text,N);
  return Rc==0?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
VOID FpRebootErrorText (CHAR16 *Buffer,UINTN Bytes,EFI_STATUS Status)
{
  UnicodeSPrint(Buffer,Bytes,L"Restart stopped at %s: %r. See QR Log for REBOOT details.",mRebootPhase,Status);
}

EFI_STATUS
FpPrepareReboot (EFI_QUESTION_ID QuestionId)
{
  EFI_STATUS Status;
  UINT8 Reason;
  mRebootReason = 0;
  mRebootPhase=L"stock service verification";
  if (QuestionId == FRONT_PAGE_ACTION_REBOOT_FASTBOOT) Reason = 2;
  else if (QuestionId == FRONT_PAGE_ACTION_REBOOT_RECOVERY) Reason = 1;
  else return EFI_INVALID_PARAMETER;
  mRebootPhase=L"stock service verification";
  Status = FpVerifyStockReset ();
  FpRebootTrace ("prepare", Reason, Status);
  if (!EFI_ERROR (Status)) mRebootReason = Reason;
  return Status;
}

BOOLEAN FpRebootPending (VOID) { return mRebootReason != 0; }

EFI_STATUS
FpRebootSelected (VOID)
{
  FP_RESET_DATA Data;
  EFI_STATUS Status;
  EFI_RUNTIME_SERVICES **Slot;
  FP_RESET_PON *Pon = NULL;
  UINT8 Reason = mRebootReason;
  mRebootReason = 0;
  if (!FpEncodeReset (Reason, &Data)) return EFI_INVALID_PARAMETER;
  Status = FpVerifyStockReset ();
  if (EFI_ERROR (Status)) return Status;
  FpRebootTrace ("confirmed", Reason, EFI_SUCCESS);
  Status = FpVerifyStockReset ();
  if (EFI_ERROR (Status)) return Status;
  // Native 2774 increments BSPowerCycles with SetVariable. Keep that optional
  // accounting RAM-private and deny every variable write in this reset module.
  // BootLogSave above remains the sole authorized filesystem write.
  if (gRT->Hdr.HeaderSize < sizeof (EFI_TABLE_HEADER) || gRT->Hdr.HeaderSize > sizeof (mResetRt) ||
      gBS->CalculateCrc32 == NULL) return EFI_UNSUPPORTED;
  mResetRt = *gRT;
  mResetRt.SetVariable = FpResetDenyVariable;
  mResetRt.Hdr.CRC32 = 0;
  Status = gBS->CalculateCrc32 (&mResetRt, mResetRt.Hdr.HeaderSize, &mResetRt.Hdr.CRC32);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->LocateProtocol (&mPonGuid, NULL, (VOID **)&Pon);
  if (EFI_ERROR (Status)) return Status;
  if (!FpResetReadable (Pon, sizeof (*Pon))) return EFI_NOT_READY;
  // Stock 2CC0 ignores transport errors. Verify the exact reason beforehand
  // so a failed PMIC operation cannot silently reboot to the wrong destination.
  CONST CHAR16 *Phases[]={L"PMIC reason",L"PMIC initial read",L"PMIC reason write",L"PMIC readback",L"PMIC rollback"};
  // Existing SPMIDxe patches its +18 RMW method to MOV W0,WZR;RET. Native
  // PmicDxe 2418 -> 104A4 -> A7D4 -> 1DB0C calls that method, reporting success
  // without a write. Keep the global patch (boot PMIC/haptics safety) and stage
  // ONLY the verified reset-reason byte through intact ReadLong/WriteLong.
  mRebootPhase=L"SPMI service verification";Status=FpVerifyRebootSpmi();
  if (EFI_ERROR(Status)) return Status;
  FP_RESET_PON Direct={{0},FpDirectReasonGet,FpDirectReasonSet};
  if (mRebootMaskedWriteSuppressed) {
    FpRebootTrace("masked-write suppressed; verified single-byte route",Reason,EFI_SUCCESS);
    Status=FpStageReset(&Direct,Reason);
  } else Status=FpStageReset(Pon,Reason);
  mRebootPhase=Phases[MIN(mFpResetStage,4)];
  CHAR8 Text[192];UINTN N=AsciiSPrint(Text,sizeof(Text),"[REBOOT b327] reason stage=%u direct=%u previous=%02x actual=%02x -> %r\r\n",
    mFpResetStage,mRebootMaskedWriteSuppressed,mFpResetBefore,mFpResetActual,Status);SerialPortWrite((UINT8 *)Text,N);
  // Only retry an INITIAL READ failure: no native write has started. The SPMI
  // fallback is identity-bound, one byte, and uses the same mask/readback logic.
  if (EFI_ERROR(Status) && mFpResetStage==1 && !mRebootMaskedWriteSuppressed) {
    mRebootPhase=L"SPMI service verification";Status=FpVerifyRebootSpmi();
    if (!EFI_ERROR(Status)) {
      Status=FpStageReset(&Direct,Reason);mRebootPhase=Phases[MIN(mFpResetStage,4)];
      FpRebootTrace("verified SPMI fallback",Reason,Status);
    }
  }
  FpRebootTrace ("PMIC reason readback", Reason, Status);
  if (EFI_ERROR (Status)) return Status;
  // Persist the verified reason, not an earlier snapshot lacking the readback.
  (VOID)BootLogSave (Reason == 2 ? "reboot-fastboot" : "reboot-recovery");
  Status = FpVerifyStockReset ();
  if (EFI_ERROR (Status)) return Status;
  Slot = (EFI_RUNTIME_SERVICES **)(mResetBase + 0x71A8);
  mRebootPhase=L"stock ResetSystem / PSCI returned";
  *Slot = &mResetRt;
  // Native 2CC0 sets PMIC0 SOFT_SPARE3 maskFE to Reason<<1, then PSCI reset.
  // No misc/BCB/recovery-message helper, block I/O or SetVariable is used.
  gRT->ResetSystem (EfiResetCold, EFI_INVALID_PARAMETER, sizeof (Data), &Data);
  if (*Slot == &mResetRt) *Slot = gRT;
  return EFI_DEVICE_ERROR;
}
