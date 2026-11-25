# sram_switch - sram.dat オフライン設定ツール
# C99 / 依存なし。

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
PREFIX  ?= /usr/local

SRCS = sram_switch.c
OBJS = $(SRCS:.c=.o)

sram_switch: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean install
clean:
	rm -f sram_switch $(OBJS)

install: sram_switch
	install -d $(PREFIX)/bin
	install -m 0755 sram_switch $(PREFIX)/bin/sram_switch
