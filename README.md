# UpFS

The Up Filesystem is a simple union-like filesystem used to add Unix
permissions and ownership to files stored in a non-Unix filesystem. Think of it
as a modern(ish) version of umsdos.

UpFS expects a permissions directory and a store directory. The permissions of
the store directory are ignored: Only its file and directory contents are
important. For each file in the store directory, if an identically-named file
exists in the permissions directory, the owner, mode and type reflect the
permissions version; for regular files, the size and content reflect the store
version. If no identically-named file exists in the permissions directory, the
file is world-readable and world-writeable, until "claimed" by being opened, at
which point the opening user owns the file with standard (umasked) permissions.

The purpose of UpFS is to share a storage drive using, e.g. FAT32, but give it
Unix permissions. Storing i-nodes for empty files doesn't take much space, so
storing the Unix permissions directly in the host filesystem is reasonable.

For instance, imagine that `/dev/sdb1` is a FAT32 filesystem to be used to store
/home files, and its permissions will be stored in `/mnt/home_p`. You could
establish such a scheme like so:

```
# mkdir /mnt/home_s
# mount -t vfat -o check=s,uid=0,gid=0,umask=077 /dev/sdb1 /mnt/home_s
# mount -t upfs /mnt/home_p:/mnt/home_s /home
```

Note in particular that for FAT, `check=s` is very important, to make the case
sensitivity of the store and permissions directories the same.

## Implementation

UpFS is implemented as a FUSE filesystem. This makes it slow. For my use case,
in which the store is on an SD card, the I/O latency is so significant that
FUSE itself adds no appreciable slowdown. This is probably the case for most
realistic setups.

It could just as well be implemented as a kernel module, but I haven't looked
into this option.
