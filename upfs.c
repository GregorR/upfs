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

/* Correct incoming paths */
static const char *correct_path(const char *path)
{
    if (path[0] == '/') path++;
    if (!path[0]) return ".";
    return path;
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

static int upfs_stat(int perm_dirfd, int store_dirfd, const char *path, struct stat *sbuf)
{
    int ret, store_ret;
    struct stat store_buf;

    drop();
    ret = UPFS(fstatat)(perm_dirfd, path, sbuf, AT_SYMLINK_NOFOLLOW);
    regain();
    if (ret >= 0) {
        store_ret = fstatat(store_dirfd, path, &store_buf, 0);
        if (store_ret < 0) return -errno;
        if (S_ISREG(sbuf->st_mode)) {
            sbuf->st_size = store_buf.st_size;
            sbuf->st_blksize = store_buf.st_blksize;
            sbuf->st_blocks = store_buf.st_blocks;
        }
        return ret;
    }
    if (errno != ENOENT) return -errno;

    ret = fstatat(store_dirfd, path, sbuf, 0);
    if (ret >= 0) return ret;
    return -errno;
}

static int upfs_getattr(const char *path, struct stat *sbuf)
{
    return upfs_stat(perm_root, store_root, correct_path(path), sbuf);
}

static int upfs_readlink(const char *path, char *buf, size_t buf_sz)
{
    ssize_t ret;
    struct stat sbuf;
#ifdef UPFS_PS
    int fd;
#endif
    path = correct_path(path);

#ifdef UPFS_PS
    /* We store the link target in the store file, so just check that it claims
     * to be a link */
    ret = UPFS(fstatat)(perm_root, path, &sbuf, 0);
    if (ret < 0) return -errno;
    if (!S_ISLNK(sbuf.st_mode))
        return -EINVAL;

    /* Then read it */
    fd = openat(store_root, path, O_RDONLY);
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
    ret = UPFS(readlinkat)(perm_root, path, buf, buf_sz-1);
    regain();
    if (ret < 0) return -errno;
    buf[ret] = 0;

    ret = fstatat(store_root, path, &sbuf, 0);
    if (ret < 0) return -errno;
    return 0;

#endif
}

static int upfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    int ret;
    path = correct_path(path);

    /* Create the full thing on the perms fs */
    drop();
    ret = UPFS(mknodat)(perm_root, path, mode, dev);
    regain();
    if (ret < 0) return -errno;

    /* Then create an empty file to represent it on the store */
    ret = mknodat(store_root, path, S_IFREG|0600, 0);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_mkdir(const char *path, mode_t mode)
{
    int ret;
    path = correct_path(path);

    drop();
    ret = UPFS(mkdirat)(perm_root, path, mode);
    regain();
    if (ret < 0) return -errno;

    ret = mkdirat(store_root, path, 0700);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_unlink(const char *path)
{
    /* For removing files/directories, we need to do it in the opposite order
     * to assure no race conditions in permissions and visibility */
    int perm_ret, store_ret;
    path = correct_path(path);

    store_ret = unlinkat(store_root, path, 0);
    if (store_ret < 0 && errno != ENOENT) return -errno;

    drop();
    perm_ret = UPFS(unlinkat)(perm_root, path, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    if (store_ret < 0) return -ENOENT;
    return 0;
}

static int upfs_rmdir(const char *path)
{
    int perm_ret, store_ret;
    path = correct_path(path);

    store_ret = unlinkat(store_root, path, AT_REMOVEDIR);
    if (store_ret < 0 && errno != ENOENT) return -errno;

    drop();
    perm_ret = UPFS(unlinkat)(perm_root, path, AT_REMOVEDIR);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    if (store_ret < 0) return -ENOENT;
    return 0;
}

static int upfs_symlink(const char *target, const char *path)
{
    int ret;
#ifdef UPFS_PS
    int fd;
    size_t target_sz;
#endif
    path = correct_path(path);

#ifdef UPFS_PS
    /* We store the symlink in the store, so do this totally differently */
    ret = UPFS(mknodat)(perm_root, path, S_IFREG, 0);
    if (ret < 0 && errno == ENOENT) {
        /* Create containing directories */
        mkdir_p(path);
        ret = UPFS(mknodat)(perm_root, path, S_IFREG, 0);
    }
    if (ret < 0) return -errno;

    /* Now write the symlink file */
    fd = openat(store_root, path, O_CREAT|O_EXCL, 0600);
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
    ret = UPFS(fchmodat_harder)(perm_root, path, S_IFLNK|0644, 0);
    if (ret < 0) return -errno;

    return 0;

#else
    drop();
    ret = UPFS(symlinkat)(target, perm_root, path);
    regain();
    if (ret < 0 && errno == ENOENT) {
        /* Maybe need to create containing directories */
        drop();
        mkdir_p(path);
        ret = UPFS(symlinkat)(target, perm_root, path);
        regain();
    }
    if (ret < 0) return -errno;

    ret = mknodat(store_root, path, S_IFREG|0600, 0);
    if (ret < 0) return -errno;
    return 0;

#endif
}

static int upfs_rename(const char *from, const char *to)
{
    int perm_ret, store_ret;
    struct stat sbuf;
    char from_parts[PATH_MAX], to_parts[PATH_MAX];
    char *from_dir, *from_file, *to_dir, *to_file;
    int from_dir_fd = -1, to_dir_fd = -1;
    int save_errno;
    from = correct_path(from);
    to = correct_path(to);

    /* To avoid directory renaming causing issues and assure some kind of
     * atomicity, get directory handles first */
    split_path(from, from_parts, &from_dir, &from_file, 0);
    split_path(to, to_parts, &to_dir, &to_file, 0);
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
    perm_ret = UPFS(fstatat)(from_dir_fd, from_file, &sbuf, 0);
    regain();
    if (perm_ret < 0) {
        if (errno != ENOENT) goto error;

        /* Doesn't exist in the permissions, so just rename in the store */
        close(from_dir_fd);
        close(to_dir_fd);
        from_dir_fd = to_dir_fd = -1;
        store_ret = renameat(store_root, from, store_root, to);
        if (store_ret < 0) goto error;
        return 0;
    }

    /* Set up an inaccessible new file to prevent tampering */
    drop();
    mkdir_p(to);
    if (S_ISDIR(sbuf.st_mode))
        perm_ret = UPFS(mkdirat)(to_dir_fd, to_file, 0);
    else
        perm_ret = UPFS(mknodat)(to_dir_fd, to_file, 0, 0);
    regain();
    if (perm_ret < 0 && errno == EEXIST) {
        /* We're overwriting a file. Just set its permissions to nil. */
        drop();
        perm_ret = UPFS(fchmodat)(to_dir_fd, to_file, 0, 0);
        regain();
    }
    if (perm_ret < 0) goto error;

    /* Rename it in the store */
    store_ret = renameat(store_root, from, store_root, to);
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
    if (to_dir_fd >= 0) close(to_dir_fd);
    errno = save_errno;
    return -1;
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
    from = correct_path(from);
    to = correct_path(to);

    /* To avoid directory renaming causing issues and assure some kind of
     * atomicity, get directory handles first */
    split_path(from, from_parts, &from_dir, &from_file, 0);
    split_path(to, to_parts, &to_dir, &to_file, 0);
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

        perm_ret = UPFS(fstatat)(store_root, from, &sbuf, AT_SYMLINK_NOFOLLOW);
        if (perm_ret < 0) goto error;
    }

    /* Only files can be linked in this way for now */
    if (!S_ISREG(sbuf.st_mode)) {
        errno = EPERM;
        goto error;
    }

    /* Set up the new permissions file */
    drop();
    mkdir_p(to);
    perm_ret = UPFS(mknodat)(to_dir_fd, to_file, sbuf.st_mode, 0);
    regain();
    if (perm_ret < 0) goto error;

    /* Copy it in the store */
    from_file_fd = openat(store_root, from, O_RDONLY);
    if (from_file_fd < 0) goto error;
    to_file_fd = openat(store_root, to, O_WRONLY|O_CREAT|O_EXCL);
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
    return -1;
}
#endif

static int upfs_chmod(const char *path, mode_t mode)
{
    int perm_ret, store_ret;
    struct stat sbuf;
    path = correct_path(path);

    drop();
    perm_ret = UPFS(fchmodat)(perm_root, path, mode, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = fstatat(store_root, path, &sbuf, 0);
    if (store_ret < 0) return -errno;

    if (perm_ret < 0) {
        drop();
        mkfull(path, &sbuf);
        perm_ret = UPFS(fchmodat)(perm_root, path, mode, 0);
        regain();
    }

    if (perm_ret < 0) return -errno;
    return 0;
}

static int upfs_chown(const char *path, uid_t uid, gid_t gid)
{
    int perm_ret, store_ret;
    struct stat sbuf;
    path = correct_path(path);

    drop();
    perm_ret = UPFS(fchownat)(perm_root, path, uid, gid, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = fstatat(store_root, path, &sbuf, 0);
    if (store_ret < 0) return -errno;

    if (perm_ret < 0) {
        drop();
        mkfull(path, &sbuf);
        perm_ret = UPFS(fchownat)(perm_root, path, uid, gid, 0);
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
    path = correct_path(path);

    drop();
    perm_fd = UPFS(openat)(perm_root, path, O_RDWR, 0);
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
        perm_fd = UPFS(openat)(perm_root, path, O_RDWR, 0);
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
    path = correct_path(path);

    drop();
    perm_fd = UPFS(openat)(perm_root, path, ffi->flags, 0);
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
        store_fd = openat(store_root, path, ffi->flags, 0);
        if (store_fd < 0) goto error;
    }

    if (perm_fd < 0) {
        struct stat sbuf;
        ret = fstatat(store_root, path, &sbuf, 0);
        if (ret < 0) goto error;
        drop();
        mkfull(path, &sbuf);
        perm_fd = UPFS(openat)(perm_root, path, ffi->flags, 0);
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
    path = correct_path(path);

    /* Check permissions */
    drop();
    perm_fd = UPFS(openat)(perm_root, path, O_RDONLY, 0);
    regain();
    if (perm_fd < 0 && errno != ENOENT) return -errno;

    /* Open the store directory */
    store_fd = openat(store_root, path, O_RDONLY, 0);
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
#ifdef UPFS_PS
        /* Skip the metafile */
        if (!strcmp(de->d_name, UPFS_META_FILE))
            continue;
#endif
        if (perm_fd >= 0) {
            ret = upfs_stat(perm_fd, store_fd, de->d_name, &sbuf);
            if (ret < 0) {
                errno = -ret;
                goto error;
            }
        } else {
            ret = fstatat(store_fd, de->d_name, &sbuf, 0);
            if (ret < 0) goto error;
        }
        if (filler(buf, de->d_name, &sbuf, 0))
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
    path = correct_path(path);

#ifndef UPFS_PS
    /* If we were using permission files in the store, we should be using
     * default_permissions, and if not, everything goes */
    drop();
    ret = UPFS(faccessat)(perm_root, path, mode, AT_EACCESS);
    regain();
    if (ret < 0 && errno != ENOENT) return -errno;
#endif

    /* Don't check execute bit on store FS */
    if (mode & X_OK) {
        mode &= ~(X_OK);
        if (!mode) mode = R_OK;
    }
    ret = faccessat(store_root, path, mode, 0);
    if (ret < 0) return -errno;
    return 0;
}

static int upfs_create(const char *path, mode_t mode,
    struct fuse_file_info *ffi)
{
    int perm_fd = -1, store_fd = -1;
    int save_errno;
    path = correct_path(path);

    drop();
    perm_fd = UPFS(openat)(perm_root, path, O_RDWR|O_CREAT|O_EXCL, mode);
    regain();
    if (perm_fd < 0) return -errno;

    store_fd = openat(store_root, path, O_RDWR|O_CREAT|O_EXCL, 0600);
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
    path = correct_path(path);

    drop();
    perm_ret = UPFS(utimensat)(perm_root, path, times, 0);
    regain();
    if (perm_ret < 0 && errno != ENOENT) return -errno;

    store_ret = fstatat(store_root, path, &sbuf, 0);
    if (store_ret < 0) return -errno;

    if (perm_ret < 0) {
        drop();
        mkfull(path, &sbuf);
        perm_ret = UPFS(utimensat)(perm_root, path, times, 0);
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
