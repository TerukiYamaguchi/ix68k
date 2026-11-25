# tools-x68k/ged/Makefile — GhostED (GED.X) のビルド（Issue #148）
#
# GED.X は argv と stdio(printf/fgets) が必要なため標準スタートアップをそのまま
# リンクする（elf2x68k の x68k.specs が x68kcrt0.o + -lx68k -lx68kdos -lx68kiocs
# を自動リンク）。tools-x68k/hostfile/Makefile と同じ流儀。

CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -Wall -Wextra

ELF2X68K = /opt/homebrew/opt/elf2x68k/bin/elf2x68k.py

all: GED.X

GED.X: ghosted.elf
	python3 $(ELF2X68K) -o $@ $<

ghosted.elf: ghosted.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	-rm -f ghosted.elf GED.X

.PHONY: all clean
