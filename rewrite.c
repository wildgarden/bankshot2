#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <malloc.h>
#include <time.h>

#define PMFS_COW_WRITE	0xBCD00012

struct extent_entry {
	off_t offset;
	size_t length;
	int dirty;
	unsigned long mmap_addr;
};

struct sync_range {
	off_t offset;
	size_t length;
};

struct write_request {
	char* buf;
	loff_t offset;
	size_t len;
};

int main(int argc, char *argv[])
{
	int fd1;
	struct write_request packet;
	off_t offset;
	size_t len;
	unsigned long count;
	char* buf = malloc(4096);
	int i;

	if (argc != 4) {
		printf("Usage: ./rewrite offset size count\n");
		return 0;
	}

	offset = atol(argv[1]);
	len = atol(argv[2]);
	count = atol(argv[3]);
	fd1 = open("/mnt/ramdisk/test1", O_RDWR | O_CREAT, 0640);
	packet.offset = offset;
	packet.len = len;
	packet.buf = buf;

	for (i = 0; i < count; i++)
		ioctl(fd1, PMFS_COW_WRITE, &packet);

	close(fd1);
	free(buf);

	return 0;
}
