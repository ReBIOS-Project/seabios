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

static inline int
atapi_is_superdisk(const char *model)
{
    // Do not classify all removable disks as floppies: ZIP media use an MBR.
    for (; *model; model++) {
        const char *p = model;
        if (atapi_model_upper(*p++) != 'L' || atapi_model_upper(*p++) != 'S')
            continue;
        if (*p == '-')
            p++;
        if (*p++ != '1' || *p++ != '2' || *p++ != '0')
            continue;
        if (*p < '0' || *p > '9')
            return 1;
    }
    return 0;
}

static inline int
atapi_floppy_setup(struct drive_s *drive)
{
    if (drive->blksize != DISK_SECTOR_SIZE)
        return -1;
    drive->lchs.head = 2;
    drive->lchs.cylinder = 80;
    switch (drive->sectors) {
    case 1440:
        drive->lchs.sector = 9;
        drive->floppy_type = 3;
        break;
    case 2880:
        drive->lchs.sector = 18;
        drive->floppy_type = 4;
        break;
    case 246528:
        drive->lchs.head = 8;
        drive->lchs.cylinder = 963;
        drive->lchs.sector = 32;
        drive->floppy_type = 0x10; // ATAPI removable media device.
        break;
    default:
        return -1;
    }
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
