/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5 16.0.3.500 PCIe0 profile. Values/order checked against the captured
 * DT and pci-msm-drv.ko SHA256 11e6da96f822c87f58cb757cf3c97ba16c32d6de3511787abb67d576c7e783cc.
 * This supplies hardware operations to the UEFI adapter, not Linux APIs.
 */
#ifndef GIULIA_WIFI_PCIE_STOCK_H
#define GIULIA_WIFI_PCIE_STOCK_H
#include "WifiPcieStockConfig.h"

/* pci-msm.c: msm_pcie_config_sid zeros all 256 BDF-to-SID words before use.
 * The reset SRAM is not an empty table: real hardware returns arbitrary data.
 * Only clear an unused/bypassed table while PERST and LTSSM keep the dedicated
 * endpoint quiescent. XNU later publishes its owned entry and enables lookup.
 */
static int gw_initialize_sid_table(const struct gw_bus *b,struct gw_report *r)
{
  r->detail=0x2c00;
  unsigned config=gw_read(b,GW_PARF,0x2c00);
  if(config==0xffffffffu)return 0;
  if(!(config&1u))return 1; /* A live routing table belongs to its current owner. */
  unsigned ltssm=gw_read(b,GW_PARF,0x1b0),perst=gw_read(b,GW_GPIO,0x1004);
  if(ltssm==0xffffffffu || perst==0xffffffffu || (ltssm&256u) || (perst&3u))return 0;
  for(unsigned i=0;i<256;i++) {
    unsigned offset=0x2000+i*4;r->detail=offset;
    gw_write(b,GW_PARF,offset,0);
    r->bar0=gw_read(b,GW_PARF,offset);
    if(r->bar0)return 0;
  }
  r->detail=0x2c00;
  return gw_read(b,GW_PARF,0x2c00)==config;
}

static int gw_stock_mask(const struct gw_bus *b,struct gw_report *r,unsigned off,unsigned clear,unsigned set)
{
  unsigned old=gw_read(b,GW_DBI,off);r->detail=off;
  if(old==0xffffffffu)return 0;
  unsigned value=(old&~clear)|set;
  gw_write(b,GW_DBI,off,value);
  return gw_read(b,GW_DBI,off)==value;
}

static unsigned gw_stock_cap(const struct gw_bus *b,struct gw_report *r,unsigned id)
{
  unsigned off=0x100;
  for(unsigned n=0;n<480;n++) {
    r->detail=off;
    if(off<0x100 || off>0xffc || (off&3))return 0;
    unsigned header=gw_read(b,GW_DBI,off);
    if(!header || header==0xffffffffu)return 0;
    if((header&65535)==id)return off;
    unsigned next=(header>>20)&0xfff;
    if(next==off)return 0;
    off=next;
  }
  return 0;
}

static int gw_configure_link(const struct gw_bus *b,struct gw_report *r)
{
  unsigned cap=gw_read(b,GW_DBI,0x7c),port=gw_read(b,GW_DBI,0x710);
  unsigned gen2=gw_read(b,GW_DBI,0x80c),misc=gw_read(b,GW_DBI,0x8bc);
  r->port_link_before=port;r->port_link_after=port;r->link_cap=cap;r->gen2_ctrl=gen2;
  r->detail=0x710;
  /* DT has no target width or target speed. Preserve the reset capabilities
     and port mode; do not manufacture a different advertised link width. */
  unsigned width=(cap>>4)&63;
  if(cap==0xffffffffu || port==0xffffffffu || gen2==0xffffffffu || misc==0xffffffffu ||
     (cap&15)!=3 || (width!=1 && width!=2) || (port&0xa0u)!=0x20u ||
     ((port>>16)&63)!=(width==2?3u:1u))return 0;
  /* Stock module at 0xc91c: NUM_OF_LANES=1 for initial training. */
  if(!gw_stock_mask(b,r,0x80c,0x1f00,0x100))return 0;
  r->gen2_ctrl=gw_read(b,GW_DBI,0x80c);
  /* Stock Gen3 shadow, EQ request vector=0, ZRXDC_NONCOMPL=0 and
     T_MIN_PHASE23=0. These optional DT overrides are absent on this board. */
  if(!gw_stock_mask(b,r,0x890,3u<<24,0) || !gw_stock_mask(b,r,0x8a8,0xffff00,0) ||
     !gw_stock_mask(b,r,0x890,1,0) || !gw_stock_mask(b,r,0x8ac,31,0))return 0;
  unsigned secondary=gw_stock_cap(b,r,0x19);
  if(!secondary || secondary>0xff0)return 0;
  int ok=gw_stock_mask(b,r,0x8bc,0,1);
  if(ok) {
    r->detail=secondary+12;
    gw_write(b,GW_DBI,secondary+12,GW_STOCK_PRESET);
    ok=gw_read(b,GW_DBI,secondary+12)==GW_STOCK_PRESET;
  }
  gw_write(b,GW_DBI,0x8bc,misc);
  if(gw_read(b,GW_DBI,0x8bc)!=misc)return 0;
  if(!ok)return 0;
  /* No qcom,target-link-speed: leave LNKCTL2 alone. Stock MRRS field=5. */
  r->detail=0x78;
  unsigned control=gw_read(b,GW_DBI,0x78);
  if(control==0xffffffffu)return 0;
  /* Upper 16 bits are W1C status. Do not replay them in a 32-bit store. */
  control=(control&0x8fff)|0x5000;
  gw_write(b,GW_DBI,0x78,control);
  return (gw_read(b,GW_DBI,0x78)&0xffff)==control;
}
#endif
