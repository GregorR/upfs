all: upfs

upfs: upfs.c
	gcc -g upfs.c `pkg-config --cflags --libs fuse` -o upfs

clean:
	rm -f upfs
