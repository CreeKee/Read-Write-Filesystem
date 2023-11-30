OS=$(shell uname -s)
ifeq ("$(OS)", "Darwin")
	OS_DEF=-DMACOSX -I/usr/local/include/osxfuse
	FUSE_LINK=-L/usr/local/lib -losxfuse
else
	OS_DEF=-DLINUX
	FUSE_LINK=-lfuse -lpthread
endif
# -Werror
CFLAGS=-Wall -DDEBUG -g -D_FILE_OFFSET_BITS=64 $(OS_DEF)
CXXFLAGS=$(CFLAGS)

all: cpe453fs #hello_cpe453fs 

cpe453fs: cpe453fs_main.o implementation.o
	$(CXX) $(CXXFLAGS) cpe453fs_main.o implementation.o -o $@ $(FUSE_LINK)

#hello_cpe453fs: cpe453fs_main.o hello_fs.o
#	$(CXX) $(CXXFLAGS) cpe453fs_main.o hello_fs.o -o $@ $(FUSE_LINK)

cpe453fs_main.o: cpe453fs_main.c cpe453fs.h
#hello_fs.o: hello_fs.cpp cpe453fs.h
implementation.o: implementation.cpp cpe453fs.h

clean:
	rm -f cpe453fs_main.o implementation.o hello_fs.o cpe453fs hello_cpe453fs



run:
	fusermount -u /tmp/skiefer/mnt
	make
	./cpe453fs -s -d /tmp/skiefer/mnt customFS.fs
#1952