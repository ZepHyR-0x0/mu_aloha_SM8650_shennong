/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded GCC debug counter measurement for the pinned giulia ClockDxe.
 * Clock node 0x27048: debug selector 0x14d, GCC debug divider 2.
 * Native HAL 0xde6c/0xdf50 identifies the registers and counter arithmetic.
 * Unlike the firmware routine, preserve the debug configuration, reject an
 * already running measurement, and bound both counter polls to 10 ms each.
 * Included after gw_bus/read/write helpers in WifiPciePolicy.h.
 */
#ifndef GIULIA_WIFI_CLOCK_MEASURE_H
#define GIULIA_WIFI_CLOCK_MEASURE_H
enum gw_measure_status {
  GW_MEASURE_NOT_RUN=0, GW_MEASURE_OK=1, GW_MEASURE_BUSY=2,
  GW_MEASURE_BAD_REGISTER=3, GW_MEASURE_CLEAR_TIMEOUT=4,
  GW_MEASURE_COUNT_TIMEOUT=5, GW_MEASURE_RESTORE_ERROR=6
};
static void gw_measure_pipe(const struct gw_bus *b,struct gw_report *r)
{
  static const unsigned offsets[]={0x24,0,4,8,0x48};
  unsigned saved[5],i,j,value=0,control,ticks=0x8000;
  unsigned status=GW_MEASURE_OK;
  unsigned long long hz;
  r->pipe_hz=0;
  for(i=0;i<5;i++) {
    saved[i]=gw_read(b,GW_TESTCLK,offsets[i]);
    if(saved[i]==0xffffffffu) {
      r->pipe_measure_status=GW_MEASURE_BAD_REGISTER;return;
    }
  }
  if(saved[4]&0x300000u) { r->pipe_measure_status=GW_MEASURE_BUSY;return; }
  gw_write(b,GW_TESTCLK,0x24,(saved[0]&~0x3ffu)|0x14du);
  gw_write(b,GW_TESTCLK,0,(saved[1]&~15u)|1u);
  gw_write(b,GW_TESTCLK,4,saved[2]|1u);
  gw_write(b,GW_TESTCLK,8,saved[3]|1u);
  if((gw_read(b,GW_TESTCLK,0x24)&0x3ffu)!=0x14du ||
     (gw_read(b,GW_TESTCLK,0)&15u)!=1 || !(gw_read(b,GW_TESTCLK,4)&1u) ||
     !(gw_read(b,GW_TESTCLK,8)&1u)) {
    status=GW_MEASURE_BAD_REGISTER;goto restore;
  }
  control=(saved[4]&~0x3fffffu)|ticks;
  gw_write(b,GW_TESTCLK,0x48,control|0x200000u);
  for(i=0;i<1000;i++) {
    value=gw_read(b,GW_TESTCLK,0x4c);
    if(value==0xffffffffu) { status=GW_MEASURE_BAD_REGISTER;break; }
    if(!(value&0x1ffffffu)) break;
    b->delay(b->context,10);
  }
  if(i==1000) status=GW_MEASURE_CLEAR_TIMEOUT;
  if(status!=GW_MEASURE_OK) goto restore;
  gw_write(b,GW_TESTCLK,0x48,control);
  gw_write(b,GW_TESTCLK,0x48,control|0x100000u);
  b->delay(b->context,5);
  for(i=0;i<1000;i++) {
    value=gw_read(b,GW_TESTCLK,0x4c);
    if(value==0xffffffffu) { status=GW_MEASURE_BAD_REGISTER;break; }
    if(value&(1u<<25)) break;
    b->delay(b->context,10);
  }
  if(i==1000) status=GW_MEASURE_COUNT_TIMEOUT;
  if(status==GW_MEASURE_OK) {
    /* ClockDxe uses the 9.6 MHz counter reference and debug divider 2. */
    hz=2*((((unsigned long long)(value&0x1ffffffu))*9600000ull+
            14400000ull+(2*ticks+7)/2)/(2*ticks+7));
    if(hz>0xffffffffull) status=GW_MEASURE_BAD_REGISTER;
    else r->pipe_hz=(unsigned)hz;
  }
restore:
  for(j=5;j>0;j--) gw_write(b,GW_TESTCLK,offsets[j-1],saved[j-1]);
  for(j=0;j<5;j++) if(gw_read(b,GW_TESTCLK,offsets[j])!=saved[j])
    status=GW_MEASURE_RESTORE_ERROR;
  if(status!=GW_MEASURE_OK) r->pipe_hz=0;
  r->pipe_measure_status=status;
}
#endif
