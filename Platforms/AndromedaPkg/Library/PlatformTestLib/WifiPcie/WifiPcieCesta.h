/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PCIe0 CESTA/CRM D0 request, before core/PHY reset. Register definitions:
 * OnePlus SM8650 kernel d763ed47deec55451366c1187f813f2f309d88a7,
 * drivers/soc/qcom/crm.c and vendor pci-msm.c. The 13 instructions and
 * seven branches below are the captured Ace 5 PCIe0 DT register data.
 * Only HW voter 0 is changed. Preserve existing BW floors, other performance
 * resources, voter 1, SW voters, global enable, voltage tables and IRQs.
 */
#ifndef GIULIA_WIFI_PCIE_CESTA_H
#define GIULIA_WIFI_PCIE_CESTA_H
#include "WifiPcieBandwidth.h"
#include "WifiPcieStockConfig.h"
enum gce_region {GCE_CRM,GCE_SM,GCE_CLOCK,GCE_GPIO_STATUS,GCE_PARF_STATUS,GCE_GCC_STATUS};
enum gce_error {GCE_OK,GCE_ARGUMENT,GCE_BUSY,GCE_GEOMETRY,GCE_UNOWNED,
    GCE_WRITE_ERROR,GCE_SWITCH_TIMEOUT,GCE_POWER_TIMEOUT,GCE_RESTORE_ERROR};
struct gce_io {
    void *context;unsigned (*read)(void *,unsigned,unsigned);
    void (*write)(void *,unsigned,unsigned,unsigned);
    void (*delay)(void *,unsigned);int (*quiescent)(void *);
};
struct gce_state {
    unsigned changed,active,retained,error,region,offset,value;
    unsigned version,geometry,geometry2,enable,perf_count,node_count;
    unsigned update,busy,source,target,sm_start,sm_control,sm_mask;
    unsigned code[13],branch[7],old[2][2][5][16];
    unsigned program_changed,cold_restore;
    unsigned bw_floor[GCB_NODES],bw_ready;
};
static const unsigned gce_code[13]={0x1c018081,0x70074002,0x50028000,0x28007003,
    0x80804002,0x70021c01,0x18001802,0x70005000,0x10004000,0x80814002,0x18001c01,0x1c018080,0x100};
static const unsigned gce_branch[7]={4,0x1c,0x24,0x2c,0,0,0};
static unsigned gce_reg(unsigned type,unsigned channel,unsigned state,unsigned resource,unsigned count) {
    static const unsigned off[2][5]={{0,0x40,0x80,0x600,0x640},{0xc0,0x140,0x680,0x700,0x780}};
    return 0x200+off[type][state]+4*(channel*count+resource);
}
static unsigned gce_read(const struct gce_io *b,struct gce_state *s,unsigned region,unsigned off) {
    unsigned v=b->read(b->context,region,off);s->region=region;s->offset=off;s->value=v;return v;
}
static int gce_write(const struct gce_io *b,struct gce_state *s,unsigned region,unsigned off,unsigned value) {
    b->write(b->context,region,off,value);return gce_read(b,s,region,off)==value;
}
static int gce_switch(const struct gce_io *b,struct gce_state *s,unsigned channel) {
    unsigned mask=1u<<channel;
    b->write(b->context,GCE_CRM,0x1020,mask);
    for(unsigned i=0;i<100;i++) {
        unsigned busy=gce_read(b,s,GCE_CRM,0x1000);
        if(busy==0xffffffffu)return 0;
        if(busy==mask && gce_read(b,s,GCE_CRM,0x1020)==mask)return 1;
        b->delay(b->context,100);
    }
    return 0;
}
static unsigned gce_wanted(const struct gce_state *s,unsigned type,unsigned power,unsigned resource) {
    unsigned value=s->old[s->source][type][power][resource];
    /* Stock requests max supported Gen3 resources before training:
       three qcom,bw-scale rows + PERF_LVL_L1SS = operating level 4. */
    if(!type && power<2 && !resource && value<GW_STOCK_D0_PERF)return GW_STOCK_D0_PERF;
    if(type && power<2 && resource<GCB_NODES && s->bw_ready==GCB_PATH_MASK)
        return gcb_merge(value,s->bw_floor[resource],resource);
    return value;
}
static int gce_copy_channel(const struct gce_io *b,struct gce_state *s,unsigned to,unsigned from,int request) {
    for(unsigned type=0;type<2;type++)for(unsigned power=0;power<5;power++) {
        unsigned count=type?s->node_count:s->perf_count;
        for(unsigned resource=0;resource<count;resource++) {
            unsigned value=request?gce_wanted(s,type,power,resource):s->old[from][type][power][resource];
            if(!gce_write(b,s,GCE_CRM,gce_reg(type,to,power,resource,count),value))return 0;
        }
    }
    return 1;
}
static int gce_restore(const struct gce_io *b,struct gce_state *s) {
    if(!s->changed)return GCE_OK;
    if(!b->quiescent(b->context)){s->retained=1;return GCE_BUSY;}
    unsigned update=gce_read(b,s,GCE_CRM,0x1020),busy=gce_read(b,s,GCE_CRM,0x1000);
    if(update!=busy || (update!=s->update && update!=(1u<<s->target))){s->retained=1;return GCE_BUSY;}
    /* Reject another owner's writes; partial failed writes may equal either
     * the captured word or this transaction's intended word. */
    for(unsigned channel=0;channel<2;channel++)for(unsigned type=0;type<2;type++)for(unsigned power=0;power<5;power++) {
        unsigned count=type?s->node_count:s->perf_count;
        for(unsigned resource=0;resource<count;resource++) {
            unsigned value=gce_read(b,s,GCE_CRM,gce_reg(type,channel,power,resource,count));
            if(value!=s->old[channel][type][power][resource] &&
               (channel!=s->target || value!=gce_wanted(s,type,power,resource))) {s->retained=1;return GCE_UNOWNED;}
        }
    }
    unsigned start=gce_read(b,s,GCE_SM,0x1090),control=gce_read(b,s,GCE_SM,0x1040),mask=gce_read(b,s,GCE_SM,0x1048);
    if((start!=s->sm_start && start!=(s->sm_start|1u)) || (control!=s->sm_control && control!=0) || (mask!=s->sm_mask && mask!=0)) {
        s->retained=1;return GCE_UNOWNED;
    }
    if(s->program_changed) {
        if(gce_read(b,s,GCE_CRM,0x1024)!=0 || gce_read(b,s,GCE_CRM,0x1004)!=0){s->retained=1;return GCE_BUSY;}
        for(unsigned i=0;i<13;i++){unsigned value=gce_read(b,s,GCE_SM,i*4);
            if(value!=s->code[i] && value!=gce_code[i]){s->retained=1;return GCE_UNOWNED;}}
        for(unsigned i=0;i<7;i++){unsigned value=gce_read(b,s,GCE_SM,0x1000+i*4);
            if(value!=s->branch[i] && value!=gce_branch[i]){s->retained=1;return GCE_UNOWNED;}}
    }
    /* Keep the sequencer and its resource handshake alive while returning
       the old channel votes. Stop/restore a program we loaded only after
       CRM acknowledges the switch, matching the vendor power-down order. */
    if(update==(1u<<s->target)) {
        if(s->update) {
            if(!gce_switch(b,s,s->source))goto failed;
        } else {
            /* CRM has no documented command to return to "never published".
             * Restore both vote banks using the alternate bank, then select
             * original bank 0. Votes are restored; the cold channel selection
             * remains initialized and is explicitly reported to the caller. */
            if(!gce_copy_channel(b,s,1,0,0) || !gce_switch(b,s,1) ||
               !gce_copy_channel(b,s,0,0,0) || !gce_switch(b,s,0) ||
               !gce_copy_channel(b,s,1,1,0))goto failed;
            s->cold_restore=1;
        }
    }
    if(!s->cold_restore && !gce_copy_channel(b,s,s->target,s->target,0))goto failed;
    if(s->program_changed) {
        if(!gce_write(b,s,GCE_SM,0x1090,s->sm_start&~1u))goto failed;
        for(unsigned i=0;i<13;i++)if(!gce_write(b,s,GCE_SM,i*4,s->code[i]))goto failed;
        for(unsigned i=0;i<7;i++)if(!gce_write(b,s,GCE_SM,0x1000+i*4,s->branch[i]))goto failed;
    }
    if(!gce_write(b,s,GCE_SM,0x1040,s->sm_control) || !gce_write(b,s,GCE_SM,0x1048,s->sm_mask))goto failed;
    s->changed=0;s->active=0;s->retained=0;return GCE_OK;
failed:s->retained=1;return GCE_RESTORE_ERROR;
}
static int gce_fail(const struct gce_io *b,struct gce_state *s,unsigned error) {
    unsigned region=s->region,offset=s->offset,value=s->value;
    s->error=error;(void)gce_restore(b,s);s->region=region;s->offset=offset;s->value=value;return (int)error;
}
static int gce_take(const struct gce_io *b,struct gce_state *s) {
    if(!b || !s || !b->read || !b->write || !b->delay || !b->quiescent || s->changed || s->retained)return GCE_ARGUMENT;
    if(!b->quiescent(b->context))return GCE_BUSY;
    s->version=gce_read(b,s,GCE_CRM,0);s->geometry=gce_read(b,s,GCE_CRM,4);
    s->geometry2=gce_read(b,s,GCE_CRM,8);s->enable=gce_read(b,s,GCE_CRM,12);
    s->perf_count=(s->geometry>>8)&15;s->node_count=(s->geometry2>>26)&31;
    if(s->version==0xffffffffu || s->geometry==0xffffffffu || s->geometry2==0xffffffffu ||
       !s->enable || s->enable==0xffffffffu || ((s->geometry>>16)&15)!=2 ||
       ((s->geometry>>4)&15)!=2 || (s->geometry&15)!=5 || !s->perf_count || s->perf_count>8 ||
       !s->node_count || s->node_count>16)return GCE_GEOMETRY;
    if(s->bw_ready && (s->bw_ready!=GCB_PATH_MASK || s->node_count<GCB_NODES))return GCE_GEOMETRY;
    s->update=gce_read(b,s,GCE_CRM,0x1020);s->busy=gce_read(b,s,GCE_CRM,0x1000);
    if(s->update!=s->busy || (s->update!=0 && s->update!=1 && s->update!=2))return GCE_BUSY;
    s->source=s->update==2?1:0;s->target=s->update?(s->source^1):0;
    for(unsigned channel=0;channel<2;channel++)for(unsigned type=0;type<2;type++)for(unsigned power=0;power<5;power++) {
        unsigned count=type?s->node_count:s->perf_count;
        for(unsigned resource=0;resource<count;resource++) {
            unsigned value=gce_read(b,s,GCE_CRM,gce_reg(type,channel,power,resource,count));
            if(value==0xffffffffu || (!type && value>5) || (type && (value&0xd0000000u)))return GCE_UNOWNED;
            s->old[channel][type][power][resource]=value;
        }
    }
    s->sm_start=gce_read(b,s,GCE_SM,0x1090);s->sm_control=gce_read(b,s,GCE_SM,0x1040);s->sm_mask=gce_read(b,s,GCE_SM,0x1048);
    if(s->sm_start==0xffffffffu || s->sm_control>1 || s->sm_mask>1)return GCE_UNOWNED;
    unsigned identical=1,empty=1;
    for(unsigned i=0;i<13;i++){s->code[i]=gce_read(b,s,GCE_SM,i*4);identical&=s->code[i]==gce_code[i];empty&=!s->code[i];}
    for(unsigned i=0;i<7;i++){s->branch[i]=gce_read(b,s,GCE_SM,0x1000+i*4);identical&=s->branch[i]==gce_branch[i];empty&=!s->branch[i];}
    if(!identical && (!empty || (s->sm_start&1)))return GCE_UNOWNED;
    /* The program RAM may be shared by both PCIe instances. Never start or
     * replace a stopped manager while voter 1 has published a channel. An
     * already running identical program is reused without rewriting it. */
    if(!(s->sm_start&1) && (gce_read(b,s,GCE_CRM,0x1024)!=0 || gce_read(b,s,GCE_CRM,0x1004)!=0))
        return GCE_BUSY;
    if(!b->quiescent(b->context) || gce_read(b,s,GCE_CRM,0x1020)!=s->update || gce_read(b,s,GCE_CRM,0x1000)!=s->busy)return GCE_BUSY;
    s->changed=1;
    /* Stock loads/enables the state manager before publishing D0 votes. */
    if(!gce_write(b,s,GCE_SM,0x1040,0) || !gce_write(b,s,GCE_SM,0x1048,0))
        return gce_fail(b,s,GCE_WRITE_ERROR);
    if(!(s->sm_start&1)) {
        s->program_changed=1;
        for(unsigned i=0;i<13;i++)if(!gce_write(b,s,GCE_SM,i*4,gce_code[i]))return gce_fail(b,s,GCE_WRITE_ERROR);
        for(unsigned i=0;i<7;i++)if(!gce_write(b,s,GCE_SM,0x1000+i*4,gce_branch[i]))return gce_fail(b,s,GCE_WRITE_ERROR);
    }
    if(!gce_write(b,s,GCE_SM,0x1090,s->sm_start|1u))
        return gce_fail(b,s,GCE_WRITE_ERROR);
    if(!gce_copy_channel(b,s,s->target,s->source,1))return gce_fail(b,s,GCE_WRITE_ERROR);
    if(!gce_switch(b,s,s->target))return gce_fail(b,s,GCE_SWITCH_TIMEOUT);
    for(unsigned i=0;i<1000;i++) {
        unsigned perf=gce_read(b,s,GCE_CLOCK,0xc);
        if(perf>=GW_STOCK_D0_PERF && perf<=5){s->active=1;return GCE_OK;}
        if(perf==0xffffffffu)return gce_fail(b,s,GCE_GEOMETRY);
        b->delay(b->context,100);
    }
    return gce_fail(b,s,GCE_POWER_TIMEOUT);
}
#endif
