// Interface to the ReBIOS hypervisor
//
// Copyright (C) 2026  Mintsuki
//
// This file may be distributed under the terms of the GNU LGPLv3 license.
#ifndef __REBIOS_H
#define __REBIOS_H

#include "types.h" // u32

// The Compatibility16 table's HypervisorInterface field, when the CSM16 runs
// under the ReBIOS hypervisor.
#define REBIOS_INTERFACE 0x52420001

// A hypercall is VMCALL with the magic in the top half of EAX and the function
// in its bottom half. On AMD processors, where VMCALL raises #UD, the
// hypervisor catches the exception and takes it as the hypercall.
#define REBIOS_HYPERCALL_MAGIC 0x52420000
#define REBIOS_HC_DEBUG_PUTC 0x0000
#define REBIOS_HC_RESET 0x0002
#define REBIOS_HC_CALL32_INIT 0x0003
#define REBIOS_HC_CALL32_ENTER 0x0004
#define REBIOS_HC_CALL32_RETURN 0x0005

// REBIOS_HC_RESET's EBX.
#define REBIOS_RESET_COLD 0
#define REBIOS_RESET_WARM 1
#define REBIOS_RESET_SHUTDOWN 2

extern int ReBIOSPresent;

static inline void rebios_hypercall(u32 function, u32 ebx)
{
    asm volatile("vmcall"
                 : : "a"(REBIOS_HYPERCALL_MAGIC | function), "b"(ebx) : "memory");
}

#endif // rebios.h
