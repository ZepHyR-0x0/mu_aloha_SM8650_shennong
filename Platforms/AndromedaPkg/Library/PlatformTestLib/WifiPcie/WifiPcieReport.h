/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed-width version-5 protocol report shared with DarwinBoot (nine rails).
 */
#ifndef GIULIA_WIFI_PCIE_REPORT_H
#define GIULIA_WIFI_PCIE_REPORT_H
/* Failure-only extension: device==0 and command high 24 bits equal this
   tag mean command[7:0]=CESTA cleanup error, bar0=region:offset, bar1=value.
   These are not PCI endpoint registers. Successful v5 reports are unchanged. */
#define GW_CESTA_CLEANUP_TAG 0xce570000u
#define GW_XNU_LINK_TAG 0x584e554cu
/* giulia_wifip/stage 18: sw_ctrl/wlan_sw_ctrl hold the pre-setup
   GPIO18/19 INTR_CFG values; bar0/bar1 hold the final values. No endpoint
   BARs or SW_CTRL CFG/IO registers have been sampled in this handoff. */
struct gw_report {
  unsigned version, stage, detail, error;
  unsigned long long firmware_status, clock_version;
  unsigned rails, clocks, phy, link, elbi, device, command, bar0, bar1;
  unsigned gpio_before, gpio_after, root_device;
  unsigned retained_rails, pipe_source; /* previous mux in high 16 bits, readback in low 16 */
  unsigned long long cleanup_status;
  unsigned perst, sw_ctrl, wlan_sw_ctrl, ref_clock;
  unsigned port_link_before, port_link_after, link_cap, gen2_ctrl;
  unsigned pipe_gate, phy_common, phy_status2, phy_status3, clkreq;
  unsigned ltssm_seen_lo, ltssm_seen_hi, ltssm_changes;
  unsigned pipe_hz, pipe_measure_status;
};
#endif
