#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# image/qtrace.sh - capture the kernel trace of our driver's request events.
#
# QNX-09 asks for one event at submission and one at reception of every request,
# and QNX-03 for the priority that served the request to be in that trace. Both
# live in the kernel's own ring, so the only honest way to show them is to read
# the ring back:
#
#   1. `tracelogger` records kernel events to a file in shared memory (the image
#      boots the instrumented kernel, `procnto-smp-instr`);
#   2. a benchmark puts requests through the driver at two priorities;
#   3. `tracelogger` is stopped and the file is printed with `traceprinter`;
#   4. the user events are printed on the console between two markers, and the
#      host harness decodes them (the payload - operation and priority packed in
#      one word - is decoded there, where an arithmetic mistake is visible).
#
# The classes this measurement does not need are switched off with `-F`, so the
# file stays small and the console transfer stays short: what has to be readable
# is our own events, not the kernel's chatter.
#
# NO COMMAND SUBSTITUTION AND NO PIPE IN THIS FILE, ON PURPOSE.
#
# Measured here, on the QNX 8.0 guest: `$(...)` fails with "can't create pipe -
# try again" once the 9P driver is serving (the shell the start-up script leaves
# on the console cannot get a pipe from the kernel in that state), so every
# construct that would need one is replaced by a redirection, an exit status or a
# shell variable: `uname` prints instead of being captured, `grep -c` prints
# instead of being stored, `grep -q` decides instead of a comparison, and the
# background job's process id comes from `$!`, which needs no substitution. A
# background job is given explicit redirections so it does not ask for a pipe
# either. This is also why the script reports what it cannot do instead of
# assuming it can.
#
# Only the applets the image really ships are used: toybox gives grep, head, tail,
# cat, ls, mkdir, rm, sleep - there is no wc and no sed.
#
# Covered requirements: QNX-03, QNX-04, QNX-09, PERF-01.

KEV=/dev/shmem/passerelle.kev
TXT=/dev/shmem/passerelle.txt
USR=/dev/shmem/passerelle.user
TAG=PGW-TRACE
MAX_LINES="${PGW_TRACE_LINES:-100000}"

say() { echo "$TAG $*"; }

# A missing tool is a fact to report, not a silent failure.
for t in tracelogger traceprinter qbench grep head slay; do
    if ! command -v "$t" >/dev/null 2>&1; then
        say "FAIL missing $t in the image"
        exit 1
    fi
done

if ! cat /host/from-host.txt >/dev/null 2>&1; then
    say "FAIL /host is not mounted: run qshare-mount.sh first"
    exit 1
fi

# Which kernel the guest booted, so a reader knows the trace is not simulated:
# tracelogger only works against an instrumented procnto.
say "INFO kernel:"
uname -s -r -v

rm -f "$KEV" "$TXT" "$USR"

# Continuous mode, stopped by hand: the run is bounded by the benchmark, not by
# a buffer count. The filters switch off the classes this measurement does not
# need (1 kernel calls, 2 interrupts, 3 process, 4 thread, 6 communication,
# 7 system), which keeps the user events and drops the chatter. The explicit
# redirections are what lets the background job run without a pipe.
tracelogger -c -F1 -F2 -F3 -F4 -F6 -F7 -f "$KEV" >/dev/null 2>&1 &
TL=$!
sleep 2

if ! ls "$KEV" >/dev/null 2>&1; then
    say "FAIL tracelogger produced no $KEV"
    slay "$TL" >/dev/null 2>&1
    exit 1
fi
say "INFO recording, tracelogger pid:"
echo "$TL"

# Two runs at two priorities, so the same operation appears in the trace at both
# priorities (QNX-03) beside the bulk load that saturates the transport (QNX-04).
# PGW_TRACE_CMD replaces the benchmark, so the same recording can be pointed at
# any request the guest makes - an exec, for instance, whose shape the trace
# shows better than any reading of the code does.
if [ -n "$PGW_TRACE_CMD" ]; then
    say "INFO cmd: $PGW_TRACE_CMD"
    $PGW_TRACE_CMD
else
    say "INFO bench priority 63"
    qbench prio /host/big.bin /host/from-host.txt 63
    say "INFO bench priority 10"
    qbench prio /host/big.bin /host/from-host.txt 10
fi

slay "$TL" >/dev/null 2>&1
sleep 1
say "INFO tracelogger stopped"

traceprinter -v -f "$KEV" > "$TXT" 2>/dev/null
say "INFO trace lines:"
grep -c '' "$TXT"

# The project's user events are numbered from 0 (_NTO_TRACE_USERFIRST), so they
# are the events of the user class.
if ! grep -qi 'user' "$TXT"; then
    # Nothing matched: show what the trace does contain, so the reader can tell
    # whether the class is named differently or the driver never emitted.
    say "WARN no user-class line; the trace begins:"
    head -n 25 "$TXT"
    exit 1
fi

grep -i 'user' "$TXT" > "$USR"
say "INFO user events:"
grep -c '' "$USR"

say "BEGIN EVENTS"
head -n "$MAX_LINES" "$USR"
say "END EVENTS"
say "RESULT PASS"
exit 0
