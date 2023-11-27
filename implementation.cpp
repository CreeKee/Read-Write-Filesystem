#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <algorithm>
#include <fuse.h>

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
#define NLINKDEX 6
#define UIDDEX 8
#define GIDDEX 12
#define ATIMESDEX 24
#define ATIMENSDEX 28
#define MTIMESDEX 32
#define MTIMENSDEX 36
#define SIZEDEX 48
#define ALLBLOCKSDEX 56

#define INODE_NUM 2
#define EXTENT_NUM 3
#define FREE_NUM 5

#define MAXDIRENTRYSIZE (BLOCKSIZE-16)

#define LAZYWRITE(a, b) (pwrite(fs->fd, (void*)(&a), BNUMSIZE, INDEX(block_num)+b) != BNUMSIZE)
#define dread(fd, buff, size, offset, msg) if(pread(fd, (void*)(buff), size, offset) != size){perror(msg);exit(-1);}
#define dwrite(fd, buff, size, offset, msg) if(pwrite(fd, (void*)(buff), size, offset) != size){perror(msg);exit(-1);}

#define FILLINODE(md, sze) {\
	inode.typeCode = INODE_NUM;\
	inode.mode =  md;\
	inode.Nlink = 1;\
	inode.uid = cntxt->uid;\
	inode.gid = cntxt->gid;\
	inode.rdev = 0;\
	inode.size = sze;\
	inode.blocks = 0;\
}

#define FILLENTRY {\
	entry.inode = inode;\
	entry.inode_num = bnum;\
	entry.name = (char*)malloc(strlen(name));\
	memcpy(entry.name, name, strlen(name));\
	entry.len = strlen(name)+SIZESIZE+BNUMSIZE;\
}
/**************************************************************
structs
**************************************************************/

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

class Cache{
	private:
	uint32_t* nextcache;
	uint32_t bmsize;

	inline void expandCache();

	public:
	Cache();
	~Cache();

	inline uint32_t getNext(int fd, uint32_t block_num);
	inline void setNext(uint32_t cur_block_num, uint32_t next_block_num);
	inline void setNext(int fd, uint32_t cur_block_num, uint32_t next_block_num);

	uint32_t getNewBlock(int fd, void* buff, uint32_t headsize, bool purge);
};

uint32_t Cache::getNewBlock(int fd, void* buff, uint32_t headsize, bool purge){
	uint8_t* cleanse;
	uint32_t bnum = getNext(fd, 0);
	setNext(fd, 0, getNext(fd, bnum));
	setNext(fd, bnum, 0);

	dwrite(fd, buff, headsize, INDEX(bnum), "failed to write inode head when making node\n");

	if(purge){
		cleanse = (uint8_t*)calloc(sizeof(uint8_t), BLOCKSIZE-BNUMSIZE-headsize);
		dwrite(fd, cleanse, BLOCKSIZE-BNUMSIZE-headsize, INDEX(bnum)+headsize, "failed to write inode head when making node\n");
		free(cleanse);
	}

	return bnum;
}

Cache::Cache(){
	bmsize = BLOCKSIZE;
	nextcache = (uint32_t*)malloc(sizeof(uint32_t)<<BLOCKSHIFT);
	memset(nextcache, -1, BLOCKSIZE*sizeof(uint32_t));
}

Cache::~Cache(){
	free(nextcache);
}

inline void Cache::expandCache(){
	bmsize = bmsize << 1;
	if((nextcache = (uint32_t*)realloc(nextcache, bmsize)) == NULL){
		perror("failed to reallocate cache");
		exit(-1);
	}
	memset(nextcache+(bmsize>>1), -1, sizeof(uint32_t)*(bmsize>>1));
}

inline uint32_t Cache::getNext(int fd, uint32_t block_num){

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

inline void Cache::setNext(uint32_t cur_block_num, uint32_t next_block_num){
	if(cur_block_num >= BLOCKSIZE){
		expandCache();
	}
	nextcache[cur_block_num] = next_block_num;

}

inline void Cache::setNext(int fd, uint32_t cur_block_num, uint32_t next_block_num){
	if(cur_block_num >= BLOCKSIZE){
		expandCache();
	}
	nextcache[cur_block_num] = next_block_num;

	dwrite(fd, (void*)(&next_block_num), BNUMSIZE, INDEX(cur_block_num)+BLOCKSIZE-BNUMSIZE, "failed to write next block num\n");

}

Cache ncache;

/**************************************************************
Helper functions
**************************************************************/

/*verified*/
inodeHead readInode(int fd, uint32_t offset){
	//fprintf(stderr,"reading Inode at offset: %d\n",offset);
	inodeHead node;

	dread(fd, &node, INODESIZE, offset, "failed to read entire Inode header\n");

	return node;
}

void freeBlock(int fd, uint32_t target_offset){

	//initialize free block stack
	uint32_t free_num_buff[2] = {FREE_NUM,ncache.getNext(fd, 0)};

	ncache.setNext(target_offset>>BLOCKSHIFT, free_num_buff[1]);

	//free targeted block
	dwrite(fd, (void*)(free_num_buff), BNUMSIZE<<1, target_offset+BLOCKSIZE-BNUMSIZE, "failed to write continuing block num");

	//update next free block in both cache and super block
	ncache.setNext(0, (free_num_buff[1] = target_offset >> BLOCKSHIFT));
	dwrite(fd, (void*)(&free_num_buff[1]), BNUMSIZE, BLOCKSIZE-BNUMSIZE, "failed to update free stack head in super block");

}



void chainFree(int fd, uint32_t start_offset){

	uint32_t target_block = start_offset>>BLOCKSHIFT;

	while(target_block != 0){
		freeBlock(fd, INDEX(target_block));
		target_block = ncache.getNext(fd, target_block);
	}
}

/**************************************************************
Classes
**************************************************************/


/**************************************************************
FileCursor
**************************************************************/
class FileCursor{
	public:
	uint32_t base;
	uint32_t offset;
	uint32_t prev;

	dirEntry readDirEntry(int fd);
	void moveToExtent(int fd, uint32_t headSize);
	bool atEnd(){return ENDBLOCK(base, offset);}
	void freeExtentBlock(int fd);

	FileCursor(uint32_t block_num){base = INDEX(block_num); offset = base+INODESIZE; prev = 0;}
	FileCursor(){base = 0; offset = 0; prev = 0;}
};

dirEntry FileCursor::readDirEntry(int fd){

	dirEntry entry;

	dread(fd, &(entry.len), LENSIZE, offset, "failed to read dir entry size\n")
	else{
		offset += LENSIZE;
	}

	if(entry.len != 0){
		dread(fd, &(entry.inode_num), BNUMSIZE, offset, "failed to read dir entry inode number\n")
		else{
			offset += BNUMSIZE;
		}
		

		if((entry.name = (char*)calloc(entry.len+1-BNUMSIZE-LENSIZE, sizeof(char))) == NULL){
			perror("failed to allocate space for dir entry name\n");
			//fprintf(stderr, "tried to allocate size of %d bytes\n", entry.len+1-BNUMSIZE-LENSIZE);
			exit(-1);
		}

		dread(fd, entry.name, entry.len-BNUMSIZE-LENSIZE, offset, "failed to read dir entry name\n")
		else{
			offset += entry.len-BNUMSIZE-LENSIZE;
		}
	}
	else{
		entry.name = NULL;

	}
	//fprintf(stderr, "finished reading dir entry and found len %d, inode num %x, and name %s\n",entry.len, entry.inode_num, entry.name);
	return entry;
}

void FileCursor::moveToExtent(int fd, uint32_t headSize){
	prev = base;
	base = INDEX(ncache.getNext(fd, base>>BLOCKSHIFT));
	offset = base + headSize;

}

void FileCursor::freeExtentBlock(int fd){
	
	//get address of next block in the chain
	uint32_t num = ncache.getNext(fd, base>>BLOCKSHIFT);

	//connect previous block to next block
	dwrite(fd, (void*)(&num), BNUMSIZE, prev+BLOCKSIZE-BNUMSIZE, "failed to write continuing block num");
	ncache.setNext(prev>>BLOCKSHIFT, num);

	//free the target block
	freeBlock(fd, base);
}

/**************************************************************
DirData
**************************************************************/
class DirData{

	private:
	void writeInsert(dirEntry entry);

	public:
	dirEntry entry;
	FileCursor cursor;
	int fd;
	inodeHead parentDir;
	uint32_t parent_offset;
	bool found;

	
	DirData(int fd, uint32_t block_num, const char* name);
	DirData(int fd, uint32_t block_num);
	DirData(int fd, uint32_t parent_block_num, dirEntry entry);
	~DirData();

	bool nextEntry();
	bool check(const char* name);
	void removeDirEntry();
	void decouple();
	void updateEntryInode(){entry.inode = readInode(fd, INDEX(entry.inode_num));}
	bool insertEntry(dirEntry entry);

	bool exists(){return found;}
	bool entryIsEmpty(){return entry.inode.size == 0;}
	uint16_t entryMode(){return entry.inode.mode;}

	void remove(){
		removeDirEntry();
		chainFree(fd, entry.inode_num);
	}

};


void DirData::removeDirEntry(){
	
	char remainder[BLOCKSIZE] = {0};
	uint32_t remaindersize = ((BLOCKSIZE-(cursor.offset%BLOCKSIZE)) - entry.len) - BNUMSIZE;

	uint32_t starset = cursor.offset-entry.len;
	uint32_t newDirSize = parentDir.size-entry.len;

	//shift remainder of directory data to cover over the deleted entry
	dread(fd, remainder, remaindersize, starset+entry.len, "failed to read dir remainder\n");
	dwrite(fd, remainder, remaindersize+entry.len, starset, "failed to write dir remainder\n");

	//update new directory size
	dwrite(fd, &newDirSize, SIZESIZE, parent_offset+SIZEDEX, "failed to write new dir size\n");

	//check if an extent block has now been emptied and should be freed
	if(remainder[0] == 0 && cursor.offset - cursor.base == BNUMSIZE){
		cursor.freeExtentBlock(fd);
	}
}

bool DirData::nextEntry(){

	if(entry.name != NULL){
		free(entry.name);
	}

	entry = cursor.readDirEntry(fd);

	if(entry.len == 0){
		cursor.moveToExtent(fd, DIREXTENTHEADSIZE);
		nextEntry();
	}
	else if(cursor.atEnd()){
		cursor.moveToExtent(fd, DIREXTENTHEADSIZE);
	}
	fprintf(stderr, "got next entry, base at: %d\n", cursor.base);
	return cursor.base != 0;
}

bool DirData::check(const char* name){
	if(entry.name != NULL && strcmp(name, entry.name) == 0){
		updateEntryInode();
		
		fprintf(stderr,"found dir entry in parent dir - len: %d name: %s\n", entry.len, entry.name);
		return true;
	}
	return false;
}

void DirData::decouple(){
	entry.inode.Nlink--;

	if(entry.inode.Nlink == 0){
			chainFree(fd, entry.inode_num);
			
		}
	else{
		
		if(pwrite(fd, (void*)(&(entry.inode.Nlink)), NLINKSIZE, INDEX(entry.inode_num)+NLINKDEX) != NLINKSIZE){
			perror("failed to decrement Nlink\n");
			exit(-1);
		}
	}

}

bool DirData::insertEntry(dirEntry entry){

	dirEntry curE;
	bool inserted = false;
	uint32_t buffer = EXTENT_NUM;
	parentDir.size += entry.len;

	while(!inserted && cursor.base != 0){
		
		curE = cursor.readDirEntry(fd);
		
		if(curE.len == 0){
			
			//check if enough space
			if(BLOCKSIZE + cursor.base - cursor.offset >= entry.len){
				//update dir size
				writeInsert(entry);
				dwrite(fd, &(parentDir.size), SIZESIZE, parent_offset+SIZEDEX, "failed to write new dir size\n");
				inserted = true;
			}
		}

		if(!inserted && (cursor.atEnd()||curE.len == 0)){
			cursor.moveToExtent(fd, DIREXTENTHEADSIZE);
		}
	}

	if(!inserted){

		//get next extent block
		buffer = ncache.getNewBlock(fd, &buffer, DIREXTENTHEADSIZE, true);
		ncache.setNext(fd, cursor.prev, buffer);

		//write entry to new block
		cursor = FileCursor(buffer);
		writeInsert(entry);

		//update parent dir info
		dwrite(fd, &(parentDir.size), SIZESIZE, parent_offset+SIZEDEX, "failed to write new dir size\n");
		parentDir.blocks += 1;
		dwrite(fd, &(parentDir.blocks), ALLBLOCKSSIZE, parent_offset+ALLBLOCKSDEX, "failed to write new block count\n");
		
		inserted = true;
	}

	return inserted;
}

void DirData::writeInsert(dirEntry entry){

	uint8_t* buffer = (uint8_t*)calloc(sizeof(uint8_t), entry.len);
	//do insertion
	*((uint16_t*)buffer) = entry.len;
	*((uint32_t*)(buffer+2)) = entry.inode_num;
	memcpy(buffer+6, entry.name, entry.len - LENSIZE - BNUMSIZE);
	dwrite(fd, buffer, entry.len, cursor.offset-LENSIZE, "failed to insert dir entry");
	free(buffer);


}

DirData::DirData(int fd, uint32_t block_num, const char* name){
	
	this->fd = fd;
	parent_offset = INDEX(block_num);
	parentDir = readInode(fd, INDEX(block_num));
	cursor = FileCursor(block_num);
	entry.name = NULL;
	entry.len = 0;
	
	while(this->nextEntry() && !(found = this->check(name)));


}

DirData::DirData(int fd, uint32_t block_num){
	
	cursor = FileCursor(block_num);
	this->fd = fd;
	parent_offset = INDEX(block_num);
	parentDir = readInode(fd, INDEX(block_num));
	entry.name = NULL;
	entry.len = 0;
	found = false;

}

DirData::DirData(int fd, uint32_t parent_block_num, dirEntry entry){
	
	cursor = FileCursor(parent_block_num);
	this->fd = fd;
	parent_offset = cursor.base;
	parentDir = readInode(fd, parent_offset);

	//parentDir = readInode(fd, INDEX(block_num));
	this->entry = entry;
	found = entry.len < MAXDIRENTRYSIZE;

	if(found){
		if(!insertEntry(entry)){
			perror("something bad happened\n");
			exit(-1);
		}
	}
	else{
		errno = ENAMETOOLONG;
	}

}

DirData::~DirData(){
	if(entry.name != NULL){
		free(entry.name);
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
	
	struct Args *fs = (struct Args*)args;
	DirData data(fs->fd, block_num);
	
	while(data.nextEntry()){
		fprintf(stderr, "calling cb\n");
		cb(buf, data.entry.name, data.entry.inode_num);
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
	FileCursor cursor(block_num);
	uint32_t index = 0;

	inodeHead inode = readInode(fs->fd, INDEX(block_num));

	int32_t delta = std::min((int)size, (int)(inode.size-offset));
	uint32_t metaSize = BLOCKSIZE - INODESIZE - BNUMSIZE;

	if(delta < 0){
		//TODO error
		exit(-2);
	}

	while(offset >= metaSize){
		cursor.moveToExtent(fs->fd,FILEEXTENTHEADSIZE);
		//cur = moveToExtent(fs->fd, &base, FILEEXTENTHEADSIZE);
		offset -= metaSize;
		metaSize = BLOCKSIZE - FILEEXTENTHEADSIZE - BNUMSIZE;
	}
	
	cursor.offset += offset;

	while(delta > 0){

		metaSize = std::min((int)(BLOCKSIZE + cursor.base - cursor.offset - BNUMSIZE), (int)delta);

		if(pread(fs->fd, buf+index, metaSize, cursor.offset) != metaSize){
			perror("failed to read from file into buffer");
			exit(-1);
		}
		index += metaSize;
		delta -= metaSize;
		

		if(delta>0) cursor.moveToExtent(fs->fd, FILEEXTENTHEADSIZE);
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

	struct Args *fs = (struct Args*)args;
	
	DirData data(fs->fd, block_num, name);
	int ret = -1;

	if(!data.found){
		errno = ENOENT;

	} else if(!S_ISDIR(data.entryMode())){

		errno = ENOTDIR;

	}
	else if(data.entryIsEmpty()){
		data.remove();
		ret = 0;
	}
	else{
		fprintf(stderr,"-> when deleting dir: data.entry.inode.size == %d\n", (int)data.entry.inode.size);
		errno = ENOTEMPTY;
				
	}

	return ret;
} 

int unlink(void* args, uint32_t block_num, const char *name){
	
	struct Args *fs = (struct Args*)args;
	DirData data(fs->fd, block_num, name);
	int ret = -1;

	if(!data.found){

		errno = ENOENT;

	} else if(S_ISDIR(data.entryMode())){

		errno = EISDIR;

	}
	else{
		//TODO check perms? 
		data.remove();
		data.decouple();
		
		ret = 0;
	}

	return ret;
}

int mknod(void* args, uint32_t parent_block, const char *name, mode_t new_mode, dev_t new_dev){
	
	struct Args *fs = (struct Args*)args;
	inodeHead inode;
	dirEntry entry;
	fuse_context* cntxt = fuse_get_context();
	uint32_t bnum = ncache.getNewBlock(fs->fd, (void*)(&inode), INODESIZE, false);

	FILLINODE(new_mode, 0);
	FILLENTRY;
	
	dwrite(fs->fd, &inode, INODESIZE, 0, "failed to write inode to new node\n");

	DirData dir(fs->fd, parent_block, entry);

	return 0;
}

int symlink(void* args, uint32_t parent_block, const char *name, const char *link_dest){
	struct Args *fs = (struct Args*)args;
	inodeHead inode;
	dirEntry entry;
	fuse_context* cntxt = fuse_get_context();
	uint32_t bnum = ncache.getNewBlock(fs->fd, (void*)(&inode), INODESIZE, false);

	FILLINODE(S_IFLNK, strlen(link_dest));
	FILLENTRY;
	
	dwrite(fs->fd, &inode, INODESIZE, INDEX(bnum), "failed to write inode to new node\n");
	dwrite(fs->fd, link_dest, (unsigned)strlen(link_dest), INDEX(bnum)+INODESIZE, "failed to write symlink data");

	DirData dir(fs->fd, parent_block, entry);

	return 0;
}

int mkdir(void* args, uint32_t parent_block, const char *name, mode_t new_mode){
	
	struct Args *fs = (struct Args*)args;
	inodeHead inode;
	dirEntry entry;
	fuse_context* cntxt = fuse_get_context();
	uint32_t bnum = ncache.getNewBlock(fs->fd, (void*)(&inode), INODESIZE, false);

	FILLINODE(new_mode, 0);
	FILLENTRY;
	
	dwrite(fs->fd, &inode, INODESIZE, INDEX(bnum), "failed to write inode to new node\n");

	DirData dir(fs->fd, parent_block, entry);
	return 0;
}

int link(void* args, uint32_t parent_block, const char *name, uint32_t dest_block){
	
	struct Args *fs = (struct Args*)args;
	inodeHead inode;
	dirEntry entry;
	fuse_context* cntxt = fuse_get_context();
	uint32_t bnum = ncache.getNewBlock(fs->fd, (void*)(&inode), INODESIZE, false);

	FILLINODE(S_IFLNK, BNUMSIZE);
	FILLENTRY;
	
	dwrite(fs->fd, &inode, INODESIZE, INDEX(bnum), "failed to write inode to new node\n");
	dwrite(fs->fd, &dest_block, BNUMSIZE, INDEX(bnum)+INODESIZE, "failed to write symlink data");

	DirData dir(fs->fd, parent_block, entry);

	inode = readInode(fs->fd, INDEX(dest_block));
	inode.Nlink+=1;
	dwrite(fs->fd, &(inode.Nlink), NLINKSIZE, INDEX(dest_block)+NLINKDEX, "failed to update nlink after linking\n");

	return 0;
}

int rename(void* args, uint32_t old_parent, const char *old_name, uint32_t new_parent, const char *new_name){
	struct Args *fs = (struct Args*)args;
	DirData data(fs->fd, old_parent, old_name);
	dirEntry oldentry;
	int ret = -1;

	if(!data.found){

		errno = ENOENT;

	} 
	else{
		//TODO check perms? 
		oldentry.inode = data.entry.inode;
		oldentry.inode_num = data.entry.inode_num;
		oldentry.name = (char*)malloc(strlen(new_name)+1);
		memcpy(oldentry.name, new_name, strlen(new_name)+1);
		oldentry.len = strlen(new_name)+1 + BNUMSIZE + SIZESIZE;

		data.remove();

		data = DirData(fs->fd, new_parent, oldentry);
				
		ret = 0;
	}

	return ret;
}

int truncate(void* args, uint32_t block_num, off_t new_size){
	struct Args *fs = (struct Args*)args;
	dwrite(fs->fd, &(new_size), SIZESIZE, INDEX(block_num)+SIZEDEX, "truncation failed\n");
	return 0;
}

int write(void* args, uint32_t block_num, const char *buff, size_t wr_len, off_t wr_offset){
	
	struct Args *fs = (struct Args*)args;
	FileCursor cursor(block_num);
	uint32_t index = 0;
	uint32_t bnum = 0;
	uint64_t extentHead = ((uint64_t)EXTENT_NUM<<32)|((uint64_t)block_num);

	inodeHead inode = readInode(fs->fd, INDEX(block_num));

	int32_t delta = std::min((int)wr_len, (int)(inode.size-wr_offset));
	uint32_t metaSize = BLOCKSIZE - INODESIZE - BNUMSIZE;

	if(delta < 0){
		//TODO error
		exit(-2);
	}

	while(wr_offset >= metaSize){
		cursor.moveToExtent(fs->fd,FILEEXTENTHEADSIZE);
		//cur = moveToExtent(fs->fd, &base, FILEEXTENTHEADSIZE);
		wr_offset -= metaSize;
		metaSize = BLOCKSIZE - FILEEXTENTHEADSIZE - BNUMSIZE;
		if(cursor.base == 0){
			//get next extent block
			bnum = ncache.getNewBlock(fs->fd, &extentHead, FILEEXTENTHEADSIZE, false);
			ncache.setNext(fs->fd, cursor.prev, bnum);

			//write entry to new block
			cursor = FileCursor(bnum);
		}
	}

	
	cursor.offset += wr_offset;

	while(delta > 0){

		metaSize = std::min((int)(BLOCKSIZE + cursor.base - cursor.offset - BNUMSIZE), (int)delta);

		dwrite(fs->fd, buff+index, metaSize, cursor.offset, "failed to write to file");

		index += metaSize;
		delta -= metaSize;
		

		if(delta>0) {
			cursor.moveToExtent(fs->fd, FILEEXTENTHEADSIZE);
		}

		if(cursor.base == 0){
			//get next extent block
			bnum = ncache.getNewBlock(fs->fd, &extentHead, FILEEXTENTHEADSIZE, false);
			ncache.setNext(fs->fd, cursor.prev, bnum);

			//write entry to new block
			cursor = FileCursor(bnum);
		}
	}

    return index;

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

	ops.chmod = chmod;
	ops.chown = chown;
	ops.utimens = utimens;
	ops.rmdir = rmdir;
	ops.unlink = unlink;
	ops.mknod = mknod;
	ops.symlink = symlink;
	ops.mkdir = mkdir;
	ops.link = link;
	ops.rename = rename;
	ops.truncate = truncate;
	ops.write = write;

	return &ops;
}

#ifdef  __cplusplus
}
#endif

