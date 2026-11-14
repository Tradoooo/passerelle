/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/resmgr_io_qnx.c - read, write, stat, lseek, sync, close
 * and devctl handlers, mapped onto the protocol-neutral rmfs layer.
 *
 * The mapping is identical for 9P and virtiofs, so it is written once. Each
 * handler runs at the caller's priority (QNX-03) and gives up between chunks
 * when the client asks to be unblocked, so a slow or interrupted client never
 * pins a thread (QNX-05). Data never goes through an intermediate allocation:
 * a read fills a thread-local buffer that is written straight into the client's
 * reply, a write reads straight out of the client's message.
 *
 * Covered requirements: FS-01, FS-04, FS-10, FS-11, FS-12, QNX-03, QNX-05,
 * QNX-08, ROB-01, ROB-05.
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _QNX_SOURCE 1 /* SETIOV() and other QNX extensions used with _RESMGR_PTR */
#include "pgw/drvsrv.h"

#include "pgw/log.h"

#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sys/dcmd_blk.h>
#include <sys/stat.h>

/* One buffer per serving thread: a request never allocates, and the pool size
 * (bounded by `requests=`) bounds what this costs. */
static _Thread_local uint8_t g_io_buf[PGW_IO_CHUNK];

/* --- read ----------------------------------------------------------------- */

/* Offset the client asked for: an explicit pread() offset, or the OCB's. */
static bool read_offset(const io_read_t *msg, const pgw_ocb *ocb, uint32_t xtype,
                        uint64_t *off)
{
    if (xtype == _IO_XTYPE_OFFSET) {
        const struct _xtype_offset *xo =
            (const struct _xtype_offset *)(const void *)(&msg->i + 1);
        if (xo->offset < 0)
            return false;
        *off = (uint64_t)xo->offset;
        return true;
    }
    if (xtype == _IO_XTYPE_NONE) {
        if (ocb->ocb.offset < 0)
            return false;
        *off = (uint64_t)ocb->ocb.offset;
        return true;
    }
    return false;
}

int pgw_io_read_file(resmgr_context_t *ctp, io_read_t *msg, pgw_ocb *ocb,
                     uint32_t xtype)
{
    pgw_node *node = ocb->node;
    uint64_t off = 0;
    if (!read_offset(msg, ocb, xtype, &off))
        return ENOSYS;
    if (node == NULL || node->fd == NULL)
        return EBADF;

    uint64_t want = _IO_READ_GET_NBYTES(msg);
    if (want > PGW_IO_CHUNK)
        want = PGW_IO_CHUNK;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;

    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_READ, 0);

    size_t done = 0;
    int status = EOK;
    while (done < (size_t)want) {
        if (pgw_client_unblocking(ctp)) {
            status = done == 0 ? EINTR : EOK; /* report what was read */
            break;
        }
        size_t left = (size_t)want - done;
        size_t n = left < PGW_IO_CHUNK ? left : PGW_IO_CHUNK;
        size_t got = 0;
        pgw_err e = pgw_rmfs_read(srv->fs, node->fd, off + done, g_io_buf, n, &got);
        if (e != PGW_OK) {
            status = done == 0 ? pgw_qnx_status(e) : EOK;
            break;
        }
        if (got > 0 && resmgr_msgwrite(ctp, g_io_buf, got, done) == -1) {
            status = errno;
            break;
        }
        done += got;
        if (got < n)
            break; /* end of file */
    }
    pgw_prio_leave(&prio);
    pgw_trace_rep(PGW_OP_READ, 0, status);
    if (status != EOK)
        return status;
    if (xtype == _IO_XTYPE_NONE)
        ocb->ocb.offset += (off_t)done;
    _IO_SET_READ_NBYTES(ctp, (long)done);
    return _RESMGR_NPARTS(0); /* the data already went to the client */
}

/* --- write ---------------------------------------------------------------- */

static bool write_offset(const io_write_t *msg, const pgw_ocb *ocb, uint32_t xtype,
                         uint64_t *off, size_t *data_off)
{
    *data_off = sizeof msg->i;
    if (xtype == _IO_XTYPE_OFFSET) {
        const struct _xtype_offset *xo =
            (const struct _xtype_offset *)(const void *)(&msg->i + 1);
        if (xo->offset < 0)
            return false;
        *off = (uint64_t)xo->offset;
        *data_off += sizeof *xo;
        return true;
    }
    if (xtype == _IO_XTYPE_NONE) {
        if (ocb->ocb.offset < 0)
            return false;
        *off = (uint64_t)ocb->ocb.offset;
        return true;
    }
    return false;
}

/* Copy the client's data chunk by chunk into the backend. Returns EOK when the
 * whole request went through, or the status of the first failure; `*done` is
 * what was written, so a partial write is reported as a short write (FS-01). */
static int write_chunks(resmgr_context_t *ctp, pgw_srv *srv, pgw_node *node,
                        uint64_t off, size_t data_off, size_t want, size_t *done)
{
    int status = EOK;
    *done = 0;
    while (*done < want) {
        if (pgw_client_unblocking(ctp)) {
            status = *done == 0 ? EINTR : EOK;
            break;
        }
        size_t left = want - *done;
        size_t n = left < PGW_IO_CHUNK ? left : PGW_IO_CHUNK;
        ssize_t got = resmgr_msgread(ctp, g_io_buf, n, data_off + *done);
        if (got < 0 || (size_t)got != n) {
            status = EFAULT; /* the client's buffer moved under us */
            break;
        }
        size_t written = 0;
        pgw_err e = pgw_rmfs_write(srv->fs, node->fd, off + *done, g_io_buf, n,
                                   &written);
        if (e != PGW_OK) {
            status = *done == 0 ? pgw_qnx_status(e) : EOK;
            break;
        }
        *done += written;
        if (written < n)
            break;
    }
    return status;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *raw)
{
    pgw_ocb *ocb = (pgw_ocb *)(void *)raw;
    pgw_node *node = ocb->node;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    if (srv->opts->fs.readonly)
        return EROFS; /* SEC-01: -o ro */
    int rc = iofunc_write_verify(ctp, msg, raw, NULL);
    if (rc != EOK)
        return rc;
    if (node == NULL || node->fd == NULL || !node->writable)
        return EBADF;

    uint32_t xtype = msg->i.xtype & _IO_XTYPE_MASK;
    uint64_t off = 0;
    size_t data_off = 0;
    if (!write_offset(msg, ocb, xtype, &off, &data_off))
        return ENOSYS;

    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_WRITE, 0);
    size_t done = 0;
    int status = write_chunks(ctp, srv, node, off, data_off,
                              (size_t)_IO_WRITE_GET_NBYTES(msg), &done);
    pgw_prio_leave(&prio);
    pgw_trace_rep(PGW_OP_WRITE, 0, status);
    if (status != EOK)
        return status;
    if (xtype == _IO_XTYPE_NONE)
        ocb->ocb.offset += (off_t)done;
    if (ocb->ocb.attr != NULL && (int64_t)(off + done) > ocb->ocb.attr->nbytes)
        ocb->ocb.attr->nbytes = (int64_t)(off + done);
    _IO_SET_WRITE_NBYTES(ctp, (long)done);
    return EOK;
}

/* --- stat, lseek, sync, close -------------------------------------------- */

/* Re-read the attributes so a file changed on the PC is seen changed here
 * (FS-12): the driver never caches file contents. */
static int refresh(pgw_ocb *ocb)
{
    pgw_srv *srv = pgw_srv_current();
    pgw_node *node = ocb->node;
    if (srv == NULL || node == NULL || node->fd == NULL)
        return EOK;
    pgw_fs_attr a;
    pgw_err e = pgw_rmfs_fstat(srv->fs, node->fd, &a);
    if (e != PGW_OK)
        return pgw_qnx_status(e);
    pgw_attr_from_fs(&node->attr, &a, &srv->mount);
    return EOK;
}

static int io_stat(resmgr_context_t *ctp, io_stat_t *msg, RESMGR_OCB_T *raw)
{
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_STAT, 0);
    int rc = refresh((pgw_ocb *)(void *)raw);
    pgw_trace_rep(PGW_OP_STAT, 0, rc);
    pgw_prio_leave(&prio);
    if (rc != EOK)
        return rc;
    return iofunc_stat_default(ctp, msg, raw);
}

static int io_lseek(resmgr_context_t *ctp, io_lseek_t *msg, RESMGR_OCB_T *raw)
{
    /* SEEK_END needs the current size, which the PC may have changed. */
    if (msg->i.whence == SEEK_END) {
        int rc = refresh((pgw_ocb *)(void *)raw);
        if (rc != EOK)
            return rc;
    }
    return iofunc_lseek_default(ctp, msg, raw);
}

static int io_sync(resmgr_context_t *ctp, io_sync_t *msg, RESMGR_OCB_T *raw)
{
    pgw_ocb *ocb = (pgw_ocb *)(void *)raw;
    pgw_srv *srv = pgw_srv_current();
    int rc = iofunc_sync_verify(ctp, msg, raw);
    if (rc != EOK)
        return rc;
    if (srv == NULL || ocb->node == NULL || ocb->node->fd == NULL)
        return EBADF;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_SYNC, 0);
    pgw_err e = pgw_rmfs_fsync(srv->fs, ocb->node->fd); /* reaches the PC (FS-10) */
    pgw_trace_rep(PGW_OP_SYNC, 0, pgw_qnx_status(e));
    pgw_prio_leave(&prio);
    return pgw_qnx_status(e);
}

/* The node goes with its last OCB, which may not be this one: a mapping adds a
 * second OCB for the memory manager (FS-11). */
static int io_close_ocb(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *raw)
{
    int rc = iofunc_close_ocb_default(ctp, reserved, raw);
    pgw_node_dispose(pgw_node_released());
    return rc;
}

/* --- devctl --------------------------------------------------------------- */

static int devctl_statvfs(resmgr_context_t *ctp, io_devctl_t *msg)
{
    static _Thread_local struct {
        struct _io_devctl_reply hdr;
        struct __msg_statvfs vfs;
    } rep;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    pgw_fs_statvfs st;
    pgw_err e = pgw_rmfs_statfs(srv->fs, &cred, &st);
    if (e != PGW_OK)
        return pgw_qnx_status(e);
    if (msg->i.nbytes < sizeof rep.vfs)
        return EOVERFLOW; /* df passes 1024 bytes; never truncate silently */
    memset(&rep, 0, sizeof rep);
    rep.vfs.f_bsize = st.bsize;
    rep.vfs.f_frsize = st.bsize;
    rep.vfs.f_blocks = st.blocks;
    rep.vfs.f_bfree = st.bfree;
    rep.vfs.f_bavail = st.bavail;
    rep.vfs.f_files = st.files;
    rep.vfs.f_ffree = st.ffree;
    rep.vfs.f_favail = st.ffree; /* no separate count for unprivileged users */
    rep.vfs.f_namemax = st.namelen;
    rep.vfs.f_flag = ST_NOSUID;
    if (srv->opts->fs.readonly)
        rep.vfs.f_flag |= ST_RDONLY;
    (void)memcpy(rep.vfs.f_basetype, srv->name, strlen(srv->name) + 1u);
    rep.hdr.nbytes = (int32_t)sizeof rep.vfs;
    return _RESMGR_PTR(ctp, &rep, sizeof rep.hdr + sizeof rep.vfs);
}

static int devctl_name(resmgr_context_t *ctp, io_devctl_t *msg, const char *what)
{
    static _Thread_local struct {
        struct _io_devctl_reply hdr;
        char text[PGW_PATH_MAX];
    } rep;
    size_t n = strlen(what) + 1u;
    if (n > sizeof rep.text || msg->i.nbytes < (uint32_t)n)
        return EOVERFLOW;
    memset(&rep.hdr, 0, sizeof rep.hdr);
    memcpy(rep.text, what, n);
    rep.hdr.nbytes = (int32_t)n;
    return _RESMGR_PTR(ctp, &rep, sizeof rep.hdr + n);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *raw)
{
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_DEVCTL, 0);
    int rc;
    switch ((unsigned)msg->i.dcmd) {
    case DCMD_FSYS_STATVFS:
        rc = devctl_statvfs(ctp, msg); /* df / statvfs() (FS-10) */
        break;
    case DCMD_FSYS_MOUNTED_ON:
        rc = devctl_name(ctp, msg, srv->opts->tag);
        break;
    case DCMD_FSYS_MOUNTED_AT:
        rc = devctl_name(ctp, msg, srv->opts->mount_point);
        break;
    case PGW_DCMD_STATS:
        rc = pgw_stats_devctl(ctp, msg); /* per-operation counters (QNX-08) */
        break;
    default:
        rc = iofunc_devctl_default(ctp, msg, raw);
        break;
    }
    pgw_trace_rep(PGW_OP_DEVCTL, 0, rc);
    pgw_prio_leave(&prio);
    return rc;
}

/* The memory manager's mapping of a file's segments. It is a request like every
 * other and QNX-09 says "each request", so it is traced: without it the trace of
 * an exec shows the reads and lookups of the shell fallback but not the mapping
 * the loader asked for, which is the one event that tells the two apart (FS-06).
 */
static int io_mmap(resmgr_context_t *ctp, io_mmap_t *msg, RESMGR_OCB_T *raw)
{
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_MMAP, 0);
    int rc = iofunc_mmap_default(ctp, msg, raw);
    pgw_trace_rep(PGW_OP_MMAP, 0, rc);
    pgw_prio_leave(&prio);
    return rc;
}

void pgw_resmgr_io_init(resmgr_io_funcs_t *io)
{
    /* The kernel maps the segments of an executable through the _IO_MMAP
     * message: without this callout the process manager cannot execute a
     * program the filesystem serves (FS-06, "iofunc_mmap_default()" in the
     * QNX 8.0 documentation). */
    io->mmap = io_mmap;
    io->read = pgw_io_read;
    io->read64 = pgw_io_read;
    io->write = io_write;
    io->write64 = io_write;
    io->stat = io_stat;
    io->lseek = io_lseek;
    io->sync = io_sync;
    io->close_ocb = io_close_ocb;
    io->devctl = io_devctl;
    pgw_resmgr_meta_init(io);
}
