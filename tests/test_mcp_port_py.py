#!/usr/bin/env python3
"""tools-host/mcp/ix68k_mcp_server.py と tools-host/ix68k-hostfile/ix68k-hostfile の parse_port() を検証する。

ポート値の受理規則は src/mcp/port.c の mcp_port_resolve() と
両言語で一致していなければならない。ここで使う入力表は tests/mcp_port_test.c
と同じもの（差分レビューしやすくするため）。

CMake/ctestには統合していない（Python3をビルド要件に追加するかは実装時に
ユーザーへ確認する方針）。手動で実行する:
    python3 tests/test_mcp_port_py.py
"""

import importlib.util
import sys
import unittest
from importlib.machinery import SourceFileLoader
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT / "tools" / "mcp"))


def _load_module(name: str, path: Path):
    # ix68k-hostfileは拡張子が無く、spec_from_file_location()の拡張子推定だけでは
    # ローダーを解決できないため、SourceFileLoaderを明示する。
    loader = SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


# ix68k-hostfileは拡張子が無いのでspec_from_file_locationで読み込む。
# どちらのモジュールもimport時に IX68K_MCP_PORT を読むが、このテストの実行環境では
# 未設定の想定（設定されていると既定値がずれてテストが誤って失敗し得る）。
_server = _load_module("ix68k_mcp_server", _ROOT / "tools" / "mcp" / "ix68k_mcp_server.py")
_hostfile = _load_module("ix68k_hostfile_cli", _ROOT / "tools" / "host" / "ix68k-hostfile")

# 両実装に同じ入力表を流す（片方だけ直して片方が残る事故を防ぐ、D14）。
_MODULES = {"ix68k_mcp_server": _server, "ix68k-hostfile": _hostfile}


class ParsePortTest(unittest.TestCase):
    def test_none_or_empty_uses_default(self):
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                self.assertEqual(mod.parse_port(None, 26800), 26800)
                self.assertEqual(mod.parse_port("", 26800), 26800)

    def test_valid_value(self):
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                self.assertEqual(mod.parse_port("26801", 26800), 26801)

    def test_boundary_min_max(self):
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                self.assertEqual(mod.parse_port("1024", 26800), 1024)
                self.assertEqual(mod.parse_port("65535", 26800), 65535)

    def test_out_of_range(self):
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                for bad in ("1023", "65536", "0"):
                    with self.assertRaises(ValueError):
                        mod.parse_port(bad, 26800)

    def test_non_digit_garbage(self):
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                for bad in ("abc", "26800x", "268 00"):
                    with self.assertRaises(ValueError):
                        mod.parse_port(bad, 26800)

    def test_surrounding_whitespace_rejected(self):
        """int() なら通ってしまう入力（D14の核心）。"""
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                for bad in (" 26800", "26800 "):
                    with self.assertRaises(ValueError):
                        mod.parse_port(bad, 26800)

    def test_sign_rejected(self):
        """int() なら'+'は通ってしまう入力（D14の核心）。"""
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                for bad in ("-1", "+26800"):
                    with self.assertRaises(ValueError):
                        mod.parse_port(bad, 26800)

    def test_underscore_separator_rejected(self):
        """int() なら通ってしまう入力（D14の核心）。"""
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                for bad in ("26_800", "1_000"):
                    with self.assertRaises(ValueError):
                        mod.parse_port(bad, 26800)

    def test_fullwidth_digit_rejected(self):
        """int() と str.isdigit() はどちらも通してしまう入力（D14の核心）。"""
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                with self.assertRaises(ValueError):
                    mod.parse_port("２６８００", 26800)

    def test_huge_input(self):
        for name, mod in _MODULES.items():
            with self.subTest(module=name):
                with self.assertRaises(ValueError):
                    mod.parse_port("99999999999999999999", 26800)


class TransportSelectionTest(unittest.TestCase):
    """接続先の選択（D1/D10）。ix68k_mcp_server.py はimport時に確定するため、
    実行環境のsys.platformに応じた分岐だけを検査する（他プラットフォームの
    分岐を動的に切り替えて再現することはしない。importが1回しかできないため）。
    """

    def test_transport_matches_platform(self):
        expected_tcp = sys.platform == "win32"
        self.assertEqual(_server._USE_TCP, expected_tcp)
        self.assertEqual(_hostfile._USE_TCP, expected_tcp)
        if expected_tcp:
            self.assertEqual(_server.HOST, "127.0.0.1")
            self.assertNotEqual(_server.HOST, "localhost")

    def test_clients_share_wire_module(self):
        self.assertEqual(_server._wire.expected_proto, 5)
        self.assertEqual(_hostfile._PROTO_EXPECTED, 5)


if __name__ == "__main__":
    unittest.main()
