MAKE=make
CC=gcc
AR=ar
RANLIB=ranlib
STRIP=strip
OPTFLAGS=-O3  -fno-strict-aliasing -Wno-pointer-sign
CPPFLAGS= -fno-strict-aliasing

CFLAGS= $(OPTFLAGS)

#common obj
OBJS= main.o

EXT=
PROG=../bin/flvmerge

SRCS := $(OBJS:.o=.c) 

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LINKFLAGS)


%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $< 

clean: 
	rm -f $(OBJS) $(PROG)

dep: depend

depend:
	rm -f .depend	
	$(CC) -MM $(CFLAGS) $(SRCS) 1>.depend

distclean: clean
	rm -f Makefile.bak .depend

# include dependency files if they exist
#
ifneq ($(wildcard .depend),)
include .depend
endif
