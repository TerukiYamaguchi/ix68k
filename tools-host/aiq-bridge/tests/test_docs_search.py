import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import aiq_bridge as bridge
import docs_search as docs

COMMAND_DOC = """---
exe: DEL.X
検証状態: ✅ 確認済み
---
# DEL

## 使い方

ファイルを削除するコマンド。DEL ファイル名 の形で指定する。ワイルドカードも使える。

## 注意

削除したファイルは元に戻せないので、実行前によく確認すること。
"""

BUILD_DOC = """---
exe: GCC.X
検証状態: 📖 マニュアル参照のみ
---
# GCC.X

## ビルド手順

C言語のソースをコンパイルして実行ファイルを作る手順。GCC ソース名.c と入力する。
"""


class DocsIndexTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        for subdir, name, body in (("official", "DEL.X.md", COMMAND_DOC), ("freeware", "GCC.X.md", BUILD_DOC)):
            (root / subdir).mkdir()
            (root / subdir / name).write_text(body, encoding="utf-8")
        (root / "mcp").mkdir()
        (root / "mcp" / "ignored.md").write_text("# 無視される\n\nファイルを削除する手順の詳細。" * 5, encoding="utf-8")
        cls.index = docs.DocsIndex(root)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_excludes_non_target_directories(self):
        self.assertFalse(any(c.source.startswith("mcp/") for c in self.index.chunks))

    def test_japanese_query_finds_relevant_file(self):
        hits = self.index.search("ファイルを削除するにはどうするの?", 6000)
        self.assertEqual(hits[0].source, "official/DEL.X.md")

    def test_exe_name_in_query_boosts_file(self):
        hits = self.index.search("GCC の使い方", 6000)
        self.assertEqual(hits[0].source, "freeware/GCC.X.md")

    def test_header_has_source_and_status(self):
        hit = self.index.search("C言語のソースをコンパイルしたい", 6000)[0]
        self.assertIn("freeware/GCC.X.md", hit.header())
        self.assertIn("マニュアル参照のみ", hit.header())

    def test_budget_is_respected(self):
        hits = self.index.search("ファイルを削除する", 200)
        self.assertLessEqual(sum(len(c.header()) + len(c.text) for c in hits), 200)

    def test_unrelated_query_returns_nothing(self):
        self.assertEqual(self.index.search("qzxvw", 6000), [])


class FrontmatterTest(unittest.TestCase):
    def test_parse(self):
        meta, body = docs.parse_frontmatter("---\nexe: A.X\n検証状態: ✅\n---\n本文")
        self.assertEqual(meta, {"exe": "A.X", "検証状態": "✅"})
        self.assertEqual(body, "本文")

    def test_without_frontmatter(self):
        self.assertEqual(docs.parse_frontmatter("本文のみ"), ({}, "本文のみ"))


class WithReferenceDocsTest(unittest.TestCase):
    def test_reference_is_attached_to_question(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "official").mkdir()
            (Path(tmp) / "official" / "DEL.X.md").write_text(COMMAND_DOC, encoding="utf-8")
            index = docs.DocsIndex(Path(tmp))
            received = []
            ask = bridge.with_reference_docs(lambda text: received.append(text) or "{}", index, 6000)
            ask("ファイルを削除したい")
            self.assertIn("## 参考資料", received[0])
            self.assertIn("official/DEL.X.md", received[0])
            self.assertTrue(received[0].rstrip().endswith("ファイルを削除したい"))

    def test_question_passes_through_without_hits(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "official").mkdir()
            (Path(tmp) / "official" / "DEL.X.md").write_text(COMMAND_DOC, encoding="utf-8")
            index = docs.DocsIndex(Path(tmp))
            received = []
            ask = bridge.with_reference_docs(lambda text: received.append(text) or "{}", index, 6000)
            ask("qzxvw")
            self.assertEqual(received, ["qzxvw"])


if __name__ == "__main__":
    unittest.main()
