// ATAPI removable-media identification and BIOS geometry.
// This file may be distributed under the terms of the GNU LGPLv3 license.
#ifndef __ATAPI_FLOPPY_H
#define __ATAPI_FLOPPY_H

#include "block.h"

static inline unsigned char
atapi_model_upper(unsigned char c)
{
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

static inline int
atapi_model_matches(const char *model, const char *wanted)
{
    if (!*wanted)
        return 0;
    for (; *model; model++) {
        const char *m = model, *w = wanted;
        while (*m && *w && atapi_model_upper(*m) == atapi_model_upper(*w)) {
            m++;
            w++;
        }
        if (!*w)
            return 1;
    }
    return 0;
}

// "LS-120"/"LS120" or "LS-240"/"LS240", not followed by another digit.
static inline int
atapi_is_ls_model(const char *model)
{
    for (; *model; model++) {
        const char *p = model;
        if (atapi_model_upper(*p++) != 'L' || atapi_model_upper(*p++) != 'S')
            continue;
        if (*p == '-')
            p++;
        if (!((p[0] == '1' && p[1] == '2') || (p[0] == '2' && p[1] == '4'))
            || p[2] != '0')
            continue;
        p += 3;
        if (*p < '0' || *p > '9')
            return 1;
    }
    return 0;
}

// Whether an ATAPI direct-access device is a SuperDisk-style floppy: an
// LS-120/LS-240 model, or "Floppy" in the model as the ARMD specification
// uses it (e.g. "MATSHITA LS-120 COSM 04 UHD Floppy"). ZIP media use an MBR
// and stay hard disks, even Iomega's "ZIP 100 ATAPI Floppy" variant.
static inline int
atapi_is_superdisk(const char *model)
{
    if (atapi_model_matches(model, "ZIP"))
        return 0;
    return atapi_is_ls_model(model) || atapi_model_matches(model, "FLOPPY");
}

// INT 13h drive type of an ATAPI removable media device (ARMD).
#define ATAPI_FLOPPY_TYPE_ARMD 0x10

// Runtime media states of an ATAPI floppy.
#define ATAPI_MEDIA_NONE        0 // No (or not yet identified) media.
#define ATAPI_MEDIA_READY       1 // Media with the geometry below.
#define ATAPI_MEDIA_UNSUPPORTED 2 // Media whose capacity has no geometry.

struct atapi_media_s {
    u8 state;           // ATAPI_MEDIA_*
    u8 changed;         // Report a media change on the next INT 13h AH=16h.
    u8 floppy_type;     // INT 13h AH=08h drive type for the current media.
    struct chs_s lchs;  // CHS geometry of the current media.
    u32 sectors;
};

// Geometry of the standard media an LS-120/LS-240 reads, from their capacity
// in 512-byte blocks. Returns 0 on success, -1 for an unknown capacity.
static inline int
atapi_floppy_geometry(u32 sectors, struct chs_s *chs, u8 *type)
{
    chs->head = 2;
    chs->cylinder = 80;
    chs->pad = 0;
    switch (sectors) {
    case 1440:  // 720 KB
        chs->sector = 9;
        *type = 3;
        break;
    case 2880:  // 1.44 MB
        chs->sector = 18;
        *type = 4;
        break;
    case 246528: // 120 MB SuperDisk
        chs->head = 8;
        chs->cylinder = 963;
        chs->sector = 32;
        *type = ATAPI_FLOPPY_TYPE_ARMD;
        break;
    default:
        return -1;
    }
    return 0;
}

// Geometry from the device's Flexible Disk mode page (SFF-8070i page 05h),
// for media the table above does not know. It must describe every block and
// fit INT 13h's CHS limits.
static inline int
atapi_floppy_page_geometry(u32 sectors, u16 heads, u16 spt, u16 cylinders,
                           struct chs_s *chs, u8 *type)
{
    if (!heads || heads > 255 || !spt || spt > 63 || !cylinders
        || cylinders > 1024
        || (u32)heads * spt * cylinders != sectors)
        return -1;
    chs->head = heads;
    chs->sector = spt;
    chs->cylinder = cylinders;
    chs->pad = 0;
    *type = ATAPI_FLOPPY_TYPE_ARMD;
    return 0;
}

// The geometry INT 13h reports without media: a 1.44-MB drive of the ARMD
// type, as the ARMD specification's AH=08h does.
static inline void
atapi_media_none(struct atapi_media_s *media)
{
    media->state = ATAPI_MEDIA_NONE;
    media->sectors = 0;
    atapi_floppy_geometry(2880, &media->lchs, &media->floppy_type);
    media->floppy_type = ATAPI_FLOPPY_TYPE_ARMD;
}

static inline int
atapi_floppy_setup(struct drive_s *drive)
{
    if (drive->blksize != DISK_SECTOR_SIZE || drive->sectors > 0xffffffff)
        return -1;
    if (atapi_floppy_geometry(drive->sectors, &drive->lchs,
                              &drive->floppy_type))
        return -1;
    drive->pchs = drive->lchs;
    return 0;
}

// Iomega's legacy type-06 ZIP partitions use 64 heads and 32 sectors/track.
// Retain that BIOS translation rather than the generic 16/63 LBA heuristic.
static inline int
atapi_zip_setup(struct drive_s *drive, const char *vendor,
                const char *product, const char *model)
{
    if (drive->blksize != DISK_SECTOR_SIZE || !drive->removable
        || (drive->lchs.head && drive->lchs.sector))
        return 0;
    if (!((atapi_model_matches(vendor, "IOMEGA")
           && atapi_model_matches(product, "ZIP"))
          || (atapi_model_matches(model, "IOMEGA")
              && atapi_model_matches(model, "ZIP"))))
        return 0;
    if (drive->sectors != 196608 && drive->sectors != 489532)
        return 0;
    drive->lchs.head = 64;
    drive->lchs.sector = 32;
    drive->lchs.cylinder = drive->sectors / (64 * 32);
    drive->pchs = drive->lchs;
    return 1;
}

#endif
