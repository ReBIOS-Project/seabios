#!/usr/bin/env python3
"""Test production ATAPI command/bounce code with a host-side DMA boundary."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def function(path, signature):
    """Compile the actual functions, leaving hardware initialization outside."""
    source = (root / path).read_text()
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


prefix = r'''
#include "hw/ahci.h"
#include "hw/blockcmd.h"
#include "std/disk.h"
#undef offsetof
#define offsetof(t, m) __builtin_offsetof(t, m)
#define CONFIG_AHCI 1
#define MODESEGMENT 0
#define GET_FLATPTR(x) (x)
#define cpu_to_be16(x) __builtin_bswap16(x)
#define cpu_to_be32(x) __builtin_bswap32(x)
#define cpu_to_be64(x) __builtin_bswap64(x)
#define memset __builtin_memset
#define memcpy __builtin_memcpy
#define memset_fl __builtin_memset
#define memcpy_fl __builtin_memcpy
#define ATA_CMD_PACKET 0xa0
#define CDROM_CDB_SIZE 12
#define CHECK(x) do { if (!(x)) return (__LINE__ % 250) + 1; } while (0)
static u8 bounce[2048] __attribute__((aligned(2)));
static u8 bytes[1538] __attribute__((aligned(2)));
u8 *bounce_buf_fl = bounce;
static unsigned int calls, bad, fail_on;
static const u8 *expected;
int default_process_op(struct disk_op_s *op) { (void)op; return DISK_RET_EPARAM; }
static int ahci_command(struct ahci_port_s *port, int write, int atapi,
                        void *buffer, u32 length)
{
    struct cdb_rwdata_10 *cdb = (void *)port->cmd->atapi;
    u32 lba = __builtin_bswap32(cdb->lba);
    u16 count = __builtin_bswap16(cdb->count);
    bad |= !atapi || port->cmd->fis.command != ATA_CMD_PACKET;
    bad |= port->cmd->fis.feature != 1 || length != count * 512;
    bad |= cdb->command != (write ? CDB_CMD_WRITE_10 : CDB_CMD_READ_10);
    calls++;
    if (calls == fail_on)
        return -1;
    u8 *data = buffer;
    u32 i;
    for (i = 0; i < length; i++) {
        if (write)
            bad |= data[i] != expected[(lba - 100) * 512 + i];
        else
            data[i] = (u8)(lba + i / 512);
    }
    return 0;
}
'''

suffix = r'''
int main(void)
{
    struct ahci_cmd_s command = {0};
    struct ahci_port_s port = {.drive = {.blksize = 512}, .cmd = &command};
    struct disk_op_s op = {.drive_fl = &port.drive, .lba = 100, .count = 2};
    unsigned int i, odd;
    for (odd = 0; odd < 2; odd++) {
        op.buf_fl = bytes + odd;
        expected = bytes + odd;
        for (i = 0; i < 1024; i++)
            bytes[odd + i] = (u8)(i * 7 + i / 512);
        calls = bad = fail_on = 0;
        op.command = CMD_WRITE;
        CHECK(!ahci_atapi_process_op(&op));
        CHECK(!bad && calls == (odd ? 2 : 1));
        calls = bad = 0;
        op.command = CMD_READ;
        CHECK(!ahci_atapi_process_op(&op));
        CHECK(!bad && calls == (odd ? 2 : 1));
        for (i = 0; i < 1024; i++)
            CHECK(bytes[odd + i] == 100 + i / 512);
    }
    calls = bad = 0;
    fail_on = 2;
    op.buf_fl = bytes + 1;
    op.count = 3;
    CHECK(ahci_atapi_process_op(&op) == DISK_RET_EBADTRACK);
    CHECK(!bad && calls == 2);
    calls = 0;
    op.command = CMD_FORMAT;
    CHECK(ahci_atapi_process_op(&op) == DISK_RET_EPARAM);
    CHECK(!calls);
    port.drive.blksize = 2048;
    op.command = CMD_WRITE;
    CHECK(ahci_atapi_process_op(&op) == DISK_RET_EWRITEPROTECT);
    CHECK(!calls);
    op.command = CMD_FORMAT;
    CHECK(ahci_atapi_process_op(&op) == DISK_RET_EWRITEPROTECT);
    CHECK(!calls);
    port.drive.blksize = 512;
    u8 cdb[16] = {CDB_CMD_INQUIRY};
    op.command = CMD_SCSI;
    op.cdbcmd = cdb;
    op.blocksize = 36;
    op.count = 1;
    CHECK(ahci_atapi_process_op(&op) == DISK_RET_EBADTRACK);
    CHECK(!calls);
    return 0;
}
'''

code = prefix
code += function("src/hw/blockcmd.c", "int\nscsi_fill_cmd(")
code += function("src/hw/ahci.c", "static void sata_prep_atapi(")
code += function("src/hw/ahci.c", "static int\nahci_atapi_issue(")
code += function("src/hw/ahci.c", "int ahci_atapi_process_op(")
code += suffix
with tempfile.TemporaryDirectory(prefix="seabios-ahci-") as tmp:
    source = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    source.write_text(code)
    cc = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(cc + ["-Isrc", "-Wall", "-Wextra", "-Werror",
                        "-Wno-pointer-to-int-cast",
                        "-fsanitize=address,undefined", "-g", str(source),
                        "-o", str(binary)], cwd=root, check=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    subprocess.run([str(binary)], env=env, check=True)
print("AHCI ATAPI aligned/unaligned reads/writes, errors and CD protection passed")
