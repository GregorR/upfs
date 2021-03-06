#define _XOPEN_SOURCE 700 /* *at */

#include "upfs.h"

#define FUSE_USE_VERSION 28
#include <fuse.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/fsuid.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef UPFS_PS

/* Using permissions tables on the store */
#include "upfs-ps.h"
#define drop() do { } while(0)
#define regain() do { } while(0)
#define UPFS(func) upfs_ ## func

#else
/* Drop to caller privileges */
static void drop(void)
{
    struct fuse_context *fctx = fuse_get_context();
    if (setfsgid(fctx->gid) < 0) {
        perror("setfsgid");
        exit(1);
    }
    if (setfsuid(fctx->uid) < 0) {
        perror("setfsuid");
        exit(1);
    }
}

/* Regain root privileges if applicable */
static void regain(void)
{
    int store_errno = errno;
    setfsgid(0);
    setfsuid(0);
    errno = store_errno;
}

#define UPFS(func) func

#endif

/* Our root paths and fds */
static char *perm_root_path = NULL, *store_root_path = NULL;
int perm_root = -1, store_root = -1;

/* Convert paths for the store */
#ifdef UPFS_FATNAMES
/* Convert paths for FAT support */
static void store_path(char *store, const char *path)
{
    int o, i;
    for (o = i = 0; path[i] && i < PATH_MAX - 1 && o < PATH_MAX - 1; i++) {
        char c = path[i];
        switch (c) {
            case '"': case '?': case ':': case '*': case '|': case '<':
            case '>':
            case '$':
            case '\\':
                if (o + 4 >= PATH_MAX - 1)
                    break;
                store[o++] = '$';
                snprintf(store+o, 3, "%02x", (int) (unsigned char) c);
                o += 2;
                break;

            default:
#ifdef UPFS_FATLOWERCASE
                if (c >= 'A' && c <= 'Z') {
                    if (o + 4 >= PATH_MAX - 1)
                        break;
                    store[o++] = '$';
                    snprintf(store+o, 3, "%02x", (int) (unsigned char) c);
                    o += 2;
                } else
#endif
                store[o++] = c;
                break;
        }
    }
    store[o] = 0;
}
#else
static void store_path(char *store, const char *path)
{
    strncpy(store, path, PATH_MAX - 1);
    store[PATH_MAX-1] = 0;
}
#endif

/* Convert paths for our permissions directory */
#ifdef UPFS_PERMLOWERCASE
static void perm_path(char *perm, const char *path)
{
    int i;
    for (i = 0; path[i] && i < PATH_MAX - 1; i++) {
        char c = path[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        perm[i] = c;
    }
    perm[i] = 0;
}
#else
static void perm_path(char *perm, const char *path)
{
    strncpy(perm, path, PATH_MAX - 1);
    perm[PATH_MAX-1] = 0;
}
#endif

/* Correct incoming paths */
static void correct_path(const char *path, char *ppath, char *spath)
{
    if (path[0] == '/') path++;
    if (!path[0]) {
        strcpy(spath, ".");
        strcpy(ppath, ".");
        return;
    }
    store_path(spath, path);
    perm_path(ppath, path);
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
        UPFS(mkdirat)(perm_root, buf, 0777);
        *slash = '/';
    }
}

/* Attempt to make a full file to represent one in the store */
static void mkfull(const char *path, struct stat *sbuf)
{
    mkdir_p(path);
    if (S_ISDIR(sbuf->st_mode))
        UPFS(mkdirat)(perm_root, path, 0777);
    else
        UPFS(mknodat)(perm_root, path, 0666, 0);
}

static int upfs_stat(int perm_dirfd, int store_dirfd, const char *path, const char *spath, struct stat *sbuf)
{
    int ret, store_ret;
    struct stat store_buf;

    drop();
    ret = UPFS(fstatat)(perm_dirfd, path, sbuf, AT_SYMLINK_NOFOLLOW);
    regain();
    if (ret >= 0) {
        if (S_ISLNK(sbuf->st_mode)) {
            /* Links don't need a backing file, to support inter-case links */
            return ret;
        }

        store_ret = fstatat(store_dirfd, spath, &store_buf, 0);
        if (store_ret < 0) return -errno;
        if (S_ISREG(sbuf->st_mode)) {
            sbuf->st_size = store_buf.st_size;
            sbuf->st_blksize = store_buf.st_blksize;
            sbuf->st_blocks = store_buf.st_blocks;
        }
        return ret;
    }
    if (errno != ENOENT) return -errno;

    ret = fstatat(store_dirfd, spath, sbuf, 0);
    if (ret >= 0) return ret;
    return -errno;
}

static int upfs_getattr(const char *path, struct stat *sbuf)
{
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);
    return upfs_stat(perm_root, store_root, ppath, spath, sbuf);
}

static int upfs_readlink(const char *path, char *buf, size_t buf_sz)
{
    ssize_t ret;
    struct stat sbuf;
    char ppath[PATH_MAX], spath[PATH_MAX];
#ifdef UPFS_PS
    int fd;
#endif
    correct_path(path, ppath, spath);

#ifdef UPFS_PS
    /* We store the link target in the store file, so just check that it claims
     * to be a link */
    ret = UPFS(fstatat)(perm_root, ppath, &sbuf, 0);
    if (ret < 0) return -errno;
    if (!S_ISLNK(sbuf.st_mode))
        return -EINVAL;

    /* Then read it */
    fd = openat(store_root, spath, O_RDONLY);
    if (fd < 0) return -errno;
    ret = read(fd, buf, buf_sz-1);
    if (ret < 0) {
        int save_errno = errno;
        close(fd);
        return -save_errno;
    }
    close(fd);
    buf[ret] = 0;
    return 0;

#else
    /* The link is stored as a link in the permissions directory */
    drop();
    ret = UPFS(readlinkat)(perm_root, ppath, buf, buf_sz-1);
    regain();
    if (ret < 0) return -errno;
    buf[ret] = 0;

#if 0
    FIXME: For now, allow links with no backing file
    ret = fstatat(store_root, spath, &sbuf, 0);
    if (ret < 0) return -errno;
#endif
    return 0;

#endif
}

static int upfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    int ret;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    /* Create the full thing on the perms fs */
    drop();
    ret = UPFS(mknodat)(perm_root, ppath, mode, dev);
    regain();
    if (ret < 0) return -errno;

    /* Then create an empty file to represent it on the store */
    ret = mknodat(store_root, spath, S_IFREG|0600, 0);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_mkdir(const char *path, mode_t mode)
{
    int ret;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    drop();
    ret = UPFS(mkdirat)(perm_root, ppath, mode);
    regain();
    if (ret < 0) return -errno;

    ret = mkdirat(store_root, spath, 0700);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_unlink(const char *path)
{
    /* For removing files/directories, we need to do it in the opposite order
     * to assure no race conditions in permissions and visibility */
    int perm_ret, store_ret;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    store_ret = unlinkat(store_root, spath, 0);
    if (store_ret < 0 && errno != ENOENT) return -errno;

    drop();
    perm_ret = UPFS(unlinkat)(perm_root, ppath, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    if (store_ret < 0) return -ENOENT;
    return 0;
}

static int upfs_rmdir(const char *path)
{
    int perm_ret, store_ret;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

#ifdef UPFS_PS
    /* The index file will cause problems */
    UPFS(unlink_empty_index)(perm_root, ppath);
#endif

    store_ret = unlinkat(store_root, spath, AT_REMOVEDIR);
    if (store_ret < 0 && errno != ENOENT) return -errno;

    drop();
    perm_ret = UPFS(unlinkat)(perm_root, ppath, AT_REMOVEDIR);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    if (store_ret < 0) return -ENOENT;
    return 0;
}

static int upfs_symlink(const char *target, const char *path)
{
    int ret;
    char ppath[PATH_MAX], spath[PATH_MAX];
#ifdef UPFS_PS
    int fd;
    size_t target_sz;
#endif
    correct_path(path, ppath, spath);

#ifdef UPFS_PS
    {
        /* As a special case, we ignore this if it's just a case link (symlink("foo", "FOO")) */
        char path_parts[PATH_MAX];
        char *path_dir, *path_file;
        split_path(ppath, path_parts, &path_dir, &path_file, 0);
        if (!strcasecmp(path_file, target))
            return 0;
    }

    /* We store the symlink in the store, so do this totally differently */
    ret = UPFS(mknodat)(perm_root, ppath, S_IFREG, 0);
    if (ret < 0 && errno == ENOENT) {
        /* Create containing directories */
        mkdir_p(ppath);
        ret = UPFS(mknodat)(perm_root, ppath, S_IFREG, 0);
    }
    if (ret < 0) return -errno;

    /* Now write the symlink file */
    fd = openat(store_root, spath, O_WRONLY|O_CREAT|O_EXCL, 0600);
    if (fd < 0) return -errno;
    target_sz = strlen(target);
    if (write(fd, target, target_sz) != target_sz) {
        int save_errno = errno;
        close(fd);
        if (save_errno) return -save_errno;
        return -EIO;
    }
    close(fd);

    /* Then replace it in the permissions */
    ret = UPFS(fchmodat_harder)(perm_root, ppath, S_IFLNK|0644, 0);
    if (ret < 0) return -errno;

    return 0;

#else
    drop();
    ret = UPFS(symlinkat)(target, perm_root, ppath);
    regain();
    if (ret < 0 && errno == ENOENT) {
        /* Maybe need to create containing directories */
        drop();
        mkdir_p(ppath);
        ret = UPFS(symlinkat)(target, perm_root, ppath);
        regain();
    }
    if (ret < 0) return -errno;

    ret = mknodat(store_root, spath, S_IFREG|0600, 0);
    if (ret < 0) {
#ifdef UPFS_FATNAMES /* FIXME: Not really related */
        if (errno == EEXIST) {
            /* As a special case, we ignore this if it's just a case link (symlink("foo", "FOO")) */
            char path_parts[PATH_MAX];
            char *path_dir, *path_file;
            split_path(ppath, path_parts, &path_dir, &path_file, 0);
            if (!strcasecmp(path_file, target))
                return 0;
        }
#endif
        return -errno;
    }
    return 0;

#endif
}

static int upfs_rename(const char *from, const char *to)
{
    int perm_ret, store_ret;
    struct stat sbuf;
    int dir = 0;
    char from_parts[PATH_MAX], to_parts[PATH_MAX];
    char *from_dir, *from_file, *to_dir, *to_file;
    int from_dir_fd = -1, to_dir_fd = -1;
    int made_placeholder = 0;
    int save_errno;
    char pfrom[PATH_MAX], sfrom[PATH_MAX], pto[PATH_MAX], sto[PATH_MAX];
    correct_path(from, pfrom, sfrom);
    correct_path(to, pto, sto);

    /* To avoid directory renaming causing issues and assure some kind of
     * atomicity, get directory handles first */
    split_path(pfrom, from_parts, &from_dir, &from_file, 0);
    split_path(pto, to_parts, &to_dir, &to_file, 0);
    drop();
    from_dir_fd = openat(perm_root, from_dir, O_RDONLY, 0);
    regain();
    if (from_dir_fd < 0) goto error;
    drop();
    to_dir_fd = openat(perm_root, to_dir, O_RDONLY, 0);
    regain();
    if (to_dir_fd < 0) goto error;

    /* Get the properties of the original file */
    drop();
    perm_ret = UPFS(fstatat)(from_dir_fd, from_file, &sbuf, AT_SYMLINK_NOFOLLOW);
    regain();
    if (perm_ret < 0) {
        if (errno != ENOENT) goto error;

        /* Doesn't exist in the permissions, so just rename in the store */
        close(from_dir_fd);
        close(to_dir_fd);
        from_dir_fd = to_dir_fd = -1;
        store_ret = renameat(store_root, sfrom, store_root, sto);
        if (store_ret < 0) goto error;
        errno = 0;
        goto error;
    }
    dir = S_ISDIR(sbuf.st_mode);

    /* Set up an inaccessible new file to prevent tampering */
    drop();
    mkdir_p(pto);
    if (dir)
        perm_ret = UPFS(mkdirat)(to_dir_fd, to_file, 0);
    else
        perm_ret = UPFS(mknodat)(to_dir_fd, to_file, 0, 0);
    regain();
    made_placeholder = 1;
    if (perm_ret < 0 && errno == EEXIST) {
        /* We're overwriting a file. Just set its permissions to nil, unless it's a symlink. */
        drop();
        perm_ret = UPFS(fstatat)(to_dir_fd, to_file, &sbuf, AT_SYMLINK_NOFOLLOW);
        regain();
        if (perm_ret < 0 || !S_ISLNK(sbuf.st_mode)) {
            drop();
            perm_ret = UPFS(fchmodat)(to_dir_fd, to_file, 0, 0);
            regain();
        }
        made_placeholder = 0;
    }
    if (perm_ret < 0) goto error;

    /* Rename it in the store */
    store_ret = renameat(store_root, sfrom, store_root, sto);
    if (store_ret < 0) goto error;

    /* And rename it in the permissions */
    drop();
    perm_ret = UPFS(renameat)(from_dir_fd, from_file, to_dir_fd, to_file);
    regain();
    if (perm_ret < 0) goto error;

    close(from_dir_fd);
    close(to_dir_fd);
    return 0;

error:
    save_errno = errno;
    if (from_dir_fd >= 0) close(from_dir_fd);
    if (to_dir_fd >= 0) {
        if (made_placeholder)
            UPFS(unlinkat)(to_dir_fd, to_file, dir?AT_REMOVEDIR:0);
        close(to_dir_fd);
    }
    errno = save_errno;
    return -errno;
}

#ifdef UPFS_LNCP
/* A fake implementation of link through copying that may be good enough for
 * some purposes */
static int upfs_lncp(const char *from, const char *to)
{
#define BUFSZ 4096
    int perm_ret, store_ret;
    struct stat sbuf;
    char from_parts[PATH_MAX], to_parts[PATH_MAX];
    char *from_dir, *from_file, *to_dir, *to_file;
    int from_dir_fd = -1, to_dir_fd = -1;
    int from_file_fd = -1, to_file_fd = -1;
    char *buf = NULL;
    ssize_t rd;
    int save_errno;
    char pfrom[PATH_MAX], sfrom[PATH_MAX], pto[PATH_MAX], sto[PATH_MAX];
    correct_path(from, pfrom, sfrom);
    correct_path(to, pto, sto);

    /* To avoid directory renaming causing issues and assure some kind of
     * atomicity, get directory handles first */
    split_path(pfrom, from_parts, &from_dir, &from_file, 0);
    split_path(pto, to_parts, &to_dir, &to_file, 0);
    drop();
    from_dir_fd = UPFS(openat)(perm_root, from_dir, O_RDONLY|O_DIRECTORY, 0);
    regain();
    if (from_dir_fd < 0) goto error;
    drop();
    to_dir_fd = UPFS(openat)(perm_root, to_dir, O_RDONLY|O_DIRECTORY, 0);
    regain();
    if (to_dir_fd < 0) goto error;

    /* Get the properties of the original file */
    drop();
    perm_ret = UPFS(fstatat)(from_dir_fd, from_file, &sbuf, AT_SYMLINK_NOFOLLOW);
    regain();
    if (perm_ret < 0) {
        if (errno != ENOENT) goto error;

        perm_ret = fstatat(store_root, sfrom, &sbuf, AT_SYMLINK_NOFOLLOW);
        if (perm_ret < 0) goto error;
    }

    /* Only files can be linked in this way for now */
    if (!S_ISREG(sbuf.st_mode)) {
        errno = EPERM;
        goto error;
    }

    /* Set up the new permissions file */
    drop();
    mkdir_p(pto);
    perm_ret = UPFS(mknodat)(to_dir_fd, to_file, sbuf.st_mode, 0);
    regain();
    if (perm_ret < 0) goto error;

    /* Copy it in the store */
    from_file_fd = openat(store_root, sfrom, O_RDONLY);
    if (from_file_fd < 0) goto error;
    to_file_fd = openat(store_root, sto, O_WRONLY|O_CREAT|O_EXCL);
    if (to_file_fd < 0) goto error;
    buf = malloc(BUFSZ);
    if (!buf) goto error;
    while ((rd = read(from_file_fd, buf, BUFSZ)) > 0) {
        if (write(to_file_fd, buf, rd) != rd) goto error;
    }
    if (rd < 0) goto error;
    free(buf);
    buf = NULL;
    close(to_file_fd);
    to_file_fd = -1;
    close(from_file_fd);
    from_file_fd = -1;

    close(from_dir_fd);
    close(to_dir_fd);
    return 0;

error:
    save_errno = errno;
    if (from_dir_fd >= 0) close(from_dir_fd);
    if (to_dir_fd >= 0) close(to_dir_fd);
    if (from_file_fd >= 0) close(from_file_fd);
    if (to_file_fd >= 0) close(to_file_fd);
    if (buf) free(buf);
    errno = save_errno;
    return -errno;
}
#endif

static int upfs_chmod(const char *path, mode_t mode)
{
    int perm_ret, store_ret;
    struct stat sbuf;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    drop();
    perm_ret = UPFS(fchmodat)(perm_root, ppath, mode, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = fstatat(store_root, spath, &sbuf, 0);
    if (store_ret < 0) return -errno;

    if (perm_ret < 0) {
        drop();
        mkfull(ppath, &sbuf);
        perm_ret = UPFS(fchmodat)(perm_root, ppath, mode, 0);
        regain();
    }

    if (perm_ret < 0) return -errno;
    return 0;
}

static int upfs_chown(const char *path, uid_t uid, gid_t gid)
{
    int perm_ret, store_ret;
    struct stat sbuf;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    drop();
    perm_ret = UPFS(fchownat)(perm_root, ppath, uid, gid, AT_SYMLINK_NOFOLLOW);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = fstatat(store_root, spath, &sbuf, 0);
    if (store_ret < 0) return -errno;

    if (perm_ret < 0) {
        drop();
        mkfull(ppath, &sbuf);
        perm_ret = UPFS(fchownat)(perm_root, ppath, uid, gid, AT_SYMLINK_NOFOLLOW);
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
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    drop();
    perm_fd = UPFS(openat)(perm_root, ppath, O_RDWR, 0);
    regain();
    if (perm_fd < 0 && errno != ENOENT) return -errno;

    store_fd = openat(store_root, spath, O_RDWR);
    if (store_fd < 0) goto error;

    if (perm_fd < 0) {
        struct stat sbuf;
        ret = fstatat(store_root, spath, &sbuf, 0);
        if (ret < 0) return -errno;
        drop();
        mkfull(ppath, &sbuf);
        perm_fd = UPFS(openat)(perm_root, ppath, O_RDWR, 0);
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
    struct stat sbuf;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    drop();
    perm_fd = UPFS(openat)(perm_root, ppath, ffi->flags, 0);
    regain();
    if (perm_fd < 0 && errno != ENOENT) return -errno;

    /* If this is a special file of any kind, use perm_fd directly */
    if (perm_fd >= 0) {
        ret = fstat(perm_fd, &sbuf);
        if (!S_ISREG(sbuf.st_mode) && !S_ISDIR(sbuf.st_mode)) {
            store_fd = dup(perm_fd);
            if (store_fd < 0) goto error;
            ffi->direct_io = 1;
            ffi->nonseekable = 1;
        }
    }

    if (store_fd < 0) {
        store_fd = openat(store_root, spath, ffi->flags, 0);
        if (store_fd < 0) goto error;
    }

    if (perm_fd < 0) {
        struct stat sbuf;
        ret = fstatat(store_root, spath, &sbuf, 0);
        if (ret < 0) goto error;
        drop();
        mkfull(ppath, &sbuf);
        perm_fd = UPFS(openat)(perm_root, ppath, ffi->flags, 0);
        regain();
    }
    if (perm_fd < 0) goto error;

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
    if (ffi->nonseekable)
        ret = read(fd, buf, size);
    else
        ret = pread(fd, buf, size, offset);
    if (ret < 0) return -errno;

    return ret;
}

static int upfs_write(const char *ignore, const char *buf, size_t size,
    off_t offset, struct fuse_file_info *ffi)
{
    int ret;
    int perm_fd, store_fd;

    if (!ffi) return -ENOTSUP;

    perm_fd = ffi->fh >> 32;
    (void) perm_fd;
    store_fd = (ffi->fh & (uint32_t) -1);
    if (ffi->nonseekable)
        ret = write(store_fd, buf, size);
    else
        ret = pwrite(store_fd, buf, size, offset);
    if (ret < 0) return -errno;

#ifndef UPFS_PS
    /* For performance reasons, with permissions in store we do this only once,
     * at the end */
    futimens(perm_fd, NULL);
#endif

    return ret;
}

static int upfs_statfs(const char *ignore, struct statvfs *sbuf)
{
    int ret = fstatvfs(store_root, sbuf);
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
    if (ffi->flags & O_WRONLY)
        UPFS(futimens)(perm_fd, NULL);
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

static int upfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *ffi)
{
    int save_errno;
    int ret;
    int perm_fd = -1, store_fd = -1, store_fd2 = -1;
    DIR *dh = NULL;
    struct dirent *de;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    /* Check permissions */
    drop();
    perm_fd = UPFS(openat)(perm_root, ppath, O_RDONLY|O_DIRECTORY, 0);
    regain();
    if (perm_fd < 0 && errno != ENOENT) return -errno;

    /* Open the store directory */
    store_fd = openat(store_root, spath, O_RDONLY, 0);
    if (store_fd < 0) goto error;

    /* Prepare for readdir */
    store_fd2 = dup(store_fd);
    if (store_fd2 < 0) goto error;

    dh = fdopendir(store_fd2);
    if (!dh) goto error;
    store_fd2 = -1;

    /* And read it */
    while ((de = readdir(dh)) != NULL) {
        struct stat sbuf;
        char pd_name[NAME_MAX];
#ifdef UPFS_PS
        /* Skip the metafile */
        if (!strcmp(de->d_name, UPFS_META_FILE))
            continue;
#endif

#ifdef UPFS_FATNAMES
        /* Convert the name back from mangling */
        {
            int o, i;
            for (o = i = 0;
                 de->d_name[i] && o < NAME_MAX - 1;
                 i++) {
                char c = de->d_name[i];
                if (c == '$' && de->d_name[i+1] && de->d_name[i+2]) {
                    char h[3];
                    h[0] = de->d_name[i+1];
                    h[1] = de->d_name[i+2];
                    h[2] = 0;
                    c = strtol(h, NULL, 16);
                    i += 2;
                }
                pd_name[o++] = c;
            }
            pd_name[o] = 0;
        }
#endif

        if (perm_fd >= 0) {
            ret = upfs_stat(perm_fd, store_fd, pd_name, de->d_name, &sbuf);
            if (ret < 0) {
                errno = -ret;
                goto error;
            }
        } else {
            ret = fstatat(store_fd, de->d_name, &sbuf, 0);
            if (ret < 0) goto error;
        }
        if (filler(buf, pd_name, &sbuf, 0))
            break;
    }

    /* Then clean up */
    closedir(dh);
    close(store_fd);
    if (perm_fd >= 0)
        close(perm_fd);
    return 0;

error:
    save_errno = errno;
    if (perm_fd >= 0) close(perm_fd);
    if (store_fd >= 0) close(store_fd);
    if (store_fd2 >= 0) close(store_fd2);
    if (dh) closedir(dh);
    return -save_errno;
}

static int upfs_access(const char *path, int mode)
{
    int ret;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

#ifndef UPFS_PS
    /* If we were using permission files in the store, we should be using
     * default_permissions, and if not, everything goes */
    drop();
    ret = UPFS(faccessat)(perm_root, ppath, mode, AT_EACCESS);
    regain();
    if (ret < 0 && errno != ENOENT) return -errno;
#endif

    /* Don't check execute bit on store FS */
    if (mode & X_OK) {
        mode &= ~(X_OK);
        if (!mode) mode = R_OK;
    }
    ret = faccessat(store_root, spath, mode, 0);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_create(const char *path, mode_t mode,
    struct fuse_file_info *ffi)
{
    int perm_fd = -1, store_fd = -1;
    int save_errno;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    drop();
    perm_fd = UPFS(openat)(perm_root, ppath, O_RDWR|O_CREAT|O_EXCL, mode);
    regain();
    if (perm_fd < 0) return -errno;

    store_fd = openat(store_root, spath, O_RDWR|O_CREAT|O_EXCL, 0600);
    if (store_fd < 0) goto error;

    ffi->fh = ((uint64_t) perm_fd << 32) + store_fd;
    return 0;

error:
    save_errno = errno;
    if (perm_fd >= 0) close(perm_fd);
    if (store_fd >= 0) close(store_fd);
    return -save_errno;
}

static int upfs_ftruncate(const char *ignore, off_t length, struct fuse_file_info *ffi)
{
    int perm_fd, store_fd, ret;

    if (!ffi) return -ENOTSUP;

    perm_fd = ffi->fh >> 32;
    (void) perm_fd;
    store_fd = (ffi->fh & (uint32_t) -1);
    ret = ftruncate(store_fd, length);
    if (ret < 0) return -errno;
#ifndef UPFS_PS
    UPFS(futimens)(perm_fd, NULL);
#endif

    return 0;
}

static int upfs_fgetattr(const char *path, struct stat *sbuf, struct fuse_file_info *ffi)
{
#ifdef UPFS_PS
    /* We just have to stat the path and hope it hasn't been changed */
    return upfs_getattr(path, sbuf);

#else
    int perm_fd, store_fd;
    int ret;
    struct stat store_buf;

    if (!ffi) return -ENOTSUP;

    perm_fd = ffi->fh >> 32;
    store_fd = (ffi->fh & (uint32_t) -1);

    ret = fstat(perm_fd, sbuf);
    if (ret < 0) return -errno;

    if (S_ISREG(sbuf->st_mode)) {
        ret = fstat(store_fd, &store_buf);
        if (ret < 0) return -errno;
        sbuf->st_size = store_buf.st_size;
        sbuf->st_blksize = store_buf.st_blksize;
        sbuf->st_blocks = store_buf.st_blocks;
    }

#endif

    return 0;
}

static int upfs_lock(const char *ignore, struct fuse_file_info *ffi, int cmd,
    struct flock *fl)
{
    int fd, ret;

    if (!ffi) return -ENOTSUP;

    fd = (ffi->fh & (uint32_t) -1);
    ret = fcntl(fd, cmd, fl);
    if (ret < 0) return -errno;

    return 0;
}

static int upfs_utimens(const char *path, const struct timespec times[2])
{
    int perm_ret, store_ret;
    struct stat sbuf;
    char ppath[PATH_MAX], spath[PATH_MAX];
    correct_path(path, ppath, spath);

    drop();
    perm_ret = UPFS(utimensat)(perm_root, ppath, times, AT_SYMLINK_NOFOLLOW);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = fstatat(store_root, spath, &sbuf, 0);
    if (store_ret < 0) return -errno;

    if (perm_ret < 0) {
        drop();
        mkfull(ppath, &sbuf);
        perm_ret = UPFS(utimensat)(perm_root, ppath, times, AT_SYMLINK_NOFOLLOW);
        regain();
    }
    if (perm_ret < 0) return -errno;

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
#ifdef UPFS_LNCP
    .link = upfs_lncp,
#endif
    .chmod = upfs_chmod,
    .chown = upfs_chown,
    .truncate = upfs_truncate,
    .open = upfs_open,
    .read = upfs_read,
    .write = upfs_write,
    .statfs = upfs_statfs,
    .flush = upfs_flush,
    .release = upfs_release,
    .fsync = upfs_fsync,
    .readdir = upfs_readdir,
    .access = upfs_access,
    .create = upfs_create,
    .ftruncate = upfs_ftruncate,
    .fgetattr = upfs_fgetattr,
    .lock = upfs_lock,
    .utimens = upfs_utimens
};

int main(int argc, char **argv)
{
    char *arg, **fuse_argv;
    int ai, fai;
    struct stat sbuf;

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

#ifdef UPFS_PS
        } else if (!perm_root_path) {
            perm_root_path = store_root_path = arg;

#else
        } else if (!perm_root_path) {
            perm_root_path = arg;

        } else if (!store_root_path) {
            store_root_path = arg;
#endif

        } else {
            fuse_argv[fai++] = arg;

        }
    }

    if (!perm_root_path || !store_root_path) {
#ifdef UPFS_PS
        fprintf(stderr, "Use: upfs-ps <root> <mount point>\n");
#else
        fprintf(stderr, "Use: upfs <perm root> <store root> <mount point>\n");
#endif
        return 1;
    }

    /* Open the roots */
#define OPEN_ROOT(root) do { \
    root ## _root = open(root ## _root_path, O_RDONLY); \
    if (root ## _root < 0) { \
        perror(root ## _root_path); \
        return 1; \
    } \
    if (fstat(root ## _root, &sbuf) < 0) { \
        perror(root ## _root_path); \
        return 1; \
    } \
    if (!S_ISDIR(sbuf.st_mode)) { \
        fprintf(stderr, "%s: Must be directory\n", root ## _root_path); \
        return 1; \
    } \
} while(0)

#ifndef UPFS_PS
    OPEN_ROOT(perm);
#endif
    OPEN_ROOT(store);
#ifdef UPFS_PS
    perm_root = store_root;
#endif

    /* And run FUSE */
    umask(0);
    return fuse_main(fai, fuse_argv, &upfs_operations, NULL);
}
