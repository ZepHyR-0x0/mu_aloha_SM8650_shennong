@echo -off
#
# startup.nsh - auto-executed by the UEFI Shell on the xiaomi-shennong port.
#
# Compact on purpose: there is no keyboard, so the screen is the only channel and
# every extra line makes the log harder to read. Keep this file short; the
# verbose variant is startup_verbose.nsh (run it manually only if needed).
#
echo == shennong SM8650 / minimal UEFI ==
ver
echo
echo == memory map ==
memmap
