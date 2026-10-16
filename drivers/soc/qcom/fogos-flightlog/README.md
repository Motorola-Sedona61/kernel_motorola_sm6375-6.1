# Fogos diagnostic flight recorder

This optional external module is built against the unchanged kernel output,
including its Module.symvers and vmlinux BTF base. It exports no symbols and
does not change the original kernel Image or driver modules.

The module is packaged in the vendor ramdisk but excluded from recovery and
vendor modules.load. First-stage init loads it with `arm=1` only when the
existing fogos normal-boot diagnostic gate is active. Recovery never starts
the recorder. Loading without the explicit parameter fails with EINVAL.

Asynchronous per-CPU calls start pinned hard hrtimers at 250 ms intervals.
Each CPU prints a heartbeat once per second from uptime10 through40seconds,
including interrupted task and PC. A separate kernel thread saves bounded
kernel stacks for up to24 named or blocked tasks at uptimes
12/14/15/16/17/18/20/24/28/32seconds. Task selection retains references under
RCU and exits both nested task loops when the scan or selection limit is hit.
No synchronous cross-CPU response wait is added.

The root-only `/proc/fogos-flightlog` view exposes current records to the
existing durable metadata collector, which saves `kernel-flightlog.txt` and
its12s checkpoint. Kernel printk output also reaches existing pstore capture.
A contiguous buffer with magic `FGFLT001` is registered as `FOGOSFLT` in the
Qualcomm minidump table. Registration does not establish bootloader export
and does not enable download mode. Its source structs define the binary
layout; commit counters distinguish incomplete writes.

There are no watchdog pet, timeout, panic, reset or filesystem-write calls.
Observation ends at uptime40seconds. The module has no unload entry so that
timer callbacks and registered dump memory remain valid until reboot.
Restore the previous boot/vendor_boot images to remove it.

Extra IRQs, printk and stack unwinds can alter timing. These are normal IRQs,
not NMIs: a CPU with interrupts masked cannot execute its recorder. A whole
SoC freeze stops software recording. Concurrent task stacks may be partial;
minidump and pstore recovery still depend on firmware/RAM preservation.
