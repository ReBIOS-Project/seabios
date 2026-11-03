ATAPI removable media checks
===========================

From the repository root, with GCC and its AddressSanitizer/UBSan runtime:

    gcc -Isrc -Wall -Wextra -Werror -fsanitize=address,undefined -g \
        tests/atapi-floppy.c -o /tmp/seabios-atapi-floppy-test
    ASAN_OPTIONS=detect_leaks=0 /tmp/seabios-atapi-floppy-test

The test uses the production model matching and geometry helpers. It checks
LS-120 identification, case-insensitive configuration matching, truncated
model names, supported floppy capacities, and rejection of ZIP and invalid
block sizes. It also checks ZIP100/250 legacy 64-head/32-sector translation
and preservation of geometry supplied by the host. Leak detection is disabled
because this test allocates nothing
and LeakSanitizer cannot run in some traced sandboxes.

Build the ReBIOS CSM configuration with GCC, GNU binutils, make and Python 3:

    cp ../ReBIOS/seabios-config .config
    make olddefconfig
    make -j4

Real hardware validation is still required for IDE-to-SATA ATAPI bridges
and the AHCI controller path. Insert the intended medium
before boot. Switching between 720-KB, 1.44-MB and 120-MB formats at runtime
is not implemented: the capacity and geometry are recorded during POST.
Low-level formatting is rejected. ZIP remains a hard disk with its MBR.

DOS BIOS diagnostic
-------------------

With NASM 2.x, build a DOS diagnostic (no ASPI manager needed):

    nasm -f bin tests/bios-ls120.asm -o /tmp/BIOSTEST.COM

Run BIOSTEST.COM under DOS using disposable test media in BIOS drive 00h.
It checks geometry and boot-sector reads, then writes/reads the last CHS
sector and restores its previous contents. It reports success through DOS
standard output. Use `BIOSTEST > AUX` to capture it through a VM's COM1.

The diagnostic passed with MS-DOS 7.1 booted from an emulated ATAPI LS-120
with 1.44-MB media in an isolated 86Box fixture. That fixture supplies a
standalone BIOS platform and the same `etc/floppy-a` ROM file that ReBIOS
publishes through CSM; it does not run the ReBIOS hypervisor or emulate
an IDE-to-SATA bridge. Keep those validation scopes distinct.

AHCI ATAPI transport checks
--------------------------

With Python 3, GCC (or CC=clang), AddressSanitizer and UBSan:

    python3 tests/ahci-atapi.py

This compiles the production CDB builder, ATAPI FIS preparation and AHCI
ATAPI transfer functions with a mock DMA submission boundary. It verifies
aligned and odd-address read/write buffers, multi-sector bounce copies,
write direction, failure propagation, CD write protection, and rejection
of unaligned pre-built SCSI requests. It does not emulate an AHCI controller
or qualify a physical SATA bridge.

DOS integration results
-----------------------

An isolated 86Box PATA fixture booted MS-DOS 7.1 from a 1.44-MB disk in an
ATAPI LS-120 as A:, then completed an installation through four setup disks,
including FDISK, reboot, FORMAT, SYS and three media swaps. A native 120-MB
FAT16 superfloppy also passed BIOS and DOS reads/writes, SYS and boot as A:.
The installed DOS system used HIMEM/EMM386 for the native-media read/write
checks; the fixture's KEYB startup line was omitted to isolate storage.

Use BIOS drive 00h in the BPB of a bootable LS-120 superfloppy (for example
`mkfs.fat -D 0` when preparing the fixture on Linux). Large FAT16 media may
otherwise default to drive 80h, and DOS SYS preserves that byte.

ATAPI ZIP100 and ZIP250 passed BIOS reads/writes, DOS file copies with and
without VERIFY, SYS, and DOS 7.1 boot from active type-06 FAT16 partitions
in MBR slot 4, starting at LBA 32. They remain hard disks. Their legacy CHS
boot sectors require 64 heads and 32 sectors/track in both the BIOS geometry
and FAT BPB. Booted file copies matched the original 100,000-byte payload
by SHA-256, and FAT checks passed after shutdown.
