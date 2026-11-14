#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Decode the raw `_io_connect` bytes the guest driver logged (FS-06).

The field offsets come from the SDP's own header, read here rather than from
memory: `<sys/iomsg.h>` lines 175..191. Counting bytes by hand is what made the
first reading of the exec open wrong, so the arithmetic lives in one place and
prints the fields beside the bytes it took them from.
"""

import re
import sys

# (name, offset, size, signed) — from struct _io_connect in <sys/iomsg.h>.
FIELDS = [
    ("type", 0, 2), ("subtype", 2, 2), ("file_type", 4, 4),
    ("reply_max", 8, 2), ("entry_max", 10, 2), ("reserved", 12, 4),
    ("handle", 16, 4), ("ioflag", 20, 4), ("mode", 24, 4),
    ("sflag", 28, 2), ("access", 30, 2), ("dirfd", 32, 2),
    ("path_len", 34, 2), ("eflag", 36, 1), ("extra_type", 37, 1),
    ("extra_len", 38, 2),
]
PATH_OFF = 40

SUBTYPES = {0: "COMBINE", 1: "COMBINE_CLOSE", 2: "OPEN", 3: "UNLINK",
            4: "RENAME", 5: "MKNOD", 6: "READLINK", 7: "LINK",
            8: "RSVD_UNBLOCK", 9: "MOUNT", 10: "MOUNT_OCB"}
TYPES = {0x0100: "_IO_CONNECT", 0x1c01: "?0x1c01"}

# The permission bits of the wire ioflag, and the QNX O_* flags above them.
IOFLAGS = [
    ("_IO_FLAG_RD", 0x1), ("_IO_FLAG_WR", 0x2), ("_IO_FLAG_EX", 0x4),
    ("O_APPEND", 0o10), ("O_DSYNC", 0o20), ("O_SYNC", 0o40),
    ("O_RSYNC", 0o100), ("O_NONBLOCK", 0o200), ("O_CREAT", 0o400),
    ("O_TRUNC", 0o1000), ("O_EXCL", 0o2000), ("O_NOCTTY", 0o4000),
    ("O_NOFOLLOW", 0o10000), ("O_CLOEXEC", 0o20000), ("O_REALIDS", 0o40000),
    ("O_LARGEFILE", 0o100000), ("O_ASYNC", 0o200000),
    ("O_NOSYMLINK", 0o400000), ("O_ANON", 0o1000000),
    ("O_PERMCHECK", 0o2000000), ("O_DIRECTORY", 0o4000000),
]


def u16(b, off):
    return int.from_bytes(b[off:off + 2], "little")


def u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def u8(b, off):
    return b[off]


def decode(blob: str) -> None:
    b = bytes(int(x, 16) for x in blob.split())
    print(f"bytes: {len(b)}")
    for name, off, size in FIELDS:
        # The width decides the reader: `eflag` and `extra_type` are single
        # bytes, and reading them as a word is how the first version of this
        # decoder reported an extra_type of 0x66 (the 'f' of the path).
        if size == 1:
            v = u8(b, off)
        elif size == 2:
            v = u16(b, off)
        else:
            v = u32(b, off)
        extra = ""
        if name == "type":
            extra = TYPES.get(v, "?")
        elif name == "subtype":
            extra = SUBTYPES.get(v, "?")
        elif name == "ioflag":
            names = [n for n, bit in IOFLAGS if v & bit]
            extra = f"mode_bas={v & 7} " + "|".join(names)
        print(f"  {name:<10} @{off:<3} = {v:#08x} ({v}) {extra}")
    plen = u16(b, 34)
    path = b[PATH_OFF:PATH_OFF + plen].split(b"\x00")[0]
    tail = b[PATH_OFF + plen:]
    print(f"  path       @{PATH_OFF} = {path!r} (path_len={plen})")
    print(f"  after path        = {tail.hex(' ')} ({len(tail)} bytes, "
          f"extra_len={u16(b, 38)})")


def main() -> int:
    pat = re.compile(r"exec open bytes: ([0-9a-f ]+)")
    found = 0
    for line in sys.stdin:
        m = pat.search(line)
        if m:
            found += 1
            print(f"=== exec open {found}")
            decode(m.group(1))
    if not found:
        print("aucun dump d'ouverture d'exec dans l'entrée")
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main())
