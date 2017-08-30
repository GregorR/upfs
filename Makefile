CC=gcc
CFLAGS=-O3
FUSE_FLAGS=`pkg-config --cflags --libs fuse`

all: upfs mount.upfs

upfs: upfs.c
	$(CC) $(CFLAGS) upfs.c $(FUSE_FLAGS) -o upfs

mount.upfs: mountupfs.c
	$(CC) $(CFLAGS) mountupfs.c -o mount.upfs

install: all
	install upfs /usr/bin/upfs
	install mount.upfs /sbin/mount.upfs

clean:
	rm -f upfs mount.upfs
