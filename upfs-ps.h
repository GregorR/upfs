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

struct upfs_time {
    uint64_t sec;
    uint32_t nsec;
};

struct upfs_entry {
    /* uid is -1 if this is as unused entry */
    uint32_t uid, gid;
    uint16_t mode, reserved;
    struct upfs_time mtime, ctime;
    char name[UPFS_NAME_LENGTH];
};

/****************************************************************
 * FILE SYSTEM SIMULATION FUNCTIONS
 ***************************************************************/
int upfs_fstatat(int dirfd, const char *pathname, struct stat *buf, int flags);
int upfs_mknodat(int dir_fd, const char *path, mode_t mode, dev_t dev);
int upfs_mkdirat(int dir_fd, const char *path, mode_t mode);
int upfs_unlinkat(int dir_fd, const char *path, int flags);
int upfs_fchmodat(int dir_fd, const char *path, mode_t mode, int flags);
int upfs_renameat(int old_dir_fd, const char *old_path,
    int new_dir_fd, const char *new_path);
int upfs_fchownat(int dir_fd, const char *path, uid_t owner, gid_t group,
    int flags);
int upfs_openat(int dir_fd, const char *path, int flags, mode_t mode);
int upfs_futimens(int fd, const struct timespec *times);
int upfs_utimensat(int dir_fd, const char *path, const struct timespec *times,
    int flags);

/* This version of fchmodat allows you to change not just the permissions but
 * the TYPE of the file */
int upfs_fchmodat_harder(int dir_fd, const char *path, mode_t mode, int flags);

#endif
