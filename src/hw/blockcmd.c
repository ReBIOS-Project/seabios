// Support for several common scsi like command data block requests
//
// Copyright (C) 2010  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2002  MandrakeSoft S.A.
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "block.h" // struct disk_op_s
#include "atapi-floppy.h" // atapi_floppy_setup
#include "blockcmd.h" // struct cdb_request_sense
#include "byteorder.h" // be32_to_cpu
#include "farptr.h" // GET_FLATPTR
#include "output.h" // dprintf
#include "std/disk.h" // DISK_RET_EPARAM
#include "string.h" // memset
#include "util.h" // timer_calc
#include "malloc.h"
#include "romfile.h" // romfile_loadfile
#include "stacks.h" // yield


/****************************************************************
 * Low level command requests
 ****************************************************************/

static int
cdb_get_inquiry(struct disk_op_s *op, struct cdbres_inquiry *data)
{
    struct cdb_request_sense cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = CDB_CMD_INQUIRY;
    cmd.length = sizeof(*data);
    op->command = CMD_SCSI;
    op->count = 1;
    op->buf_fl = data;
    op->cdbcmd = &cmd;
    op->blocksize = sizeof(*data);
    return process_op(op);
}

// Request SENSE
static int
cdb_get_sense(struct disk_op_s *op, struct cdbres_request_sense *data)
{
    struct cdb_request_sense cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = CDB_CMD_REQUEST_SENSE;
    cmd.length = sizeof(*data);
    op->command = CMD_SCSI;
    op->count = 1;
    op->buf_fl = data;
    op->cdbcmd = &cmd;
    op->blocksize = sizeof(*data);
    return process_op(op);
}

// Test unit ready
static int
cdb_test_unit_ready(struct disk_op_s *op)
{
    struct cdb_request_sense cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = CDB_CMD_TEST_UNIT_READY;
    op->command = CMD_SCSI;
    op->count = 0;
    op->buf_fl = NULL;
    op->cdbcmd = &cmd;
    op->blocksize = 0;
    return process_op(op);
}

static int
cdb_read_capacity16(struct disk_op_s *op, struct cdbres_read_capacity_16 *data)
{
    struct cdb_sai_read_capacity_16 cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = CDB_CMD_SERVICE_ACTION_IN;
    cmd.flags = CDB_CMD_SAI_READ_CAPACITY_16;
    cmd.len = cpu_to_be32(sizeof(struct cdbres_read_capacity_16));
    op->command = CMD_SCSI;
    op->count = 1;
    op->buf_fl = data;
    op->cdbcmd = &cmd;
    op->blocksize = sizeof(*data);
    return process_op(op);
}


// Request capacity
static int
cdb_read_capacity(struct disk_op_s *op, struct cdbres_read_capacity *data)
{
    struct cdb_read_capacity cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = CDB_CMD_READ_CAPACITY;
    op->command = CMD_SCSI;
    op->count = 1;
    op->buf_fl = data;
    op->cdbcmd = &cmd;
    op->blocksize = sizeof(*data);
    return process_op(op);
}

// Mode sense, geometry page.
static int
cdb_mode_sense_geom(struct disk_op_s *op, struct cdbres_mode_sense_geom *data)
{
    struct cdb_mode_sense cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = CDB_CMD_MODE_SENSE;
    cmd.flags = 8; /* DBD */
    cmd.page = MODE_PAGE_HD_GEOMETRY;
    cmd.count = cpu_to_be16(sizeof(*data));
    op->command = CMD_SCSI;
    op->count = 1;
    op->buf_fl = data;
    op->cdbcmd = &cmd;
    op->blocksize = sizeof(*data);
    return process_op(op);
}


/****************************************************************
 * ATAPI floppy (LS-120) media handling, in 16bit and 32bit mode
 ****************************************************************/

// A flat pointer to a variable on the current stack.
#define STACK_FLAT(p) ((void*)(MODESEGMENT                               \
                               ? (u32)MAKE_FLATPTR(GET_SEG(SS), (p))    \
                               : (u32)(p)))

// Issue a CDB whose data (if any) is read into a buffer on the stack.
static int
atapi_media_cdb(struct drive_s *drive_fl, void *cdb, void *buf, u16 len)
{
    struct disk_op_s op;
    memset(&op, 0, sizeof(op));
    op.drive_fl = drive_fl;
    op.command = CMD_SCSI;
    op.cdbcmd = STACK_FLAT(cdb);
    op.buf_fl = len ? STACK_FLAT(buf) : NULL;
    op.count = len ? 1 : 0;
    op.blocksize = len;
    return process_op(&op);
}

struct atapi_mode_flexible {
    u8 header[8];       // MODE SENSE (10) parameter header.
    u8 page_code;
    u8 page_length;
    u16 transfer_rate;
    u8 heads;
    u8 sectors;
    u16 bytes_per_sector;
    u16 cylinders;
    u8 unused[22];
} PACKED;

// Identify the inserted media: capacity, then CHS geometry.
static int
atapi_media_identify(struct drive_s *drive_fl, struct atapi_media_s *media_fl)
{
    struct cdb_read_capacity cap;
    memset(&cap, 0, sizeof(cap));
    cap.command = CDB_CMD_READ_CAPACITY;
    struct cdbres_read_capacity capdata __aligned(4);
    memset(&capdata, 0, sizeof(capdata));
    int ret = atapi_media_cdb(drive_fl, &cap, &capdata, sizeof(capdata));
    if (ret)
        return ret;
    u32 sectors = be32_to_cpu(capdata.sectors) + 1;
    if (be32_to_cpu(capdata.blksize) != DISK_SECTOR_SIZE || !sectors) {
        SET_FLATPTR(media_fl->state, ATAPI_MEDIA_UNSUPPORTED);
        return DISK_RET_EBADTRACK;
    }

    struct chs_s chs;
    u8 type;
    if (atapi_floppy_geometry(sectors, &chs, &type)) {
        struct cdb_mode_sense ms;
        memset(&ms, 0, sizeof(ms));
        ms.command = CDB_CMD_MODE_SENSE;
        ms.page = 0x05; // Flexible Disk page, current values.
        ms.count = cpu_to_be16(sizeof(struct atapi_mode_flexible));
        struct atapi_mode_flexible page __aligned(4);
        memset(&page, 0, sizeof(page));
        ret = atapi_media_cdb(drive_fl, &ms, &page, sizeof(page));
        if ((ret || (page.page_code & 0x3f) != 0x05
             || atapi_floppy_page_geometry(sectors, page.heads, page.sectors
                                           , be16_to_cpu(page.cylinders)
                                           , &chs, &type))
            && atapi_floppy_large_geometry(sectors, &chs, &type)) {
            dprintf(1, "ATAPI floppy: no geometry for %u sectors\n", sectors);
            SET_FLATPTR(media_fl->state, ATAPI_MEDIA_UNSUPPORTED);
            return DISK_RET_EBADTRACK;
        }
    }
    dprintf(3, "ATAPI floppy: media %u sectors, CHS %u/%u/%u\n", sectors
            , chs.cylinder, chs.head, chs.sector);
    SET_FLATPTR(media_fl->sectors, sectors);
    SET_FLATPTR(media_fl->lchs.cylinder, chs.cylinder);
    SET_FLATPTR(media_fl->lchs.head, chs.head);
    SET_FLATPTR(media_fl->lchs.sector, chs.sector);
    SET_FLATPTR(media_fl->floppy_type, type);
    SET_FLATPTR(media_fl->state, ATAPI_MEDIA_READY);
    return DISK_RET_SUCCESS;
}

static void
atapi_media_gone(struct atapi_media_s *media_fl)
{
    struct atapi_media_s empty;
    atapi_media_none(&empty);
    if (GET_FLATPTR(media_fl->state) != ATAPI_MEDIA_NONE)
        SET_FLATPTR(media_fl->changed, 1);
    SET_FLATPTR(media_fl->state, empty.state);
    SET_FLATPTR(media_fl->sectors, empty.sectors);
    SET_FLATPTR(media_fl->lchs.cylinder, empty.lchs.cylinder);
    SET_FLATPTR(media_fl->lchs.head, empty.lchs.head);
    SET_FLATPTR(media_fl->lchs.sector, empty.lchs.sector);
    SET_FLATPTR(media_fl->floppy_type, empty.floppy_type);
}

// Check the media of an ATAPI floppy before an INT 13h access, as the ARMD
// specification has the BIOS do: TEST UNIT READY, and on a media change (or
// newly inserted media) identify the media again. Returns DISK_RET_SUCCESS
// with media ready, DISK_RET_ETIMEOUT without media (DOS: "not ready"), or
// DISK_RET_EBADTRACK for media of unknown geometry.
int
atapi_media_check(struct drive_s *drive_fl)
{
    struct atapi_media_s *media_fl = GET_FLATPTR(drive_fl->media);
    if (!media_fl)
        return DISK_RET_SUCCESS;

    u32 end = timer_calc(5000);
    int attentions = 0;
    for (;;) {
        struct cdb_request_sense tur;
        memset(&tur, 0, sizeof(tur));
        tur.command = CDB_CMD_TEST_UNIT_READY;
        if (!atapi_media_cdb(drive_fl, &tur, NULL, 0))
            break;

        struct cdb_request_sense rs;
        memset(&rs, 0, sizeof(rs));
        rs.command = CDB_CMD_REQUEST_SENSE;
        rs.length = sizeof(struct cdbres_request_sense);
        struct cdbres_request_sense sense __aligned(4);
        memset(&sense, 0, sizeof(sense));
        int ret = atapi_media_cdb(drive_fl, &rs, &sense, sizeof(sense));
        u8 key = sense.flags & 0x0f;
        if (!ret && key == 0x06 && attentions++ < 4) {
            // UNIT ATTENTION: media changed (or a reset); it is cleared now.
            SET_FLATPTR(media_fl->changed, 1);
            SET_FLATPTR(media_fl->state, ATAPI_MEDIA_NONE);
            continue;
        }
        if (!ret && key == 0x02 && sense.asc == 0x3a) {
            // MEDIUM NOT PRESENT
            atapi_media_gone(media_fl);
            return DISK_RET_ETIMEOUT;
        }
        // Becoming ready, or a transient error: retry for a while.
        if (timer_check(end)) {
            dprintf(1, "ATAPI floppy: not ready (key %x asc %x ascq %x)\n"
                    , key, sense.asc, sense.ascq);
            atapi_media_gone(media_fl);
            return DISK_RET_ETIMEOUT;
        }
        yield();
    }

    u8 state = GET_FLATPTR(media_fl->state);
    if (state == ATAPI_MEDIA_READY)
        return DISK_RET_SUCCESS;
    if (state == ATAPI_MEDIA_UNSUPPORTED)
        return DISK_RET_EBADTRACK;
    SET_FLATPTR(media_fl->changed, 1);
    return atapi_media_identify(drive_fl, media_fl);
}


/****************************************************************
 * Main SCSI commands
 ****************************************************************/

// Create a scsi command request from a disk_op_s request
int
scsi_fill_cmd(struct disk_op_s *op, void *cdbcmd, int maxcdb)
{
    switch (op->command) {
    case CMD_READ:
    case CMD_WRITE: ;
        // Use the (10) variant only when the last touched LBA still fits.
        if (op->lba + op->count <= 0x100000000ULL) {
            struct cdb_rwdata_10 *cmd = cdbcmd;
            memset(cmd, 0, maxcdb);
            cmd->command = (op->command == CMD_READ ? CDB_CMD_READ_10
                            : CDB_CMD_WRITE_10);
            cmd->lba = cpu_to_be32(op->lba);
            cmd->count = cpu_to_be16(op->count);
        } else {
            struct cdb_rwdata_16 *cmd = cdbcmd;
            memset(cmd, 0, maxcdb);
            cmd->command = (op->command == CMD_READ ? CDB_CMD_READ_16
                            : CDB_CMD_WRITE_16);
            cmd->lba = cpu_to_be64(op->lba);
            cmd->count = cpu_to_be32(op->count);
        }
        return GET_FLATPTR(op->drive_fl->blksize);
    case CMD_SCSI:
        // op->cdbcmd is a flat pointer, as 32bit code passes it and
        // atapi_media_check() makes it in 16bit mode.
        if (MODESEGMENT)
            memcpy_fl(MAKE_FLATPTR(GET_SEG(SS), cdbcmd), op->cdbcmd, maxcdb);
        else
            memcpy(cdbcmd, op->cdbcmd, maxcdb);
        return op->blocksize;
    default:
        return -1;
    }
}

// Determine if the command is a request to pull data from the device
int
scsi_is_read(struct disk_op_s *op)
{
    return op->command == CMD_READ || (
        op->command == CMD_SCSI && op->blocksize);
}

// Check if a SCSI device is ready to receive commands
int
scsi_is_ready(struct disk_op_s *op)
{
    ASSERT32FLAT();
    dprintf(6, "scsi_is_ready (drive=%p)\n", op->drive_fl);

    /* Retry TEST UNIT READY for 5 seconds unless MEDIUM NOT PRESENT is
     * reported by the device 3 times.  If the device reports "IN PROGRESS",
     * 30 seconds is added. */
    int tries = 3;
    int in_progress = 0;
    u32 end = timer_calc(5000);
    for (;;) {
        if (timer_check(end)) {
            dprintf(1, "test unit ready failed\n");
            return -1;
        }

        int ret = cdb_test_unit_ready(op);
        if (!ret)
            // Success
            break;

        struct cdbres_request_sense sense;
        memset(&sense, 0, sizeof(sense));
        ret = cdb_get_sense(op, &sense);
        if (ret)
            // Error - retry.
            continue;

        // Require fixed-format sense data and ASC/ASCQ within the response.
        if ((sense.errcode & 0x7f) != 0x70 && (sense.errcode & 0x7f) != 0x71)
            continue;
        if (sense.additional < 6)
            continue;

        if (sense.asc == 0x3a) { /* MEDIUM NOT PRESENT */
            tries--;
            dprintf(1, "Device reports MEDIUM NOT PRESENT - %d tries left\n",
                tries);
            if (!tries)
                return -1;
        }

        if (sense.asc == 0x04 && sense.ascq == 0x01 && !in_progress) {
            /* IN PROGRESS OF BECOMING READY */
            dprintf(1, "Waiting for device to detect medium... ");
            /* Allow 30 seconds more */
            end = timer_calc(30000);
            in_progress = 1;
        }
    }
    return 0;
}

#define CDB_CMD_REPORT_LUNS  0xA0

struct cdb_report_luns {
    u8 command;
    u8 reserved_01[5];
    u32 length;
    u8 pad[6];
} PACKED;

struct scsi_lun {
    u16 lun[4];
};

struct cdbres_report_luns {
    u32 length;
    u32 reserved;
    struct scsi_lun luns[];
};

static u64 scsilun2u64(struct scsi_lun *scsi_lun)
{
    int i;
    u64 ret = 0;
    for (i = 0; i < ARRAY_SIZE(scsi_lun->lun); i++)
        ret |= (u64)be16_to_cpu(scsi_lun->lun[i]) << (16 * i);
    return ret;
}

// Issue REPORT LUNS on a temporary drive and iterate reported luns calling
// @add_lun for each
int scsi_rep_luns_scan(struct drive_s *tmp_drive, scsi_add_lun add_lun)
{
    int ret = -1;
    /* start with the smallest possible buffer, otherwise some devices in QEMU
     * may (incorrectly) error out on returning less data than fits in it */
    u32 maxluns = 1;
    u32 nluns, i;
    struct cdb_report_luns cdb = {
        .command = CDB_CMD_REPORT_LUNS,
    };
    struct disk_op_s op = {
        .drive_fl = tmp_drive,
        .command = CMD_SCSI,
        .count = 1,
        .cdbcmd = &cdb,
    };
    struct cdbres_report_luns *resp;

    ASSERT32FLAT();

    while (1) {
        op.blocksize = sizeof(struct cdbres_report_luns) +
            maxluns * sizeof(struct scsi_lun);
        op.buf_fl = malloc_tmp(op.blocksize);
        if (!op.buf_fl) {
            warn_noalloc();
            return -1;
        }

        cdb.length = cpu_to_be32(op.blocksize);
        if (process_op(&op) != DISK_RET_SUCCESS)
            goto out;

        resp = op.buf_fl;
        nluns = be32_to_cpu(resp->length) / sizeof(struct scsi_lun);
        if (nluns <= maxluns)
            break;

        free(op.buf_fl);
        maxluns = nluns;
    }

    for (i = 0, ret = 0; i < nluns; i++) {
        u64 lun = scsilun2u64(&resp->luns[i]);
        if (lun >> 32)
            continue;
        ret += !add_lun((u32)lun, tmp_drive);
    }
out:
    free(op.buf_fl);
    return ret;
}

// Iterate LUNs on the target and call @add_lun for each
int scsi_sequential_scan(struct drive_s *tmp_drive, u32 maxluns,
                         scsi_add_lun add_lun)
{
    int ret;
    u32 lun;

    for (lun = 0, ret = 0; lun < maxluns; lun++)
        ret += !add_lun(lun, tmp_drive);
    return ret;
}

// Whether the media's sector 0 has a partition table (a ZIP auto mode).
static int
atapi_zip_partitioned(struct disk_op_s *dop)
{
    u8 *buf = malloc_tmp(DISK_SECTOR_SIZE);
    if (!buf) {
        warn_noalloc();
        return 0;
    }
    dop->command = CMD_READ;
    dop->lba = 0;
    dop->count = 1;
    dop->buf_fl = buf;
    int has_mbr = !process_op(dop) && atapi_media_has_mbr(buf);
    free(buf);
    return has_mbr;
}

// Validate drive, find block size / sector count, and register drive.
int
scsi_drive_setup(struct drive_s *drive, const char *s, int prio)
{
    ASSERT32FLAT();
    struct disk_op_s dop;
    memset(&dop, 0, sizeof(dop));
    dop.drive_fl = drive;
    struct cdbres_inquiry data;
    memset(&data, 0, sizeof(data));
    int ret = cdb_get_inquiry(&dop, &data);
    if (ret)
        return ret;
    char vendor[sizeof(data.vendor)+1], product[sizeof(data.product)+1];
    char rev[sizeof(data.rev)+1];
    strtcpy(vendor, data.vendor, sizeof(vendor));
    nullTrailingSpace(vendor);
    strtcpy(product, data.product, sizeof(product));
    nullTrailingSpace(product);
    strtcpy(rev, data.rev, sizeof(rev));
    nullTrailingSpace(rev);
    int pdt = data.pdt & 0x1f;
    int removable = !!(data.removable & 0x80);
    dprintf(1, "%s vendor='%s' product='%s' rev='%s' type=%d removable=%d\n"
            , s, vendor, product, rev, pdt, removable);
    drive->removable = removable;

    if (pdt == SCSI_TYPE_CDROM) {
        drive->blksize = CDROM_SECTOR_SIZE;
        drive->sectors = (u64)-1;

        char *desc = znprintf(MAXDESCSIZE, "DVD/CD [%s Drive %s %s %s]"
                              , s, vendor, product, rev);
        boot_add_cd(drive, desc, prio);
        return 0;
    }

    if (pdt != SCSI_TYPE_DISK)
        return -1;

    // Some bridges (and emulators) have a generic ATA model but preserve
    // the drive's identity in INQUIRY.
    int is_atapi = drive->type == DTYPE_ATA_ATAPI
        || drive->type == DTYPE_AHCI_ATAPI;
    // Named by its port (floppy_slot_port), or else by its model.
    int floppy_slot = is_atapi ? drive->floppy_slot : 0;
    if (is_atapi) {
        static const char *const files[] = { "etc/floppy-a", "etc/floppy-b" };
        int i;
        for (i = 0; i < ARRAY_SIZE(files); i++) {
            char *wanted = romfile_loadfile(files[i], NULL);
            // Port names are matched by floppy_slot_port alone.
            int port = wanted && (wanted[0] == '/'
                                  || atapi_model_matches(wanted, "AHCI/")
                                  || (atapi_model_upper(wanted[0]) == 'A'
                                      && atapi_model_upper(wanted[1]) == 'T'
                                      && atapi_model_upper(wanted[2]) == 'A'
                                      && wanted[3] >= '0' && wanted[3] <= '9'));
            if (wanted && *wanted && !port && (atapi_model_matches(s, wanted)
                                      || atapi_model_matches(product, wanted)))
                floppy_slot |= 1 << i;
            free(wanted);
        }
    }
    // A ZIP is a floppy or a hard disk as etc/zip-mode says; in auto mode
    // (the default) by its media below.
    int zip = is_atapi && !floppy_slot && atapi_is_zip(vendor, product, s);
    int zip_mode = zip ? romfile_loadint("etc/zip-mode", ATAPI_ZIP_AUTO) : 0;
    if (zip)
        dprintf(1, "%s: ZIP mode %d\n", s, zip_mode);
    // A drive named by floppy_a or floppy_b is a floppy even if its model
    // does not say so.
    int atapi_floppy = is_atapi
        && (drive->floppy_type || atapi_is_superdisk(product) || floppy_slot
            || (zip && zip_mode != ATAPI_ZIP_HDD));
    if (atapi_floppy) {
        drive->floppy_type = ATAPI_FLOPPY_TYPE_ARMD;
        drive->floppy_slot = floppy_slot;
        drive->media = malloc_low(sizeof(*drive->media));
        if (!drive->media) {
            warn_noalloc();
            return -1;
        }
        memset(drive->media, 0, sizeof(*drive->media));
        atapi_media_none(drive->media);
    }

    ret = scsi_is_ready(&dop);
    if (ret && atapi_floppy) {
        // Like the floppy drive it stands in for, the drive is there without
        // media; INT 13h finds media inserted later (ARMD specification).
        dprintf(1, "%s: no media, registered as an empty floppy\n", s);
        drive->blksize = DISK_SECTOR_SIZE;
        drive->sectors = 2880;
        atapi_floppy_setup(drive);
        drive->floppy_type = ATAPI_FLOPPY_TYPE_ARMD;
        char *desc = znprintf(MAXDESCSIZE, "%s Drive %s %s %s"
                              , s, vendor, product, rev);
        boot_add_floppy(drive, desc, prio);
        return 0;
    }
    if (ret) {
        dprintf(1, "scsi_is_ready returned %d\n", ret);
        return ret;
    }

    struct cdbres_read_capacity capdata;
    ret = cdb_read_capacity(&dop, &capdata);
    if (ret)
        return ret;

    if (be32_to_cpu(capdata.sectors) == 0xFFFFFFFFUL) {
        dprintf(3, "%s: >2TB Detected trying READCAP(16)\n", s);
        struct cdbres_read_capacity_16 capdata16;
        ret = cdb_read_capacity16(&dop, &capdata16);
        if (ret)
            return ret;
        drive->blksize = be32_to_cpu(capdata16.blksize);
        drive->sectors = be64_to_cpu(capdata16.sectors) + 1;
    } else {
        drive->blksize = be32_to_cpu(capdata.blksize);
        drive->sectors = (u64)be32_to_cpu(capdata.sectors) + 1;
    }

    if (drive->blksize != DISK_SECTOR_SIZE) {
        dprintf(1, "%s: unsupported block size %d\n", s, drive->blksize);
        return -1;
    }
    dprintf(1, "%s blksize=%d sectors=0x%llx\n", s, drive->blksize, drive->sectors);
    // We do not recover from USB stalls, so try to be safe and avoid
    // sending the command if the (obsolete, but still provided by QEMU)
    // fixed disk geometry page may not be supported.
    //
    // We could also send the command only to small disks (e.g. <504MiB)
    // but some old USB keys only support a very small subset of SCSI which
    // does not even include the MODE SENSE command!
    //
    if (CONFIG_QEMU_HARDWARE && memcmp(vendor, "QEMU", 5) == 0) {
        struct cdbres_mode_sense_geom geomdata;
        ret = cdb_mode_sense_geom(&dop, &geomdata);
        if (ret == 0) {
            u32 cylinders;
            cylinders = geomdata.cyl[0] << 16;
            cylinders |= geomdata.cyl[1] << 8;
            cylinders |= geomdata.cyl[2];
            if (cylinders && geomdata.heads &&
                drive->sectors <= 0xFFFFFFFFULL &&
                ((u32)drive->sectors % (geomdata.heads * cylinders) == 0)) {
                drive->pchs.cylinder = cylinders;
                drive->pchs.head = geomdata.heads;
                drive->pchs.sector = (u32)drive->sectors / (geomdata.heads * cylinders);
            }
        }
    }

    if (atapi_floppy && zip && zip_mode == ATAPI_ZIP_AUTO
        && atapi_zip_partitioned(&dop)) {
        dprintf(1, "%s: partitioned ZIP media, mapped as a hard disk\n", s);
        free(drive->media);
        drive->media = NULL;
        drive->floppy_type = 0;
        atapi_floppy = 0;
    }

    char *desc = znprintf(MAXDESCSIZE, "%s Drive %s %s %s"
                          , s, vendor, product, rev);
    if (atapi_floppy) {
        // Media of unknown capacity is looked at again at its first access
        // (the Flexible Disk page); the drive itself stays registered.
        drive->blksize = DISK_SECTOR_SIZE;
        if (atapi_floppy_setup(drive)) {
            dprintf(1, "%s: media of unknown geometry\n", s);
            drive->sectors = 2880;
            atapi_floppy_setup(drive);
        } else {
            struct atapi_media_s *media = drive->media;
            media->state = ATAPI_MEDIA_READY;
            media->sectors = drive->sectors;
            media->lchs = drive->lchs;
            media->floppy_type = drive->floppy_type;
        }
        boot_add_floppy(drive, desc, prio);
    } else {
        if (drive->type == DTYPE_ATA_ATAPI || drive->type == DTYPE_AHCI_ATAPI)
            atapi_zip_setup(drive, vendor, product, s);
        boot_add_hd(drive, desc, prio);
    }
    return 0;
}

// IDENTIFY PACKET DEVICE's model survives bridges which change INQUIRY.
int
atapi_drive_setup(struct drive_s *drive, const char *model, int prio)
{
    drive->floppy_type = atapi_is_superdisk(model) ? ATAPI_FLOPPY_TYPE_ARMD : 0;
    return scsi_drive_setup(drive, model, prio);
}
