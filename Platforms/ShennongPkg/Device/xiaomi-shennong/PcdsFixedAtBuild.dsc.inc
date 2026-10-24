# Device PCD values for Xiaomi 14 (shennong / SM8650)
# All values below are CONFIRMED from the live DTB dump (2026-10-21) unless noted:
#   - Panel 1440x3200: DT node qcom,mdss_dsi_n2_42_02_0a_p2_dsc_cmd
#     (qcom,mdss-dsi-panel-width=0x5A0 / panel-height=0xC80, bpp=0x1E DSC, 60Hz),
#     selected by bootargs "msm_drm.dsi_display0=qcom,mdss_dsi_n2_42_02_0a_p2_dsc_cmd".
#   - Framebuffer base comes from "Display Reserved" (cont_splash_region @0xFC400000/0x02B00000)
#     in Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c - SimpleFbDxe looks that region up by name.
# TODO(port): PcdABLProduct must match the string XBL negotiates (candidates: "Xiaomi 14", "shennong",
#   "pineapple" - the shennong DTB compatible is "qcom,pineapple-mtp", msm-id 0x22d/0x241, board-id 0x08).
[PcdsFixedAtBuild.common]
gAndromedaPkgTokenSpaceGuid.PcdMipiFrameBufferWidth|1440
gAndromedaPkgTokenSpaceGuid.PcdMipiFrameBufferHeight|3200

# Display Caller
gAndromedaPkgTokenSpaceGuid.PcdDisplayCallerExitDisableDisplay|TRUE
gAndromedaPkgTokenSpaceGuid.PcdDisplayCallerStallBeforeEnable|1000

# Smbios Info
gAndromedaPkgTokenSpaceGuid.PcdSmbiosSystemBrand|"Xiaomi"
gAndromedaPkgTokenSpaceGuid.PcdSmbiosSystemModel|"14"
gAndromedaPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"shennong"
gAndromedaPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"CN"

gAndromedaPkgTokenSpaceGuid.PcdABLProduct|"Xiaomi 14"

[PcdsDynamicDefault.common]
gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|1440
gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|3200
gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|1440
gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|3200
gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|180  # = 1440 / EFI_GLYPH_WIDTH(8)
gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|168    # = 3200 / EFI_GLYPH_HEIGHT(19)
gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|180       # = 1440 / EFI_GLYPH_WIDTH(8)
gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|168         # = 3200 / EFI_GLYPH_HEIGHT(19)
