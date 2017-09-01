#ifndef UPFS_H
#define UPFS_H 1

#include <ctype.h>
#include <limits.h>
#include <string.h>

/* Split path into dir/file parts */
static void split_path(const char *path, char path_parts[PATH_MAX],
    char **path_dir, char **path_file, int decap)
{
    char *path_base, *path_tmp;

    /* Get our copy */
    strncpy(path_parts, path, PATH_MAX);
    path_parts[PATH_MAX-1] = 0;

    path_base = path_parts;
    path_tmp = strrchr(path_base, '/');

    /* Remove any terminal / */
    while (path_tmp && !path_tmp[1]) {
        /* Ends with / */
        *path_tmp = 0;
        path_tmp = strrchr(path_base, '/');
    }

    /* Split it */
    if (!path_tmp) {
        /* No / */
        *path_dir = ".";
        *path_file = path_base;
        if (!(*path_file)[0])
            *path_file = ".";

    } else {
        /* Split it */
        *path_tmp++ = 0;
        *path_dir = path_base;
        *path_file = path_tmp;

    }

    /* Because the root is assumed to be vfat, we merge case */
    if (decap)
        for (path_tmp = *path_file; *path_tmp; path_tmp++)
            *path_tmp = tolower(*path_tmp);
}

#endif
