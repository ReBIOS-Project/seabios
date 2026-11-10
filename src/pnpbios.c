// PNP BIOS calls
//
// Copyright (C) 2008  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "biosvar.h" // GET_GLOBAL
#include "config.h" // BUILD_BIOS_ADDR
#include "farptr.h" // SET_FARVAR
#include "fw/rebios.h" // rebios_hypercall
#include "malloc.h" // malloc_fseg
#include "output.h" // dprintf
#include "std/pnpbios.h" // PNP_SIGNATURE
#include "string.h" // checksum
#include "util.h" // pnp_init

extern struct pnpheader PNPHEADER;
extern char pnp_string[];

#if CONFIG_PNPBIOS
struct pnpheader PNPHEADER __aligned(16) VARFSEG = {
    .signature = PNP_SIGNATURE,
    .version = 0x10,
    .length = sizeof(PNPHEADER),
    .real_cs = SEG_BIOS,
    .prot_base = BUILD_BIOS_ADDR,
    .real_ds = SEG_BIOS,
    .prot_database = BUILD_BIOS_ADDR,
};
#else
// We need a copy of this string in the 0xf000 segment, but we are not
// actually a PnP BIOS, so make sure it is *not* aligned, so OSes will
// not see it if they scan.
char pnp_string[] __aligned(2) VARFSEG = " $PnP";
#endif

// The system device nodes (Plug and Play BIOS 1.0A, section 4.2), one after
// the other at an offset into the f-segment, where the code segment reaches
// them in real and 16-bit protected mode alike, as the specification has it
// readable.
u16 PnpNodes VARFSEG;
u16 PnpNodesLength VARFSEG;
u8 PnpNodeCount VARFSEG;
u16 PnpNodeLargest VARFSEG;

// A node's size, handle, identifier, type code, and attributes.
#define PNP_NODE_HEADER 12
#define PNP_NODES_MAX 1024
#define PNP_NO_NODE 0xffff
#define PNP_NO_HANDLE 0xff

static u8
pnp_node_byte(u16 offset)
{
    return GET_FARVAR(GET_SEG(CS), *(u8*)(u32)offset);
}

static u16
pnp_node_word(u16 offset)
{
    return GET_FARVAR(GET_SEG(CS), *(u16*)(u32)offset);
}

// The offset of the node with the handle, or PNP_NO_NODE.
static u16
pnp_node_find(u8 handle)
{
    u16 node = GET_GLOBAL(PnpNodes);
    u16 end = node + GET_GLOBAL(PnpNodesLength);
    for (; node < end; node += pnp_node_word(node))
        if (pnp_node_byte(node + 2) == handle)
            return node;
    return PNP_NO_NODE;
}

// Get Number of System Device Nodes
static u16
handle_pnp00(u16 *args)
{
    u16 count_ptr = args[1];
    u16 count_seg = args[2];
    u16 size_ptr = args[3];
    u16 size_seg = args[4];
    SET_FARVAR(count_seg, *(u8*)(u32)count_ptr, GET_GLOBAL(PnpNodeCount));
    SET_FARVAR(size_seg, *(u16*)(u32)size_ptr, GET_GLOBAL(PnpNodeLargest));
    return 0;
}

// Get System Device Node, configured now and for the next boot alike.
static u16
handle_pnp01(u16 *args)
{
    u16 node_ptr = args[1];
    u16 node_seg = args[2];
    u16 buffer_ptr = args[3];
    u16 buffer_seg = args[4];
    u16 control = args[5] & 3;
    if (control != 1 && control != 2)
        return BAD_PARAMETER;
    u16 node = pnp_node_find(GET_FARVAR(node_seg, *(u8*)(u32)node_ptr));
    if (node == PNP_NO_NODE)
        return INVALID_HANDLE;
    u16 size = pnp_node_word(node);
    memcpy_far(buffer_seg, (void*)(u32)buffer_ptr
               , GET_SEG(CS), (void*)(u32)node, size);
    u16 next = node + size;
    u8 handle = PNP_NO_HANDLE;
    if (next < GET_GLOBAL(PnpNodes) + GET_GLOBAL(PnpNodesLength))
        handle = pnp_node_byte(next + 2);
    SET_FARVAR(node_seg, *(u8*)(u32)node_ptr, handle);
    return 0;
}

// Set System Device Node: no node's device can be configured.
static u16
handle_pnp02(u16 *args)
{
    u8 handle = args[1];
    if (pnp_node_find(handle) == PNP_NO_NODE)
        return INVALID_HANDLE;
    return SET_FAILED;
}

// BBS - Get Version and Installation Check
static u16
handle_pnp60(u16 *args)
{
    u16 version_ptr = args[1];
    u16 version_seg = args[2];
    SET_FARVAR(version_seg, *(u16*)(version_ptr+0), 0x0101);
    return 0;
}

static u16
handle_pnpXX(u16 *args)
{
    return FUNCTION_NOT_SUPPORTED;
}

u16 VISIBLE16
handle_pnp(u16 *args)
{
    if (! CONFIG_PNPBIOS)
        return FUNCTION_NOT_SUPPORTED;

    u16 arg1 = args[0];
    dprintf(DEBUG_HDL_pnp, "pnp call arg1=%x\n", arg1);

    // The system configuration functions need device nodes to report.
    if (arg1 <= 0x02 && !GET_GLOBAL(PnpNodeCount))
        return FUNCTION_NOT_SUPPORTED;

    switch (arg1) {
    case 0x00: return handle_pnp00(args);
    case 0x01: return handle_pnp01(args);
    case 0x02: return handle_pnp02(args);
    case 0x60: return handle_pnp60(args);
    default:   return handle_pnpXX(args);
    }
}

u16
get_pnp_offset(void)
{
    if (! CONFIG_PNPBIOS)
        return (u32)pnp_string + 1 - BUILD_BIOS_ADDR;
    return (u32)&PNPHEADER - BUILD_BIOS_ADDR;
}

// romlayout.S
extern void entry_pnp_real(void);
extern void entry_pnp_prot(void);

// Keep the system device nodes the ReBIOS hypervisor gives, once each one's
// size is found to fit.
static void
rebios_pnp_nodes(void)
{
    u8 *buffer = malloc_tmp(PNP_NODES_MAX);
    if (!buffer) {
        warn_noalloc();
        return;
    }
    struct rebios_pnp_nodes call = {
        .buffer = (u32)buffer, .size = PNP_NODES_MAX
    };
    rebios_hypercall(REBIOS_HC_PNP_NODES, (u32)&call);
    u32 length = call.length, offset = 0, count = 0, largest = 0;
    while (length <= PNP_NODES_MAX && offset + PNP_NODE_HEADER <= length) {
        u16 size = *(u16*)&buffer[offset];
        if (size < PNP_NODE_HEADER || size > length - offset)
            break;
        if (size > largest)
            largest = size;
        offset += size;
        count++;
    }
    u8 *nodes = NULL;
    if (count && offset == length && count < PNP_NO_HANDLE) {
        nodes = malloc_fseg(length);
        if (!nodes)
            warn_noalloc();
    }
    if (nodes) {
        memcpy(nodes, buffer, length);
        PnpNodes = (u32)nodes - BUILD_BIOS_ADDR;
        PnpNodesLength = length;
        PnpNodeCount = count;
        PnpNodeLargest = largest;
        dprintf(1, "PnP BIOS: %d system device nodes\n", count);
    }
    free(buffer);
}

void
pnp_init(void)
{
    if (! CONFIG_PNPBIOS)
        return;

    dprintf(3, "init PNPBIOS table\n");

    PNPHEADER.real_ip = (u32)entry_pnp_real - BUILD_BIOS_ADDR;
    PNPHEADER.prot_ip = (u32)entry_pnp_prot - BUILD_BIOS_ADDR;
    PNPHEADER.checksum -= checksum(&PNPHEADER, sizeof(PNPHEADER));

    if (CONFIG_CSM && ReBIOSPresent)
        rebios_pnp_nodes();
}
