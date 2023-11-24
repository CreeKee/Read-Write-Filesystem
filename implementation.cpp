#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <algorithm>

#include "cpe453fs.h"

#define TYPECODESIZE 4
#define MODESIZE 2
#define NLINKSIZE 2
#define UIDSIZE 4
#define GIDSIZE 4
#define RDEVSIZE 4
#define USERFLAGSSIZE 4
#define TIMESIZE 4
#define SIZESIZE 8
#define ALLBLOCKSSIZE 8
#define NEXTEXTENTSIZE 4
#define DIREXTENTHEADSIZE TYPECODESIZE
#define FILEEXTENTHEADSIZE (TYPECODESIZE+BNUMSIZE)
#define MINDIRSIZE 7

#define INODESIZE 64

#define SUPERBLOCK 0
#define INODE 1

#define BLOCKSIZE 4096
#define BLOCKSHIFT 12
#define INDEX(block) (block<<BLOCKSHIFT)
#define ENDBLOCK(base, offset) ((offset - base) >= (BLOCKSIZE-BNUMSIZE-MINDIRSIZE))


#define LENSIZE 2
#define BNUMSIZE 4

#define MODEDEX 4
#define UIDDEX 8
#define GIDDEX 12
#define ATIMESDEX 24
#define ATIMENSDEX 28
#define MTIMESDEX 32
#define MTIMENSDEX 36
#define SIZEDEX 48
#define ALLBLOCKSDEX 56

#define EXTENT_NUM 3
#define FREE_NUM 5


#define LAZYWRITE(a, b) (pwrite(fs->fd, (void*)(&a), BNUMSIZE, INDEX(block_num)+b) != BNUMSIZE)
#define dread(fd, buff, size, offset, msg) if(pread(fd, buff, size, offset) != 0){perror(msg);exit(-1);}
#define dwrite(fd, buff, size, offset, msg) if(pwrite(fd, buff, size, offset) != 0){perror(msg);exit(-1);}

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


/**************************************************************
cache functions
**************************************************************/
uint32_t* nextcache = (uint32_t*)malloc(sizeof(uint32_t)<<BLOCKSHIFT);
uint32_t bmsize = BLOCKSIZE;

void myinit(void){
	memset(nextcache, -1, BLOCKSIZE*sizeof(uint32_t));
}

void mydestroy(void){
	free(nextcache);
}

inline void expandCache(){
	bmsize = bmsize << 1;
	if((nextcache = (uint32_t*)realloc(nextcache, bmsize)) == NULL){
		perror("failed to reallocate cache");
		exit(-1);
	}
	memset(nextcache+(bmsize>>1), -1, sizeof(uint32_t)*(bmsize>>1));
}

inline uint32_t getNextCache(int fd, uint32_t block_num){

	uint32_t mru = 0;

	if(block_num >= bmsize){
		expandCache();
	}
	if((signed)(mru = nextcache[block_num]) == -1){

		dread(fd, (void*)(&nextcache[block_num]), BNUMSIZE, INDEX(block_num)+BLOCKSIZE-BNUMSIZE, "failed to rea value into cache");
		mru = nextcache[block_num];
	}
	return mru;
}

inline void setNextCache(uint32_t cur_block_num, uint32_t next_block_num){
	if(cur_block_num >= BLOCKSIZE){
		expandCache();
	}
	
	nextcache[cur_block_num] = next_block_num;

}



/**************************************************************
Helper functions
**************************************************************/

//void (*CPE453_readdir_callback_t)(void *, const char*, uint32_t)

/*verified*/
inodeHead readInode(int fd, uint32_t offset){
	//fprintf(stderr,"reading Inode at offset: %d\n",offset);
	inodeHead node;

	if(pread(fd, (void*)(&node), INODESIZE, offset) != INODESIZE){
		perror("failed to read entire Inode header\n");
		exit(-1);
	}

	return node;
}

/*verified*/
dirEntry readDirEntry(int fd, uint32_t* offset){

	//fprintf(stderr, "reading dir entry at offset %d\n", *offset);

	dirEntry entry;

	if(pread(fd, (void*)(&(entry.len)), LENSIZE, *offset) != LENSIZE){
		perror("failed to read dir entry size\n");
		exit(-1);
	}
	else{
		*offset += LENSIZE;
	}

	if(entry.len != 0){
		if(pread(fd, (void*)(&(entry.inode_num)), BNUMSIZE, *offset) != BNUMSIZE){
			perror("failed to read dir entry inode number\n");
			exit(-1);
		}
		else{
			*offset += BNUMSIZE;
		}
		

		if((entry.name = (char*)calloc(entry.len+1-BNUMSIZE-LENSIZE, sizeof(char))) == NULL){
			perror("failed to allocate space for dir entry name\n");
			//fprintf(stderr, "tried to allocate size of %d bytes\n", entry.len+1-BNUMSIZE-LENSIZE);
			exit(-1);
		}

		if(pread(fd, (void*)(entry.name), entry.len-BNUMSIZE-LENSIZE, *offset) != entry.len-BNUMSIZE-LENSIZE){
			perror("failed to read dir entry name\n");
			exit(-1);
		}
		else{
			*offset += entry.len-BNUMSIZE-LENSIZE;
		}
	}
	//fprintf(stderr, "finished reading dir entry and found len %d, inode num %x, and name %s\n",entry.len, entry.inode_num, entry.name);
	return entry;
}

/*
returns the block number of a now empty extent block which needs to be removed
or 0 if no extent block has been emptied
*/


/*verified*/
uint32_t moveToExtent(int fd, uint32_t* base, uint32_t offset, uint32_t headSize){

	if(pread(fd, (void*)(base), BNUMSIZE, offset) != BNUMSIZE){
		perror("failed to read extent number\n");
		exit(-1);
	}

	*base = INDEX(*base);
	return *base+headSize;
}

void freeBlock(int fd, uint32_t target_offset){

	//initialize free block stack
	uint32_t free_num_buff[2] = {FREE_NUM,getNextCache(fd, 0)};

	setNextCache(target_offset>>BLOCKSHIFT, free_num_buff[1]);

	//free targeted block
	dwrite(fd, (void*)(free_num_buff), BNUMSIZE<<1, target_offset+BLOCKSIZE-BNUMSIZE, "failed to write continuing block num");

	//update next free block in both cache and super block
	setNextCache(0, (free_num_buff[1] = target_offset >> BLOCKSHIFT));
	dwrite(fd, (void*)(&free_num_buff[1]), BNUMSIZE, BLOCKSIZE-BNUMSIZE, "failed to update free stack head in super block");

}

void freeExtentBlock(int fd, uint32_t target_block_offset, uint32_t prev_block_offset){
	
	//get address of next block in the chain
	uint32_t num = getNextCache(fd, target_block_offset>>BLOCKSHIFT);

	//connect previous block to next block
	dwrite(fd, (void*)(&num), BNUMSIZE, prev_block_offset+BLOCKSIZE-BNUMSIZE, "failed to write continuing block num");
	setNextCache(prev_block_offset>>BLOCKSHIFT, num);

	//free the target block
	freeBlock(fd, target_block_offset);
}

void chainFree(int fd, uint32_t start_offset){

	uint32_t target_block = start_offset>>BLOCKSHIFT;

	while(target_block != 0){
		freeBlock(fd, INDEX(target_block));
		target_block = getNextCache(fd, target_block);
	}
}

void removeDirEntry(int fd, uint32_t dirHeadOffset, uint32_t base, uint32_t offset, uint32_t size, uint64_t newDirSize, uint32_t prev_block_num){
	char remainder[BLOCKSIZE] = {0};

	//shift remainder of directory data to cover over the deleted entry
	dread(fd, remainder, ((offset%BLOCKSIZE) - size) - BNUMSIZE, offset+size, "failed to read dir remainder\n");
	dwrite(fd, remainder, ((offset%BLOCKSIZE)) - BNUMSIZE, offset, "failed to write dir remainder\n");

	//update new directory size
	dwrite(fd, (void*)(&offset), SIZESIZE, dirHeadOffset+SIZEDEX, "failed to write new dir size\n");

	//check if an extent block has now been emptied and should be freed
	if(remainder[0] == 0 && offset - base == BNUMSIZE){
		freeExtentBlock(fd, base, prev_block_num);
	}
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
static int myreaddir(void *args, uint32_t block_num, void *buf, CPE453_readdir_callback_t cb){
	//fprintf(stderr, "reading dir at block num %d\n",block_num);
	struct Args *fs = (struct Args*)args;
	uint32_t base = INDEX(block_num);
	inodeHead dirHead = readInode(fs->fd, base);
	uint32_t offset = base+INODESIZE;
	dirEntry entry;

	while((signed)dirHead.size > 0){

		entry = readDirEntry(fs->fd, &offset);

		if(entry.len != 0){

			entry.inode = readInode(fs->fd, INDEX(entry.inode_num));
			
			cb(buf, entry.name, entry.inode_num);
			
			dirHead.size -= (entry.len);
			free(entry.name);
		}
		else{
			offset = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE, DIREXTENTHEADSIZE);
		}

		if(ENDBLOCK(base, offset)) offset = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE, DIREXTENTHEADSIZE);
	}
    return 0;
}

/*verified*/
static int myopen(void *args, uint32_t block_num)
{
	struct Args *fs = (struct Args*)args;

	inodeHead inode = readInode(fs->fd, INDEX(block_num));


	return S_ISREG(inode.mode)/*&&(check permissions)*/ ? 0 : -1;


    return 0;
}

/*verified*/
static int myread(void *args, uint32_t block_num, char *buf, size_t size, off_t offset)
{
	//fprintf(stderr, "reading from block %d, size %d, offset %d\n",(int)block_num, (int)size, (int)offset);
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

	cur = base+INODESIZE;

	while(offset >= metaSize){
		cur = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE, FILEEXTENTHEADSIZE);
		offset -= metaSize;
		metaSize = BLOCKSIZE - FILEEXTENTHEADSIZE - BNUMSIZE;
	}
	cur += offset;

	while(delta > 0){

		metaSize = std::min((int)(BLOCKSIZE + base - cur - BNUMSIZE), (int)delta);

		if(pread(fs->fd, buf+index, metaSize, cur) != metaSize){
			perror("failed to read from file into buffer");
			exit(-1);
		}
		index += metaSize;
		delta -= metaSize;
		

		if(delta>0) cur = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE, FILEEXTENTHEADSIZE);
	}

    return index;
}

/*verified*/
static int myreadlink(void *args, uint32_t block_num, char *buf, size_t size)
{
	struct Args *fs = (struct Args*)args;
	uint32_t base = INDEX(block_num);
	//assuming file is properly openend
	inodeHead inode = readInode(fs->fd, INDEX(block_num));

	int32_t delta = std::min((int)size-1, (int)(inode.size));

	if(delta < 0){
		//TODO error
		exit(-2);
	}

	if(pread(fs->fd, buf, delta, base+INODESIZE) != delta){
		perror("failed to read from file into buffer");
		exit(-1);
	}
	buf[delta] = 0;
	return 0;
}

/*verified*/
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



/**************************************************************/
/*Read only functions*/
/**************************************************************/
int chmod(void *args, uint32_t block_num, mode_t new_mode){
	struct Args *fs = (struct Args*)args;
	return LAZYWRITE(new_mode, MODEDEX);
}

int chown(void* args, uint32_t block_num, uid_t new_uid, gid_t new_gid){
	
	struct Args *fs = (struct Args*)args;

	return -(LAZYWRITE(new_uid, UIDDEX)||LAZYWRITE(new_gid, GIDDEX));

}

int utimens(void* args, uint32_t block_num, const struct timespec tv[2]){
	
	struct Args *fs = (struct Args*)args;
	
	return -(LAZYWRITE(tv[0].tv_sec, ATIMESDEX)||LAZYWRITE(tv[0].tv_nsec,ATIMENSDEX)||
			LAZYWRITE(tv[1].tv_sec, MTIMESDEX)||LAZYWRITE(tv[1].tv_nsec, MTIMENSDEX));
}

int rmdir(void* args, uint32_t block_num, const char *name){
	//locate entry name

	struct Args *fs = (struct Args*)args;
	bool found = false;
	dirEntry entry;
	uint32_t prev = 0;
	uint32_t base = INDEX(block_num);
	uint32_t offset = base+INODESIZE;

	inodeHead dirHead = readInode(fs->fd, base);

	while(!found){

		entry = readDirEntry(fs->fd, &offset);

		if(entry.len == 0){
			prev = base;
			offset = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE, DIREXTENTHEADSIZE);
			

		}
		else if(strcmp(name, entry.name) == 0){
			found = true;

			removeDirEntry(fs->fd, INDEX(block_num), base, offset, entry.len, dirHead.size-entry.len, prev);

			chainFree(fs->fd, entry.inode_num);
		}

		free(entry.name);

		if(ENDBLOCK(base, offset)){
			prev = base;
			offset = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE, DIREXTENTHEADSIZE);
		}
	}
	return 0;
	//free dir blocks

	//create buffer for remaining section of dir
	//read remainder of dir entry
	//write remainder of dir entry
	//update entry count
} 

int unlink(void* args, uint32_t block_num, const char *name){
	struct Args *fs = (struct Args*)args;

	fprintf(stderr, "unlink placeholder\n");
	return 0;
}

int mknod(void* args, uint32_t parent_block, const char *name, mode_t new_mode, dev_t new_dev){
	struct Args *fs = (struct Args*)args;
	fprintf(stderr, "mknod placeholder\n");
	return 0;
}

int symlink(void* args, uint32_t parent_block, const char *name, const char *link_dest){
	struct Args *fs = (struct Args*)args;
	fprintf(stderr, "symlink placeholder\n");
	return 0;
}

int mkdir(void* args, uint32_t parent_block, const char *name, mode_t new_mode){
	struct Args *fs = (struct Args*)args;
	fprintf(stderr, "mkdir placeholder\n");
	return 0;
}

int link(void* args, uint32_t parent_block, const char *name, uint32_t dest_block){
	struct Args *fs = (struct Args*)args;
	fprintf(stderr, "link placeholder\n");
	return 0;
}

int rename(void* args, uint32_t old_parent, const char *old_name, uint32_t new_parent, const char *new_name){
	struct Args *fs = (struct Args*)args;
	fprintf(stderr, "rename placeholder\n");
	return 0;
}

int truncate(void* args, uint32_t block_num, off_t new_size){
	struct Args *fs = (struct Args*)args;
	fprintf(stderr, "truncate placeholder\n");
	return 0;
}

int write(void* args, uint32_t block_num, const char *buff, size_t wr_len, off_t wr_offset){
	struct Args *fs = (struct Args*)args;
	fprintf(stderr, "write placeholder\n");
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
	ops.init = myinit;
	ops.destroy = mydestroy;

	return &ops;
}

#ifdef  __cplusplus
}
#endif

