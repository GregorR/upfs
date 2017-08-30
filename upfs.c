#define _XOPEN_SOURCE 700 /* fstatat */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "upfs.h"

static char *root_path = NULL;
static int root = 0; /* fd */

struct upfs_open_out {
    struct stat *sbuf;
    struct upfs_directory_entry *de_name;
    struct upfs_directory_entry *de_node;
    int *name_tbl_fd, *node_tbl_fd;
    off_t name_tbl_off, node_tbl_off;
    int *dfd, *ffd;
};

static uint32_t upfs_alloc_entry(int tbl_fd, struct upfs_directory_entry *data)
{
    struct upfs_directory_header dh;
    struct upfs_directory_entry old_de;
    uint32_t next, ret;
    ssize_t rd;
    off_t loc;

    /* Get the current freelist entry */
    if (lseek(tbl_fd, 0, SEEK_SET) < 0)
        return -1;
    rd = read(tbl_fd, &dh, sizeof(struct upfs_directory_header));
    if (rd < 0) {
        return (uint32_t) -1;
    } else if (rd != sizeof(struct upfs_directory_header)) {
        errno = EIO;
        return (uint32_t) -1;
    }
    ret = dh.free_list;

    /* Extend the file if there's no entry free */
    if (ret == (uint32_t) -1) {
        loc = lseek(tbl_fd, 0, SEEK_END);
        if (loc < 0)
            return -1;

        loc -= sizeof(struct upfs_directory_header);
        if (loc % sizeof(struct upfs_directory_entry) != 0) {
            /* Directory table is corrupted! */
            errno = EIO;
            return -1;
        }

        ret = loc / sizeof(struct upfs_directory_entry);
        if (ret == (uint32_t) -1) {
            /* Overflow! */
            errno = ENOSPC;
            return -1;
        }
        if (write(tbl_fd, data, sizeof(struct upfs_directory_entry)) !=
            sizeof(struct upfs_directory_entry))
            return -1;

        return ret;
    }

    /* There's an entry free, so claim it */
    loc = sizeof(struct upfs_directory_header) +
        ret * sizeof(struct upfs_directory_entry);
    if (lseek(tbl_fd, loc, SEEK_SET) < 0)
        return -1;
    rd = read(tbl_fd, &old_de, sizeof(struct upfs_directory_entry));
    if (rd < 0) {
        return -1;
    } else if (rd != sizeof(struct upfs_directory_entry)) {
        errno = EIO;
        return -1;
    }
    if (old_de.type != UPFS_ENTRY_UNUSED) {
        errno = EIO;
        return -1;
    }
    next = old_de.d.unused.next;

    /* Update the next pointer in the header */
    if (lseek(tbl_fd, 0, SEEK_SET) < 0)
        return -1;
    dh.free_list = next;
    if (write(tbl_fd, &dh, sizeof(struct upfs_directory_header)) !=
        sizeof(struct upfs_directory_header))
        return -1;

    /* And write the node */
    if (lseek(tbl_fd, loc, SEEK_SET) < 0)
        return -1;
    if (write(tbl_fd, data, sizeof(struct upfs_directory_entry)) !=
        sizeof(struct upfs_directory_entry))
        return -1;

    return ret;
}

static int upfs_free_entry(int tbl_fd, off_t entry_offset)
{
    struct upfs_directory_header dh;
    struct upfs_directory_entry de = {0};
    ssize_t rd;

    /* Get the current freelist entry */
    if (lseek(tbl_fd, 0, SEEK_SET) < 0)
        return -1;
    rd = read(tbl_fd, &dh, sizeof(struct upfs_directory_header));
    if (rd < 0) {
        return -1;
    } else if (rd != sizeof(struct upfs_directory_header)) {
        errno = EIO;
        return -1;
    }

    /* Link this one in */
    if (lseek(tbl_fd, entry_offset, SEEK_SET) < 0)
        return -1;
    de.type = UPFS_ENTRY_UNUSED;
    de.d.unused.next = dh.free_list;
    if (write(tbl_fd, &de, sizeof(struct upfs_directory_entry))
        != sizeof(struct upfs_directory_entry))
        return -1;

    /* And make it the head */
    if (lseek(tbl_fd, 0, SEEK_SET) < 0)
        return -1;
    entry_offset -= sizeof(struct upfs_directory_header);
    entry_offset /= sizeof(struct upfs_directory_entry);
    dh.free_list = entry_offset;
    if (write(tbl_fd, &dh, sizeof(struct upfs_directory_header))
        != sizeof(struct upfs_directory_header))
        return -1;

    return 0;
}

static int upfs_create(int dir_fd, const char *path, int flags, mode_t mode)
{
    if (mode & S_IFDIR) {
        /* Creating a directory */
        return mkdirat(dir_fd, path, 0700);

    } else {
        /* Creating a file */
        return openat(dir_fd, path, flags|O_CREAT|O_EXCL, 0600);

    }
}

static int upfs_open_trynames(int dir_fd, struct upfs_directory_entry *de,
    int flags, mode_t mode)
{
    char file_base[UPFS_NAME_LENGTH], file_ext[UPFS_NAME_LENGTH], *dot;
    int ret, part;

    /* Before doing anything rash, try the given name */
    ret = upfs_create(dir_fd, de->d.node.down_name, flags, mode);
    if (ret >= 0 || errno != EEXIST)
        return ret;

    /* OK, time for rash actions! First split the name. */
    strncpy(file_base, de->d.node.down_name, UPFS_NAME_LENGTH);
    file_base[UPFS_NAME_LENGTH-1] = 0;
    file_ext[0] = 0;
    if (!(mode & S_IFDIR)) {
        dot = strrchr(file_base, '.');
        if (dot) {
            strncpy(file_ext, dot, UPFS_NAME_LENGTH);
            *dot = 0;
        }
    }

    /* Now try name extensions */
    de->d.node.down_name[UPFS_NAME_LENGTH-1] = 0;
    for (part = 2; part < 16; part++) {
        snprintf(de->d.node.down_name, UPFS_NAME_LENGTH-1, "%s (%d)%s",
            file_base, part, file_ext);
        ret = upfs_create(dir_fd, de->d.node.down_name, flags, mode);
        if (ret >= 0 || errno != EEXIST)
            return ret;
    }

    /* Last resort, try useless names */
    for (part = 0; part < 1000000; part++) {
        snprintf(de->d.node.down_name, UPFS_NAME_LENGTH-1, "UP%06d.DAT", part);
        ret = upfs_create(dir_fd, de->d.node.down_name, flags, mode);
        if (ret >= 0 || errno != EEXIST)
            return ret;
    }

    /* We can't escape it! */
    errno = EEXIST;
    return -1;
}

static int upfs_open_prime(const char *path, int flags, mode_t mode,
    struct upfs_open_out *o)
{
    char path_parts[PATH_MAX], *path_base, *path_tmp;
    const char *path_dir, *path_file;
    struct upfs_directory_header dh;
    struct upfs_directory_entry de_name, de_node;
    struct fuse_context *fctx;
    int dir_fd = -1, file_fd = -1, tbl_fd = -1;
    if (o) {
        if (o->de_name) o->de_name->type = UPFS_ENTRY_UNUSED;
        if (o->de_node) o->de_node->type = UPFS_ENTRY_UNUSED;
        if (o->name_tbl_fd) *o->name_tbl_fd = -1;
        if (o->node_tbl_fd) *o->node_tbl_fd = -1;
        if (o->dfd) *o->dfd = -1;
        if (o->ffd) *o->ffd = -1;
    }

    /* Check for unsupported modes */
    if ((mode & UPFS_SUPPORTED_MODES) != mode) {
        errno = ENOTSUP;
        goto error;
    }

    if (path[0] == '/' && !path[1]) {
        /* Special case for the root */
        if (flags & O_EXCL) {
            errno = EEXIST;
            goto error;
        }
        if (o) {
            if (o->sbuf && fstatat(root, ".", o->sbuf, 0) < 0)
                goto error;
            if (o->dfd && (*o->dfd = dir_fd = dup(root)) < 0)
                goto error;
            if (o->ffd && (*o->ffd = file_fd = dup(root)) < 0)
                goto error;
        }
        return 0;
    }

    /* Split up the path */
    strncpy(path_parts, path, PATH_MAX);
    path_parts[PATH_MAX-1] = 0;
    path_base = path_parts;
    while (*path_base == '/')
        path_base++;
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

    /* First follow the directory */
    dir_fd = openat(root, path_dir, O_RDONLY);
    if (dir_fd < 0)
        goto error;
    if (o && o->dfd) *o->dfd = dir_fd;

    /* Now see if there's a directory entry for it */
    tbl_fd = openat(dir_fd, UPFS_META_FILE, (flags&O_CREAT) ? (O_RDWR|O_CREAT) : O_RDONLY, 0600);
    if (tbl_fd >= 0) {
        int shadowed = 0, found = 0;
        ssize_t rd;
        off_t node_off, name_off;

        /* Check the header */
        rd = read(tbl_fd, &dh, sizeof(struct upfs_directory_header));
        if (rd == 0) {
            /* Fresh table, make the hader */
            memcpy(dh.magic, UPFS_MAGIC, UPFS_MAGIC_LENGTH);
            dh.version = UPFS_VERSION;
            dh.free_list = (uint32_t) -1;
            if (write(tbl_fd, &dh, sizeof(struct upfs_directory_header)) < 0) {
                errno = EIO;
                goto error;
            }

        } else if (rd == sizeof(struct upfs_directory_header)) {
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
        while (read(tbl_fd, &de_name, sizeof(struct upfs_directory_entry)) == sizeof(struct upfs_directory_entry)) {
            if (de_name.type == UPFS_ENTRY_NAME && !strncmp(path_file, de_name.d.name.up_name, UPFS_NAME_LENGTH)) {
                /* Found it! */
                found = 1;
                break;

            } else if (de_name.type == UPFS_ENTRY_NODE && !strncmp(path_file, de_name.d.node.down_name, UPFS_NAME_LENGTH)) {
                /* Didn't find it, but worth remembering that it's been shadowed */
                shadowed = 1;

            }
        }

        if (found) {
            /* Tell the caller */
            if (o) {
                if (o->de_name) *o->de_name = de_name;
                if (o->name_tbl_fd && (*o->name_tbl_fd = dup(tbl_fd)) < 0)
                    goto error;
                o->name_tbl_off = lseek(tbl_fd, 0, SEEK_CUR) - sizeof(struct upfs_directory_entry);
            }

            /* We found the entry, so find the corresponding node */
            if (de_name.d.name.directory != (uint32_t) -1) {
                /* It's in another directory, not yet supported */
                errno = EIO;
                goto error;
            }

            /* Seek to it */
            node_off = sizeof(struct upfs_directory_header) +
                de_name.d.name.node * sizeof(struct upfs_directory_entry);
            if (lseek(tbl_fd, node_off, SEEK_SET) < 0) {
                errno = EIO;
                goto error;
            }

            /* Read in the node */
            rd = read(tbl_fd, &de_node, sizeof(struct upfs_directory_entry));
            if (rd < 0)
                goto error;
            if (rd != sizeof(struct upfs_directory_entry)) {
                errno = EIO;
                goto error;
            }

            /* Verify it */
            if (de_node.type != UPFS_ENTRY_NODE) {
                errno = EIO;
                goto error;
            }

            /* And copy it over */
            if (o) {
                if (o->de_node) *o->de_node = de_node;
                if (o->node_tbl_fd && (*o->node_tbl_fd = dup(tbl_fd)) < 0)
                    goto error;
                o->node_tbl_off = node_off;
            }

            path_file = de_node.d.node.down_name;

        } else if (flags & O_CREAT) {
            /* Create an entry for it. First we have to create the actual file,
             * if it doesn't exist. If ffd is set, that indicates that the
             * request is a real open+creat, and thus might be exclusive.
             * Without ffd, we're just making a node, perhaps for an existing
             * file. */
            uint32_t name_entry_num;
            int create_file = 0;
            if (o && o->ffd) create_file = 1;

            /* See if it already exists */
            file_fd = openat(dir_fd, path_file, O_RDONLY);
            if (file_fd >= 0) {
                /* It does, just make an entry for the existing file */
                if (create_file && (flags&O_EXCL)) {
                    /* Already exists! */
                    errno = EEXIST;
                    goto error;
                }
                create_file = 0;
                if (!o || !o->ffd) {
                    close(file_fd);
                    file_fd = -1;
                }
            }

            memset(&de_node, 0, sizeof(struct upfs_directory_entry));
            memset(&de_name, 0, sizeof(struct upfs_directory_entry));

            /* Create it if we were asked to */
            if (create_file) {
                strncpy(de_node.d.node.down_name, path_file, UPFS_NAME_LENGTH-1);
                file_fd = upfs_open_trynames(dir_fd, &de_node, flags, (mode&(S_IFREG|S_IFDIR))|0600);
                if (file_fd < 0)
                    goto error;

            } else {
                strncpy(de_node.d.node.down_name, path_file, UPFS_NAME_LENGTH-1);

            }

            /* Create the node */
            fctx = fuse_get_context();
            de_node.type = UPFS_ENTRY_NODE;
            de_node.d.node.mode = mode;
            de_node.d.node.nlink = 1;
            de_node.d.node.uid = fctx->uid;
            de_node.d.node.gid = fctx->gid;
            de_name.d.name.node = upfs_alloc_entry(tbl_fd, &de_node);
            if (de_name.d.name.node == (uint32_t) -1)
                goto error;
            node_off = sizeof(struct upfs_directory_header) +
                de_name.d.name.node * sizeof(struct upfs_directory_entry);

            /* Create the name */
            strncpy(de_name.d.name.up_name, path_file, UPFS_NAME_LENGTH-1);
            de_name.d.name.directory = (uint32_t) -1;
            name_entry_num = upfs_alloc_entry(tbl_fd, &de_name);
            if (name_entry_num == (uint32_t) -1) {
                /* Big problem! We need to free the node we just made. */
                upfs_free_entry(tbl_fd, node_off);
                errno = EIO;
                goto error;
            }
            name_off = sizeof(struct upfs_directory_header) +
                name_entry_num * sizeof(struct upfs_directory_entry);

            /* And copy it over */
            if (o) {
                if (o->de_name) *o->de_name = de_name;
                if (o->name_tbl_fd && (*o->name_tbl_fd = dup(tbl_fd)) < 0)
                    goto error;
                o->name_tbl_off = name_off;
                if (o->de_node) *o->de_node = de_node;
                if (o->node_tbl_fd && (*o->node_tbl_fd = dup(tbl_fd)) < 0)
                    goto error;
                o->node_tbl_off = node_off;
            }

        } else if (shadowed) {
            /* We have to imagine that this file doesn't exist */
            errno = ENOENT;
            goto error;

        }

        close(tbl_fd);
        tbl_fd = -1;
    }

    /* Open it if asked */
    if (o && o->ffd) {
        if (file_fd < 0)
            file_fd = openat(dir_fd, path_file, flags, mode);
        *o->ffd = file_fd;
        if (file_fd < 0)
            goto error;

        if (o->sbuf && fstat(file_fd, o->sbuf) < 0)
            goto error;

    } else if (o && o->sbuf) {
        /* Just stat it */
        if (fstatat(dir_fd, path_file, o->sbuf, 0) < 0)
            goto error;

    }

    if (!o || !o->dfd)
        close(dir_fd);

    return 0;

error:
    if (tbl_fd >= 0)
        close(tbl_fd);
    if (o && o->name_tbl_fd && *o->name_tbl_fd >= 0)
        close(*o->name_tbl_fd);
    if (o && o->node_tbl_fd && *o->node_tbl_fd >= 0)
        close(*o->node_tbl_fd);
    if (file_fd >= 0)
        close(file_fd);
    if (dir_fd >= 0)
        close(dir_fd);
    return -1;
}

static int upfs_getattr(const char *path, struct stat *sbuf) {
    struct upfs_directory_entry de_node;
    struct upfs_open_out oo = {0};

    /* Do the underlying stat */
    oo.sbuf = sbuf;
    oo.de_node = &de_node;
    if (upfs_open_prime(path, 0, 0, &oo) < 0)
        return -1;
    if (de_node.type == UPFS_ENTRY_UNUSED) {
        /* No extended info, use underlying permissions */
        sbuf->st_mode &= UPFS_SUPPORTED_MODES;
        return 0;
    }

    /* Then merge the permissions */
    sbuf->st_mode = de_node.d.node.mode;
    sbuf->st_nlink = de_node.d.node.nlink;
    sbuf->st_uid = de_node.d.node.uid;
    sbuf->st_gid = de_node.d.node.gid;
    return 0;
}

static int upfs_readlink(const char *path, char *buf, size_t bufsz)
{
    struct upfs_directory_entry de_node;
    int ffd;
    ssize_t rd;
    struct upfs_open_out oo = {0};

    /* Stat the link file */
    oo.de_node = &de_node;
    oo.ffd = &ffd;
    if (upfs_open_prime(path, O_RDONLY, 0, &oo) < 0)
        return -1;

    /* Make sure it IS a symlink */
    if (de_node.type == UPFS_ENTRY_UNUSED ||
        !(de_node.d.node.mode & S_IFLNK)) {
        close(ffd);
        errno = EINVAL;
        return -1;
    }

    /* And read it */
    rd = read(ffd, buf, bufsz - 1);
    close(ffd);
    if (rd < 0) {
        return -1;
    } else if (rd == 0) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int upfs_unlink(const char *path)
{
    struct stat sbuf;
    struct upfs_directory_entry de_name, de_node;
    int name_tbl_fd = -1, node_tbl_fd = -1;
    int dfd;
    struct upfs_open_out oo = {0};

    /* Get the relevant file stats */
    oo.sbuf = &sbuf;
    oo.de_name = &de_name;
    oo.de_node = &de_node;
    oo.name_tbl_fd = &name_tbl_fd;
    oo.node_tbl_fd = &node_tbl_fd;
    oo.dfd = &dfd;
    if (upfs_open_prime(path, O_CREAT, 0, &oo) < 0)
        goto error;

    /* Make sure it's a normal file */
    if (de_name.type != UPFS_ENTRY_UNUSED)
        sbuf.st_mode = de_node.d.node.mode;
    if (!(sbuf.st_mode & S_IFREG)) {
        errno = EPERM;
        goto error;
    }

    /* If the file exists in UpFS, delete that name */
    if (de_name.type != UPFS_ENTRY_UNUSED) {
        if (upfs_free_entry(name_tbl_fd, oo.name_tbl_off) < 0)
            goto error;
        close(name_tbl_fd);
        name_tbl_fd = -1;
    }

    /* Remove a link from the node */
    if (de_node.type != UPFS_ENTRY_UNUSED) {
        if (de_node.d.node.nlink)
            de_node.d.node.nlink--;
        if (!de_node.d.node.nlink) {
            if (upfs_free_entry(node_tbl_fd, oo.node_tbl_off) < 0)
                goto error;
            if (unlinkat(dfd, de_node.d.node.down_name, 0) < 0)
                goto error;
        }
        close(node_tbl_fd);
        node_tbl_fd = -1;
    }

    /* Remove the regular file if it wasn't mapped */
    if (de_node.type == UPFS_ENTRY_UNUSED) {
        if (unlinkat(root, path, 0) < 0)
            goto error;
    }

    close(dfd);
    return 0;

error:
    if (name_tbl_fd >= 0)
        close(name_tbl_fd);
    if (node_tbl_fd >= 0)
        close(node_tbl_fd);
    if (dfd >= 0)
        close(dfd);
    return -1;
}

static int upfs_rmdir(const char *path)
{
    struct stat sbuf;
    struct upfs_directory_entry de_name, de_node;
    int name_tbl_fd = -1, node_tbl_fd = -1;
    int dfd;
    struct upfs_open_out oo = {0};

    /* Get the relevant file stats */
    oo.sbuf = &sbuf;
    oo.de_name = &de_name;
    oo.de_node = &de_node;
    oo.name_tbl_fd = &name_tbl_fd;
    oo.node_tbl_fd = &node_tbl_fd;
    oo.dfd = &dfd;
    if (upfs_open_prime(path, O_CREAT, 0, &oo) < 0)
        goto error;

    /* Make sure it's a directory */
    if (de_name.type != UPFS_ENTRY_UNUSED)
        sbuf.st_mode = de_node.d.node.mode;
    if (!(sbuf.st_mode & S_IFDIR)) {
        errno = EPERM;
        goto error;
    }

    /* If the file exists in UpFS, delete that name */
    if (de_name.type != UPFS_ENTRY_UNUSED) {
        if (upfs_free_entry(name_tbl_fd, oo.name_tbl_off) < 0)
            goto error;
        close(name_tbl_fd);
        name_tbl_fd = -1;
    }

    /* Remove a link from the node */
    if (de_node.type != UPFS_ENTRY_UNUSED) {
        if (upfs_free_entry(node_tbl_fd, oo.node_tbl_off) < 0)
            goto error;
        if (unlinkat(dfd, de_node.d.node.down_name, AT_REMOVEDIR) < 0)
            goto error;
        close(node_tbl_fd);
        node_tbl_fd = -1;
    }

    /* Remove the regular file if it wasn't mapped */
    if (de_node.type == UPFS_ENTRY_UNUSED) {
        if (unlinkat(root, path, AT_REMOVEDIR) < 0)
            goto error;
    }

    close(dfd);
    return 0;

error:
    if (name_tbl_fd >= 0)
        close(name_tbl_fd);
    if (node_tbl_fd >= 0)
        close(node_tbl_fd);
    if (dfd >= 0)
        close(dfd);
    return -1;
}

static struct fuse_operations upfs_operations = {
    .getattr = upfs_getattr,
    .readlink = upfs_readlink,
    .unlink = upfs_unlink,
    .rmdir = upfs_rmdir
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

    /* Extract the one argument we care about, the root */
    fuse_argv[0] = argv[0];
    for (ai = fai = 1; ai < argc; ai++) {
        arg = argv[ai];
        if (arg[0] == '-') {
            fuse_argv[fai++] = arg;
            if (arg[1] == 'o' && !arg[2])
                fuse_argv[fai++] = argv[++ai];

        } else if (!root_path) {
            root_path = arg;

        } else {
            fuse_argv[fai++] = arg;

        }
    }

    if (!root_path) {
        fprintf(stderr, "Use: upfs <root> <mount point>\n");
        return 1;
    }

    /* Open the root */
    root = open(root_path, O_RDONLY);
    if (root < 0) {
        perror(root_path);
        return 1;
    }

    /* And run FUSE */
    umask(0);
    return fuse_main(fai, fuse_argv, &upfs_operations, NULL);
}
