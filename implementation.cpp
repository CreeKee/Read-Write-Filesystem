#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <algorithm>

#include "defs.h"

#include "cpe453fs.h"


/**************************************************************/
/*Structs*/
/**************************************************************/

struct Args
{
	int fd;
};

struct __attribute__ ((packed)) inodeHead{
	uint32_t typeCode;
	uint16_t mode;
	uint16_t Nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t rdev;
	uint32_t flags;
	uint32_t accessTimeS;
	uint32_t accessTimeNS;
	uint32_t modTimeS;
	uint32_t modTimeNS;
	uint32_t statusTimeS;
	uint32_t statusTimeNS;
	uint64_t size;
	uint64_t blocks;
};

struct dirEntry{
	uint16_t len;
	uint32_t inode_num;
	char* name;
	inodeHead inode;
};



/**************************************************************/
/*Helper functions*/
/**************************************************************/

/*verified*/
inodeHead readInode(int fd, uint32_t offset){
	fprintf(stderr,"reading Inode at offset: %d\n",offset);
	inodeHead node;

	if(pread(fd, (void*)(&node), INODESIZE, offset) != INODESIZE){
		perror("failed to read entire Inode header\n");
		exit(-1);
	}

	return node;
}

/*verified*/
dirEntry readDirEntry(int fd, uint32_t* offset){

	fprintf(stderr, "reading dir entry at offset %d\n", *offset);

	dirEntry entry;

	if(pread(fd, (void*)(&(entry.len)), LENSIZE, *offset) != LENSIZE){
		perror("failed to read dir entry size\n");
		exit(-1);
	}
	else{
		*offset += LENSIZE;
	}

	if(pread(fd, (void*)(&(entry.inode_num)), BNUMSIZE, *offset) != BNUMSIZE){
		perror("failed to read dir entry inode number\n");
		exit(-1);
	}
	else{
		*offset += BNUMSIZE;
	}

	if((entry.name = (char*)calloc(entry.len+1-BNUMSIZE-LENSIZE, sizeof(char))) == NULL){
		perror("failed to allocate space for dir entry name\n");
		exit(-1);
	}

	if(pread(fd, (void*)(entry.name), entry.len-BNUMSIZE-LENSIZE, *offset) != entry.len-BNUMSIZE-LENSIZE){
		perror("failed to read dir entry name\n");
		exit(-1);
	}
	else{
		*offset += entry.len-BNUMSIZE-LENSIZE;
	}
	fprintf(stderr, "finished reading dir entry and found len %d, inode num %x, and name %s\n",entry.len, entry.inode_num, entry.name);
	return entry;
}

/*untested*/
uint32_t moveToExtent(int fd, uint32_t* base, uint32_t offset){

	if(pread(fd, (void*)(base), BNUMSIZE, offset) != BNUMSIZE){
		perror("failed to read extent number\n");
		exit(-1);
	}

	*base = INDEX(*base);
	return *base+4;
}



/**************************************************************/
/*Read only functions*/
/**************************************************************/

/*verified*/
static void set_file_descriptor(void *args, int fd)
{
	struct Args *fs = (struct Args*)args;
	fs->fd = fd;
}

/*verified*/
static int mygetattr(void *args, uint32_t block_num, struct stat *stbuf){
	struct Args *fs = (struct Args*)args;

	//check if valid blocknum?
	inodeHead curHead = readInode(fs->fd, INDEX(block_num));

	stbuf->st_dev = 0;			//idk
	stbuf->st_ino = block_num; 	//maybe?
	stbuf->st_mode = curHead.mode;
	stbuf->st_nlink = curHead.Nlink;
	stbuf->st_uid = curHead.uid;
	stbuf->st_gid = curHead.gid;
	stbuf->st_rdev = curHead.rdev;
	stbuf->st_size = curHead.size;
	stbuf->st_blksize = BLOCKSIZE;
	stbuf->st_blocks = curHead.blocks*8;

	//need to update??
	stbuf->st_atim.tv_sec = curHead.accessTimeS;
	stbuf->st_atim.tv_nsec = curHead.accessTimeNS;

	stbuf->st_mtim.tv_sec = curHead.modTimeS;
	stbuf->st_mtim.tv_nsec = curHead.modTimeNS;

	stbuf->st_ctim.tv_sec = curHead.statusTimeS;
	stbuf->st_ctim.tv_nsec = curHead.statusTimeNS;

    return 0;
}

/*trusted*/
static int myreaddir(void *args, uint32_t block_num, void *buf, CPE453_readdir_callback_t cb)
{
	fprintf(stderr, "reading dir at block num %d\n",block_num);
	struct Args *fs = (struct Args*)args;
	uint32_t base = INDEX(block_num);
	inodeHead dirHead = readInode(fs->fd, base);
	uint32_t offset = base+INODESIZE;
	dirEntry entry;

	while((signed)dirHead.size > 0){

		entry = readDirEntry(fs->fd, &offset);
		entry.inode = readInode(fs->fd, INDEX(entry.inode_num));
		
		cb(buf, entry.name, entry.inode_num);
		
		dirHead.size -= (entry.len);
		free(entry.name);

		if(ENDBLOCK(base, offset)) offset = moveToExtent(fs->fd, &base, offset);
	}
    return 0;
}

static int myopen(void *args, uint32_t block_num)
{
	struct Args *fs = (struct Args*)args;

	inodeHead inode = readInode(fs->fd, INDEX(block_num));


	return S_ISREG(inode.mode)/*&&(check permissions)*/ ? 0 : -1;


    return 0;
}

static int myread(void *args, uint32_t block_num, char *buf, size_t size, off_t offset)
{
	struct Args *fs = (struct Args*)args;
	uint32_t base = INDEX(block_num);
	uint32_t cur;
	uint32_t index = 0;
	//assuming file is properly openend
	inodeHead inode = readInode(fs->fd, INDEX(block_num));

	int32_t delta = std::min((int)size, (int)(inode.size-offset));
	uint32_t metaSize = BLOCKSIZE - INODESIZE - BNUMSIZE;

	if(delta < 0){
		//TODO error
		exit(-2);
	}

	while(offset > metaSize){
		moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE);
		offset -= metaSize;
		metaSize = BLOCKSIZE - 2*BNUMSIZE - TYPECODESIZE;
	}

	cur = base+BNUMSIZE+TYPECODESIZE+offset+INODESIZE;

	while(delta > 0){

		metaSize = std::min((int)(BLOCKSIZE + base - cur - BNUMSIZE), (int)delta);

		if(pread(fs->fd, buf+index, metaSize, cur) != metaSize){
			perror("failed to read from file into buffer");
			exit(-1);
		}
		index += metaSize;
		delta -= metaSize;
		

		if(delta>0) cur = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE);
	}

    return index;
}

static int myreadlink(void *args, uint32_t block_num, char *buf, size_t size)
{
	struct Args *fs = (struct Args*)args;
	uint32_t base = INDEX(block_num);
	//assuming file is properly openend
	inodeHead inode = readInode(fs->fd, INDEX(block_num));

	int32_t delta = std::min((int)size, (int)(inode.size));

	if(delta < 0){
		//TODO error
		exit(-2);
	}

	if(pread(fs->fd, buf, delta, base+INODESIZE) != delta){
		perror("failed to read from file into buffer");
		exit(-1);
	}


	return 0;
}

static uint32_t root_node(void *args)
{
	struct Args *fs = (struct Args*)args;

	uint32_t root_block;

	if(pread(fs->fd, (void*)(&root_block), BNUMSIZE, BLOCKSIZE - 2*BNUMSIZE)!= BNUMSIZE){
		perror("failed to read root block\n");
		exit(-1);
	}
	return root_block;
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

