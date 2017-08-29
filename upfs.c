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

static int upfs_get_directory_entry(const char *path, struct stat *sbuf,
    struct upfs_directory_entry *de_name, int *dfd,
    struct upfs_directory_entry *de_node, int create) {

    char path_parts[PATH_MAX], *path_tmp;
    const char *path_dir, *path_file;
    int tbl_fd = -1;
    *dfd = -1;

    if (path[0] == '/' && !path[1]) {
        /* Special case for the root */
        de_name->type = de_node->type = UPFS_ENTRY_UNUSED;
        return fstatat(root, ".", sbuf, 0);
    }

    /* Split up the path */
    strncpy(path_parts, path, PATH_MAX);
    path_parts[PATH_MAX-1] = 0;
    path_tmp = strrchr(path_parts, '/');
    while (path_tmp && !path_tmp[1]) {
        /* Ends with / */
        *path_tmp = 0;
        path_tmp = strrchr(path_parts, '/');
    }
    if (!path_tmp) {
        /* No / */
        path_dir = ".";
        path_file = path_parts;
        if (!path_file[0]) {
            /* Empty path */
            errno = ENOENT;
            goto error;
        }

    } else {
        /* Split it */
        *path_tmp++ = 0;
        path_dir = path_parts;
        path_file = path_tmp;

    }

    /* First follow the directory */
    *dfd = openat(root, path_dir, O_RDONLY);
    if (*dfd < 0)
        goto error;

    /* Now see if there's a directory entry for it */
    tbl_fd = openat(*dfd, UPFS_META_FILE, create ? O_RDONLY : (O_RDWR|O_CREAT));
    if (tbl_fd > 0) {
        struct upfs_directory_header dh;
        struct upfs_directory_entry de;
        int shadowed = 0, found = 0;
        ssize_t rd;

        /* Check the header */
        rd = read(tbl_fd, &dh, sizeof(struct upfs_directory_header));
        if (rd == 0) {
            /* Fresh table, make the hader */
            memcpy(dh.magic, UPFS_MAGIC, UPFS_MAGIC_LENGTH);
            dh.version = UPFS_VERSION;
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
                *de_name = de;
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
            if (lseek(tbl_fd, SEEK_SET, sizeof(struct upfs_directory_header) +
                    de.d.name.node * sizeof(struct upfs_directory_entry)) < 0) {
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
            *de_node = de;

        } else if (create) {
            /* Create an entry for it (not yet supported) */
            errno = EIO;
            return -1;

        } else if (shadowed) {
            /* We have to imagine that this file doesn't exist */
            errno = ENOENT;
            goto error;

        } else {
            /* Othwise, indicate we didn't find it */
            de_name->type = de_node->type = UPFS_ENTRY_UNUSED;

        }

        close(tbl_fd);
        tbl_fd = -1;
    }

    /* Now we can stat the actual file */
    if (fstatat(*dfd, de_node->type ? de_node->d.node.down_name : path_file, sbuf, 0) < 0)
        goto error;

    return 0;

error:
    if (tbl_fd >= 0)
        close(tbl_fd);
    if (*dfd >= 0)
        close(*dfd);
    return -1;
}

static int upfs_getattr(const char *path, struct stat *sbuf) {
    int ret = 0, dfd;
    struct upfs_directory_entry de_name, de_node;

    /* Do the underlying stat */
    if (upfs_get_directory_entry(path, sbuf, &de_name, &dfd, &de_node, 0) < 0)
        return -1;
    if (de_name.type == UPFS_ENTRY_UNUSED) {
        /* No extended info, use underlying permissions */
        sbuf->st_mode &= (0777|S_IFLNK|S_IFREG|S_IFDIR);
        return 0;
    }

    /* Then merge the permissions */
    sbuf->st_mode = de_node.d.node.mode;
    sbuf->st_nlink = de_node.d.node.nlink;
    sbuf->st_uid = de_node.d.node.uid;
    sbuf->st_gid = de_node.d.node.gid;
}

static struct fuse_operations upfs_operations = {
    .getattr = upfs_getattr
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
