CC			 = i686-pc-mingw32-gcc
LD			 = i686-pc-mingw32-ld

OBJS		+= smooth.o

LIBS		+= -L../libdemo
LIBS		+= -ldemo

CFLAGS		+= -I../libdemo/inc
CFLAGS		+= -g
CFLAGS		+= -O0

all: demsmooth.exe

demsmooth.exe: $(OBJS)
	$(CC) $(OBJS) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f *.o demsmooth.exe
