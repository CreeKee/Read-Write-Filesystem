#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/statvfs.h>

#include "defs.h"

#include "cpe453fs.h"


struct Args
{
	int fd;
};

struct __attribute__ ((packed)) inodeHead{
	unsigned char typeCode;
	unsigned char modeNlink;
	unsigned char uid;
	unsigned char gid;
	unsigned char accessTimeS;
	unsigned char accessTimeNS;
	unsigned char modTimeS;
	unsigned char modTimeNS;
	unsigned char statusTimeS;
	unsigned char statusTimeNS;
	uint16_t size;
	uint16_t blocks;
};

inodeHead readInode(int fd, uint32_t offset){

	inodeHead node;

	if(pread(fd, (void*)(&node), INODESIZE, offset) != INODESIZE){
		perror("failed to read entire Inode header\n");
		exit(-1);
	}

	return node;
}

static void set_file_descriptor(void *args, int fd)
{
	struct Args *fs = (struct Args*)args;
	fs->fd = fd;
}

static int mygetattr(void *args, uint32_t block_num, struct stat *stbuf)
{
	struct Args *fs = (struct Args*)args;

	//check if valid blocknum?
	inodeHead curHead = readInode(fs->fd, INDEX(block_num));

	//stbuf->st_dev =
	//stbuf->st_ino = 
	stbuf->st_mode = curHead.modeNlink>>16;
	stbuf->st_nlink = curHead.modeNlink&LINKMASK;
	stbuf->st_uid = curHead.uid;
	stbuf->st_gid = curHead.gid;
	//...


    return 0;
}

static int myreaddir(void *args, uint32_t block_num, void *buf, CPE453_readdir_callback_t cb)
{
	// struct Args *fs = (struct Args*)args;
    return 0;
}

static int myopen(void *args, uint32_t block_num)
{
	// struct Args *fs = (struct Args*)args;
    return 0;
}

static int myread(void *args, uint32_t block_num, char *buf, size_t size, off_t offset)
{
	// struct Args *fs = (struct Args*)args;
    return 0;
}

static int myreadlink(void *args, uint32_t block_num, char *buf, size_t size)
{
	// struct Args *fs = (struct Args*)args;
	return 0;
}


static uint32_t root_node(void *args)
{
	// struct Args *fs = (struct Args*)args;
	return 0;
}

#ifdef  __cplusplus
extern "C" {
#endif

struct cpe453fs_ops *CPE453_get_operations(void)
{
	static struct cpe453fs_ops ops;
	static struct Args args;
	memset(&ops, 0, sizeof(ops));
	ops.arg = &args;

	ops.getattr = mygetattr;
	ops.readdir = myreaddir;
	ops.open = myopen;
	ops.read = myread;
	ops.readlink = myreadlink;
	ops.root_node = root_node;
	ops.set_file_descriptor = set_file_descriptor;

	return &ops;
}

#ifdef  __cplusplus
}
#endif

