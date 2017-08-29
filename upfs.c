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

static int upfs_open_prime(const char *path, struct stat *sbuf,
    struct upfs_directory_entry *de_name, 
    struct upfs_directory_entry *de_node,
    int *dfd, int *ffd,
    int flags, mode_t mode) {

    char path_parts[PATH_MAX], *path_base, *path_tmp;
    const char *path_dir, *path_file;
    struct upfs_directory_header dh;
    struct upfs_directory_entry de;
    int dir_fd = -1, file_fd = -1, tbl_fd = -1;
    if (de_name) de_name->type = UPFS_ENTRY_UNUSED;
    if (de_node) de_node->type = UPFS_ENTRY_UNUSED;
    if (dfd) *dfd = -1;
    if (ffd) *ffd = -1;

    if (path[0] == '/' && !path[1]) {
        /* Special case for the root */
        if (flags & O_EXCL) {
            errno = EEXIST;
            goto error;
        }
        if (sbuf && fstatat(root, ".", sbuf, 0) < 0)
            goto error;
        if (dfd && (dir_fd = *dfd = dup(root)) < 0)
            goto error;
        if (ffd && (file_fd = *ffd = dup(root)) < 0)
            goto error;
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
    if (dfd) *dfd = dir_fd;

    /* Now see if there's a directory entry for it */
    tbl_fd = openat(dir_fd, UPFS_META_FILE, (flags&O_CREAT) ? (O_RDWR|O_CREAT) : O_RDONLY, 0600);
    de.type = UPFS_ENTRY_UNUSED;
    if (tbl_fd >= 0) {
        int shadowed = 0, found = 0;
        ssize_t rd;

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
        while (read(tbl_fd, &de, sizeof(struct upfs_directory_entry)) == sizeof(struct upfs_directory_entry)) {
            if (de.type == UPFS_ENTRY_NAME && !strncmp(path_file, de.d.name.up_name, UPFS_NAME_LENGTH)) {
                /* Found it! */
                found = 1;
                if (de_name) *de_name = de;
                break;

            } else if (de.type == UPFS_ENTRY_NODE && !strncmp(path_file, de.d.node.down_name, UPFS_NAME_LENGTH)) {
                /* Didn't find it, but worth remembering that it's been shadowed */
                shadowed = 1;

            }
        }

        if (found) {
            /* We found the entry, so find the corresponding node */
            if (de.d.name.directory != (uint32_t) -1) {
                /* It's in another directory, not yet supported */
                errno = EIO;
                goto error;
            }

            /* Node doesn't exist! */
            if (lseek(tbl_fd, sizeof(struct upfs_directory_header) +
                    de.d.name.node * sizeof(struct upfs_directory_entry),
                    SEEK_SET) < 0) {
                fprintf(stderr, "Aw heck\n");
                errno = EIO;
                goto error;
            }

            /* Read in the node */
            rd = read(tbl_fd, &de, sizeof(struct upfs_directory_entry));
            if (rd < 0)
                goto error;
            if (rd != sizeof(struct upfs_directory_entry)) {
                errno = EIO;
                goto error;
            }

            /* And copy it over */
            if (de_node) *de_node = de;

            path_file = de.d.node.down_name;

        } else if (flags & O_CREAT) {
            /* Create an entry for it (not yet supported) */
            errno = EIO;
            goto error;

        } else if (shadowed) {
            /* We have to imagine that this file doesn't exist */
            errno = ENOENT;
            goto error;

        } else {
            /* Othwise, indicate we didn't find it */
            de.type = UPFS_ENTRY_UNUSED;
            if (de_name) de_name->type = UPFS_ENTRY_UNUSED;
            if (de_node) de_node->type = UPFS_ENTRY_UNUSED;

        }

        close(tbl_fd);
        tbl_fd = -1;
    }

    /* Open it if asked */
    if (ffd) {
        *ffd = file_fd = openat(dir_fd, path_file, flags, mode);
        if (file_fd < 0)
            goto error;

        if (sbuf && fstat(file_fd, sbuf) < 0)
            goto error;

    } else if (sbuf) {
        /* Just stat it */
        if (fstatat(dir_fd, path_file, sbuf, 0) < 0)
            goto error;

    }

    if (!dfd)
        close(dir_fd);

    return 0;

error:
    if (tbl_fd >= 0)
        close(tbl_fd);
    if (file_fd >= 0)
        close(file_fd);
    if (dir_fd >= 0)
        close(dir_fd);
    return -1;
}

static int upfs_getattr(const char *path, struct stat *sbuf) {
    struct upfs_directory_entry de_node;

    /* Do the underlying stat */
    if (upfs_open_prime(path, sbuf, NULL, &de_node, NULL, NULL, 0, 0) < 0)
        return -1;
    if (de_node.type == UPFS_ENTRY_UNUSED) {
        /* No extended info, use underlying permissions */
        sbuf->st_mode &= (0777|S_IFLNK|S_IFREG|S_IFDIR);
        return 0;
    }

    /* Then merge the permissions */
    sbuf->st_mode = de_node.d.node.mode;
    sbuf->st_nlink = de_node.d.node.nlink;
    sbuf->st_uid = de_node.d.node.uid;
    sbuf->st_gid = de_node.d.node.gid;
    return 0;
}

static int upfs_readlink(const char *path, char *buf, size_t bufsz) {
    struct upfs_directory_entry de_node;
    int ffd;
    ssize_t rd;

    /* Stat the link file */
    if (upfs_open_prime(path, NULL, NULL, &de_node, NULL, &ffd, 0, 0) < 0)
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

static struct fuse_operations upfs_operations = {
    .getattr = upfs_getattr,
    .readlink = upfs_readlink
};

int main(int argc, char **argv) {
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
