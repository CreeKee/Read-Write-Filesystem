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
uint32_t* MRUblock = (uint32_t*)calloc(BLOCKSIZE, sizeof(uint32_t));
uint32_t bmsize = BLOCKSIZE;

inline uint32_t getMRU(uint32_t fileHead){

	uint32_t mru = 0;

	if(fileHead >= BLOCKSIZE){
		expandCache();
	}
	else{
		mru = MRUblock[fileHead];
	}
	return mru;
}

inline void setMRU(uint32_t fileHead, uint32_t curBlock){
	if(fileHead >= BLOCKSIZE){
		expandCache();
	}
	
	MRUblock[fileHead] = curBlock;

}

inline void expandCache(){
	bmsize = bmsize << 1;
	if((MRUblock = (uint32_t*)realloc(MRUblock, bmsize)) == NULL){
		perror("failed to reallocate cache");
		exit(-1);
	}
	memset(MRUblock+(bmsize>>1), 0, bmsize>>1);
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
uint32_t removeDirEntry(int fd, uint32_t dirHeadOffset, uint32_t base, uint32_t offset, uint32_t size, uint64_t newDirSize){
	char remainder[BLOCKSIZE] = {0};

	dread(fd, remainder, ((offset%BLOCKSIZE) - size) - BNUMSIZE, offset+size, "failed to read dir remainder\n");
	dwrite(fd, remainder, ((offset%BLOCKSIZE)) - BNUMSIZE, offset, "failed to write dir remainder\n");

	dwrite(fd, (void*)(&offset), SIZESIZE, dirHeadOffset+SIZEDEX, "failed to write new dir size\n");

	return remainder[0] == 0 && offset - base == BNUMSIZE ? base : 0;

}

/*verified*/
uint32_t moveToExtent(int fd, uint32_t* base, uint32_t offset, uint32_t headSize){

	if(pread(fd, (void*)(base), BNUMSIZE, offset) != BNUMSIZE){
		perror("failed to read extent number\n");
		exit(-1);
	}

	*base = INDEX(*base);
	return *base+headSize;
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

	bool found = false;
	struct Args *fs = (struct Args*)args;
	dirEntry entry;
	
	uint32_t base = INDEX(block_num);
	uint32_t offset = base+INODESIZE;

	inodeHead dirHead = readInode(fs->fd, base);

	while(!found){

		entry = readDirEntry(fs->fd, &offset);

		if(entry.len == 0){

			offset = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE, DIREXTENTHEADSIZE);

		}
		else if(strcmp(name, entry.name) == 0){
			found = true;

			if((base = removeDirEntry(fs->fd, INDEX(block_num), base, offset, entry.len, dirHead.size-entry.len)) != 0){
				//collapse empty extent block
			}

			//remove delted directory blocks

			free(entry.name);
		}
		else{
			free(entry.name);
		}

		if(ENDBLOCK(base, offset)) offset = moveToExtent(fs->fd, &base, base+BLOCKSIZE-BNUMSIZE, DIREXTENTHEADSIZE);
	}

	//free dir blocks

	//create buffer for remaining section of dir
	//read remainder of dir entry
	//write remainder of dir entry
	//update entry count
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

