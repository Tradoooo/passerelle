#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare an exec that works with an exec that does not, under the kernel trace.

Everything else has been ruled out by measurement on this guest:

* the bytes: `pread` at 0, 64, 512, 1024, 2048 and 4096 returns the host's own
  bytes, and a copy made through the driver executes from /dev/shmem;
* the mount: `statvfs` reports RDONLY|NOSUID and no NOEXEC;
* the right to execute: `access(X_OK)` returns 0;
* the mapping, from a client: `mmap(PROT_READ|PROT_EXEC)` on an O_RDONLY
  descriptor is answered, and the ELF magic is read through it.

What is left is the one anomaly those measurements do not explain: the trace of an
exec from the share carries **no `_IO_MMAP` event at all**, while the trace of a
client asking for the same mapping does. Either the loader never asks (so it
rejected the file before mapping), or it asks and the request does not reach us.

This runs the same recording twice, on two programs that are the same shape and
differ only in where they live:

    /proc/boot/clockprobe   inside the image - it runs at boot, so it executes
    /host/fs6-demo          served by our driver - "no closing quote"

`clockprobe` is the control because nothing about it is special: it is a
cross-compiled ELF the guest already runs successfully every boot. The
difference in MMAP events between the two recordings is the answer, and it comes
from the kernel's own ring rather than from reading our code.
"""

import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
CAP = HERE / "capture_trace.py"

# The program to exec under trace, and the marker that says the run ended.
TARGET = sys.argv[1] if len(sys.argv) > 1 else "/proc/boot/clockprobe"
WORK = pathlib.Path("/tmp/pgw-exectwo-" + TARGET.strip("/").replace("/", "_"))

CMDS = [
    f"PGW_TRACE_CMD={TARGET} /proc/boot/qtrace.sh",
    "echo SENTINEL_TWO",
]


def main() -> int:
    subprocess.run(["rm", "-rf", str(WORK)], check=True)
    WORK.mkdir(parents=True)
    argv = [sys.executable, str(CAP), "--keep", "--workdir", str(WORK),
            "--timeout", "240", "--marker", "SENTINEL_TWO",
            "--out", str(WORK / "t.log")]
    for c in CMDS:
        argv += ["--cmd", c]
    rc = subprocess.run(argv, timeout=320, capture_output=True).returncode
    text = (WORK / "t.log").read_text(encoding="utf-8", errors="replace")
    print(f"target={TARGET} capture rc={rc}")
    print(f"  transcript: {WORK / 't.log'}")
    for ln in text.splitlines():
        ln = ln.replace("\r", "")
        if any(k in ln for k in ("running as uid", "no closing", "clockprobe:",
                                 "RESULT", "FAIL", "user events")):
            print("  " + ln[:110])
    return rc


if __name__ == "__main__":
    sys.exit(main())
