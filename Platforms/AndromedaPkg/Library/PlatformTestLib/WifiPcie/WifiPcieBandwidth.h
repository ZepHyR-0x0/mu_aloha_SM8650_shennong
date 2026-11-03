/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PCIe0 -> SN4 -> SH1/SH0 -> MC0/ACV interconnect request.
 * Hardware topology and arithmetic are documented by the pinned OnePlus
 * pineapple.c, bcm-voter.c, pci-msm.c and the 8-byte Command DB BCM record.
 * Only HW voter 0 states 0/1 are used. IP0 (node 4) is not on this path.
 */
#ifndef GIULIA_WIFI_PCIE_BANDWIDTH_H
#define GIULIA_WIFI_PCIE_BANDWIDTH_H
#define GCB_NODES 6u
#define GCB_PATH_MASK 0x2fu
#define GCB_VALID 0x20000000u
#define GCB_VOTE_MASK 0x3fffu
static inline const char *gcb_name(unsigned node) {
    static const char *const names[GCB_NODES]={"MC0","SH0","SH1","SN4",0,"ACV"};
    return node<GCB_NODES?names[node]:0;
}
static inline unsigned long long gcb_div(unsigned long long n,unsigned d) {
    return n && n<d?1:n/d;
}
static inline int gcb_vote(unsigned node,const unsigned char *aux,unsigned length,unsigned *vote) {
    static const unsigned bus[GCB_NODES]={4,16,16,16,0,4};
    static const unsigned channels[GCB_NODES]={4,4,1,1,0,4};
    if(node>=GCB_NODES || !((1u<<node)&GCB_PATH_MASK) || !aux || length!=8 || !vote)return 0;
    unsigned unit=(unsigned)aux[0]|(unsigned)aux[1]<<8|(unsigned)aux[2]<<16|(unsigned)aux[3]<<24;
    unsigned width=(unsigned)aux[4]|(unsigned)aux[5]<<8;
    /* qcom_icc_bcm_init normalizes zero unit/width to one. VCD and the
       reserved byte are not used for a fixed CRM node mapping. In
       particular, one-hot resources need no numeric unit conversion. */
    if(!unit)unit=1;
    if(!width)width=1;
    /* Vendor initial request: average 500 kB/s, peak 800 kB/s; scale 1000.
       Its division rounds a positive sub-unit request to one, otherwise
       truncates. Use 64-bit intermediates before the 14-bit saturation. */
    unsigned long long x=gcb_div(gcb_div(500ull*width,bus[node]*channels[node])*1000,unit);
    unsigned long long y=gcb_div(gcb_div(800ull*width,bus[node])*1000,unit);
    if(node==2 || node==5){x=0;y=1;} /* one-hot enable resources */
    if(x>GCB_VOTE_MASK)x=GCB_VOTE_MASK;
    if(y>GCB_VOTE_MASK)y=GCB_VOTE_MASK;
    /* COMMIT/RESP_REQ belong to SW voters; HW votes publish via CHN_UPDATE. */
    *vote=GCB_VALID|((unsigned)x<<14)|(unsigned)y;
    return 1;
}
static inline unsigned gcb_merge(unsigned old,unsigned floor,unsigned node) {
    if(!floor)return old;
    if(node==2 || node==5)return old|floor;
    unsigned x=(old>>14)&GCB_VOTE_MASK,y=old&GCB_VOTE_MASK;
    unsigned fx=(floor>>14)&GCB_VOTE_MASK,fy=floor&GCB_VOTE_MASK;
    return GCB_VALID|((x>fx?x:fx)<<14)|(y>fy?y:fy);
}
#endif
