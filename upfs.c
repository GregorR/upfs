#define _XOPEN_SOURCE 700 /* *at */

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "upfs.h"

char *perm_root_path = NULL, *store_root_path = NULL;
int perm_root = -1, store_root = -1;

#define CHECK_RET() \
do { \
    if (ret >= 0) return ret; \
    if (errno != EEXIST) return -errno; \
} while(0)

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
}

static void regain(void)
{
    seteuid(0);
    setegid(0);
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
    if (errno != EEXIST) return -errno;

    ret = fstatat(store_root, path, sbuf, 0);
    CHECK_RET();
    return -EEXIST;
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

static struct fuse_operations upfs_operations = {
    .getattr = upfs_getattr,
    .readlink = upfs_readlink
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
