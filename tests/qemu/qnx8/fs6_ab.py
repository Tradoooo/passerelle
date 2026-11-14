#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the FS-06 A/B in the guest: does the share serve the right BYTES?

The guest prints "no closing quote" when it runs /host/fs6-demo, which is the
shell's complaint about a binary it was handed as a script. Two causes look the
same from outside:

  a) our read path corrupts the file, so procnto never sees the ELF magic;
  b) the bytes are right and what is missing is the exec-info reply.

Copy the file out of the share and run the copy: (a) fails the same way, (b)
runs. One experiment, two answers. The commands live here instead of a shell
command line because a `>` inside a quoted argument is easy to get wrong, and
getting it wrong four times is a reason to stop typing it inline.
"""

import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
CAP = HERE / "capture_trace.py"

CMDS = [
    "cat /host/fs6-demo > /dev/shmem/d",   # read it all through our driver
    "ls -l /dev/shmem/d",                   # the size the guest saw
    "chmod 755 /dev/shmem/d",
    "/dev/shmem/d",                         # run the COPY, off the share
    "echo BYTES_ABOVE",
    "/host/fs6-demo",                       # run it FROM the share, for contrast
    "echo SENTINEL_FS6AB",
]


def main() -> int:
    work = pathlib.Path("/tmp/pgw-fs6ab")
    subprocess.run(["rm", "-rf", str(work)], check=True)
    work.mkdir(parents=True)
    argv = [sys.executable, str(CAP), "--keep", "--workdir", str(work),
            "--timeout", "240", "--marker", "SENTINEL_FS6AB",
            "--out", str(work / "t.log")]
    for c in CMDS:
        argv += ["--cmd", c]
    rc = subprocess.run(argv, timeout=300, capture_output=True).returncode
    text = (work / "t.log").read_text(encoding="utf-8", errors="replace")
    tail = text.splitlines()[-14:]
    print(f"capture rc={rc}")
    for line in tail:
        print("  " + line.replace("\r", "")[:100])
    return rc


if __name__ == "__main__":
    sys.exit(main())
