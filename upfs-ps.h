/* Definitions for UpFS-PS's permissions tables */

#ifndef UPFS_PS_H
#define UPFS_PS_H 1

#include <stdint.h>

#define UPFS_VERSION            1

#define UPFS_NAME_LENGTH        256
#define UPFS_META_FILE          ".upfs"
#define UPFS_MAGIC              "UpFSPTbl"
#define UPFS_MAGIC_LENGTH       8

/* The mode bits we support */
#define UPFS_SUPPORTED_MODES    (07777|S_IFLNK|S_IFREG|S_IFDIR)

struct upfs_header {
    char magic[UPFS_MAGIC_LENGTH];
    uint32_t version, free_list;
};

struct upfs_entry_unused {
    uint32_t header, next;
};

struct upfs_entry {
    /* uid is -1 if this is as unused entry */
    uint32_t uid, gid;
    uint16_t mode, reserved;
    char name[UPFS_NAME_LENGTH];
};

/* Functions for accessing the permissions file will go here */

#endif
