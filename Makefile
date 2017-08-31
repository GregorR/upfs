CC=gcc
CFLAGS=-O3
FUSE_FLAGS=`pkg-config --cflags --libs fuse`

all: upfs upfs-ps mount.upfs

upfs: upfs.c
	$(CC) $(CFLAGS) upfs.c $(FUSE_FLAGS) -o upfs

upfs-ps: upfs.c upfs-ps.c
	$(CC) $(CFLAGS) -DUPFS_PS=1 upfs.c upfs-ps.c $(FUSE_FLAGS) -o upfs-ps

mount.upfs: mountupfs.c
	$(CC) $(CFLAGS) mountupfs.c -o mount.upfs

install: all
	install upfs /usr/bin/upfs
	install upfs-ps /usr/bin/upfs-ps
	install mount.upfs /sbin/mount.upfs

clean:
	rm -f upfs upfs-ps mount.upfs
