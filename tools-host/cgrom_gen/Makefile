# cgrom_gen - ビットマップフォント(BDF)から互換CGROMを生成するツール
# C99 / 依存なし。

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
PREFIX  ?= /usr/local

SRCS = cgrom_gen.c
OBJS = $(SRCS:.c=.o)

cgrom_gen: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean install
clean:
	rm -f cgrom_gen $(OBJS)

install: cgrom_gen
	install -d $(PREFIX)/bin
	install -m 0755 cgrom_gen $(PREFIX)/bin/cgrom_gen
