#ifndef UPFS_H
#define UPFS_H 1

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include <stdint.h>

#define UPFS_VERSION            1

#define UPFS_NAME_LENGTH        256
#define UPFS_META_FILE          ".upfs"
#define UPFS_ALT_META_FILE      "UPFS.DAT"
#define UPFS_MAGIC              "UpFSDTbl"
#define UPFS_MAGIC_LENGTH       8

/* The mode bits we support */
#define UPFS_SUPPORTED_MODES    (07777|S_IFLNK|S_IFREG|S_IFDIR)

/* Unused directory entry, free to be reused */
#define UPFS_ENTRY_UNUSED       0

/* A filename */
#define UPFS_ENTRY_NAME         1

/* An "inode" */
#define UPFS_ENTRY_NODE         2

/* A reference to a distant directory, for hardlinks */
#define UPFS_ENTRY_DIRECTORY    3

struct upfs_directory_header {
    char magic[UPFS_MAGIC_LENGTH];
    uint32_t version, free_list;
};

struct upfs_directory_entry_unused {
    uint32_t next;
};

struct upfs_directory_entry_name {
    uint32_t directory; /* Where to find the right inode (-1 = here) */
    uint32_t node; /* The index of the node containing this file */
    uint32_t reserved;
    char up_name[UPFS_NAME_LENGTH];
};

struct upfs_directory_entry_node {
    uint16_t mode, nlink;
    uint32_t uid;
    uint32_t gid;
    char down_name[UPFS_NAME_LENGTH];
};

struct upfs_directory_entry {
    uint32_t type;
    union {
        struct upfs_directory_entry_unused unused;
        struct upfs_directory_entry_name name;
        struct upfs_directory_entry_node node;
    } d;
};

#endif
