/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded, polling-only SM8650 PCIe0 bring-up. Register provenance and the
 * DT PHY sequence are recorded in hardware/sm8650-wifi.json. No DMA, MSI,
 * endpoint BAR writes, firmware execution, or network-interface emulation.
 * This header is shared with host fault-injection tests.
 */
#ifndef GIULIA_WIFI_PCIE_POLICY_H
#define GIULIA_WIFI_PCIE_POLICY_H
#include "WifiPcieReport.h"

enum gw_region { GW_GPIO, GW_GCC, GW_PARF, GW_PHY, GW_DBI, GW_ATU, GW_CFG, GW_REFCLK, GW_TESTCLK };
enum gw_resource { GW_RAIL, GW_DOMAIN, GW_CLOCK, GW_CESTA };
enum gw_stage {
  GW_START=1, GW_ABI=2, GW_RESOLVE=3, GW_MAP=4, GW_PINS=5,
  GW_POWER=6, GW_ENABLE=7, GW_DOMAINS=8, GW_CLOCKS=9, GW_RESET=10,
  GW_PHY_INIT=11, GW_PHY_READY=12, GW_LINK=13, GW_CONFIG=14,
  GW_IDENTIFY=15, GW_DONE=16, GW_CESTA_READY=17, GW_XNU_PENDING=18
};
enum gw_error {
  GW_OK=0, GW_RESOURCE_ERROR=1, GW_BAD_PIN=2, GW_ALREADY_ACTIVE=3,
  GW_BAD_SEQUENCE=4, GW_BAD_REGISTER=5, GW_PHY_TIMEOUT=6,
  GW_LINK_TIMEOUT=7, GW_ATU_TIMEOUT=8, GW_WRONG_DEVICE=9,
  GW_UNEXPECTED_DMA=10
};
#define GW_RAILS 9u
#define GW_DOMAINS_COUNT 2u
#define GW_CLOCKS_COUNT 12u
#define GW_PIPE_CLOCK 11u
struct gw_phy_word { unsigned offset, value, delay_us; };
struct gw_bus {
  void *context;
  unsigned (*read)(void *, unsigned, unsigned);
  void (*write)(void *, unsigned, unsigned, unsigned);
  void (*delay)(void *, unsigned);
  int (*resource)(void *, unsigned, unsigned, int);
  void (*step)(void *, struct gw_report *);
};

static void gw_step(const struct gw_bus *b, struct gw_report *r, unsigned stage, unsigned detail)
{
  r->stage=stage; r->detail=detail; b->step(b->context,r);
}
static unsigned gw_read(const struct gw_bus *b,unsigned region,unsigned offset)
{ return b->read(b->context,region,offset); }
static void gw_write(const struct gw_bus *b,unsigned region,unsigned offset,unsigned value)
{
  b->write(b->context,region,offset,value);
  /* OnePlus pci-msm.c msm_pcie_write_reg/write_mask drain posted writes
     with a read of the same register. CPU barriers alone do not provide
     that device-side completion before the next reset/clock/power step.
     This is a flush, not an equality check (status/W1C bits may differ). */
  (void)b->read(b->context,region,offset);
}
static void gw_mask(const struct gw_bus *b,unsigned region,unsigned offset,unsigned clear,unsigned set)
{ gw_write(b,region,offset,(gw_read(b,region,offset)&~clear)|set); }
static int gw_pin_owned(unsigned value)
{ return value!=0xffffffffu && (!(value&(1u<<11)) || (value&(1u<<12))); }

/* GPIO indices: WLAN_EN16, PERST94, CLKREQ95, WAKE96; BT17 is read-only.
   SW18/WLAN_SW19 remain inputs; only their PDC wake-forwarding bit is set. */
static void gw_gpio(const struct gw_bus *b,unsigned index,unsigned value)
{ gw_mask(b,GW_GPIO,index*0x1000+4,2,value?2u:0u); }

#include "WifiClockMeasure.h"

#include "WifiPcieStock.h"

static void gw_link_snapshot(const struct gw_bus *b,struct gw_report *r)
{
  /* All values are sampled before failure cleanup changes clocks or pins. */
  r->pipe_gate=gw_read(b,GW_GCC,0x6b048);
  r->phy_common=gw_read(b,GW_PHY,0x1f8);
  r->phy_status2=gw_read(b,GW_PHY,0x218);
  r->phy_status3=gw_read(b,GW_PHY,0x21c);
  r->clkreq=((gw_read(b,GW_GPIO,0x2000)&0xffff)<<16)|
    (gw_read(b,GW_GPIO,0x2004)&0xffff);
}

static int gw_sequence_valid(const struct gw_phy_word *seq,unsigned count)
{
  unsigned i;
  if (!seq || count!=115 || seq[0].offset!=0x240 || seq[0].value!=3 ||
      seq[count-1].offset!=0x244 || seq[count-1].value!=3) return 0;
  for(i=0;i<count;i++) if ((seq[i].offset&3) || seq[i].offset>0x1ffc ||
      seq[i].value>255 || seq[i].delay_us>1000) return 0;
  return 1;
}

static int gw_reset_block(const struct gw_bus *b,unsigned offset,unsigned old)
{
  gw_write(b,GW_GCC,offset,old|1u);
  if(!(gw_read(b,GW_GCC,offset)&1u))return 0;
  b->delay(b->context,1000);
  gw_write(b,GW_GCC,offset,old&~1u);
  if(gw_read(b,GW_GCC,offset)&1u)return 0;
  b->delay(b->context,1000);
  return 1;
}

/* OnePlus gcc-pineapple.c: PHY_RCHNG RCG at 0x6b058, XO/1 and
   HW_CLK_CTRL_MODE. clk-rcg2.c sets CFG[20], commits CMD_UPDATE and polls.
   Only the HW-control bit is changed here; native ClockDxe owns frequency
   selection and its source/voltage accounting. Never touch M/N/D registers. */
static int gw_rchng_update(const struct gw_bus *b,unsigned value)
{
  unsigned cmd=gw_read(b,GW_GCC,0x6b058);
  if(cmd==0xffffffffu || (cmd&1))return 0;
  gw_write(b,GW_GCC,0x6b05c,value);
  if(gw_read(b,GW_GCC,0x6b05c)!=value)return 0;
  gw_write(b,GW_GCC,0x6b058,cmd|1u);
  for(unsigned i=0;i<500;i++) {
    cmd=gw_read(b,GW_GCC,0x6b058);
    if(cmd==0xffffffffu)return 0;
    if(!(cmd&1))return gw_read(b,GW_GCC,0x6b05c)==value;
    b->delay(b->context,1);
  }
  return 0;
}

static int gw_bringup_mode(const struct gw_bus *b, struct gw_report *r,
                     const struct gw_phy_word *seq,unsigned count,int xnu_link)
{
  unsigned cfg[4],io[4],i,j,domains=0,rails=0,clocks=0,last_ltssm=64;
  unsigned core_reset=0,phy_reset=0,reset_changed=0,pins_changed=0,controller=0,atu_changed=0,cesta_attempted=0;
  unsigned rchng_old=0,rchng_changed=0;
  unsigned wake_old[2]={0,0},wake_changed=0;
  int result=GW_OK;
  if (!b || !r || !b->read || !b->write || !b->delay || !b->resource || !b->step)
    return GW_RESOURCE_ERROR;
  if (!gw_sequence_valid(seq,count)) { r->error=GW_BAD_SEQUENCE; return r->error; }
  gw_step(b,r,GW_PINS,0);
  for(i=0;i<4;i++) {
    cfg[i]=gw_read(b,GW_GPIO,i*0x1000); io[i]=gw_read(b,GW_GPIO,i*0x1000+4);
    if (!gw_pin_owned(cfg[i]) || io[i]==0xffffffffu ||
        ((cfg[i]>>2)&15)>(i==2?1u:0u)) { r->detail=i; r->error=GW_BAD_PIN; return r->error; }
  }
  r->gpio_before=io[0];
  /* A live inherited radio must be investigated instead of resetting it. */
  if (io[0]&3) { r->error=GW_ALREADY_ACTIVE; return r->error; }
  i=gw_read(b,GW_GPIO,0x4004);
  if(i==0xffffffffu || (i&3)) {
    r->detail=4; r->error=GW_ALREADY_ACTIVE; return r->error;
  }
  /* CNSS cnss_get_pinctrl(): mpm_wake_set_gpios=<18 19>.
     OnePlus pinctrl-msm.c forwards these PDC-mapped pins using
     GPIO_INTR_CFG[7], after checking the hardware-present bit [6].
     GPIO CFG/IN_OUT alone do not configure this forwarding path.
     Preserve CPU IRQ enable, polarity, target and detection fields. */
  for(i=0;i<2;i++) {
    unsigned pin=(i+5)*0x1000,config=gw_read(b,GW_GPIO,pin);
    r->detail=pin+8;wake_old[i]=gw_read(b,GW_GPIO,pin+8);
    if(i==0)r->bar0=wake_old[i];else r->bar1=wake_old[i];
    if(!gw_pin_owned(config) || (config&0x23c) || wake_old[i]==0xffffffffu || !(wake_old[i]&64)) {
      result=GW_BAD_PIN;goto fail;
    }
    if(!(wake_old[i]&128)) {
      wake_changed|=1u<<i;
      gw_write(b,GW_GPIO,pin+8,wake_old[i]|128);
      unsigned route=gw_read(b,GW_GPIO,pin+8);
      if(i==0)r->bar0=route;else r->bar1=route;
      if(route==0xffffffffu || (route&192)!=192) {result=GW_BAD_REGISTER;goto fail;}
    }
  }
  r->bar0=r->bar1=0;
  for(i=0;i<GW_RAILS;i++) {
    gw_step(b,r,GW_POWER,i);
    if(b->resource(b->context,GW_RAIL,i,1)) { result=GW_RESOURCE_ERROR; goto fail; }
    rails|=1u<<i; r->rails=rails;
  }
  gw_step(b,r,GW_ENABLE,0);
  pins_changed=1;
  gw_gpio(b,1,0);
  gw_write(b,GW_GPIO,0x1000,(cfg[1]&~0x3ffu)|0x201u); /* PERST low */
  gw_write(b,GW_GPIO,0x2000,(cfg[2]&~0x3ffu)|7u); /* CLKREQ mux1, pullup */
  gw_write(b,GW_GPIO,0x3000,(cfg[3]&~0x3ffu)|3u); /* WAKE input, pullup */
  gw_gpio(b,0,0);
  gw_write(b,GW_GPIO,0,(cfg[0]&~0x3ffu)|0x3c3u); /* 16mA GPIO output */
  gw_gpio(b,0,1); b->delay(b->context,GW_STOCK_WLAN_DELAY_US);
  r->gpio_after=gw_read(b,GW_GPIO,4);
  if (!(r->gpio_after&2)) { result=GW_BAD_REGISTER; goto fail; }
  for(i=0;i<GW_DOMAINS_COUNT;i++) {
    gw_step(b,r,GW_DOMAINS,i);
    if(b->resource(b->context,GW_DOMAIN,i,1)) { result=GW_RESOURCE_ERROR; goto fail; }
    domains|=1u<<i;
  }
  for(i=0;i<GW_PIPE_CLOCK;i++) {
    gw_step(b,r,GW_CLOCKS,i);
    if(b->resource(b->context,GW_CLOCK,i,1)) { result=GW_RESOURCE_ERROR; goto fail; }
    clocks|=1u<<i; r->clocks=clocks;
    if(i==8) {
      r->detail=0x6b05c;
      rchng_old=gw_read(b,GW_GCC,0x6b05c);
      /* Source=XO, HID=1, bypass M/N; reject a stale 100 MHz selection. */
      if(rchng_old==0xffffffffu || (rchng_old&0x371fu)!=1u) {
        result=GW_BAD_REGISTER;goto fail;
      }
      if(!(rchng_old&(1u<<20))) {
        unsigned cmd=gw_read(b,GW_GCC,0x6b058);
        if(cmd==0xffffffffu || (cmd&1)) { result=GW_BAD_REGISTER;goto fail; }
        rchng_changed=1;
        if(!gw_rchng_update(b,rchng_old|(1u<<20))) { result=GW_BAD_REGISTER;goto fail; }
      }
    }
  }
  r->ref_clock=gw_read(b,GW_REFCLK,0);
  if(r->ref_clock==0xffffffffu || !(r->ref_clock&1)) { result=GW_BAD_REGISTER; goto fail; }
  gw_step(b,r,GW_CESTA_READY,0);
  cesta_attempted=1;
  if(b->resource(b->context,GW_CESTA,0,1)) { result=GW_RESOURCE_ERROR; goto fail; }
  gw_step(b,r,GW_RESET,0);
  core_reset=gw_read(b,GW_GCC,0x6b000); phy_reset=gw_read(b,GW_GCC,0x6c01c);
  if(core_reset==0xffffffffu || phy_reset==0xffffffffu) { result=GW_BAD_REGISTER; goto fail; }
  reset_changed=1;
  /* Vendor reset list: complete core assert/deassert, then PHY. Confirm
     both edges before continuing, instead of assuming each store landed. */
  r->detail=0x6b000;
  if(!gw_reset_block(b,0x6b000,core_reset)) { result=GW_BAD_REGISTER; goto fail; }
  gw_step(b,r,GW_RESET,0x6b000); /* record the core reset before touching PHY reset */
  r->detail=0x6c01c;
  if(!gw_reset_block(b,0x6c01c,phy_reset)) { result=GW_BAD_REGISTER; goto fail; }
  gw_step(b,r,GW_PHY_INIT,0);
  if(gw_read(b,GW_PARF,0x40)==0xffffffffu) { result=GW_BAD_REGISTER; goto fail; }
  controller=1;
  gw_write(b,GW_PARF,0x1000,4);
  gw_mask(b,GW_PARF,0x20,1u<<5,0); /* stock DT allows L1 */
  gw_mask(b,GW_PARF,0x40,1,0);
  gw_write(b,GW_PARF,0x350,0); gw_write(b,GW_PARF,0,0x365e);
  gw_mask(b,GW_PARF,0x174,0,1u<<4);
  gw_write(b,GW_PARF,0x22c,0); gw_write(b,GW_PARF,0x508,0); /* no IRQs yet */
  gw_write(b,GW_PARF,0x358,0x4000000);
  gw_mask(b,GW_PARF,0x1a8,0,1u<<31); /* stock AXI write-halt window policy */
  for(i=0;i<count;i++) {
    gw_write(b,GW_PHY,seq[i].offset,seq[i].value);
    if(seq[i].delay_us) b->delay(b->context,seq[i].delay_us);
  }
  b->delay(b->context,1000);
  gw_step(b,r,GW_PHY_READY,GW_PIPE_CLOCK);
  if(b->resource(b->context,GW_CLOCK,GW_PIPE_CLOCK,1)) { result=GW_RESOURCE_ERROR; goto fail; }
  clocks|=1u<<GW_PIPE_CLOCK; r->clocks=clocks;
  for(i=0;i<GW_STOCK_PHY_POLLS;i++) {
    r->phy=gw_read(b,GW_PHY,0x214);
    if(r->phy!=0xffffffffu && !(r->phy&(1u<<6))) break;
    b->delay(b->context,1000);
  }
  if(i==GW_STOCK_PHY_POLLS) {
    r->phy=gw_read(b,GW_PHY,0x214);
    if(r->phy==0xffffffffu || (r->phy&(1u<<6))) { result=GW_PHY_TIMEOUT; goto fail; }
  }
  r->root_device=gw_read(b,GW_DBI,0);
  if((r->root_device&0xffff)!=0x17cb) { result=GW_BAD_REGISTER; goto fail; }
  gw_step(b,r,GW_LINK,0);
  if(!gw_initialize_sid_table(b,r)) { result=GW_BAD_REGISTER;goto fail; }
  b->delay(b->context,GW_STOCK_EP_DELAY_US);
  gw_mask(b,GW_PARF,0x2c4,1,1u<<8);
  gw_mask(b,GW_DBI,0x714,0,1u<<5);
  if(xnu_link) {
    r->perst=((gw_read(b,GW_GPIO,0x1000)&0xffff)<<16)|(gw_read(b,GW_GPIO,0x1004)&0xffff);
    unsigned ltssm=gw_read(b,GW_PARF,0x1b0);
    if((r->perst&3) || ltssm==0xffffffffu || (ltssm&256) || !gw_configure_link(b,r)) {
      result=GW_BAD_REGISTER;goto fail;
    }
    /* Transfer prepared resources with the endpoint still held in reset.
       This is not enumeration success. A separate boot argument ensures
       older kernels reject the pending handoff instead of starting DMA. */
    gw_step(b,r,GW_XNU_PENDING,GW_XNU_LINK_TAG);
    /* Pending reports have no endpoint BARs. Preserve the two routing
       readbacks there; the decoder labels their meaning by handoff kind. */
    r->bar0=gw_read(b,GW_GPIO,0x5008);r->bar1=gw_read(b,GW_GPIO,0x6008);
    r->sw_ctrl=wake_old[0];r->wlan_sw_ctrl=wake_old[1];
    r->error=GW_OK;return GW_OK;
  }
  gw_gpio(b,1,1); b->delay(b->context,GW_STOCK_PERST_DELAY_US);
  r->perst=((gw_read(b,GW_GPIO,0x1000)&0xffff)<<16)|(gw_read(b,GW_GPIO,0x1004)&0xffff);
  r->sw_ctrl=((gw_read(b,GW_GPIO,0x5000)&0xffff)<<16)|(gw_read(b,GW_GPIO,0x5004)&0xffff);
  r->wlan_sw_ctrl=((gw_read(b,GW_GPIO,0x6000)&0xffff)<<16)|(gw_read(b,GW_GPIO,0x6004)&0xffff);
  if(!(r->perst&2)) { result=GW_BAD_REGISTER; goto fail; }
  if(!gw_configure_link(b,r)) { result=GW_BAD_REGISTER; goto fail; }
  r->detail=0;
  gw_mask(b,GW_PARF,0x1b0,0,1u<<8);
  for(i=0;i<GW_STOCK_LINK_POLLS;i++) {
    b->delay(b->context,5000);
    r->elbi=gw_read(b,GW_DBI,0xf28); r->link=gw_read(b,GW_DBI,0x80);
    if(r->elbi!=0xffffffffu) {
      unsigned state=(r->elbi>>12)&63;
      if(state<32) r->ltssm_seen_lo|=1u<<state;
      else r->ltssm_seen_hi|=1u<<(state-32);
      if(last_ltssm!=64 && state!=last_ltssm) r->ltssm_changes++;
      last_ltssm=state;
    }
    if(r->elbi!=0xffffffffu && r->link!=0xffffffffu &&
        (r->elbi&(1u<<10)) && (r->link&(1u<<29))) break;
  }
  gw_link_snapshot(b,r);
  gw_measure_pipe(b,r);
  if(r->pipe_measure_status==GW_MEASURE_RESTORE_ERROR) {
    r->detail=0x62000; result=GW_BAD_REGISTER; goto fail;
  }
  if(i==GW_STOCK_LINK_POLLS) { r->detail=gw_read(b,GW_PARF,0x1b0); result=GW_LINK_TIMEOUT; goto fail; }
  gw_step(b,r,GW_CONFIG,0);
  gw_mask(b,GW_DBI,0x18,0xffffff,0x10100); /* UEFI owns bus-1 CFG0 window */
  atu_changed=1;
  gw_write(b,GW_ATU,4,0); gw_write(b,GW_ATU,0,4);
  gw_write(b,GW_ATU,8,0x60100000); gw_write(b,GW_ATU,12,0);
  gw_write(b,GW_ATU,16,0x60100fff); gw_write(b,GW_ATU,20,0x01000000);
  gw_write(b,GW_ATU,24,0); gw_write(b,GW_ATU,4,1u<<31);
  for(i=0;i<100;i++) {
    unsigned value=gw_read(b,GW_ATU,4);
    if(value!=0xffffffffu && (value&(1u<<31))) break;
    b->delay(b->context,100);
  }
  if(i==100) { result=GW_ATU_TIMEOUT; goto fail; }
  gw_step(b,r,GW_IDENTIFY,0);
  r->device=gw_read(b,GW_CFG,0);
  if(r->device!=0x110717cbu) { result=GW_WRONG_DEVICE; goto fail; }
  r->command=gw_read(b,GW_CFG,4); r->bar0=gw_read(b,GW_CFG,0x10); r->bar1=gw_read(b,GW_CFG,0x14);
  if(r->command&4) { result=GW_UNEXPECTED_DMA; goto fail; }
  gw_step(b,r,GW_DONE,0);
  r->error=GW_OK;
  return GW_OK;
fail:
  r->error=(unsigned)result;
  b->step(b->context,r); /* Preserve the failing stage before cleanup. */
  if(pins_changed) { gw_gpio(b,1,0); gw_gpio(b,0,0); }
  if(controller) { gw_mask(b,GW_PARF,0x1b0,1u<<8,0); if(atu_changed) gw_write(b,GW_ATU,4,0); gw_write(b,GW_PHY,0x240,0); }
  if(rchng_changed) {
    unsigned cfg_now=gw_read(b,GW_GCC,0x6b05c);
    /* CESTA can change the rate after our XO setup (observed 0x100605
       during D0 training). Restore only our HW-control bit, preserving
       the current rate. Native cleanup restores its frequency request. */
    if(cfg_now==0xffffffffu ||
       !gw_rchng_update(b,(cfg_now&~(1u<<20))|(rchng_old&(1u<<20)))) {
      /* An unacknowledged RCG switch must retain its clocks and supplies. */
      r->cleanup_status=0x8000000000000007ull;
      r->retained_rails|=rails;
      return result;
    }
  }
  if(cesta_attempted && b->resource(b->context,GW_CESTA,0,0)) {
    /* An unresolved CRM channel switch must not lose clocks or supplies.
       Keep PERST/WLAN low and retain this transaction's resource votes. */
    r->retained_rails|=rails;
    return result;
  }
  if(reset_changed) { gw_write(b,GW_GCC,0x6b000,core_reset); gw_write(b,GW_GCC,0x6c01c,phy_reset); }
  for(j=GW_CLOCKS_COUNT;j>0;j--) if(clocks&(1u<<(j-1))) (void)b->resource(b->context,GW_CLOCK,j-1,0);
  for(j=GW_DOMAINS_COUNT;j>0;j--) if(domains&(1u<<(j-1))) (void)b->resource(b->context,GW_DOMAIN,j-1,0);
  for(j=GW_RAILS;j>0;j--) if(rails&(1u<<(j-1))) (void)b->resource(b->context,GW_RAIL,j-1,0);
  if(pins_changed) for(i=0;i<4;i++) { gw_write(b,GW_GPIO,i*0x1000+4,io[i]); gw_write(b,GW_GPIO,i*0x1000,cfg[i]); }
  for(j=2;j>0;j--) if(wake_changed&(1u<<(j-1))) {
    unsigned offset=(j+4)*0x1000+8,current=gw_read(b,GW_GPIO,offset);
    if(current==0xffffffffu) {r->cleanup_status=0x8000000000000007ull;continue;}
    gw_write(b,GW_GPIO,offset,(current&~128u)|(wake_old[j-1]&128));
    if((gw_read(b,GW_GPIO,offset)&128)!=(wake_old[j-1]&128))r->cleanup_status=0x8000000000000007ull;
  }
  return result;
}
static inline int gw_bringup(const struct gw_bus *b,struct gw_report *r,
                            const struct gw_phy_word *seq,unsigned count)
{ return gw_bringup_mode(b,r,seq,count,0); }
#endif
