/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/qemu/qnx8/mapprobe.c - which mapping a served file refuses, and at which
 * open mode (FS-06).
 *
 * Measured before this probe existed: mapping /host/fs6-demo with PROT_READ
 * works and the ELF magic is read through it, while PROT_READ|PROT_EXEC on a
 * descriptor opened O_EXEC returns EACCES - even when the mount is `rw`, so the
 * read-only mount is not what refuses it.
 *
 * `iofunc_mmap()` (which `iofunc_mmap_default()` calls) documents three checks:
 * the requested protections against the descriptor's open mode, the mountpoint
 * not being read-only, and the file being executable before it puts PROT_EXEC in
 * `allowed_prot`. Two of the three are left, and they want different fixes, so
 * this asks the driver rather than reading its code: a matrix of
 * (open mode) x (protections), on the served file and on the same bytes off the
 * share.
 *
 * The control matters as much as the result. /dev/shmem is served by another
 * resource manager; whatever it accepts and we refuse is the difference the
 * project has to close, and whatever it refuses too is not our bug.
 *
 * Usage: mapprobe <file> [<control>]
 */
#define _POSIX_C_SOURCE 200809L
#define _QNX_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* The three open modes a client can ask for, and the protections a loader can
 * ask for. O_EXEC is QNX's fourth access mode (000003): an exec open carries
 * `_IO_FLAG_EX` on the wire and neither RD nor WR. */
static const struct {
    const char *name;
    int oflag;
} opens[] = {
        {"O_RDONLY", O_RDONLY},
        {"O_EXEC", O_EXEC},
        {"O_RDWR", O_RDWR},
};

static const struct {
    const char *name;
    int prot;
} prots[] = {
        {"PROT_READ", PROT_READ},
        {"PROT_EXEC", PROT_EXEC},
        {"PROT_READ|PROT_EXEC", PROT_READ | PROT_EXEC},
};

static void show_statvfs(const char *path)
{
    /* What the kernel was told about the MOUNT. `iofunc_mmap()` refuses a
     * mapping on a read-only mount and a setuid file on a nosuid mount, and this
     * driver publishes nosuid by default (SEC-01). `statvfs` is the client's own
     * view of those flags, so the two paths can be compared in one run instead
     * of by reading the code that sets them. */
    struct statvfs vfs;
    memset(&vfs, 0, sizeof vfs);
    if (statvfs(path, &vfs) != 0) {
        printf("mapprobe: statvfs failed: %s\n", strerror(errno));
        return;
    }
    printf("mapprobe: statvfs flag=0x%lx (RDONLY=%lu NOSUID=%lu NOEXEC=%lu)\n",
           (unsigned long)vfs.f_flag, (unsigned long)(vfs.f_flag & ST_RDONLY),
           (unsigned long)(vfs.f_flag & ST_NOSUID),
#ifdef ST_NOEXEC
           (unsigned long)(vfs.f_flag & ST_NOEXEC)
#else
           0ul
#endif
    );
}

/* The bytes the loader reads to identify the file, on THIS descriptor. The exec
 * path opens with O_EXEC - a fourth access mode on QNX - and if the answer it
 * reads is not the ELF magic it falls back to the shell, which is the failure
 * FS-06 shows and which no mapping explains. The program headers live at
 * `e_phoff`, not at 0, so reading only offset 0 proves nothing: these offsets are
 * compared against the host's own bytes beside this output. */
static void show_reads(int f, off_t size)
{
    static const off_t offs[] = {0, 64, 512, 1024, 2048, 4096};
    for (size_t k = 0; k < sizeof offs / sizeof offs[0]; k++) {
        if (offs[k] >= size)
            continue;
        unsigned char b[8];
        memset(b, 0, sizeof b);
        errno = 0;
        ssize_t got = pread(f, b, sizeof b, offs[k]);
        printf("mapprobe:   pread@%-5lld n=%zd bytes=%02x%02x%02x%02x%02x%02x"
               "%02x%02x errno=%d\n",
               (long long)offs[k], got, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
               errno);
    }
}

/* Every protection the loader can ask for, on this descriptor. `iofunc_mmap()`
 * refuses a mapping whose protections conflict with the open mode, so the same
 * refusal on the control and on us is QNX, and a refusal only on us is ours. */
static void show_maps(int f, off_t size)
{
    for (size_t p = 0; p < sizeof prots / sizeof prots[0]; p++) {
        errno = 0;
        void *m = mmap(NULL, (size_t)size, prots[p].prot, MAP_PRIVATE, f, 0);
        printf("mapprobe:   %-20s -> %s", prots[p].name,
               m == MAP_FAILED ? "REFUSED" : "ok");
        if (m == MAP_FAILED) {
            printf(" errno=%d (%s)\n", errno, strerror(errno));
            continue;
        }
        const uint8_t *b = m;
        printf(" magic=%02x%02x%02x%02x\n", b[0], b[1], b[2], b[3]);
        munmap(m, (size_t)size);
    }
}

/* The right to execute, as the filesystem answers it. `access()` is the client
 * call that produces the `_IO_CONNECT_COMBINE_CLOSE` with `O_PERMCHECK` the
 * process manager sends before it loads anything, so asking it here reads the
 * same answer the loader gets (FS-09, FS-06). */
static void show_access(const char *path)
{
    static const struct {
        const char *name;
        int mode;
    } amodes[] = {
            {"F_OK", F_OK},
            {"R_OK", R_OK},
            {"W_OK", W_OK},
            {"X_OK", X_OK},
    };
    for (size_t i = 0; i < sizeof amodes / sizeof amodes[0]; i++) {
        errno = 0;
        int ok = access(path, amodes[i].mode);
        printf("mapprobe:   access(%s) -> %d%s\n", amodes[i].name, ok,
               ok == 0 ? "" : (errno ? strerror(errno) : "?"));
    }
}

static void probe_file(const char *path)
{
    printf("mapprobe: === %s\n", path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("mapprobe: cannot open %s: %s\n", path, strerror(errno));
        return;
    }
    struct stat st;
    memset(&st, 0, sizeof st);
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        printf("mapprobe: fstat failed or empty: %s\n", strerror(errno));
        close(fd);
        return;
    }
    close(fd);
    printf("mapprobe: size=%lld mode=0%o uid=%d gid=%d\n", (long long)st.st_size,
           (unsigned)(st.st_mode & 07777u), (int)st.st_uid, (int)st.st_gid);
    show_statvfs(path);
    show_access(path);

    for (size_t o = 0; o < sizeof opens / sizeof opens[0]; o++) {
        int f = open(path, opens[o].oflag);
        printf("mapprobe: open(%s) fd=%d", opens[o].name, f);
        if (f < 0) {
            printf(" errno=%d (%s)\n", errno, strerror(errno));
            continue;
        }
        printf("\n");
        show_reads(f, st.st_size);
        show_maps(f, st.st_size);
        close(f);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            probe_file(argv[i]);
        return 0;
    }
    probe_file("/host/fs6-demo");
    probe_file("/dev/shmem/d");
    return 0;
}
