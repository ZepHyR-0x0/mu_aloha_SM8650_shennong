// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef FRONT_PAGE_EVENT_SERVICE_H
#define FRONT_PAGE_EVENT_SERVICE_H
#include <Protocol/ApplicationEventService.h>
STATIC VOID FpServiceEvents (VOID)
{
  EFI_GUID Guid=APPLICATION_EVENT_SERVICE_PROTOCOL_GUID;
  APPLICATION_EVENT_SERVICE_PROTOCOL *Service=NULL;
  // Custom-painted pages poll keys instead of using SWM WaitForEvent. Keep
  // the USB provider serviced at the same application TPL in these loops.
  if (!EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,(VOID **)&Service)) &&
      Service!=NULL && Service->Revision==1 && Service->WakeEvent!=NULL &&
      Service->Service!=NULL && !EFI_ERROR(gBS->CheckEvent(Service->WakeEvent)))
    (VOID)Service->Service(Service);
}
#endif
