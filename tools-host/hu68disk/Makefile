# hu68disk - Human68k (X68000) ディスクイメージ ファイル操作ツール
# C99 / 依存なし（iconv のみ）。macOS/Linux 対応（将来 Windows）。

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
PREFIX  ?= /usr/local

# macOS は libiconv を別リンク。Linux(glibc) は libc 内蔵で不要。
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LDLIBS  += -liconv
endif

# sjis.c は src/peripheral/ と共有（本体側と同一の変換実装を使う）
VPATH   = ../../src/peripheral
SRCS = sjis.c compat.c image.c fat.c dir.c cmd.c fsck.c main.c
OBJS = $(SRCS:.c=.o)

hu68disk: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# すべての .o は共有ヘッダに依存
$(OBJS): hu68disk.h

%.o: %.c
	$(CC) $(CFLAGS) -I. -c $< -o $@

.PHONY: clean install test
clean:
	rm -f hu68disk $(OBJS)

install: hu68disk
	install -d $(PREFIX)/bin
	install -m 0755 hu68disk $(PREFIX)/bin/hu68disk

test: hu68disk
	sh tests/test_multi_partition.sh ./hu68disk
	sh tests/test_empty_file.sh ./hu68disk
