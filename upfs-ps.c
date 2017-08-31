#define _XOPEN_SOURCE 700 /* *at */

#define FUSE_USE_VERSION 28
#include <fuse.h>

#include "upfs-ps.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ENTRY_OFFSET(field) \
    ((char *) &((struct upfs_entry *) NULL)->field - (char *) NULL)

struct upfs_open_out {
    struct upfs_entry de;
    int *tbl_fd;
    off_t tbl_off;
};

/* Allocate and initialize a directory entry */
static off_t upfs_alloc_entry(int tbl_fd, struct upfs_entry *data)
{
    struct upfs_header dh;
    struct upfs_entry_unused old_de;
    uint32_t next;
    ssize_t rd;
    off_t loc;

    /* We need to lock the table to make these changes */
    if (flock(tbl_fd, LOCK_EX) < 0)
        return -1;

    /* Get the current freelist entry */
    rd = pread(tbl_fd, &dh, sizeof(struct upfs_header), 0);
    if (rd < 0) {
        return -1;
    } else if (rd != sizeof(struct upfs_header)) {
        errno = EIO;
        return -1;
    }

    /* Extend the file if there's no entry free */
    if (dh.free_list == (uint32_t) -1) {
        loc = lseek(tbl_fd, 0, SEEK_END);
        if (loc == (off_t) -1)
            return -1;

        if ((loc - sizeof(struct upfs_header)) % sizeof(struct upfs_entry)
            != 0) {
            /* Directory table is corrupted! */
            errno = EIO;
            return -1;
        }

        if (write(tbl_fd, data, sizeof(struct upfs_entry)) !=
            sizeof(struct upfs_entry))
            return -1;

        return loc;
    }

    /* There's an entry free, so claim it */
    loc = sizeof(struct upfs_header) +
        dh.free_list * sizeof(struct upfs_entry);
    rd = pread(tbl_fd, &old_de, sizeof(struct upfs_entry_unused), loc);
    if (rd < 0) {
        return -1;
    } else if (rd != sizeof(struct upfs_entry)) {
        errno = EIO;
        return -1;
    }
    if (old_de.header != (uint32_t) -1) {
        /* Used entry! */
        errno = EIO;
        return -1;
    }
    next = old_de.next;

    /* Update the next pointer in the header */
    dh.free_list = next;
    if (pwrite(tbl_fd, &dh, sizeof(struct upfs_header), 0) !=
        sizeof(struct upfs_header))
        return -1;

    /* And write the node */
    if (pwrite(tbl_fd, data, sizeof(struct upfs_entry), loc) !=
        sizeof(struct upfs_entry))
        return -1;

    return loc;
}

/* Free (unlink) a directory entry */
static int upfs_free_entry(int tbl_fd, off_t entry_offset)
{
    struct upfs_header dh;
    union {
        struct upfs_entry e;
        struct upfs_entry_unused u;
    } de = {0};
    ssize_t rd;

    /* We need to lock the table to make these changes */
    if (flock(tbl_fd, LOCK_EX) < 0)
        return -1;

    /* Get the current freelist entry */
    rd = pread(tbl_fd, &dh, sizeof(struct upfs_header), 0);
    if (rd < 0) {
        return -1;
    } else if (rd != sizeof(struct upfs_header)) {
        errno = EIO;
        return -1;
    }

    /* Link this one in */
    de.u.header = (uint32_t) -1;
    de.u.next = dh.free_list;
    if (pwrite(tbl_fd, &de, sizeof(struct upfs_entry), entry_offset) !=
        sizeof(struct upfs_entry))
        return -1;

    /* And make it the head */
    entry_offset -= sizeof(struct upfs_header);
    entry_offset /= sizeof(struct upfs_entry);
    dh.free_list = entry_offset;
    if (pwrite(tbl_fd, &dh, sizeof(struct upfs_header), 0)
        != sizeof(struct upfs_header))
        return -1;

    return 0;
}

/* General-purpose permissions file "open". O_CREAT and O_EXCL are as in open,
 * O_APPEND means "open the directory with an exclusive lock", i.e., we intend
 * to change this directory entry. Other flags are ignored. */
static int upfs_ps_open(int root_fd, const char *path, int flags, mode_t mode,
    struct upfs_open_out *o)
{
    char path_parts[PATH_MAX], *path_base, *path_tmp;
    const char *path_dir, *path_file;
    struct upfs_header dh;
    struct upfs_entry de;
    struct fuse_context *fctx;
    int dir_fd = -1, tbl_fd = -1;
    int save_errno;
    int found = 0;
    ssize_t rd;
    off_t tbl_off;

    o->de.uid = (uint32_t) -1;
    if (o->tbl_fd) *o->tbl_fd = -1;

    /* Check for unsupported modes */
    if ((mode & UPFS_SUPPORTED_MODES) != mode) {
        errno = ENOTSUP;
        goto error;
    }

    /* Split up the path */
    strncpy(path_parts, path, PATH_MAX);
    path_parts[PATH_MAX-1] = 0;
    path_base = path_parts;
    path_tmp = strrchr(path_base, '/');
    while (path_tmp && !path_tmp[1]) {
        /* Ends with / */
        *path_tmp = 0;
        path_tmp = strrchr(path_base, '/');
    }
    if (!path_tmp) {
        /* No / */
        path_dir = ".";
        path_file = path_base;
        if (!path_file[0]) {
            /* Empty path */
            errno = ENOENT;
            goto error;
        }

    } else {
        /* Split it */
        *path_tmp++ = 0;
        path_dir = path_base;
        path_file = path_tmp;

    }

    /* The metafile itself is verboten */
    if (!strcmp(path_file, UPFS_META_FILE)) {
        errno = EACCES;
        goto error;
    }

    /* First follow the directory */
    dir_fd = openat(root_fd, path_dir, O_RDONLY);
    if (dir_fd < 0)
        goto error;

    /* Now see if there's a directory entry for it */
    tbl_fd = openat(dir_fd, UPFS_META_FILE, (flags&O_CREAT) ? (O_RDWR|O_CREAT) : O_RDWR, 0600);
    if (tbl_fd < 0)
        goto error; /* Can be a very minor error, but expected upstream */

    close(dir_fd);
    dir_fd = -1;

    if (flock(tbl_fd, (flags&O_APPEND) ? LOCK_EX : LOCK_SH) < 0)
        goto error;

    /* Check the header */
    rd = read(tbl_fd, &dh, sizeof(struct upfs_header));
    if (rd == 0 && (flags&O_CREAT)) {
        /* Fresh table, make the hader */
        memcpy(dh.magic, UPFS_MAGIC, UPFS_MAGIC_LENGTH);
        dh.version = UPFS_VERSION;
        dh.free_list = (uint32_t) -1;
        if (write(tbl_fd, &dh, sizeof(struct upfs_header)) != sizeof(struct upfs_header)) {
            errno = EIO;
            goto error;
        }

    } else if (rd == sizeof(struct upfs_header)) {
        /* Check that it's a UPFS directory table */
        if (memcmp(dh.magic, UPFS_MAGIC, UPFS_MAGIC_LENGTH) || dh.version > UPFS_VERSION) {
            errno = EIO;
            goto error;
        }

    } else if (rd < 0) {
        /* Error reading the table file! */
        goto error;

    } else {
        /* Some other kind of file */
        errno = EIO;
        goto error;
    }

    /* Now read the directory entries */
    while (read(tbl_fd, &de, sizeof(struct upfs_entry)) ==
        sizeof(struct upfs_entry)) {
        if (de.uid != (uint32_t) -1 && !strncmp(path_file, de.name, UPFS_NAME_LENGTH)) {
            /* Found it! */
            found = 1;
            break;
        }
    }

    if (found) {
        if (flags & (O_CREAT|O_EXCL)) {
            /* Shouldn't have existed! */
            errno = -EEXIST;
            goto error;
        }

        /* Tell the caller */
        o->de = de;
        if (o->tbl_fd) *o->tbl_fd = tbl_fd;
        o->tbl_off = lseek(tbl_fd, 0, SEEK_CUR) - sizeof(struct upfs_entry);

    } else if (flags & O_CREAT) {
        /* Create an entry for it */
        memset(&de, 0, sizeof(struct upfs_entry));

        fctx = fuse_get_context();
        de.uid = fctx->uid;
        de.gid = fctx->gid;
        de.mode = mode;
        strncpy(de.name, path_file, UPFS_NAME_LENGTH-1);
        o->tbl_off = upfs_alloc_entry(tbl_fd, &de);
        if (o->tbl_off == (off_t) -1)
            goto error;
        o->de = de;
        if (o->tbl_fd) *o->tbl_fd = tbl_fd;

    }

    if (!o->tbl_fd)
        close(tbl_fd);

    return 0;

error:
    save_errno = errno;
    if (dir_fd >= 0) close(dir_fd);
    if (tbl_fd >= 0) close(tbl_fd);
    errno = save_errno;
    return -1;
}

/* Current time in UpFS format */
struct upfs_time time_now(void)
{
    struct upfs_time ret;
    struct timespec ts = {0};
    clock_gettime(CLOCK_REALTIME, &ts);
    ret.sec = ts.tv_sec;
    ret.nsec = ts.tv_nsec;
    return ret;
}

/****************************************************************
 * FILE SYSTEM SIMULATION FUNCTIONS
 ***************************************************************/

int upfs_fstatat(int dir_fd, const char *path, struct stat *buf, int flags)
{
    struct upfs_open_out o = {0};

    if (upfs_ps_open(dir_fd, path, 0, 0, &o) < 0)
        return -1;

    memset(buf, 0, sizeof(struct stat));
    buf->st_mode = o.de.mode;
    buf->st_nlink = 1;
    buf->st_uid = o.de.uid;
    buf->st_gid = o.de.gid;
    /* FIXME: Times */
    return 0;
}

int upfs_mknodat(int dir_fd, const char *path, mode_t mode, dev_t dev)
{
    struct upfs_open_out o = {0};

    if ((mode&UPFS_SUPPORTED_MODES) != mode) {
        errno = ENOTSUP;
        return -1;
    }

    if (upfs_ps_open(dir_fd, path, O_CREAT|O_EXCL, mode, &o) < 0)
        return -1;
    return 0;
}

int upfs_mkdirat(int dir_fd, const char *path, mode_t mode)
{
    struct upfs_open_out o = {0};
    if (upfs_ps_open(dir_fd, path, O_CREAT|O_EXCL, S_IFDIR|(mode&07777), &o) < 0)
        return -1;
    return 0;
}

int upfs_unlinkat(int dir_fd, const char *path, int flags)
{
    int tbl_fd;
    struct upfs_open_out o = {0};
    o.tbl_fd = &tbl_fd;
    if (upfs_ps_open(dir_fd, path, 0, 0, &o) < 0)
        return -1;

    /* FIXME: Need empty directory testing */
    if ((S_ISDIR(o.de.mode) && !(flags & AT_REMOVEDIR)) ||
        (!S_ISDIR(o.de.mode) && (flags & AT_REMOVEDIR))) {
        errno = EPERM;
        return -1;
    }

    if (upfs_free_entry(tbl_fd, o.tbl_off) < 0) {
        int save_errno = errno;
        close(tbl_fd);
        errno = save_errno;
        return -1;
    }

    close(tbl_fd);
    return 0;
}

static int upfs_fchmodat_prime(int dir_fd, const char *path, mode_t mode, int full_mode)
{
    int tbl_fd;
    struct upfs_open_out o = {0};
    o.tbl_fd = &tbl_fd;
    if (upfs_ps_open(dir_fd, path, O_APPEND, 0, &o) < 0)
        return -1;
    if (full_mode)
        o.de.mode = mode;
    else
        o.de.mode = (o.de.mode&S_IFMT) | (mode&07777);
    o.de.ctime = time_now();
    if (pwrite(tbl_fd, &o.de, sizeof(struct upfs_entry), o.tbl_off) !=
        sizeof(struct upfs_entry)) {
        int save_errno = errno;
        close(tbl_fd);
        errno = save_errno;
        if (!errno) errno = EIO;
        return -1;
    }
    close(tbl_fd);
    return 0;
}

int upfs_fchmodat_harder(int dir_fd, const char *path, mode_t mode, int flags)
{
    return upfs_fchmodat_prime(dir_fd, path, mode, 1);
}

int upfs_fchmodat(int dir_fd, const char *path, mode_t mode, int flags)
{
    return upfs_fchmodat_prime(dir_fd, path, mode, 0);
}

int upfs_renameat(int old_dir_fd, const char *old_path,
    int new_dir_fd, const char *new_path)
{
    int old_tbl_fd = -1, new_tbl_fd = -1;
    struct upfs_open_out oo = {0}, no = {0};
    int save_errno;
    oo.tbl_fd = &old_tbl_fd;
    no.tbl_fd = &new_tbl_fd;

    /* First stat both locations */
    if (upfs_ps_open(old_dir_fd, old_path, 0, 0, &oo) < 0)
        return -1;
    if (upfs_ps_open(new_dir_fd, new_path, O_CREAT, 0, &no) < 0)
        goto error;

    /* Check for an incompatible move. (FIXME: Need to check for empty
     * directories) */
    if (S_ISDIR(oo.de.mode) && !S_ISDIR(no.de.mode)) {
        errno = ENOTDIR;
        goto error;
    }
    if (!S_ISDIR(oo.de.mode) && S_ISDIR(no.de.mode)) {
        errno = EISDIR;
        goto error;
    }

    /* Copy the metadata (FIXME: Do this in an automated way without
     * overwriting name and without locking issues) */
    no.de.uid = oo.de.uid;
    no.de.gid = oo.de.gid;
    no.de.mode = oo.de.mode;
    no.de.reserved = 0;
    no.de.mtime = oo.de.mtime;
    no.de.ctime = oo.de.ctime;
    if (pwrite(new_tbl_fd, &no.de, sizeof(struct upfs_entry), no.tbl_off) !=
        sizeof(struct upfs_entry))
        goto error;
    close(new_tbl_fd);
    new_tbl_fd = -1;

    /* And remove the old one */
    if (upfs_free_entry(old_tbl_fd, oo.tbl_off) < 0)
        goto error;
    close(old_tbl_fd);
    old_tbl_fd = -1;

    return 0;

error:
    save_errno = errno;
    if (old_tbl_fd >= 0) close(old_tbl_fd);
    if (new_tbl_fd >= 0) close(new_tbl_fd);
    errno = save_errno;
    return -1;
}

int upfs_fchownat(int dir_fd, const char *path, uid_t owner, gid_t group,
    int flags)
{
    int tbl_fd;
    struct upfs_open_out o = {0};
    o.tbl_fd = &tbl_fd;
    if (upfs_ps_open(dir_fd, path, O_APPEND, 0, &o) < 0)
        return -1;

    o.de.uid = owner;
    o.de.gid = group;
    o.de.ctime = time_now();
    if (pwrite(tbl_fd, &o.de, sizeof(struct upfs_entry), o.tbl_off) !=
        sizeof(struct upfs_entry)) {
        int save_errno = errno;
        close(tbl_fd);
        errno = save_errno;
        return -1;
    }

    close(tbl_fd);
    return 0;
}

int upfs_openat(int dir_fd, const char *path, int flags, mode_t mode)
{
    int tbl_fd;
    struct upfs_open_out o = {0};
    o.tbl_fd = &tbl_fd;
    if (upfs_ps_open(dir_fd, path, flags&(O_CREAT|O_EXCL), S_IFREG|(mode&0777), &o) < 0)
        return -1;

    /* So that the fd is actually useable, seek it properly */
    if (lseek(tbl_fd, o.tbl_off, SEEK_SET) == (off_t) -1) {
        int save_errno = errno;
        close(tbl_fd);
        errno = save_errno;
        return -1;
    }

    /* We give UpFS tbl_fd, but to prevent locking from going too stupid, we
     * don't give it locked */
    flock(tbl_fd, LOCK_UN);

    return tbl_fd;
}

int upfs_futimens(int fd, const struct timespec *times)
{
    /* This is a screwy function for a lot of reasons. First and foremost, we
     * can't verify that nobody's deleted nodes, so we may actually be updating
     * the modification times of an unrelated file. That's reasonably harmless,
     * so rather than keeping gross locks around, we let the bug be */
    struct upfs_entry de;
    off_t loc;
    int save_errno;

    if (flock(fd, LOCK_EX) < 0)
        return -1;

    loc = lseek(fd, 0, SEEK_CUR);
    if (loc == (off_t) -1)
        goto error;

    if (pread(fd, &de, sizeof(struct upfs_entry), loc) !=
        sizeof(struct upfs_entry))
        goto error;

    if (de.uid == (uint32_t) -1) {
        errno = EIO;
        goto error;
    }

    if (times) {
        de.mtime.sec = times[1].tv_sec;
        de.mtime.nsec = times[1].tv_nsec;
    } else {
        de.mtime = time_now();
    }

    if (pwrite(fd, &de, sizeof(struct upfs_entry), loc) !=
        sizeof(struct upfs_entry))
        goto error;

    flock(fd, LOCK_UN);
    return 0;

error:
    save_errno = errno;
    flock(fd, LOCK_UN);
    errno = save_errno;
    return -1;
}

int upfs_utimensat(int dir_fd, const char *path, const struct timespec *times,
    int flags)
{
    int tbl_fd;
    struct upfs_open_out o = {0};
    o.tbl_fd = &tbl_fd;
    if (upfs_ps_open(dir_fd, path, O_APPEND, 0, &o) < 0)
        return -1;

    if (times) {
        o.de.mtime.sec = times[1].tv_sec;
        o.de.mtime.nsec = times[1].tv_nsec;
    } else {
        o.de.mtime = time_now();
    }

    if (pwrite(tbl_fd, &o.de, sizeof(struct upfs_entry), o.tbl_off) !=
        sizeof(struct upfs_entry)) {
        int save_errno = errno;
        close(tbl_fd);
        errno = save_errno;
        return -1;
    }

    close(tbl_fd);
    return 0;
}
