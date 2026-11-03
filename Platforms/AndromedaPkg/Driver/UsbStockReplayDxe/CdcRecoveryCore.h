// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded recovery decisions; lifecycle changes are performed by the caller
// only after leaving the native pump at TPL_APPLICATION.
typedef struct { UINT64 FaultAt,HealthyAt,DetachAt;UINT32 Attempts;BOOLEAN Fault,Healthy,Unsafe,CableKnown,CablePresent,Detached; } CDC_RECOVERY;
STATIC BOOLEAN CrCableObserve(CDC_RECOVERY *R,UINT64 Now,BOOLEAN Present)
{
  BOOLEAN Falling=R->CableKnown && R->CablePresent && !Present;
  R->CableKnown=TRUE;R->CablePresent=Present;
  if(Falling){R->Detached=TRUE;R->DetachAt=Now;R->Healthy=FALSE;R->Fault=FALSE;}
  return Falling;
}
STATIC BOOLEAN CrObserve(CDC_RECOVERY *R,UINT64 Now,BOOLEAN CablePresent,BOOLEAN Configured,BOOLEAN ClassFailed,BOOLEAN Unsafe)
{
  if(Unsafe){R->Unsafe=TRUE;return FALSE;}
  if(R->Unsafe)return FALSE;
  // Rebuild the upper session while the cable is absent, before the next
  // native attach can reuse stale EP0 state. Ordinary bus reset is excluded.
  if(R->Detached){
    if(Now-R->DetachAt<200 || R->Attempts>=3)return FALSE;
    R->Detached=FALSE;R->Attempts++;R->Fault=FALSE;return TRUE;
  }
  if(Configured && !ClassFailed){
    R->Fault=FALSE;
    if(!R->Healthy){R->Healthy=TRUE;R->HealthyAt=Now;}
    if(Now-R->HealthyAt>=5000)R->Attempts=0;
    return FALSE;
  }
  R->Healthy=FALSE;
  if(!ClassFailed && !CablePresent){R->Fault=FALSE;return FALSE;}
  if(!R->Fault){R->Fault=TRUE;R->FaultAt=Now;return FALSE;}
  UINT64 Wait=ClassFailed?1500:7000;
  if(R->Attempts>=3 || Now-R->FaultAt<Wait)return FALSE;
  R->Attempts++;R->FaultAt=Now;return TRUE;
}
