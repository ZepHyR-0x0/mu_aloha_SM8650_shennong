/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Version-pinned RPMh resource-cache reader. The native allocator at RVA
 * 0xa768 returns allocation+4: copy records before interpreting pointer fields.
 * The returned firmware pointers are trusted; list length and values are bounded.
 */
#ifndef GIULIA_WIFI_POWER_CACHE_H
#define GIULIA_WIFI_POWER_CACHE_H
typedef struct { unsigned Data; unsigned char Valid,Pad[3]; unsigned Dirty; } GW_VOTE;
typedef struct GW_VOTER {
  unsigned Driver,State,Priority;
  GW_VOTE Hardware,Local[3];
  struct GW_VOTER *Next;
} GW_VOTER;
typedef struct { unsigned Address; GW_VOTER *Voters; void *Pdc; } GW_COMMAND;
enum { GW_CACHE_OK=0,GW_CACHE_BUSY=1,GW_CACHE_BAD=2 };

static void gw_cache_copy(void *to,const void *from,unsigned length)
{
  unsigned char *d=(unsigned char *)to;
  const unsigned char *s=(const unsigned char *)from;
  for(unsigned i=0;i<length;i++) d[i]=s[i];
}
static int gw_cached_vote(const void *pointer,unsigned address,unsigned *data,int *valid)
{
  GW_COMMAND command;
  const void *next;
  unsigned count=0;
  *data=0; *valid=0;
  if(!pointer) return GW_CACHE_OK;
  if((unsigned long long)pointer&3) return GW_CACHE_BAD;
  gw_cache_copy(&command,pointer,sizeof(command));
  if(command.Address!=address) return GW_CACHE_BAD;
  next=command.Voters;
  while(next) {
    GW_VOTER voter;
    if(++count>32 || ((unsigned long long)next&3)) return GW_CACHE_BAD;
    gw_cache_copy(&voter,next,sizeof(voter));
    if(voter.Driver==2) {
      if(voter.State!=0 || voter.Local[0].Dirty>1) return GW_CACHE_BUSY;
      if(voter.Local[0].Valid>1) return GW_CACHE_BAD;
      if(voter.Local[0].Valid) { *data=voter.Local[0].Data; *valid=1; }
      return GW_CACHE_OK;
    }
    next=voter.Next;
  }
  return GW_CACHE_OK;
}
#endif
