/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/resmgr_connect_qnx.c - namespace handlers: open (and
 * create), unlink, rmdir, rename, mkdir, symlink, link, readlink.
 *
 * All of them start from the client's identity (FS-09): the permission check is
 * made in the guest, by the rmfs layer, against the uid/gid the kernel reports
 * for the sender, because the PC side can only check as the host user.
 *
 * Symlinks are never followed here. QNX lets a resource manager answer an open
 * with a *redirect*: the client restarts the resolution itself, so the loop
 * limit and the "does this absolute path mean the guest or the host" question
 * stay on the client's side (spec 5.3, FS-08).
 *
 * Covered requirements: FS-05, FS-08, FS-09, FS-11, QNX-01, QNX-03, SEC-01.
 */
/* The project compiles with -std=c11 (no GNU extensions). QNX ignores these
 * feature macros: its own headers always expose S_IF*, strnlen and the POSIX
 * realtime interfaces. The host needs them declared to build this file against
 * the stubs in tests/qnx-stub, so both are spelled out here once. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "pgw/drvsrv.h"

#include "pgw/log.h"

#include <stdio.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#include <sys/stat.h>

/* A redirect reply is a link reply (nentries 0) followed by a NUL-terminated
 * path; the client sized its buffer for exactly that. */
typedef struct pgw_redirect {
    struct _io_connect_link_reply hdr;
    char path[PGW_PATH_MAX + 1u];
} pgw_redirect;

static _Thread_local pgw_redirect t_redir;

/* Reply "the path you asked for is this link, restart there". */
static int reply_redirect(resmgr_context_t *ctp, io_open_t *msg, const char *target,
                          size_t len)
{
    if (len > PGW_PATH_MAX ||
        sizeof t_redir.hdr + len + 1u > (size_t)msg->connect.reply_max)
        return ENAMETOOLONG;
    memset(&t_redir.hdr, 0, sizeof t_redir.hdr);
    t_redir.hdr.file_type = msg->connect.file_type;
    t_redir.hdr.eflag = msg->connect.eflag;
    t_redir.hdr.path_len = (uint8_t)(len + 1u);
    memcpy(t_redir.path, target, len);
    t_redir.path[len] = '\0';
    if (MsgReply(ctp->rcvid, (long)(_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_LINK),
                 &t_redir, sizeof t_redir.hdr + len + 1u) == -1)
        return errno;
    return _RESMGR_NOREPLY; /* the reply is sent */
}

/* Client flags -> the project's own flag bits, then to the backend. */
static uint32_t oflags_of(uint32_t ioflag)
{
    uint32_t host = 0;
    if ((ioflag & _IO_FLAG_WR) != 0u && (ioflag & _IO_FLAG_RD) != 0u)
        host |= O_RDWR;
    else if ((ioflag & _IO_FLAG_WR) != 0u)
        host |= O_WRONLY;
    else
        host |= O_RDONLY;
    /* The remaining ioflag bits are the target's own O_* values, so they can be
     * handed to the shared translator as they are (no second table here). */
    host |= ioflag & ~(uint32_t)(_IO_FLAG_RD | _IO_FLAG_WR);
    return pgw_flags_from_host(host);
}

/* --- open ----------------------------------------------------------------- */

static int open_path(resmgr_context_t *ctp, io_open_t *msg, const char *path,
                     pgw_fs_cred *cred, uint32_t pgw_oflags)
{
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    uint32_t mode = msg->connect.mode & PGW_S_PERM_MASK;
    pgw_rmfs_fd *fd = NULL;
    pgw_err e = pgw_rmfs_open(srv->fs, cred, path, pgw_oflags, mode, &fd);
    if (e != PGW_OK)
        return pgw_qnx_status(e);
    pgw_fs_attr a;
    e = pgw_rmfs_fstat(srv->fs, fd, &a);
    if (e != PGW_OK) {
        (void)pgw_rmfs_close(srv->fs, fd);
        return pgw_qnx_status(e);
    }
    bool writable = (pgw_oflags & (PGW_O_WRONLY | PGW_O_RDWR)) != 0u;
    e = pgw_node_open(ctp, msg, path, fd, &a, writable);
    if (e != PGW_OK) {
        (void)pgw_rmfs_close(srv->fs, fd);
        return pgw_qnx_status(e);
    }
    return EOK;
}

static int connect_open(resmgr_context_t *ctp, io_open_t *msg,
                        RESMGR_HANDLE_T *handle, void *extra)
{
    (void)handle;
    (void)extra;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    const char *path = msg->connect.path;
    /* The kernel sends an empty path when the client opens the mount point
     * itself: that is the root of the filesystem, not "nothing" (QNX-01). */
    if (path == NULL || path[0] == '\0')
        path = "/";
    uint32_t ioflag = msg->connect.ioflag;
    /* What the exec open carries, at debug level.
     *
     * Measured on the real guest (14 November 2026): the process manager does not
     * send a plain `_IO_CONNECT_OPEN` to execute a file - it sends a
     * `_IO_CONNECT_COMBINE_CLOSE` with `_IO_FLAG_EX|O_PERMCHECK|O_REALIDS` (the
     * permission check) and then a `_IO_CONNECT_COMBINE` whose 8-byte second
     * message is an `_IO_STAT`. `_IO_MMAP` never arrives at all, which is the
     * shape of a loader that refused the file before mapping it. That is FS-06,
     * and the smallest thing that records it is the subtype with the flags - not
     * forty bytes of hex, which is what an earlier version of this line printed.
     */
    if ((ioflag & _IO_FLAG_EX) != 0u)
        PGW_DBG("fs9p: exec open: subtype=%u ioflag=0x%x extra_len=%u "
                 "reply_max=%u", (unsigned)msg->connect.subtype, (unsigned)ioflag,
                 (unsigned)msg->connect.extra_len,
                 (unsigned)msg->connect.reply_max);

    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_OPEN, 0);

    int rc;
    if (srv->opts->fs.readonly &&
        ((ioflag & _IO_FLAG_WR) != 0u || (ioflag & O_TRUNC) != 0u)) {
        rc = EROFS; /* SEC-01 */
    } else {
        /* The mount point itself is the root of the share. */
        rc = open_path(ctp, msg, path, &cred, oflags_of(ioflag));
        if (rc == ENOENT && (ioflag & O_CREAT) != 0u)
            rc = open_path(ctp, msg, path, &cred, oflags_of(ioflag) | PGW_O_CREAT);
    }
    pgw_trace_rep(PGW_OP_OPEN, 0, rc);
    pgw_prio_leave(&prio);
    return rc;
}

/* --- readlink ------------------------------------------------------------- */

static int connect_readlink(resmgr_context_t *ctp, io_readlink_t *msg,
                            RESMGR_HANDLE_T *handle, void *reserved)
{
    (void)handle;
    (void)reserved;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    static _Thread_local char target[PGW_PATH_MAX];
    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_READLINK, 0);
    pgw_err e = pgw_rmfs_readlink(srv->fs, &cred, msg->connect.path, target,
                                  sizeof target);
    int rc = e == PGW_OK ? reply_redirect(ctp, (io_open_t *)msg, target,
                                          strnlen(target, sizeof target))
                         : pgw_qnx_status(e);
    pgw_trace_rep(PGW_OP_READLINK, 0, rc == _RESMGR_NOREPLY ? EOK : rc);
    pgw_prio_leave(&prio);
    return rc;
}

/* --- unlink / rmdir ------------------------------------------------------ */

static int connect_unlink(resmgr_context_t *ctp, io_unlink_t *msg,
                          RESMGR_HANDLE_T *handle, void *reserved)
{
    (void)handle;
    (void)reserved;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    if (srv->opts->fs.readonly)
        return EROFS;
    if (msg->connect.path[0] == '\0')
        return EBUSY; /* the mount point cannot be removed */
    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    /* rmdir() sends mode S_IFDIR, unlink() sends S_IFLNK ("do not follow"). */
    bool is_dir = (msg->connect.mode & S_IFMT) == S_IFDIR;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_UNLINK, 0);
    pgw_err e = pgw_rmfs_unlink(srv->fs, &cred, msg->connect.path, is_dir);
    pgw_trace_rep(PGW_OP_UNLINK, 0, pgw_qnx_status(e));
    pgw_prio_leave(&prio);
    return pgw_qnx_status(e);
}

/* --- mkdir ---------------------------------------------------------------- */

static int connect_mknod(resmgr_context_t *ctp, io_mknod_t *msg,
                         RESMGR_HANDLE_T *handle, void *reserved)
{
    (void)handle;
    (void)reserved;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    if (srv->opts->fs.readonly)
        return EROFS;
    /* mkdir() arrives here with S_IFDIR; device nodes and FIFOs make no sense
     * on a host share and are refused rather than silently turned into files. */
    if ((msg->connect.mode & S_IFMT) != S_IFDIR)
        return ENOTSUP;
    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_MKDIR, 0);
    pgw_err e = pgw_rmfs_mkdir(srv->fs, &cred, msg->connect.path,
                               msg->connect.mode & PGW_S_PERM_MASK);
    pgw_trace_rep(PGW_OP_MKDIR, 0, pgw_qnx_status(e));
    pgw_prio_leave(&prio);
    return pgw_qnx_status(e);
}

/* --- rename --------------------------------------------------------------- */

static int connect_rename(resmgr_context_t *ctp, io_rename_t *msg,
                          RESMGR_HANDLE_T *handle, io_rename_extra_t *extra)
{
    (void)handle;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    if (srv->opts->fs.readonly)
        return EROFS;
    if (extra == NULL || msg->connect.extra_type != _IO_CONNECT_EXTRA_RENAME)
        return ENOSYS;
    /* The new name is the connect path, the old one is in the extra. */
    size_t n = strnlen(extra->path, msg->connect.extra_len);
    if (n == 0 || n >= PGW_PATH_MAX)
        return n == 0 ? EINVAL : ENAMETOOLONG;
    static _Thread_local char old_path[PGW_PATH_MAX];
    memcpy(old_path, extra->path, n);
    old_path[n] = '\0';

    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    pgw_trace_req(PGW_OP_RENAME, 0);
    pgw_err e = pgw_rmfs_rename(srv->fs, &cred, old_path, msg->connect.path);
    pgw_trace_rep(PGW_OP_RENAME, 0, pgw_qnx_status(e));
    pgw_prio_leave(&prio);
    return pgw_qnx_status(e);
}

/* --- symlink / link ------------------------------------------------------ */

static int connect_link(resmgr_context_t *ctp, io_link_t *msg,
                        RESMGR_HANDLE_T *handle, io_link_extra_t *extra)
{
    (void)handle;
    pgw_srv *srv = pgw_srv_current();
    if (srv == NULL)
        return EIO;
    if (srv->opts->fs.readonly)
        return EROFS;
    if (extra == NULL)
        return ENOSYS;
    size_t n = strnlen(extra->path, msg->connect.extra_len);
    if (n == 0 || n >= PGW_PATH_MAX)
        return n == 0 ? ENOENT : ENAMETOOLONG;
    static _Thread_local char other[PGW_PATH_MAX];
    memcpy(other, extra->path, n);
    other[n] = '\0';

    pgw_fs_cred cred;
    if (pgw_cred_from_ctp(ctp, &cred) != PGW_OK)
        return EACCES;
    pgw_prio_state prio;
    pgw_prio_enter(ctp, &prio);
    uint8_t type = msg->connect.extra_type;
    pgw_err e;
    if (type == _IO_CONNECT_EXTRA_SYMLINK) {
        /* The extra is the link target as written; a relative target names a
         * path inside the share, an absolute one a path in the guest. */
        pgw_trace_req(PGW_OP_SYMLINK, 0);
        e = pgw_rmfs_symlink(srv->fs, &cred, other, msg->connect.path);
        pgw_trace_rep(PGW_OP_SYMLINK, 0, pgw_qnx_status(e));
    } else if (type == _IO_CONNECT_EXTRA_LINK) {
        /* The extra is the existing file, relative to the mount. */
        pgw_trace_req(PGW_OP_LINK, 0);
        e = pgw_rmfs_link(srv->fs, &cred, other, msg->connect.path);
        pgw_trace_rep(PGW_OP_LINK, 0, pgw_qnx_status(e));
    } else {
        e = PGW_ENOSYS;
    }
    pgw_prio_leave(&prio);
    return pgw_qnx_status(e);
}

void pgw_resmgr_connect_init(resmgr_connect_funcs_t *c)
{
    c->open = connect_open;
    c->readlink = connect_readlink;
    c->unlink = connect_unlink;
    c->mknod = connect_mknod;
    c->rename = connect_rename;
    c->link = connect_link;
    pgw_resmgr_mount_init(c);
}
