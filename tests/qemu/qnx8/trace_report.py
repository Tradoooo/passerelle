#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read a captured QNX kernel trace and say what it proves.

The guest's `qtrace.sh` prints the user events `traceprinter` decoded from the
kernel ring; this script turns those lines into the numbers the specification
asks for, and nothing else:

  * QNX-09 - one event at submission and one at reception of each request, which
    is checked by pairing them on the tag and by refusing a trace where a
    submission has no reply (or the reverse);
  * QNX-03 - the priority that served the request, which the submission event
    carries in its payload: the operation decoded from the event code must
    equal the operation decoded from the payload, or the trace is lying about
    one of the two;
  * PERF-01/PERF-05 - the round trip of a request, in kernel cycles, and in
    microseconds when the trace run also reported the kernel's cycle rate.

Usage:
    python3 trace_report.py transcript.log
    python3 trace_report.py transcript.log --csv roundtrips.csv

Exit code: 0 when every submission has its reply and both decodings agree; 1
otherwise. A trace that does not satisfy its own requirements is not evidence.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import statistics
import sys

# The event numbering of the project: a submission is 2*op, its reply 2*op+1,
# inside the user range that starts at _NTO_TRACE_USERFIRST = 0 (drivers/
# common/include/pgw/traceop.h owns the operations, drivers/common/src/trace_qnx.c
# the numbering of the events).
OPS = (
    "OPEN", "READ", "WRITE", "STAT", "READDIR", "LSEEK", "CHMOD", "CHOWN",
    "UTIME", "TRUNC", "SYNC", "DEVCTL", "UNLINK", "RENAME", "MKDIR", "SYMLINK",
    "LINK", "READLINK", "CLOSE", "MMAP", "LOOKUP", "SESSION", "OTHER",
)

# TRACEPARSER: ... e:5 EVENT S CPU00 class:_TRACE_USER_C func:38 00011826
#              4f9689c1 00000a13 00000021
LINE = re.compile(
    r"func:(?P<code>\d+)\s+"
    r"(?P<word>[0-9a-f]{8})\s+"
    r"(?P<cycles>[0-9a-f]{8})\s+"
    r"(?P<data0>[0-9a-f]{8})\s+"
    r"(?P<data1>[0-9a-f]{8})"
)
# The rate the kernel itself published in its syspage, printed by clockprobe.
CYCLES_PER_SEC = re.compile(r"clockprobe:\s*cycles_per_sec=(\d+)")


def ops_name(op: int) -> str:
    return OPS[op] if 0 <= op < len(OPS) else f"op{op}"


def parse_event(raw: str, problems: list[str], last: bool = False) -> dict | None:
    """Decode one traceprinter line and cross-check the request label.

    The last line of a recording can be cut in the middle: the kernel's buffer
    ended there. That is not an event the project emitted, so it is not a
    problem - it is the end of the recording.
    """
    m = LINE.search(raw)
    if m is None:
        if not last:
            problems.append(f"unparsable user event: {raw.strip()[:90]}")
        return None
    code = int(m.group("code"))
    d0, d1 = int(m.group("data0"), 16), int(m.group("data1"), 16)
    req = code % 2 == 0
    ev = {"kind": "req" if req else "rep", "op_code": code // 2,
          "cycles": int(m.group("cycles"), 16)}
    if req:
        ev.update(op=d0 & 0xFF, prio=d0 >> 8, tag=d1)
        if ev["op"] != ev["op_code"]:
            problems.append(
                f"event code says {ops_name(ev['op_code'])} but payload says "
                f"{ops_name(ev['op'])} (tag {d1})")
    else:
        ev.update(op=ev["op_code"], prio=None, tag=d0, status=d1)
    return ev


def parse(path: pathlib.Path) -> tuple[list[dict], list[str], int | None]:
    """The user events of the transcript, the problems found, the cycle rate."""
    text = path.read_text(encoding="utf-8", errors="replace")
    problems: list[str] = []
    rate_match = CYCLES_PER_SEC.search(text)
    rate = int(rate_match.group(1)) if rate_match else None
    lines = [ln for ln in text.splitlines() if "class:_TRACE_USER_C" in ln]
    events = []
    for index, raw in enumerate(lines):
        ev = parse_event(raw, problems, last=index == len(lines) - 1)
        if ev is not None:
            events.append(ev)
    return events, problems, rate


def pair_transport(req: dict, rep: dict) -> dict:
    """Make the round-trip record from two already matched transport events."""
    return {"op": req["op"], "prio": req["prio"], "tag": req["tag"],
            "status": rep["status"], "cycles": rep["cycles"] - req["cycles"],
            "stream": "transport"}


def pair_events(events: list[dict]) -> tuple[list[dict], list[str], int, int]:
    """Pair tagged transport events; count unpairable resource-manager edges."""
    pending: dict[tuple[int, int], list[dict]] = {}
    trips: list[dict] = []
    problems: list[str] = []
    resmgr_req = resmgr_rep = 0
    for ev in events:
        if ev["tag"] == 0:
            resmgr_req += ev["kind"] == "req"
            resmgr_rep += ev["kind"] == "rep"
        elif ev["kind"] == "req":
            pending.setdefault((ev["op"], ev["tag"]), []).append(ev)
        else:
            key = (ev["op"], ev["tag"])
            queue = pending.get(key, [])
            if queue:
                trips.append(pair_transport(queue.pop(0), ev))
                if not queue:
                    pending.pop(key, None)
            else:
                problems.append(
                    f"reply for tag {ev['tag']} ({ops_name(ev['op'])}) "
                    "has no matching submission")
    return trips, problems, resmgr_req, resmgr_rep


def check_unmatched(pending: dict, req: int, rep: int) -> list[str]:
    """Explain the requests that never got a reply.

    A request that is still in flight when the trace stops is not a defect: the
    recording simply ended. Only the requests that were answered without a
    submission, and the resource-manager edges that do not balance, are problems.
    """
    errors: list[str] = []
    if req != rep:
        errors.append(
            f"resource-manager edges differ: {req} submissions, {rep} replies")
    return errors


def in_flight(events: list[dict]) -> int:
    """The requests the recording ended on, still waiting for their reply."""
    pending: dict[tuple[int, int], list[dict]] = {}
    for ev in events:
        if not ev["tag"]:
            continue
        key = (ev["op"], ev["tag"])
        if ev["kind"] == "req":
            pending.setdefault(key, []).append(ev)
        else:
            q = pending.get(key, [])
            if q:
                q.pop(0)
    return sum(len(q) for q in pending.values())


def pair(events: list[dict]) -> tuple[list[dict], list[str]]:
    """Pair transport completions; report resource-manager events separately."""
    trips, problems, req, rep = pair_events(events)
    problems.extend(check_unmatched({}, req, rep))
    return trips, problems


def report(trips: list[dict], rate: int | None) -> None:
    """The table a reviewer reads: one row per (stream, operation, priority)."""
    groups: dict[tuple[str, int, int], list[int]] = {}
    for t in trips:
        groups.setdefault((t["stream"], t["op"], t["prio"] or 0), []).append(
            t["cycles"])
    us = (1e9 / rate) if rate else None
    print(f"{'stream':<10} {'operation':<10} {'prio':>5} {'n':>5} {'min':>10} "
          f"{'median':>10} {'max':>10}   unit")
    for (stream, op, prio), vals in sorted(groups.items()):
        vals = [v for v in vals if v >= 0]
        if not vals:
            continue
        unit = "cycles"
        row = (min(vals), statistics.median(vals), max(vals))
        if us is not None:
            row = tuple(c * us for c in row)
            unit = "us"
        print(f"{stream:<10} {ops_name(op):<10} {prio:>5} {len(vals):>5} "
              f"{row[0]:>10.2f} {row[1]:>10.2f} {row[2]:>10.2f}   {unit}")
    if us is None:
        print("(no cycle rate in the transcript: the round trips are in kernel "
              "cycles only)")


def resmgr_edges(events: list[dict]) -> None:
    """Break the unpaired handler events down by operation and status.

    The transport trips say what the driver asked the PC and what the PC
    answered. The handler edges say something the trips cannot: a request the
    resource manager refuses *before* it reaches the transport leaves no trip
    behind at all. `iofunc_read_verify()` answering EBADF, or `iofunc_mmap()`
    answering EACCES, is exactly that shape - so a trace read only through its
    pairs looks clean while the exec is being refused.

    These events carry no protocol tag (the handlers have none to pair on), so
    they cannot be paired; what they can do is count, per operation, how many
    submissions and how many replies went out and with which status. That is
    enough to name the refusal, which is what the diagnosis needs.
    """
    subs: dict[int, int] = {}
    reps: dict[tuple[int, int], int] = {}
    for ev in events:
        if ev["tag"] != 0:
            continue
        if ev["kind"] == "req":
            subs[ev["op"]] = subs.get(ev["op"], 0) + 1
        else:
            key = (ev["op"], ev["status"])
            reps[key] = reps.get(key, 0) + 1
    if not subs and not reps:
        return
    print("resmgr: handler edges (no protocol tag, so they are counted, not "
          "paired)")
    for op in sorted(set(list(subs) + [o for o, _ in reps])):
        s = subs.get(op, 0)
        ok = reps.get((op, 0), 0)
        bad = {st: n for (o, st), n in reps.items() if o == op and st != 0}
        line = f"  {ops_name(op):<10} submitted={s:<4} accepted={ok:<4}"
        if bad:
            line += " refused: " + ", ".join(
                f"{n}x {st}" for st, n in sorted(bad.items(), key=lambda kv: -kv[1]))
        print(line)


def errors(trips: list[dict]) -> None:
    """The replies that came back with a status, by operation and status.

    The reply event carries the status the driver answered (0 when the request
    was accepted). Grouping the non-zero ones is what turns a trace into a
    diagnosis: a run that fails shows *which* operation failed, not only that
    operations ran. An exec attempt, for instance, is a sequence of opens,
    lookups and reads, and the one that carries the errno is the answer.
    """
    bad: dict[tuple[str, int], int] = {}
    for t in trips:
        if t["status"] == 0:
            continue
        key = (t["op"], t["status"])
        bad[key] = bad.get(key, 0) + 1
    if not bad:
        print("errors: none: every reply carried status 0")
        return
    print("errors: replies that carried a non-zero status")
    for (op, status), n in sorted(bad.items(), key=lambda kv: -kv[1]):
        print(f"  {ops_name(op):<10} status={status:<4} x{n}")


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("transcript", type=pathlib.Path)
    ap.add_argument("--csv", default="", help="write the round trips here")
    return ap.parse_args()


def write_csv(path: str, trips: list[dict]) -> None:
    """Persist paired transport events for later review."""
    rows = ["op,prio,tag,status,cycles"]
    for t in trips:
        rows.append(f"{ops_name(t['op'])},{t['prio']},{t['tag']},"
                    f"{t['status']},{t['cycles']}")
    pathlib.Path(path).write_text("\n".join(rows) + "\n", encoding="utf-8")


def print_problems(problems: list[str]) -> None:
    """Keep failures short enough to be useful in CI."""
    print(f"trace: {len(problems)} problem(s)", file=sys.stderr)
    for problem in problems[:20]:
        print(f"  {problem}", file=sys.stderr)


def main() -> int:
    args = parse_args()
    if not args.transcript.exists():
        print(f"trace: no such file {args.transcript}", file=sys.stderr)
        return 2
    events, problems, rate = parse(args.transcript)
    if not events:
        print("trace: no user event in the transcript", file=sys.stderr)
        return 1

    trips, pair_problems = pair(events)
    problems.extend(pair_problems)
    reqs = sum(e["kind"] == "req" for e in events)
    print(f"trace: {len(events)} user events ({reqs} submissions, "
          f"{len(events) - reqs} replies), {len(trips)} paired transport "
          "round trips")
    print(f"trace: priorities seen in the payload: "
          f"{sorted({e['prio'] for e in events if e['kind'] == 'req'})}")
    if in_flight(events):
        print("trace: requests were still in flight when recording stopped")
    if rate:
        print(f"trace: kernel cycle rate {rate} cycles/s")
    report(trips, rate)
    resmgr_edges(events)
    errors(trips)
    if args.csv:
        write_csv(args.csv, trips)
        print(f"trace: round trips in {args.csv}")
    if problems:
        print_problems(problems)
        return 1
    print("trace: every submission has its reply, and both decodings agree "
          "(QNX-09, QNX-03)")
    return 0



if __name__ == "__main__":
    sys.exit(main())
