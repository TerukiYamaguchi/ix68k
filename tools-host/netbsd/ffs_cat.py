#!/usr/bin/env python3
"""NetBSD/x68k の .hds（X68SCSI1 ヘッダ付き）内の FFS(UFS1) から1ファイルを取り出す。

読み取り専用。ゲストを起動せずに /netbsd などを取り出し、text_probe の binary_path
（SHA-256 照合）へ渡すために使う。

  ffs_cat.py <image.hds> <path-in-fs> [-o <out>] [--partition N]

出力先を省略すると標準出力へ書く。UFS1（big-endian、m68k の NetBSD 既定）のみ対応する。
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import BinaryIO

SCSI_MAGIC = b"X68SCSI1"
X68K_MAGIC = b"X68K"
SCSI_HEADER_BLEN_OFFSET = 8
X68K_TABLE_OFFSET = 0x800  # LBA4 (512B 単位): X68K パーティション表
X68K_ENTRY_SIZE = 16
X68K_ENTRY_BASE = 0x10
X68K_MAX_PARTITIONS = 15
X68K_UNIT = 1024  # パーティション表の start/size は 1KiB 単位
FS_NAME_PREFIX = b"BSD ffs"

UFS1_SBLOCK = 8192
UFS1_MAGIC = 0x011954
FS_MAGIC_OFFSET = 0x55C
# struct fs（sys/ufs/ffs/fs.h）のオフセット
FS_IBLKNO = 16
FS_OLD_CGOFFSET = 24
FS_OLD_CGMASK = 28
FS_BSIZE = 48
FS_FSIZE = 52
FS_FRAG = 56
FS_NINDIR = 116
FS_INOPB = 120
FS_IPG = 184
FS_FPG = 188

# struct ufs1_dinode
DINODE_SIZE = 128
DI_MODE = 0
DI_SIZE = 8
DI_DB = 40
DI_IB = 88
NDADDR = 12
NIADDR = 3
ROOT_INO = 2
IFMT = 0xF000
IFDIR = 0x4000
IFREG = 0x8000
IFLNK = 0xA000
MAX_SYMLINKS = 8
MAXNAMLEN = 255


class FfsError(RuntimeError):
    pass


class Ufs1:
    def __init__(self, fp: BinaryIO, base: int):
        self.fp = fp
        self.base = base
        sb = self._read(UFS1_SBLOCK, 1376)
        if struct.unpack_from(">I", sb, FS_MAGIC_OFFSET)[0] != UFS1_MAGIC:
            raise FfsError("UFS1 のスーパーブロックが見つかりません（UFS2 などは未対応）")
        self.iblkno = struct.unpack_from(">i", sb, FS_IBLKNO)[0]
        self.cgoffset = struct.unpack_from(">i", sb, FS_OLD_CGOFFSET)[0]
        self.cgmask = struct.unpack_from(">i", sb, FS_OLD_CGMASK)[0]
        self.bsize, self.fsize, self.frag = struct.unpack_from(">iii", sb, FS_BSIZE)
        self.nindir = struct.unpack_from(">i", sb, FS_NINDIR)[0]
        self.inopb = struct.unpack_from(">I", sb, FS_INOPB)[0]
        self.ipg = struct.unpack_from(">I", sb, FS_IPG)[0]
        self.fpg = struct.unpack_from(">i", sb, FS_FPG)[0]
        if self.bsize <= 0 or self.fsize <= 0 or self.inopb == 0 or self.ipg == 0:
            raise FfsError("スーパーブロックの値が不正です")

    def _read(self, offset: int, length: int) -> bytes:
        self.fp.seek(self.base + offset)
        data = self.fp.read(length)
        if len(data) != length:
            raise FfsError("イメージの範囲外を読みました")
        return data

    def _fsbtob(self, frag_addr: int) -> int:
        return frag_addr * self.fsize

    def inode(self, ino: int) -> dict:
        cg = ino // self.ipg
        cgstart = self.fpg * cg + self.cgoffset * (cg & ~self.cgmask)
        index = ino % self.ipg
        block = cgstart + self.iblkno + (index // self.inopb) * self.frag
        raw = self._read(self._fsbtob(block) + (index % self.inopb) * DINODE_SIZE, DINODE_SIZE)
        return {
            "mode": struct.unpack_from(">H", raw, DI_MODE)[0],
            "size": struct.unpack_from(">Q", raw, DI_SIZE)[0],
            "db": list(struct.unpack_from(">12i", raw, DI_DB)),
            "ib": list(struct.unpack_from(">3i", raw, DI_IB)),
            "raw": raw,
        }

    def _indirect(self, frag_addr: int, level: int) -> list[int]:
        """間接ブロック frag_addr 以下の論理ブロック列（level 1=単間接）を返す。0 は穴。"""
        if frag_addr == 0:
            return [0] * (self.nindir ** level)
        entries = struct.unpack(f">{self.nindir}i", self._read(self._fsbtob(frag_addr), self.bsize))
        if level == 1:
            return list(entries)
        blocks: list[int] = []
        for entry in entries:
            blocks.extend(self._indirect(entry, level - 1))
        return blocks

    def read_file(self, inode: dict) -> bytes:
        size = inode["size"]
        nblocks = (size + self.bsize - 1) // self.bsize
        blocks = list(inode["db"])
        for level, frag_addr in enumerate(inode["ib"], start=1):
            if len(blocks) >= nblocks:
                break
            blocks.extend(self._indirect(frag_addr, level))
        out = bytearray()
        for i in range(nblocks):
            addr = blocks[i]
            chunk = min(self.bsize, size - i * self.bsize)
            out += b"\0" * chunk if addr == 0 else self._read(self._fsbtob(addr), chunk)
        return bytes(out)

    def _dir_lookup(self, dir_inode: dict, name: str) -> int | None:
        data = self.read_file(dir_inode)
        pos = 0
        target = name.encode()
        while pos + 8 <= len(data):
            ino, reclen, _type, namlen = struct.unpack_from(">IHBB", data, pos)
            if reclen == 0:
                break
            if ino and data[pos + 8:pos + 8 + namlen] == target:
                return ino
            pos += reclen
        return None

    def resolve(self, path: str, depth: int = 0) -> dict:
        if depth > MAX_SYMLINKS:
            raise FfsError("シンボリックリンクが深すぎます")
        current = self.inode(ROOT_INO)
        parts = [p for p in path.split("/") if p]
        for index, part in enumerate(parts):
            if len(part) > MAXNAMLEN or (current["mode"] & IFMT) != IFDIR:
                raise FfsError(f"パスを辿れません: {path}")
            ino = self._dir_lookup(current, part)
            if ino is None:
                raise FfsError(f"見つかりません: {path}")
            current = self.inode(ino)
            if (current["mode"] & IFMT) == IFLNK:
                target = self._symlink_target(current)
                rest = "/".join(parts[index + 1:])
                return self.resolve(target.rstrip("/") + "/" + rest if rest else target, depth + 1)
        return current

    def _symlink_target(self, inode: dict) -> str:
        if inode["size"] < NDADDR * 4 + NIADDR * 4:  # 短いリンクは di_db/di_ib 領域に直接入る
            return inode["raw"][DI_DB:DI_DB + inode["size"]].decode()
        return self.read_file(inode).decode()


def find_partition(fp: BinaryIO, index: int | None) -> int:
    """イメージ先頭からの FFS パーティション開始バイトオフセットを返す。"""
    fp.seek(0)
    header = fp.read(X68K_TABLE_OFFSET + X68K_ENTRY_BASE + X68K_ENTRY_SIZE * X68K_MAX_PARTITIONS)
    if header[:len(SCSI_MAGIC)] != SCSI_MAGIC:
        raise FfsError("X68SCSI1 ヘッダがありません")
    if header[X68K_TABLE_OFFSET:X68K_TABLE_OFFSET + 4] != X68K_MAGIC:
        raise FfsError("X68K パーティション表がありません")
    found = []
    for i in range(X68K_MAX_PARTITIONS):
        entry = X68K_TABLE_OFFSET + X68K_ENTRY_BASE + i * X68K_ENTRY_SIZE
        name = header[entry:entry + 8]
        start = struct.unpack_from(">I", header, entry + 8)[0] & 0x00FFFFFF
        if name.startswith(FS_NAME_PREFIX):
            found.append((i, start * X68K_UNIT))
    if not found:
        raise FfsError("BSD ffs パーティションがありません")
    if index is None:
        return found[0][1]
    for i, offset in found:
        if i == index:
            return offset
    raise FfsError(f"パーティション {index} は BSD ffs ではありません")


def extract(image: Path, path: str, partition: int | None = None) -> bytes:
    with image.open("rb") as fp:
        fs = Ufs1(fp, find_partition(fp, partition))
        inode = fs.resolve(path)
        if (inode["mode"] & IFMT) != IFREG:
            raise FfsError(f"通常ファイルではありません: {path}")
        return fs.read_file(inode)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=Path)
    parser.add_argument("path")
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--partition", type=int, default=None, help="X68K パーティション表の番号（既定: 最初の BSD ffs）")
    args = parser.parse_args()
    try:
        data = extract(args.image, args.path, args.partition)
    except (FfsError, OSError) as error:
        print(f"エラー: {error}", file=sys.stderr)
        return 1
    if args.output:
        args.output.write_bytes(data)
    else:
        sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
