/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/include/pgw/traceop.h - what a traced request was.
 *
 * The kernel trace of QNX-09 carries one user event per edge of a request, and
 * a user event has two payload words. Both drivers share one numbering of
 * operations and one rule for putting an operation and the priority that served
 * it into those two words, so a reader of a trace can say, for every request:
 * which operation, at which priority, paired by tag between submission and
 * reception.
 *
 * This header pulls no OS header on purpose: the mapping from a protocol
 * message to an operation is a table, and a table is checked on the PC bench
 * (drivers/tests/test_traceop.c) long before a guest can run it.
 *
 * Covered requirements: QNX-03 (the priority that served a request is in the
 * trace), QNX-09 (one event pair per request, labelled with its operation),
 * PERF-01 (the round trips are read from those events).
 */
#ifndef PGW_TRACEOP_H
#define PGW_TRACEOP_H

#include <stdbool.h>

/* Operations, numbered once for both drivers: the trace events are keyed by
 * them. The three last entries exist so that no request is ever dropped from
 * the trace or labelled with an operation it does not have: a protocol has
 * messages that are not resource-manager operations (QNX-09 says "each
 * request"). */
enum pgw_op {
    PGW_OP_OPEN = 0,
    PGW_OP_READ,
    PGW_OP_WRITE,
    PGW_OP_STAT,
    PGW_OP_READDIR,
    PGW_OP_LSEEK,
    PGW_OP_CHMOD,
    PGW_OP_CHOWN,
    PGW_OP_UTIME,
    PGW_OP_TRUNC,
    PGW_OP_SYNC,
    PGW_OP_DEVCTL,
    PGW_OP_UNLINK,
    PGW_OP_RENAME,
    PGW_OP_MKDIR,
    PGW_OP_SYMLINK,
    PGW_OP_LINK,
    PGW_OP_READLINK,
    PGW_OP_CLOSE,
    PGW_OP_MMAP,    /* the memory manager mapping a file's segments (FS-06) */
    PGW_OP_LOOKUP,  /* path resolution: 9P Twalk, FUSE LOOKUP */
    PGW_OP_SESSION, /* version/attach/flush: no file operation behind it */
    PGW_OP_OTHER,   /* a message this project's table does not know */
    PGW_OP__MAX
};

/* The submission event's payload is two words: the first carries the operation
 * and the priority that served the request (QNX-03), the second the tag. One
 * word holds both because an operation is at most PGW_OP__MAX and a QNX
 * priority is at most 255, so each fits in a byte. */
#define PGW_TRACE_PACK_OP_MASK 0xFF
#define PGW_TRACE_PACK_PRIO_SHIFT 8

/* Pack/unwrap the first payload word. A priority outside 0..255 is clamped: a
 * trace must never show a number the reader cannot interpret. */
int pgw_trace_pack(enum pgw_op op, int prio);
enum pgw_op pgw_trace_unpack_op(int packed);
int pgw_trace_unpack_prio(int packed);

/* The priority the calling thread runs at, in QNX terms (1 is the most urgent,
 * 255 the least). This is the value the submission event carries: the driver
 * re-applies the caller's priority to the thread that serves the request
 * (drivers/common/src/prio_qnx.c), so what this reads back is the caller's
 * priority, which is exactly the claim QNX-03 makes. */
int pgw_trace_prio_now(void);

#endif /* PGW_TRACEOP_H */
