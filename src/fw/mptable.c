// MPTable generation (on emulators)
// DO NOT ADD NEW FEATURES HERE.  (See paravirt.c / biostables.c instead.)
//
// Copyright (C) 2008-2010  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2006 Fabrice Bellard
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "config.h" // CONFIG_*
#include "hw/pci.h" // pci_bdf_to_bus
#include "hw/pcidevice.h" // foreachpci
#include "hw/pci_regs.h" // PCI_INTERRUPT_PIN
#include "malloc.h" // free
#include "output.h" // dprintf
#include "romfile.h" // romfile_loadint
#include "std/mptable.h" // MPTABLE_SIGNATURE
#include "string.h" // memset
#include "util.h" // MaxCountCPUs
#include "x86.h" // cpuid

// The power of 2 at or above count.
static u32
mptable_pow2(u32 count)
{
    return count <= 1 ? 1 : 1 << (__fls(count - 1) + 1);
}

// The APIC IDs a core spans: the logical CPUs it can have, as a power of
// 2, or 0 where CPUID does not tell them from the cores of a package.
static u32
mptable_ids_per_core(u32 cpuid_signature, u32 package_ids)
{
    u32 maxleaf, vendor[3], eax, ebx, ecx, edx;
    __cpuid(0, &maxleaf, &vendor[0], &vendor[2], &vendor[1]);

    // The width of the x2APIC ID's logical processor field (Intel SDM
    // 325462-093 Vol. 3A, 11.6 and 11.9.3.1).
    if (maxleaf >= 0xb) {
        __cpuid_count(0xb, 0, &eax, &ebx, &ecx, &edx);
        if (ebx && ((ecx >> 8) & 0xff) == 1)
            return 1 << (eax & 0x1f);
    }

    if (memcmp(vendor, "GenuineIntel", sizeof(vendor)) == 0) {
        // The package's logical processor IDs over its core IDs (Intel
        // SDM 325462-093 Vol. 3A, 11.9.4); without leaf 4 there is one
        // core.
        if (maxleaf < 4)
            return 0;
        __cpuid_count(4, 0, &eax, &ebx, &ecx, &edx);
        u32 core_ids = ((eax >> 26) & 0x3f) + 1;
        if (!(eax & 0x1f) || package_ids < core_ids)
            return 0;
        return mptable_pow2(package_ids / core_ids);
    }

    if (memcmp(vendor, "AuthenticAMD", sizeof(vendor)) == 0
        || memcmp(vendor, "HygonGenuine", sizeof(vendor)) == 0) {
        u32 family = (cpuid_signature >> 8) & 0xf;
        if (family == 0xf)
            family += (cpuid_signature >> 20) & 0xff;
        // Before family 17h, every logical CPU is a core, as are those of
        // family 15h's compute units (BKDG for AMD Family 15h Models
        // 00h-0Fh 42301 rev. 3.14, CPUID Fn8000_001E_EBX).
        if (family < 0x17)
            return 1;
        // From family 17h, compute units are cores and their threads
        // logical CPUs (AMD64 Architecture Programmer's Manual Volume 3,
        // 24594 rev. 3.38, E.4.16).
        __cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
        if (eax < 0x8000001e)
            return 0;
        __cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
        if (!(ecx & (1 << 22)))
            return 0;
        __cpuid(0x8000001e, &eax, &ebx, &ecx, &edx);
        return mptable_pow2(((ebx >> 8) & 0xff) + 1);
    }
    return 0;
}

void
mptable_setup(void)
{
    if (! CONFIG_MPTABLE)
        return;

    dprintf(3, "init MPTable\n");

    // Config structure in temp area.
    struct mptable_config_s *config = malloc_tmp(32*1024);
    if (!config) {
        warn_noalloc();
        return;
    }
    memset(config, 0, sizeof(*config));
    config->signature = MPCONFIG_SIGNATURE;
    config->spec = 4;
    memcpy(config->oemid, BUILD_CPUNAME8, sizeof(config->oemid));
    memcpy(config->productid, "0.1         ", sizeof(config->productid));
    config->lapic = BUILD_APIC_ADDR;

    // Detect cpu info
    u32 cpuid_signature, ebx, ecx, cpuid_features;
    cpuid(1, &cpuid_signature, &ebx, &ecx, &cpuid_features);
    if (! cpuid_signature) {
        // Use default values.
        cpuid_signature = 0x600;
        cpuid_features = 0x201;
    }
    int pkgcpus = 1;
    if (cpuid_features & (1 << 28)) {
        /* Only populate the MPS tables with the first logical CPU in
           each core, or in each package where CPUID gives no cores */
        u32 package_ids = (ebx >> 16) & 0xff;
        pkgcpus = mptable_ids_per_core(cpuid_signature, package_ids);
        if (!pkgcpus)
            pkgcpus = mptable_pow2(package_ids);
    }
    u8 apic_version;
    u64 apic_base = rdmsr(0x1b);
    if (apic_base & (1 << 10))  /* x2APIC mode: use MSR */
        apic_version = rdmsr(0x803) & 0xff;
    else
        apic_version = readl((u8*)BUILD_APIC_ADDR + 0x30) & 0xff;

    // CPU definitions.
    struct mpt_cpu *cpus = (void*)&config[1], *cpu = cpus;
    int i;
    for (i = 0; i < MaxCountCPUs; i+=pkgcpus) {
        memset(cpu, 0, sizeof(*cpu));
        cpu->type = MPT_TYPE_CPU;
        cpu->apicid = i;
        cpu->apicver = apic_version;
        /* cpu flags: enabled, bootstrap cpu */
        cpu->cpuflag = (apic_id_is_present(i) ? 0x01 : 0x00) | ((i==0) ? 0x02 : 0x00);
        cpu->cpusignature = cpuid_signature;
        cpu->featureflag = cpuid_features;
        cpu++;
    }
    int entrycount = cpu - cpus;

    // PCI bus
    struct mpt_bus *buses = (void*)cpu, *bus = buses;
    if (!hlist_empty(&PCIDevices)) {
        memset(bus, 0, sizeof(*bus));
        bus->type = MPT_TYPE_BUS;
        bus->busid = 0;
        memcpy(bus->bustype, "PCI   ", sizeof(bus->bustype));
        bus++;
        entrycount++;
    }

    /* isa bus */
    int isabusid = bus - buses;
    memset(bus, 0, sizeof(*bus));
    bus->type = MPT_TYPE_BUS;
    bus->busid = isabusid;
    memcpy(bus->bustype, "ISA   ", sizeof(bus->bustype));
    bus++;
    entrycount++;

    /* ioapic */
    u8 ioapic_id = BUILD_IOAPIC_ID;
    struct mpt_ioapic *ioapic = (void*)bus;
    memset(ioapic, 0, sizeof(*ioapic));
    ioapic->type = MPT_TYPE_IOAPIC;
    ioapic->apicid = ioapic_id;
    ioapic->apicver = 0x11;
    ioapic->flags = 1; // enable
    ioapic->apicaddr = BUILD_IOAPIC_ADDR;
    entrycount++;

    /* irqs */
    struct mpt_intsrc *intsrcs = (void*)&ioapic[1], *intsrc = intsrcs;
    int dev = -1;
    unsigned short pinmask = 0;

    struct pci_device *pci;
    foreachpci(pci) {
        u16 bdf = pci->bdf;
        if (pci_bdf_to_bus(bdf) != 0)
            break;
        int pin = pci_config_readb(bdf, PCI_INTERRUPT_PIN);
        int irq = pci_config_readb(bdf, PCI_INTERRUPT_LINE);
        if (pin == 0)
            continue;
        if (dev != pci_bdf_to_busdev(bdf)) {
            dev = pci_bdf_to_busdev(bdf);
            pinmask = 0;
        }
        if (pinmask & (1 << pin)) /* pin was seen already */
            continue;
        pinmask |= (1 << pin);
        memset(intsrc, 0, sizeof(*intsrc));
        intsrc->type = MPT_TYPE_INTSRC;
        intsrc->irqtype = 0; /* INT */
        intsrc->irqflag = 1; /* active high */
        intsrc->srcbus = pci_bdf_to_bus(bdf); /* PCI bus */
        intsrc->srcbusirq = (pci_bdf_to_dev(bdf) << 2) | (pin - 1);
        intsrc->dstapic = ioapic_id;
        intsrc->dstirq = irq;
        intsrc++;
    }

    int irq0_override = romfile_loadint("etc/irq0-override", 0);
    for (i = 0; i < 16; i++) {
        memset(intsrc, 0, sizeof(*intsrc));
        if (BUILD_PCI_IRQS & (1 << i))
            continue;
        intsrc->type = MPT_TYPE_INTSRC;
        intsrc->irqtype = 0; /* INT */
        intsrc->irqflag = 0; /* conform to bus spec */
        intsrc->srcbus = isabusid; /* ISA bus */
        intsrc->srcbusirq = i;
        intsrc->dstapic = ioapic_id;
        intsrc->dstirq = i;
        if (irq0_override) {
            /* Destination 2 is covered by irq0->inti2 override (i ==
               0). Source IRQ 2 is unused */
            if (i == 0)
                intsrc->dstirq = 2;
            else if (i == 2)
                intsrc--;
        }
        intsrc++;
    }

    /* Local interrupt assignment */
    intsrc->type = MPT_TYPE_LOCAL_INT;
    intsrc->irqtype = 3; /* ExtINT */
    intsrc->irqflag = 0; /* PO, EL default */
    intsrc->srcbus = isabusid; /* ISA */
    intsrc->srcbusirq = 0;
    intsrc->dstapic = 0; /* BSP == APIC #0 */
    intsrc->dstirq = 0; /* LINTIN0 */
    intsrc++;

    intsrc->type = MPT_TYPE_LOCAL_INT;
    intsrc->irqtype = 1; /* NMI */
    intsrc->irqflag = 0; /* PO, EL default */
    intsrc->srcbus = isabusid; /* ISA */
    intsrc->srcbusirq = 0;
    intsrc->dstapic = 0xff; /* to all local APICs */
    intsrc->dstirq = 1; /* LINTIN1 */
    intsrc++;
    entrycount += intsrc - intsrcs;

    // Finalize config structure.
    int length = (void*)intsrc - (void*)config;
    config->entrycount = entrycount;
    config->length = length;
    config->checksum -= checksum(config, length);

    // floating pointer structure
    struct mptable_floating_s floating;
    memset(&floating, 0, sizeof(floating));
    floating.signature = MPTABLE_SIGNATURE;
    floating.physaddr = (u32)config;
    floating.length = 1;
    floating.spec_rev = 4;
    floating.checksum -= checksum(&floating, sizeof(floating));
    copy_mptable(&floating);
    free(config);
}
