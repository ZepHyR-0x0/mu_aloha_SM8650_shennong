@echo -off
#
# startup_verbose.nsh - long version of the boot log.
#
# NOT executed automatically: it is kept in the repo as the source for
# startup.nsh. Copy it over startup.nsh (and rebuild) when the extra sections
# are needed.
#
echo == firmware ==
ver
echo
echo == memory map ==
memmap
echo
echo == dispatched drivers ==
drivers
echo
echo == handles holding GOP ==
dh -p 9042A9DE-23DC-4A38-96FB-7ADED080516A
echo
echo == handles holding SimpleTextOut ==
dh -p 387477C1-69C7-11D9-8E39-0002A5D5C51B
echo
echo ==== end ====
