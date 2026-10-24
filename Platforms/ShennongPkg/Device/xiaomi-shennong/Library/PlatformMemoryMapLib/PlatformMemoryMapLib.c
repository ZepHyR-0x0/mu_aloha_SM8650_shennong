/*
 * Xiaomi 14 (shennong, SM8650 / Qualcomm "pineapple"/"lanai") memory map.
 *
 * Sources:
 *  - Low regions (0x80000000..0xA8000000) and the UEFI FD placement follow the
 *    PROVEN SM8650 layout from UEFI-For-OnePlus13R (Resources/Configs/giulia.toml:
 *    fd_base=0xA7100000; Platforms/OnePlus/giuliaPkg/Library/MemoryMapLib).
 *  - High regions (>0xA8000000) come from this device's live DTB dump
 *    (/sys/firmware/fdt of shennong, 2026-10-21): cont_splash @0xFC400000,
 *    TZ merged @0xD8000000, kernel RAM @0xE7830000..0xFF300000, 16GB SKU.
 *  - CPU vectors / Info Blk / PrePi stack at 0xA7600000 are identical on both
 *    references (kalama and lanai) - confirmed.
 */
#include <Library/BaseLib.h>
#include <Library/PlatformMemoryMapLib.h>

static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
    /* Name               Address     Length      HobOption        ResourceAttribute    ArmAttributes
                                                          ResourceType          MemoryType */
    /* DDR Regions */
    /* DDR Bank 0 Start */
    // gunyah_hyp_region (0xE00000) + cpusys_vm_region (0x400000) + XBL reserved
    { "NOMAP",             0x80000000, 0x016E0000, NoHob,  MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN },
    { "RSRV0",             0x816E0000, 0x00320000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN },
    { "XBL DT",            0x81A00000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN },
    { "XBL Ramdump",       0x81A40000, 0x001C0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN },
    { "AOP",               0x81C00000, 0x000A0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN },
    { "RAM Partition",     0x81CA0000, 0x00044000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN },
    { "UEFI Log",          0x81CE4000, 0x00010000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN },
    { "RAM Partition",     0x81CF4000, 0x0000C000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN },
    { "SMEM",              0x81D00000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN },
    { "RAM Partition",     0x81F00000, 0x005A0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN },
    { "PvmFw",             0x824A0000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN },
    { "RAM Partition",     0x825A0000, 0x09660000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN },
    { "PIL Reserved",      0x8BC00000, 0x17580000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN },
    { "RAM Partition",     0xA3180000, 0x02F80000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN },
    { "DBI Dump",          0xA6100000, 0x00F00000, NoHob,  MMAP_IO, INITIALIZED, Conv,   UNCACHED_UNBUFFERED_XN },
    /* UEFI FD window: 0xA7100000 (SM8650 XBL window, proven by OnePlus 13R port) */
    { "FD Reserved",       0xA7000000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK },
    { "UEFI FD",           0xA7100000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK },
    { "UEFI FD Reserved",  0xA7500000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK },
    { "CPU Vectors",       0xA7600000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK },
    { "Info Blk",          0xA7601000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN },
    { "MMU PageTables",    0xA7602000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN },
    { "Log Buffer",        0xA7605000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN },
    { "UEFI Stack",        0xA760D000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN },
    { "SEC Heap",          0xA764D000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN },
    { "Sched Heap",        0xA76D9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN },
    { "FV Region",         0xA7AD9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN },
    { "UEFI RESV",         0xA7ED9000, 0x00127000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN },
    { "Kernel",            0xA8000000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN },
    { "DXE Heap",          0xB8000000, 0x20000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN },
    /* High regions from shennong DTB (device specific) */
    { "TZ Merged",         0xD8000000, 0x00800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN },
    { "TZApps Reserved",   0xD8800000, 0x09030000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN },
    { "RAM Partition",     0xE7830000, 0x14BD0000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN },
    { "Display Reserved",  0xFC400000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN },
    { "RAM Partition",     0xFEF00000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN },
    { "RAM Partition",     0xFF3DD000, 0x00423000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN },
    { "RAM Partition",     0xFFE00000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN },

    /* RAM partition regions */
    /* >4GB banks: 0x880000000 (+6GB) and 0xA00000000 (+8GB) per /ddr-regions (16GB SKU). */
    { "RAM Partition",     0x880000000, 0x180000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN },

    /* Other memory regions */
    {"IMEM Base",         0x14680000, 0x0002A000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
    {"IMEM Cookie Base",  0x146AA000, 0x00016000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
    {"AOP_SS_MSG_RAM",    0x0C300000, 0x00100000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},

    /* Register regions */
    // SPMI_PMIC_ARB removed: listed before AOSS and inside it (0x0C400000).
    // DxeCore adds SPMI first then rejects AOSS as an overlap -> AOSS nonexistent.
    // AOSS covers 0x0B000000..0x0F000000 and TLMM covers the rest. odin2 has no SPMI entry.
    { "IPC_ROUTER_TOP",     0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "SECURITY CONTROL",   0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "QUP",                0x00800000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "PRNG_CFG_PRNG",      0x010C0000, 0x0000C000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    // Extended down to 0x1DB0000: HALIOC asks to map [0x1DB0000, 0x1DF0000)
    // (256 KB, ending exactly at CRYPTO_KEY_MANAGER) and the range used to
    // start 0x10000 above the old base, so SetMemorySpaceAttributes failed:
    //   Failed to map PhysicalAddr:0x1DB0000, size:0x40000
    { "CRYPTO0 CRYPTO",     0x01DB0000, 0x00050000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "CORE_TOP_CSR",       0x01F00000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "PERIPH_SS",          0x08800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "USB",                0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "AOSS",               0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    //
    // PDC removed: it sits INSIDE AOSS (0x0B000000 + 0x04000000) and the overlap
    // made DxeCore reject the whole AOSS descriptor. The GCD probe showed the
    // consequence:
    //   [GCD] AOSS @0x0B000000 in [0x0B000000+0x220000] type=nonexistent attr=0
    //   -> SetMemorySpaceAttributes fails for 0x0B000000/0x04000000
    //   -> HWIO mapping failed, and TLMM at 0x0F000000 sits right behind it
    // The working SM8550 port lists AOSS only, with no PDC entry.
    //
    { "TLMM",               0x0F000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "SMMU",               0x15000000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "APSS_HM",            0x17000000, 0x02000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "GICD",               0x17100000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    //
    // Kept at the original 0x9000. I had shrunk it to 0x3000 on the assumption
    // that overlapping AddDev ranges break gDS->AddMemorySpace - that assumption
    // is wrong: this table already carries eight overlapping pairs
    // (PERIPH_SS/USB_SS identical, AOSS containing PDC, APSS_HM containing GICD,
    // CPU Vectors/CPU_Vectors identical). Overlap is normal here.
    //
    //
    // 0x1D80000, not 0x1D84000, and 0x30000 wide.
    //
    // HWIO's UFS_MEM_UFS_2LANE_G5_REGS is at 0x1D80000 and UFSDxe asks to map
    // [0x1D80000, 0x1DC0000). The old entry only covered [0x1D84000, 0x1D8D000),
    // leaving two holes inside that request - [0x1D80000,0x1D84000) and
    // [0x1D90000,0x1DB0000) - so SetMemorySpaceAttributes failed and UFSDxe got a
    // garbage base address.
    //
    // This range ends exactly where CRYPTO begins (0x1DB0000); adjacent MMIO
    // descriptors with matching attributes merge in the GCD, so the two together
    // cover the whole request. The narrower UFS_MEM_UFS / UFS_MEM_ICE entries stay:
    // the outer UFS wins in the GCD, but the config RegisterMap still needs those
    // names for HWIO lookups by name.
    //
    { "UFS",                0x1D80000,  0x00030000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    //
    // UFSDxe does not ask for "UFS". Its strings are explicit:
    //   ERROR: 0x%08x Failed to map UFS_MEM_UFS base address: 0x%08x
    //   ERROR: 0x%08x Failed to map UFS_MEM_ICE base address: 0x%08x
    // With no entry under those names the lookup returns a wild address, which
    // is the 0x32B84239 seen on the device. Ranges come from the DT
    // (ufshc@1d84000, reg-names ufs_mem / ufs_ice).
    //
    { "UFS_MEM_UFS",        0x1D84000,  0x00003000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "UFS_MEM_ICE",        0x1D88000,  0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    //
    // NOTE: CRYPTO0_CRYPTO / CRYPTO0_CRYPTO_BAM / MCCC_MCCC_MSTR were added here
    // and then removed again. With them in the table EnvDxe dies while building
    // its config database:
    //   Synchronous Exception, IMG = EnvDxe.dll + 0x3208
    //   ESR EC=0x25 (data abort), FAR=0x148E9318
    // Without them the boot gets past EnvDxe and on to ConfigWords. The existing
    // "CRYPTO0 CRYPTO" entry (0x1DC0000 + 0x40000) already covers the crypto
    // registers, so nothing is missing for those drivers anyway.
    //
    { "USB_SS",             0x08800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },
    { "USB",                0x0A000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE },

    /* Terminator for MMU */
    {"Terminator", 0, 0, 0, 0, 0, 0, 0}};

ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap()
{
  return gDeviceMemoryDescriptorEx;
}
