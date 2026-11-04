// Host tests of the geometry and model matching used by the ATAPI drivers.
#include "../src/hw/atapi-floppy.h"

#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)

int main(void)
{
    CHECK(atapi_is_superdisk("MATSHITA LS-120 VER5 00"));
    CHECK(atapi_is_superdisk("LS120 SLIM"));
    CHECK(atapi_is_superdisk("MATSHITA ls-120 COSM 04"));
    CHECK(!atapi_is_superdisk("IOMEGA ZIP 100 ATAPI"));
    CHECK(!atapi_is_superdisk("LS-1200"));
    CHECK(!atapi_is_superdisk(""));
    CHECK(!atapi_is_superdisk("L"));
    CHECK(!atapi_is_superdisk("LS"));
    CHECK(!atapi_is_superdisk("LS-"));
    CHECK(!atapi_is_superdisk("LS-1"));
    CHECK(!atapi_is_superdisk("LS-12"));
    CHECK(atapi_is_superdisk("LS-240 COSM 01"));
    CHECK(atapi_is_superdisk("LS240"));
    CHECK(!atapi_is_superdisk("LS-2400"));
    CHECK(atapi_is_superdisk("TEAC UHD Floppy"));
    CHECK(!atapi_is_superdisk("IOMEGA ZIP 100 ATAPI Floppy"));

    struct chs_s chs;
    u8 type;
    CHECK(!atapi_floppy_geometry(2880, &chs, &type));
    CHECK(chs.cylinder == 80 && chs.head == 2 && chs.sector == 18);
    CHECK(type == 4);
    CHECK(!atapi_floppy_geometry(246528, &chs, &type));
    CHECK(type == ATAPI_FLOPPY_TYPE_ARMD);
    CHECK(atapi_floppy_geometry(196608, &chs, &type));
    // An LS-240 page: 262 cylinders, 32 heads, 56 sectors (469504 blocks).
    CHECK(!atapi_floppy_page_geometry(469504, 32, 56, 262, &chs, &type));
    CHECK(chs.cylinder == 262 && chs.head == 32 && chs.sector == 56);
    CHECK(type == ATAPI_FLOPPY_TYPE_ARMD);
    CHECK(atapi_floppy_page_geometry(469505, 32, 56, 262, &chs, &type));
    CHECK(atapi_floppy_page_geometry(64 * 2 * 1100, 2, 64, 1100, &chs, &type));
    CHECK(atapi_floppy_page_geometry(0, 0, 0, 0, &chs, &type));
    struct atapi_media_s media = { .state = ATAPI_MEDIA_READY };
    atapi_media_none(&media);
    CHECK(media.state == ATAPI_MEDIA_NONE && media.sectors == 0);
    CHECK(media.lchs.cylinder == 80 && media.lchs.head == 2);
    CHECK(media.lchs.sector == 18);
    CHECK(media.floppy_type == ATAPI_FLOPPY_TYPE_ARMD);
    CHECK(atapi_model_matches("ATAPI [AHCI/1: MATSHITA LS-120]", "ls-120"));
    CHECK(!atapi_model_matches("IOMEGA ZIP 100", "LS-120"));
    CHECK(!atapi_model_matches("LS-120", "LS-120 VER5"));
    CHECK(!atapi_model_matches("LS-120", ""));

    struct drive_s drive = { .blksize = 512 };
    drive.sectors = 1440;
    CHECK(!atapi_floppy_setup(&drive));
    CHECK(drive.floppy_type == 3);
    CHECK(drive.lchs.head == 2 && drive.lchs.cylinder == 80);
    CHECK(drive.lchs.sector == 9);
    drive.sectors = 2880;
    CHECK(!atapi_floppy_setup(&drive));
    CHECK(drive.floppy_type == 4 && drive.lchs.sector == 18);
    drive.sectors = 246528;
    CHECK(!atapi_floppy_setup(&drive));
    CHECK(drive.floppy_type == 0x10);
    CHECK(drive.lchs.head == 8 && drive.lchs.cylinder == 963);
    CHECK(drive.lchs.sector == 32);
    CHECK((u64)drive.lchs.head * drive.lchs.cylinder * drive.lchs.sector
          == drive.sectors);
    CHECK(drive.pchs.head == drive.lchs.head);
    CHECK(drive.pchs.cylinder == drive.lchs.cylinder);
    CHECK(drive.pchs.sector == drive.lchs.sector);
    drive.sectors = 0;
    CHECK(atapi_floppy_setup(&drive));
    drive.sectors = 196608; // ZIP capacity must not become a SuperDisk floppy.
    CHECK(atapi_floppy_setup(&drive));
    drive.sectors = 246528;
    drive.blksize = 2048;
    CHECK(atapi_floppy_setup(&drive));
    drive = (struct drive_s){ .blksize = 512, .removable = 1,
                              .sectors = 196608 };
    CHECK(atapi_zip_setup(&drive, "IOMEGA", "ZIP 100", "bridge"));
    CHECK(drive.lchs.head == 64 && drive.lchs.sector == 32);
    CHECK(drive.lchs.cylinder == 96);
    drive.lchs = (struct chs_s){0};
    drive.sectors = 489532;
    CHECK(atapi_zip_setup(&drive, "bridge", "device", "IOMEGA ZIP 250 ATAPI"));
    CHECK(drive.lchs.cylinder == 239);
    drive.lchs = (struct chs_s){ .head = 16, .sector = 63, .cylinder = 485 };
    CHECK(!atapi_zip_setup(&drive, "IOMEGA", "ZIP 250", "IOMEGA ZIP"));
    CHECK(drive.lchs.head == 16 && drive.lchs.sector == 63);
    drive.lchs = (struct chs_s){0};
    CHECK(!atapi_zip_setup(&drive, "OTHER", "disk", "disk"));
    drive.sectors = 1000000;
    CHECK(!atapi_zip_setup(&drive, "IOMEGA", "ZIP 250", "IOMEGA ZIP"));
    drive.sectors = 196608;
    drive.removable = 0;
    CHECK(!atapi_zip_setup(&drive, "IOMEGA", "ZIP 100", "IOMEGA ZIP"));
    return 0;
}
