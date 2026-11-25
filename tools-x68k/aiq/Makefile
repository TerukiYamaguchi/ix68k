# tools-x68k/aiq/Makefile — AIQ.X（Issue #64）のビルド
#
# argv と stdio(printf) が必要なため標準スタートアップをそのままリンクする
# （tools-x68k/hostfile/Makefile と同じ構成）。

CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -Wall -Wextra

ELF2X68K = /opt/homebrew/opt/elf2x68k/bin/elf2x68k.py

all: AIQ.X

AIQ.X: aiq.elf
	python3 $(ELF2X68K) -o $@ $<

aiq.elf: aiq.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	-rm -f aiq.elf AIQ.X

.PHONY: all clean
