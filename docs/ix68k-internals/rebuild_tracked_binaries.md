# 追跡バイナリの再ビルド

Git で管理しているバイナリを、ソースから作り直す手順です。ビルド済みを管理するのは、
m68k-xelf を持たない環境（Windows 機など）でもリリース ZIP を作れるようにするためです。

## 対象

| バイナリ | 作り方 |
|---|---|
| `tools-x68k/hostdrv/HOSTDRV.SYS` | `tools-x68k/hostdrv/` で `make` |
| `tools-x68k/hostfile/hostfile.x` | `tools-x68k/hostfile/` で `make` |
| `tools-x68k/joyget_test/joyget_test.x` | `tools-x68k/joyget_test/` で `make` |
| `tools-x68k/ged/GED.X` | `tools-x68k/ged/` で `make` |
| `tools-x68k/aiq/aiq.x` | `tools-x68k/aiq/` で `make` |
| `tools-x68k/fntadr/fntadr.x` | `tools-x68k/fntadr/` で `make` |
| `packaging/release/BLANK.XDF` | `hu68disk mkfs new.xdf`（下記） |

## ツールチェーン

[elf2x68k](https://github.com/yunkya2/elf2x68k) の `m68k-xelf-gcc` と `elf2x68k.py` を使います。
追跡中のバイナリは、次の版でビルドしたものです。

- `m68k-xelf-gcc (elf2x68k) 13.5.0`
- elf2x68k 20261004（Homebrew）

## 手順

```bash
cd tools-x68k/<ツール名>
make clean
make
```

作ったバイナリと追跡中のバイナリを `cmp` で比べます。上の版では、6 本とも一致することを確認しています。
版が違うと、コンパイラの出力が変わって一致しないことがあります。その場合は、動作確認をしてから
差し替えます。

macOS の既定のファイルシステムは大文字小文字を区別しないため、`GED.X` と `ged.x` のように
Makefile の出力名と追跡名の大文字小文字が違っても、同じファイルとして上書きされます。

## BLANK.XDF

`BLANK.XDF` は、フォーマット済みで空の 2HD イメージ（1,261,568 バイト、FAT12、0 ファイル）です。
ブートセクタは BPB だけで、IPL コードは含みません。

```bash
hu68disk mkfs BLANK.XDF
cmp BLANK.XDF packaging/release/BLANK.XDF
```

`hu68disk` は、ビルドした `build/hu68disk` を使います。出力は追跡中のファイルと完全に一致することを
確認しています。
