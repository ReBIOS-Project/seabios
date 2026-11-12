// Interface to the ReBIOS hypervisor
//
// Copyright (C) 2026  Mintsuki
//
// This file may be distributed under the terms of the GNU LGPLv3 license.
#ifndef __REBIOS_H
#define __REBIOS_H

#include "biosvar.h" // GET_GLOBAL
#include "types.h" // u32

// The Compatibility16 table's HypervisorInterface field, when the CSM16 runs
// under the ReBIOS hypervisor.
#define REBIOS_INTERFACE 0x52420001

// A hypercall is VMCALL on Intel processors and VMMCALL on AMD's, where VMCALL
// raises #UD, with the magic in the top half of EAX and the function in its
// bottom half.
#define REBIOS_HYPERCALL_MAGIC 0x52420000
#define REBIOS_HC_DEBUG_PUTC 0x0000
#define REBIOS_HC_RESET 0x0002
#define REBIOS_HC_CALL32_INIT 0x0003
#define REBIOS_HC_CALL32_ENTER 0x0004
#define REBIOS_HC_CALL32_RETURN 0x0005
#define REBIOS_HC_USB_DISK 0x0006
#define REBIOS_HC_PNP_NODES 0x0007

// REBIOS_HC_RESET's EBX.
#define REBIOS_RESET_COLD 0
#define REBIOS_RESET_WARM 1
#define REBIOS_RESET_SHUTDOWN 2

// REBIOS_HC_USB_DISK's EBX is the physical address of one of these.
struct rebios_usb_disk {
    u32 function;
    u32 disk;
    u64 lba;
    u32 count;
    u32 buffer;
    u32 status;
    u32 reserved;
};

#define REBIOS_USB_DISK_READY 0
#define REBIOS_USB_DISK_INFO 1
#define REBIOS_USB_DISK_READ 2
#define REBIOS_USB_DISK_WRITE 3
#define REBIOS_USB_DISK_POLL 4

#define REBIOS_USB_DISK_STATUS_OK 0
#define REBIOS_USB_DISK_STATUS_PENDING 1
#define REBIOS_USB_DISK_STATUS_NO_MEDIUM 2
#define REBIOS_USB_DISK_STATUS_WRITE_PROTECTED 3
#define REBIOS_USB_DISK_STATUS_FAILED 4
#define REBIOS_USB_DISK_STATUS_NO_DISK 5

#define REBIOS_USB_DISK_CD (1 << 0)
#define REBIOS_USB_DISK_REMOVABLE (1 << 1)

// REBIOS_USB_DISK_INFO's buffer.
struct rebios_usb_disk_info {
    u64 blocks;
    u32 block_size;
    u32 flags;
    // The controller, and the device's bootorder path below it.
    u32 bdf;
    char path[64];
    char description[64];
};

// REBIOS_HC_PNP_NODES's EBX is the physical address of one of these: the
// hypervisor writes the Plug and Play BIOS's system device nodes to buffer,
// one after the other, and their length, or 0 where size bytes do not hold
// them.
struct rebios_pnp_nodes {
    u32 buffer;
    u32 size;
    u32 length;
    u32 reserved;
};

extern int ReBIOSPresent;
// Whether hypercalls are VMMCALL, as on AMD processors, rather than VMCALL.
extern int ReBIOSVmmcall;

static inline void rebios_hypercall(u32 function, u32 ebx)
{
    if (GET_GLOBAL(ReBIOSVmmcall))
        asm volatile("vmmcall"
                     : : "a"(REBIOS_HYPERCALL_MAGIC | function), "b"(ebx)
                     : "memory");
    else
        asm volatile("vmcall"
                     : : "a"(REBIOS_HYPERCALL_MAGIC | function), "b"(ebx)
                     : "memory");
}

#endif // rebios.h
