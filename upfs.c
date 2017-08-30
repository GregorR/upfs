#define _XOPEN_SOURCE 700 /* *at */

#define FUSE_USE_VERSION 28
#include <fuse.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

char *perm_root_path = NULL, *store_root_path = NULL;
int perm_root = -1, store_root = -1;

/* Drop to caller privileges */
static void drop(void)
{
    struct fuse_context *fctx = fuse_get_context();
    if (seteuid(fctx->uid) < 0) {
        perror("seteuid");
        exit(1);
    }
    if (setegid(fctx->gid) < 0) {
        perror("setegid");
        exit(1);
    }
    if (fctx->umask == 0)
        umask(022);
    else
        umask(fctx->umask);
}

/* Regain root privileges if applicable */
static void regain(void)
{
    int store_errno = errno;
    seteuid(0);
    setegid(0);
    umask(0);
    errno = store_errno;
}

/* Attempt to make the directory component of this path */
static void mkdir_p(const char *path)
{
    char buf[PATH_MAX], *slash;

    strncpy(buf, path, PATH_MAX);
    buf[PATH_MAX-1] = 0;

    /* Make each component (except the last) in turn */
    slash = buf - 1;
    while ((slash = strchr(slash + 1, '/'))) {
        *slash = 0;
        mkdirat(perm_root, buf, 0777);
        *slash = '/';
    }
}

/* Attempt to make a full file to represent one in the store */
static void mkfull(const char *path, struct stat *sbuf)
{
    mkdir_p(path);
    if (S_ISDIR(sbuf->st_mode))
        mkdirat(perm_root, path, 0777);
    else
        mknodat(perm_root, path, 0666, 0);
}

static int upfs_getattr(const char *path, struct stat *sbuf)
{
    int ret, store_ret;
    struct stat store_buf;
    path++;

    drop();
    ret = fstatat(perm_root, path, sbuf, AT_SYMLINK_NOFOLLOW);
    regain();
    if (ret >= 0) {
        store_ret = fstatat(store_root, path, &store_buf, 0);
        if (store_ret < 0) return -errno;
        if (S_ISREG(sbuf->st_mode)) {
            sbuf->st_size = store_buf.st_size;
            sbuf->st_blksize = store_buf.st_blksize;
            sbuf->st_blocks = store_buf.st_blocks;
        }
        return ret;
    }
    if (errno != ENOENT) return -errno;

    ret = fstatat(store_root, path, sbuf, 0);
    if (ret >= 0) return ret;
    return -errno;
}

static int upfs_readlink(const char *path, char *buf, size_t buf_sz)
{
    ssize_t ret;
    struct stat sbuf;
    path++;

    drop();
    ret = readlinkat(perm_root, path, buf, buf_sz);
    regain();
    if (ret < 0) return -errno;

    ret = fstatat(store_root, path, &sbuf, 0);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    int ret;
    path++;

    /* Create the full thing on the perms fs */
    drop();
    ret = mknodat(perm_root, path, mode, dev);
    regain();
    fprintf(stderr, " %d\n", ret);
    if (ret < 0) return -errno;

    /* Then create an empty file to represent it on the store */
    ret = mknodat(store_root, path, S_IFREG|0600, 0);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_mkdir(const char *path, mode_t mode)
{
    int ret;
    path++;

    drop();
    ret = mkdirat(perm_root, path, mode);
    regain();
    if (ret < 0) return -errno;

    ret = mkdirat(store_root, path, 0700);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_unlink(const char *path)
{
    int perm_ret, store_ret;
    path++;

    drop();
    perm_ret = unlinkat(perm_root, path, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = unlinkat(store_root, path, 0);
    if (store_ret < 0) return -errno;
    return 0;
}

static int upfs_rmdir(const char *path)
{
    int perm_ret, store_ret;
    path++;

    drop();
    perm_ret = unlinkat(perm_root, path, AT_REMOVEDIR);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = unlinkat(store_root, path, AT_REMOVEDIR);
    if (store_ret < 0) return -errno;
    return 0;
}

static int upfs_symlink(const char *target, const char *path)
{
    int ret;
    path++;

    drop();
    ret = symlinkat(target, perm_root, path);
    regain();
    if (ret < 0 && errno == ENOENT) {
        /* Maybe need to create containing directories */
        drop();
        mkdir_p(path);
        ret = symlinkat(target, perm_root, path);
        regain();
    }
    if (ret < 0) return -errno;

    ret = mknodat(store_root, path, S_IFREG|0600, 0);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_rename(const char *from, const char *to)
{
    int perm_ret, store_ret;
    from++;
    to++;

    drop();
    perm_ret = renameat(perm_root, from, perm_root, to);
    regain();
    if (perm_ret < 0 && errno == ENOENT) {
        drop();
        mkdir_p(to);
        perm_ret = renameat(perm_root, from, perm_root, to);
        regain();
    }
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = renameat(store_root, from, store_root, to);
    if (store_ret < 0) return -errno;
    return 0;
}

static int upfs_chmod(const char *path, mode_t mode)
{
    int perm_ret, store_ret;
    struct stat sbuf;
    path++;

    drop();
    perm_ret = fchmodat(perm_root, path, mode, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = fstatat(store_root, path, &sbuf, 0);
    if (store_ret < 0) return -errno;

    if (perm_ret < 0) {
        drop();
        mkfull(path, &sbuf);
        perm_ret = fchmodat(perm_root, path, mode, 0);
        regain();
    }

    if (perm_ret < 0) return -errno;
    return 0;
}

static int upfs_chown(const char *path, uid_t uid, gid_t gid)
{
    int perm_ret, store_ret;
    struct stat sbuf;
    path++;

    drop();
    perm_ret = fchownat(perm_root, path, uid, gid, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = fstatat(store_root, path, &sbuf, 0);
    if (store_ret < 0) return -errno;

    if (perm_ret < 0) {
        drop();
        mkfull(path, &sbuf);
        perm_ret = fchownat(perm_root, path, uid, gid, 0);
        regain();
    }

    if (perm_ret < 0) return -errno;
    return 0;
}

static int upfs_truncate(const char *path, off_t length)
{
    int ret;
    int perm_fd = -1, store_fd = -1;
    int save_errno;
    path++;

    drop();
    perm_fd = openat(perm_root, path, O_RDWR);
    regain();
    if (perm_fd < 0 && errno != ENOENT) return -errno;

    store_fd = openat(store_root, path, O_RDWR);
    if (store_fd < 0) goto error;

    if (perm_fd < 0) {
        struct stat sbuf;
        ret = fstatat(store_root, path, &sbuf, 0);
        if (ret < 0) return -errno;
        drop();
        mkfull(path, &sbuf);
        perm_fd = openat(perm_root, path, O_RDWR);
        regain();
    }

    if (perm_fd < 0) goto error;

    ret = ftruncate(store_fd, length);
    if (ret < 0) goto error;

    close(perm_fd);
    close(store_fd);
    return 0;

error:
    save_errno = errno;
    if (perm_fd >= 0) close(perm_fd);
    if (store_fd >= 0) close(store_fd);
    return -save_errno;
}

static int upfs_open(const char *path, struct fuse_file_info *ffi)
{
    int ret;
    int perm_fd = -1, store_fd = -1;
    int save_errno;
    path++;

    drop();
    perm_fd = openat(perm_root, path, ffi->flags, 0);
    regain();
    if (perm_fd < 0 && errno != ENOENT) return -errno;

    store_fd = openat(store_root, path, ffi->flags, 0);
    if (store_fd < 0) goto error;

    if (perm_fd < 0) {
        struct stat sbuf;
        ret = fstatat(store_root, path, &sbuf, 0);
        if (ret < 0) goto error;
        drop();
        mkfull(path, &sbuf);
        perm_fd = openat(perm_root, path, ffi->flags, 0);
        regain();
    }
    if (perm_fd < 0) goto error;

    ffi->direct_io = 1;
    ffi->fh = ((uint64_t) perm_fd << 32) + store_fd;
    return 0;

error:
    save_errno = errno;
    if (perm_fd >= 0) close(perm_fd);
    if (store_fd >= 0) close(store_fd);
    return -save_errno;
}

static int upfs_read(const char *ignore, char *buf, size_t size, off_t offset,
    struct fuse_file_info *ffi)
{
    int ret;
    int fd;

    if (!ffi) return -ENOTSUP;

    fd = (ffi->fh & (uint32_t) -1);
    ret = pread(fd, buf, size, offset);
    if (ret < 0) return -errno;

    return ret;
}

static int upfs_write(const char *ignore, const char *buf, size_t size,
    off_t offset, struct fuse_file_info *ffi)
{
    int ret;
    int fd;

    if (!ffi) return -ENOTSUP;

    fd = (ffi->fh & (uint32_t) -1);
    ret = pwrite(fd, buf, size, offset);
    if (ret < 0) return -errno;

    return ret;
}

static int upfs_flush(const char *ignore, struct fuse_file_info *ffi)
{
    int ret;
    int fd;

    if (!ffi) return -ENOTSUP;

    fd = (ffi->fh & (uint32_t) -1);
    fd = dup(fd);
    if (fd < 0) return -errno;
    ret = close(fd);
    if (ret < 0) return -errno;

    return 0;
}

static int upfs_release(const char *ignore, struct fuse_file_info *ffi)
{
    int perm_fd, store_fd;

    if (!ffi) return -ENOTSUP;

    perm_fd = ffi->fh >> 32;
    store_fd = (ffi->fh & (uint32_t) -1);
    close(perm_fd);
    close(store_fd);

    return 0;
}

static int upfs_fsync(const char *ignore, int datasync, struct fuse_file_info *ffi)
{
    int fd, ret;

    if (!ffi) return -ENOTSUP;

    fd = (ffi->fh & (uint32_t) -1);
    if (datasync)
        ret = fdatasync(fd);
    else
        ret = fsync(fd);

    if (ret < 0) return -errno;
    return 0;
}

static struct fuse_operations upfs_operations = {
    .getattr = upfs_getattr,
    .readlink = upfs_readlink,
    .mknod = upfs_mknod,
    .mkdir = upfs_mkdir,
    .unlink = upfs_unlink,
    .rmdir = upfs_rmdir,
    .symlink = upfs_symlink,
    .rename = upfs_rename,
    .chmod = upfs_chmod,
    .chown = upfs_chown,
    .truncate = upfs_truncate,
    .open = upfs_open,
    .read = upfs_read,
    .write = upfs_write,
    .flush = upfs_flush,
    .release = upfs_release,
    .fsync = upfs_fsync
};

int main(int argc, char **argv)
{
    char *arg, **fuse_argv;
    int ai, fai;

    fuse_argv = calloc(argc + 1, sizeof(char *));
    if (!fuse_argv) {
        perror("calloc");
        return 1;
    }

    /* Extract the two arguments we care about, the permissions root and store
     * root */
    fuse_argv[0] = argv[0];
    for (ai = fai = 1; ai < argc; ai++) {
        arg = argv[ai];
        if (arg[0] == '-') {
            fuse_argv[fai++] = arg;
            if (arg[1] == 'o' && !arg[2])
                fuse_argv[fai++] = argv[++ai];

        } else if (!perm_root_path) {
            perm_root_path = arg;

        } else if (!store_root_path) {
            store_root_path = arg;

        } else {
            fuse_argv[fai++] = arg;

        }
    }

    if (!perm_root_path || !store_root_path) {
        fprintf(stderr, "Use: upfs <perm root> <store root> <mount point>\n");
        return 1;
    }

    /* Open the roots */
    perm_root = open(perm_root_path, O_RDONLY);
    if (perm_root < 0) {
        perror(perm_root_path);
        return 1;
    }
    store_root = open(store_root_path, O_RDONLY);
    if (store_root < 0) {
        perror(store_root_path);
        return 1;
    }

    /* And run FUSE */
    umask(0);
    return fuse_main(fai, fuse_argv, &upfs_operations, NULL);
}
