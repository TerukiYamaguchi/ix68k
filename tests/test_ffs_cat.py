#!/usr/bin/env python3
"""tools-host/netbsd/ffs_cat.py の単体テスト。

NetBSD の makefs（nbmakefs）でビッグエンディアン UFS1 イメージを作り、X68SCSI1 ヘッダと
X68K パーティション表を被せて抽出結果を突き合わせる。nbmakefs が無い環境では SKIP する。
環境変数 NBMAKEFS で実行ファイルを指定できる。
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT / "tools" / "netbsd"))
import ffs_cat  # noqa: E402

DEFAULT_NBMAKEFS = Path.home() / "Programs/netbsd-obj-10.1-x68k/tooldir/bin/nbmakefs"
BLOCK = 512
FS_START_UNITS = 0x20  # X68K パーティション表の start（1KiB 単位）
FS_BSIZE = 8192
DIRECT_BYTES = 12 * FS_BSIZE


def _makefs() -> str | None:
    candidate = os.environ.get("NBMAKEFS") or (str(DEFAULT_NBMAKEFS) if DEFAULT_NBMAKEFS.exists() else None)
    return candidate or shutil.which("nbmakefs")


def _wrap_x68k(fs_image: bytes) -> bytes:
    """FFS イメージへ X68SCSI1 ヘッダと X68K パーティション表を被せる。"""
    start = FS_START_UNITS * ffs_cat.X68K_UNIT
    out = bytearray(start + len(fs_image))
    out[0:8] = ffs_cat.SCSI_MAGIC
    struct.pack_into(">HI", out, ffs_cat.SCSI_HEADER_BLEN_OFFSET, BLOCK, len(out) // BLOCK)
    table = ffs_cat.X68K_TABLE_OFFSET
    out[table:table + 4] = ffs_cat.X68K_MAGIC
    entry = table + ffs_cat.X68K_ENTRY_BASE
    out[entry:entry + 8] = b"BSD ffs "
    struct.pack_into(">II", out, entry + 8, FS_START_UNITS, len(fs_image) // ffs_cat.X68K_UNIT)
    out[start:] = fs_image
    return bytes(out)


@unittest.skipUnless(_makefs(), "nbmakefs が見つからない（NBMAKEFS で指定可）")
class FfsCatTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        tree = root / "tree"
        (tree / "dir/sub").mkdir(parents=True)
        cls.files = {
            "/small": b"hello ffs\n",
            "/direct": bytes(range(256)) * ((DIRECT_BYTES - 100) // 256),
            "/indirect": bytes((i * 7) & 0xFF for i in range(DIRECT_BYTES + 5 * FS_BSIZE + 123)),
            "/dir/sub/nested": b"nested file",
            "/empty": b"",
        }
        for name, data in cls.files.items():
            (tree / name.lstrip("/")).write_bytes(data)
        os.symlink("dir/sub/nested", tree / "link")
        fs_path = root / "fs.img"
        subprocess.run([_makefs(), "-B", "be", "-t", "ffs", "-o", f"version=1,bsize={FS_BSIZE},fsize=1024",
                        "-s", "2m", str(fs_path), str(tree)], check=True, capture_output=True)
        cls.image = root / "disk.hds"
        cls.image.write_bytes(_wrap_x68k(fs_path.read_bytes()))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_extract_matches_for_direct_indirect_and_nested(self):
        for name, data in self.files.items():
            with self.subTest(path=name):
                self.assertEqual(ffs_cat.extract(self.image, name), data)

    def test_symlink_is_followed(self):
        self.assertEqual(ffs_cat.extract(self.image, "/link"), self.files["/dir/sub/nested"])

    def test_missing_and_directory_are_errors(self):
        with self.assertRaises(ffs_cat.FfsError):
            ffs_cat.extract(self.image, "/nope")
        with self.assertRaises(ffs_cat.FfsError):
            ffs_cat.extract(self.image, "/dir")
        with self.assertRaises(ffs_cat.FfsError):
            ffs_cat.extract(self.image, "/small/child")

    def test_bad_partition_and_header(self):
        with self.assertRaises(ffs_cat.FfsError):
            ffs_cat.extract(self.image, "/small", partition=3)
        bad = Path(self.tmp.name) / "bad.hds"
        bad.write_bytes(b"\0" * 4096)
        with self.assertRaises(ffs_cat.FfsError):
            ffs_cat.extract(bad, "/small")


if __name__ == "__main__":
    unittest.main()
