


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

#define LINKMASK 0XFF

#define INODESIZE 128

#define SUPERBLOCK 0
#define INODE 1

#define BLOCKSIZE 4096
#define INDEX(block) (block*BLOCKSIZE)
