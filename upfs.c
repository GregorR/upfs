#define _XOPEN_SOURCE 700 /* *at */

#define FUSE_USE_VERSION 28
#include <fuse.h>

#include <errno.h>
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
static void mkdirP(int dirfd, const char *path, mode_t mode)
{
    char buf[PATH_MAX], *slash;

    strncpy(buf, path, PATH_MAX);
    buf[PATH_MAX-1] = 0;

    /* Make each component (except the last) in turn */
    slash = buf - 1;
    while ((slash = strchr(slash + 1, '/'))) {
        *slash = 0;
        mkdirat(dirfd, buf, mode);
        *slash = '/';
    }
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
        if (S_ISREG(sbuf->st_mode)) {
            store_ret = fstatat(store_root, path, &store_buf, 0);
            if (store_ret >= 0) {
                sbuf->st_size = store_buf.st_size;
                sbuf->st_blksize = store_buf.st_blksize;
                sbuf->st_blocks = store_buf.st_blocks;
            }
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
    path++;

    drop();
    ret = readlinkat(perm_root, path, buf, buf_sz);
    regain();
    if (ret >= 0) return 0;
    return -errno;
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
    if (ret >= 0) return ret;
    return -errno;
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
    if (ret >= 0) return ret;
    return -errno;
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
    if (store_ret < 0 && errno != ENOENT) return -errno;

    if (perm_ret >= 0 || store_ret >= 0) return 0;
    return -ENOENT;
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
    if (store_ret < 0 && errno != ENOENT) return -errno;

    if (perm_ret >= 0 || store_ret >= 0) return 0;
    return -ENOENT;
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
        mkdirP(perm_root, path, 0777);
        ret = symlinkat(target, perm_root, path);
        regain();
    }
    if (ret < 0) return -errno;

    ret = mknodat(store_root, path, S_IFREG|0600, 0);
    if (ret < 0 && errno == ENOENT) {
        mkdirP(store_root, path, 0700);
        ret = mknodat(store_root, path, S_IFREG|0600, 0);
    }
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
    .symlink = upfs_symlink
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
