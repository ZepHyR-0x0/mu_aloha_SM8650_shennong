// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef FRONT_PAGE_EVENT_SERVICE_H
#define FRONT_PAGE_EVENT_SERVICE_H
#include <Protocol/ApplicationEventService.h>
#include <Protocol/UsbBringup.h>
#include <Library/TimerLib.h>
STATIC inline BOOLEAN FpUploadActive (VOID)
{
  EFI_GUID Guid=USB_BRINGUP_PROTOCOL_GUID;
  USB_BRINGUP_PROTOCOL *Usb=NULL;
  USB_RAM_DISK_INFO Info;
  return !EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,(VOID **)&Usb)) &&
    Usb!=NULL && Usb->Revision>=7 && Usb->GetRamInfo!=NULL &&
    !EFI_ERROR(Usb->GetRamInfo(Usb,&Info)) && Info.UploadActive;
}
STATIC VOID FpServiceEvents (VOID)
{
  EFI_GUID Guid=APPLICATION_EVENT_SERVICE_PROTOCOL_GUID;
  EFI_GUID UsbGuid=USB_BRINGUP_PROTOCOL_GUID;
  APPLICATION_EVENT_SERVICE_PROTOCOL *Service=NULL;
  USB_BRINGUP_PROTOCOL *Usb=NULL;
  USB_RAM_DISK_INFO Info;
  UINT64 Started;
  UINTN Poll;
  EFI_STATUS Status;
  // Custom-painted pages poll keys instead of using SWM WaitForEvent. Keep
  // the USB provider serviced at the same application TPL in these loops.
  if (EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,(VOID **)&Service)) ||
      Service==NULL || Service->Revision!=1 || Service->WakeEvent==NULL ||
      Service->Service==NULL)
    return;
  // Active uploads can service explicitly between timer ticks. Idle pages
  // still wait for the provider's wake event, preserving their normal cadence.
  if(EFI_ERROR(gBS->CheckEvent(Service->WakeEvent)) && !FpUploadActive())
    return;
  (VOID)gBS->LocateProtocol(&UsbGuid,NULL,(VOID **)&Usb);
  Started=GetTimeInNanoSecond(GetPerformanceCounter());
  // One event per 20 ms page iteration serializes RX, ACK and rearm. Drain a
  // bounded burst during uploads; keep normal idle and mode-switch behavior.
  // Recheck the provider after every call: it may close its wake event or
  // cancel/commit the upload. Never spin beyond 16 calls or 4 ms per frame.
  for(Poll=0;Poll<16;Poll++) {
    Status=Service->Service(Service);
    if(EFI_ERROR(Status) || Service->WakeEvent==NULL ||
       Usb==NULL || Usb->Revision<7 || Usb->GetRamInfo==NULL ||
       (Usb->Revision>=8 && Usb->GetSwitchBusy!=NULL && Usb->GetSwitchBusy(Usb)) ||
       EFI_ERROR(Usb->GetRamInfo(Usb,&Info)) || !Info.UploadActive ||
       GetTimeInNanoSecond(GetPerformanceCounter())-Started>=4000000ULL)
      break;
  }
}
STATIC inline VOID FpServiceDelay (UINTN Microseconds)
{
  UINT64 Started=GetTimeInNanoSecond(GetPerformanceCounter());
  UINTN Remaining=Microseconds;
  while(Remaining!=0) {
    if(!FpUploadActive()) { gBS->Stall(Remaining);break; }
    gBS->Stall(MIN(Remaining,1000));
    FpServiceEvents();
    UINT64 Elapsed=(GetTimeInNanoSecond(GetPerformanceCounter())-Started)/1000;
    if(Elapsed>=Microseconds)break;
    Remaining=Microseconds-(UINTN)Elapsed;
  }
}
#endif
