#
# MINIMAL PROFILE - FV contents (no Qualcomm binaries)
#
# Only the drivers that do not need the Qualcomm configuration database.
# Everything else that the platform still needs (DxeCore, RuntimeDxe, BdsDxe,
# DevicePathDxe, EnglishDxe, HiiDatabaseDxe, Fat, FvSimpleFileSystemDxe,
# WatchdogTimer, RealTimeClockRuntimeDxe, CapsuleRuntimeDxe, ...) is already
# listed explicitly in Shennong.fdf and therefore stays in the firmware volume.
#
# Switch with: scripts/profile.sh full|min
#

INF AndromedaPkg/Driver/SimpleFbDxe/SimpleFbDxe.inf
INF AndromedaPkg/Driver/RamPartitionDxe/RamPartitionDxe.inf

INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

# QUALCOMM PREBUILT, but it needs no configuration database: it loads
# "SerialPort Lib" through the ShLib HOB (PlatformPeiLib) and produces a
# SimpleTextOut/SimpleTextIn whose output goes straight to
# FrameBufferSerialPortLib. That is the only way to get visible UEFI console
# text here - GraphicsConsoleDxe needs GOP + HII fonts and renders nothing.
INF AndromedaPkg/Driver/SerialTextConsoleDxe/SerialTextConsoleDxe.inf
