# UpFS

The Up Filesystem is a simple union-like filesystem used to add Unix
permissions and ownership to files stored in a non-Unix filesystem. Think of it
as a modern(ish) version of umsdos.

UpFS expects a permissions directory and a store directory. The permissions of
the files in the store directory are ignored: Only its file and directory
contents are important. For each file in the store directory, if an
identically-named file exists in the permissions directory, the owner, mode and
type reflect the permissions version; for regular files, the size and content
reflect the store version. If no identically-named file exists in the
permissions directory, the file's permissions reflect those of the store
directory, until "claimed" by being opened for writing, at which point the
opening user owns the file with standard (umasked) permissions.

The purpose of UpFS is to share a storage drive using, e.g. FAT32, but give it
Unix permissions. Storing i-nodes for empty files doesn't take much space, so
storing the Unix permissions directly in the host filesystem is reasonable.

Limitations unrelated to file ownership and permissions, such as file name or
size restrictions, are inherited from the store directory. For instance, UpFS
backed by a FAT32 store cannot store files larger than 2G.

As an example usage, imagine that `/dev/sdb1` is a FAT32 filesystem to be used
to store /home, and its permissions will be stored in `/mnt/home_p`. You could
establish such a scheme like so:

```
# mkdir /mnt/home_s
# mount -t vfat -o check=s,shortname=winnt,uid=0,gid=0,umask=077 /dev/sdb1 /mnt/home_s
# mount -t upfs /mnt/home_p:/mnt/home_s /home
```

Note in particular that for FAT, `check=s,shortname=winnt` is very important,
to make the case sensitivity of the store and permissions directories the same.
If the permissions directory is case sensitive and the store directory is case
insensitive, the permissions are bypassable, so be careful!

When `upfs` is used directly, instead of through `mount.upfs`, you likely want
`allow_others`. Do NOT enable `default_permissions`: `upfs` implements
permissions directly, so `default_permissions` just wastes time.

## Sharing

For the most part, it's harmless to do anything with the store directory while
UpFS isn't mounted. In particular, of course, this means that it's mostly
harmless to mount the store directory in another operating system. That is the
point, after all. Of course, it will be accessible with no permissions
protection.

There are some exceptions. Deleting files in the store can cause strange
effects, as the files will not appear to exist when mounted under UpFS, but
also cannot be created, because they already exist! This is deliberate, so that
incorrect mounting doesn't clobber the permissions directory. Since the
permissions directory has normal Unix permissions, it's easy to rectify this
issue: The user encountering it can delete the files in the permissions
directory. Of course, that means that the user has to know and understand what
is happening and why.

## fstab

`mount.upfs` is provided for usage in `/etc/fstab`. The permissions and store
roots should be colon-separated, so an example full `fstab` line would be:

```
/mnt/disk_p:/mnt/disk_s /mnt/disk upfs defaults 0 2
```

Because mounting of `fstab` filesystems is unordered, and because it's common
for one or both of the permissions and store directories to be full filesystems
which must be fully mounted before UpFS, `mount.upfs` provides mount options to
explicitly mount its root directories, `mount_p` and `mount_s`.  These are
options to `mount.upfs`, but not `upfs`. An example with the storage directory
mounted from another filesystem:

```
/dev/sdb1 /mnt/disk_s vfat check=s,shortname=winnt,uid=0,gid=0,umask=077,noauto 0 2
/mnt/disk_p:/mnt/disk_s /mnt/disk upfs mount_s 0 2
```

Note that `noauto` on directories to be mounted by `mount_p` or `mount_s` isn't
critical, but it does make the output a bit cleaner, as `upfs`'s `mount` will
reliably succeed.

## Permissions in the store

As an alternative to mirroring the store directory into a separate permissions
directory, UpFS is capable of storing the permissions in its own index files in
the store directory, in a mode called UpFS-PS (UpFS-permissions-in-store).
UpFS-PS is implemented in `upfs-ps` and `mount.upfsps`. For instance:

```
# mkdir /mnt/home_s
# mount -t vfat -o check=s,shortname=winnt,uid=0,gid=0,umask=077 /dev/sdb1 /mnt/home_s
# mount -t upfsps /mnt/home_s /home
```

Note that when using `upfs-ps` directly, is is mandatory to use
`default_permissions`; otherwise, permissions checks are not performed at all.
`mount.upfsps` always sets `default_permissions`.

UpFS-PS is much slower than UpFS, as `default_permissions` is slow, and the
implementation of UpFS's index files is inefficient. UpFS-PS's index files are
case insensitive, so UpFS-PS can be safely used with a case-insensitive store
filesystem.

For `fstab` usage, `mount.upfsps` implements a `mount_r` option to mount its
store directory.

## Implementation

UpFS is implemented as a FUSE filesystem. This makes it slow. For my use case,
in which the store is on an SD card, the I/O latency is so significant that
FUSE itself adds no appreciable slowdown. This is probably the case for most
realistic setups.

It could just as well be implemented as a kernel module, but I haven't looked
into this option.

UpFS-PS's index files are named `.upfs`. Rather than having a single index file
at the root of the store (which is how umsdos worked), there is an index file
in every directory in which permissions have been set. This is a bit messy, but
has some advantages with respect to performance and interoperability.

`.upfs` files have a very trivial format, documented in `upfs-ps.h`. `.upfs`
files are safe across machines with different word sizes, but are not safe
across machines with different endians, they use the host endianness. This is
more-or-less intentional, as big endian machines are sufficiently dead that
compatibility with them isn't worthwhile.
