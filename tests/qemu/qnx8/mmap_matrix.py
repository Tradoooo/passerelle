#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the mmap matrix (mapprobe.c) on the served file and on the same bytes off
the share.

One run, one guest, two paths: /host/fs6-demo is served by our driver,
/dev/shmem/d is the same bytes copied *through* it, and /dev/shmem is served by
another resource manager. Whatever the control accepts and we refuse is the gap
FS-06 has to close; whatever the control refuses too is not our bug.
"""

import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
CAP = HERE / "capture_trace.py"
WORK = pathlib.Path("/tmp/pgw-matrix")

# The copy is made through the driver, so the control's bytes are the driver's
# output, not the host's file: a difference between the two columns can only come
# from serving them.
CMDS = [
    "cat /host/fs6-demo > /dev/shmem/d",
    "chmod 755 /dev/shmem/d",
    "mapprobe /host/fs6-demo /dev/shmem/d",
    "echo SENTINEL_MATRIX",
]


def main() -> int:
    subprocess.run(["rm", "-rf", str(WORK)], check=True)
    WORK.mkdir(parents=True)
    argv = [sys.executable, str(CAP), "--keep", "--workdir", str(WORK),
            "--timeout", "240", "--marker", "SENTINEL_MATRIX",
            "--out", str(WORK / "t.log")]
    for c in CMDS:
        argv += ["--cmd", c]
    rc = subprocess.run(argv, timeout=320, capture_output=True).returncode
    text = (WORK / "t.log").read_text(encoding="utf-8", errors="replace")
    print(f"capture rc={rc}")
    for ln in text.splitlines():
        ln = ln.replace("\r", "")
        if ln.startswith("mapprobe:") or "SENTINEL" in ln:
            print("  " + ln[:110])
    return rc


if __name__ == "__main__":
    sys.exit(main())
