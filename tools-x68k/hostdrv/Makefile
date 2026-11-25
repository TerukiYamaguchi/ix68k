CROSS = m68k-xelf-
CC = $(CROSS)gcc
AS = $(CROSS)gcc
LD = $(CROSS)gcc

CFLAGS  = -g -m68000 -I. -Os -Wall -Wextra
ASFLAGS = -m68000 -I.

all: HOSTDRV.SYS

HOSTDRV.SYS: head.o hostdrv.o
	$(LD) -o $@ $^ -nostartfiles -s

hostdrv.o: hostdrv_drv.h

%.o: %.c
	$(CC) $(CFLAGS) -c $<

%.o: %.S
	$(AS) $(ASFLAGS) -c $<

clean:
	-rm -f *.o HOSTDRV.SYS *.elf

.PHONY: all clean
