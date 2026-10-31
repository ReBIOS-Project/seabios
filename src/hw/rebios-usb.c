// Disks on USB controllers that the ReBIOS hypervisor drives
//
// Copyright (C) 2026  Mintsuki
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "block.h" // struct drive_s
#include "blockcmd.h" // CDB_CMD_TEST_UNIT_READY
#include "config.h" // CONFIG_CSM
#include "fw/rebios.h" // rebios_hypercall
#include "malloc.h" // malloc_fseg
#include "output.h" // dprintf
#include "rebios-usb.h" // rebios_usb_setup
#include "stacks.h" // run_thread
#include "std/disk.h" // DISK_RET_
#include "string.h" // memset
#include "util.h" // boot_add_hd

#define REBIOS_USB_MAX_DISKS 16
// A mass storage unit may take this long to come up after a reset.
#define REBIOS_USB_READY_TIMEOUT 16000
#define REBIOS_USB_TRANSFER_TIMEOUT 30000

struct rebios_usb_drive_s {
    struct drive_s drive;
    u32 disk;
};

static u32
rebios_usb_call(u32 function, u32 disk, u64 lba, u32 count, void *buffer)
{
    struct rebios_usb_disk call;
    memset(&call, 0, sizeof(call));
    call.function = function;
    call.disk = disk;
    call.lba = lba;
    call.count = count;
    call.buffer = (u32)buffer;
    rebios_hypercall(REBIOS_HC_USB_DISK, (u32)&call);
    return call.status;
}

int
rebios_usb_process_op(struct disk_op_s *op)
{
    if (!CONFIG_CSM)
        return DISK_RET_EPARAM;

    u32 function;
    switch (op->command) {
    case CMD_READ:
        function = REBIOS_USB_DISK_READ;
        break;
    case CMD_WRITE:
        function = REBIOS_USB_DISK_WRITE;
        break;
    case CMD_SCSI:
        // The hypervisor looks after the unit, and a read tells of no medium.
        if (*(u8*)op->cdbcmd == CDB_CMD_TEST_UNIT_READY)
            return DISK_RET_SUCCESS;
        return DISK_RET_EPARAM;
    default:
        return default_process_op(op);
    }

    struct rebios_usb_drive_s *rdrive = container_of(
        op->drive_fl, struct rebios_usb_drive_s, drive);
    u32 status = rebios_usb_call(function, rdrive->disk, op->lba, op->count
                                 , op->buf_fl);
    u32 end = timer_calc(REBIOS_USB_TRANSFER_TIMEOUT);
    while (status == REBIOS_USB_DISK_STATUS_PENDING) {
        if (timer_check(end)) {
            warn_timeout();
            return DISK_RET_ETIMEOUT;
        }
        yield();
        status = rebios_usb_call(REBIOS_USB_DISK_POLL, rdrive->disk, 0, 0, NULL);
    }
    switch (status) {
    case REBIOS_USB_DISK_STATUS_OK:
        return DISK_RET_SUCCESS;
    case REBIOS_USB_DISK_STATUS_NO_MEDIUM:
        return DISK_RET_ENOTREADY;
    case REBIOS_USB_DISK_STATUS_WRITE_PROTECTED:
        return DISK_RET_EWRITEPROTECT;
    default:
        return DISK_RET_EBADTRACK;
    }
}

static void
rebios_usb_add_disk(u32 disk, struct rebios_usb_disk_info *info)
{
    int cd = !!(info->flags & REBIOS_USB_DISK_CD);
    if (!cd && (info->blocks == 0 || info->block_size != DISK_SECTOR_SIZE)) {
        dprintf(1, "ReBIOS USB disk %d: %d blocks of %d bytes, skipped\n"
                , disk, (u32)info->blocks, info->block_size);
        return;
    }
    struct rebios_usb_drive_s *rdrive = malloc_fseg(sizeof(*rdrive));
    if (!rdrive) {
        warn_noalloc();
        return;
    }
    memset(rdrive, 0, sizeof(*rdrive));
    rdrive->disk = disk;
    rdrive->drive.type = DTYPE_REBIOS_USB;
    rdrive->drive.cntl_id = disk;
    rdrive->drive.removable = !!(info->flags & REBIOS_USB_DISK_REMOVABLE);
    char *desc = znprintf(MAXDESCSIZE, "%s", info->description);
    int prio = bootprio_find_rebios_usb(info->bdf, info->path);
    if (cd) {
        rdrive->drive.blksize = CDROM_SECTOR_SIZE;
        rdrive->drive.sectors = (u64)-1;
        boot_add_cd(&rdrive->drive, desc, prio);
    } else {
        rdrive->drive.blksize = info->block_size;
        rdrive->drive.sectors = info->blocks;
        boot_add_hd(&rdrive->drive, desc, prio);
    }
}

static void
rebios_usb_scan(void *data)
{
    u32 end = timer_calc(REBIOS_USB_READY_TIMEOUT);
    while (!rebios_usb_call(REBIOS_USB_DISK_READY, 0, 0, 0, NULL)) {
        if (timer_check(end)) {
            warn_timeout();
            break;
        }
        msleep(10);
    }
    u32 disk;
    for (disk = 0; disk < REBIOS_USB_MAX_DISKS; disk++) {
        struct rebios_usb_disk_info info;
        memset(&info, 0, sizeof(info));
        u32 status = rebios_usb_call(REBIOS_USB_DISK_INFO, disk, 0, 0, &info);
        if (status == REBIOS_USB_DISK_STATUS_OK)
            rebios_usb_add_disk(disk, &info);
    }
}

void
rebios_usb_setup(void)
{
    if (!CONFIG_CSM || !ReBIOSPresent)
        return;
    run_thread(rebios_usb_scan, NULL);
}
