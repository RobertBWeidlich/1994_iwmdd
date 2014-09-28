#  file:	Makefile
#  date:	March 10, 1992
#
#  "@(#)Makefile	1.14 10/8/93 GTE-TSC"
#
IW_BIN = $(IW_HOME)/bin
IW_INCLUDE = $(IW_HOME)/include

LIBSLOC = -L${IW_HOME}/lib
LIBS = -liwdb

CC = cc
#CC = gcc

SRCS = iwmdd.c mdlog.c

OBJS = $(SRCS:.c=.o)

CFLAGS = -g

C_DEBUG_FLAGS = $(CFLAGS) -DDEBUG

all:  iwmdd

debug:  iwmdd.debug

# stuff to do CenterLine ObjectCenter load call.  Note the adding of the
# std "c" libraries at the end

saber:	clsrc

clsrc:	$(SRCS)
	#load -C $(C_DEBUG_FLAGS) $(SRCS) $(INCLUDES) $(LIBSLOC) $(LIBS) -lc

#saber:
#	#load -C $(CFLAGS) -DDEBUG iwmdd.c mdlog.o

iwmdd: iwmdd.o mdlog.o
	$(CC) $(CFLAGS) -o iwmdd iwmdd.o mdlog.o

iwmdd.debug: iwmdd.debug.o mdlog.o
	$(CC) $(C_DEBUG_FLAGS) -o iwmdd iwmdd.o mdlog.o

iwmdd.o:	iwmdd.c
	$(CC) -c $(CFLAGS) iwmdd.c

iwmdd.debug.o:	iwmdd.c
	$(CC) -c $(C_DEBUG_FLAGS) iwmdd.c

mdlog.o:	mdlog.c
	$(CC) -c $(CFLAGS) mdlog.c

install:
	if [ -f iwmdd ];         then install -m 0775 iwmdd       $(IW_BIN); fi


pack:
	tar cvf - *.c *.doc Makefile |compress >iwmdd.tar.Z

clean:
	rm -rf core a.out *.o iwmdd
