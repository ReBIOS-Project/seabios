#!/usr/bin/env python3
# Script to keep a linker from merging the constants of ELF files
#
# Copyright (C) 2026  Mintsuki <mintsuki@protonmail.com>
#
# This file may be distributed under the terms of the GNU GPLv3 license.
import optparse, struct

SHF_ALLOC = 0x2
SHF_MERGE = 0x10
SHF_STRINGS = 0x20

def main():
    # Parse command-line arguments
    usage = "%prog <file>..."
    opts = optparse.OptionParser(usage)
    options, args = opts.parse_args()
    if len(args) < 1:
        opts.error("Incorrect number of arguments")
    for filename in args:
        # Read input
        f = open(filename, "rb")
        data = bytearray(f.read())
        f.close()
        # Update
        shoff = struct.unpack_from("<I", data, 32)[0]
        shentsize, shnum = struct.unpack_from("<HH", data, 46)
        for i in range(shnum):
            flagsoff = shoff + i * shentsize + 8
            flags = struct.unpack_from("<I", data, flagsoff)[0]
            if flags & SHF_ALLOC:
                flags &= ~(SHF_MERGE | SHF_STRINGS)
                struct.pack_into("<I", data, flagsoff, flags)
        # Write output
        f = open(filename, "wb")
        f.write(data)
        f.close()

if __name__ == '__main__':
    main()
