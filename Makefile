CC=gcc
ECFLAGS=-O3
CFLAGS=-DUPFS_LNCP $(ECFLAGS)
FUSE_FLAGS=`pkg-config --cflags --libs fuse`

all: upfs upfs-ps mount.upfs mount.upfsps

upfs: upfs.c
	$(CC) $(CFLAGS) upfs.c $(FUSE_FLAGS) -o upfs

upfs-ps: upfs.c upfs-ps.c
	$(CC) $(CFLAGS) -DUPFS_PS=1 upfs.c upfs-ps.c $(FUSE_FLAGS) -o upfs-ps

mount.upfs: mountupfs.c
	$(CC) $(CFLAGS) mountupfs.c -o mount.upfs

mount.upfsps: mountupfsps.c
	$(CC) $(CFLAGS) mountupfsps.c -o mount.upfsps

install: all
	install upfs /usr/bin/upfs
	install upfs-ps /usr/bin/upfs-ps
	install mount.upfs /sbin/mount.upfs
	install mount.upfsps /sbin/mount.upfsps

clean:
	rm -f upfs upfs-ps mount.upfs mount.upfsps
