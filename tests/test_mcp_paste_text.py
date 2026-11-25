#!/usr/bin/env python3
"""paste_text() の wait 引数と KEY PASTE 利用の契約テスト。"""

import unittest
from unittest.mock import patch

from test_mcp_text_events import _server


class FakeEmulator:
    """_send_command を差し替え、送られたコマンドを記録する。"""

    def __init__(self, status_replies=None):
        self.commands = []
        self.status_replies = list(status_replies or [])

    def send(self, cmd, timeout=10.0, body=b""):
        self.commands.append(cmd)
        if cmd == "KEY PASTE":
            return (self.status_replies.pop(0) if self.status_replies else "OK 0 0 0"), b""
        return "OK", b""


class PasteTextTest(unittest.TestCase):
    def run_paste(self, text, emu, **kwargs):
        with patch.object(_server, "_send_command", emu.send), \
                patch.object(_server.time, "sleep", lambda _s: None):
            return _server.paste_text(text, **kwargs)

    def test_none_sends_every_line_without_key_check(self):
        emu = FakeEmulator()
        result = self.run_paste("a\nb\n", emu, wait="none")
        pastes = [c for c in emu.commands if c.startswith("PASTE ")]
        self.assertEqual(pastes, ["PASTE 610D", "PASTE 620D"])
        self.assertNotIn("KEY CHECK", emu.commands)
        self.assertIn("4 バイト", result)

    def test_none_waits_until_queue_and_rx_drained(self):
        emu = FakeEmulator(["OK 0 0 0", "OK 3 0 0", "OK 0 1 0", "OK 0 0 0", "OK 0 0 0"])
        self.run_paste("ab", emu, wait="none")
        self.assertEqual(emu.commands.count("KEY PASTE"), 5)

    def test_none_reports_fifo_drop(self):
        emu = FakeEmulator(["OK 0 0 2", "OK 0 0 7", "OK 0 0 7"])
        result = self.run_paste("ab", emu, wait="none")
        self.assertIn("5バイトが破棄", result)

    def test_none_timeout_names_line(self):
        emu = FakeEmulator(["OK 0 0 0"] + ["OK 5 0 0"] * 10000)
        with patch.object(_server.time, "time", side_effect=iter([0.0] + [100.0] * 10)):
            with self.assertRaisesRegex(RuntimeError, "1行目"):
                self.run_paste("ab", emu, wait="none")

    def test_iocs_timeout_names_line_and_suggests_none(self):
        emu = FakeEmulator()
        with patch.object(_server, "_wait_paste_line_stable", return_value=False):
            with self.assertRaisesRegex(RuntimeError, r"1行目.*wait=\"none\""):
                self.run_paste("uname -a\nls\n", emu)
        self.assertEqual(len([c for c in emu.commands if c.startswith("PASTE ")]), 1)

    def test_line_over_queue_limit_is_rejected_before_sending(self):
        emu = FakeEmulator()
        with self.assertRaisesRegex(ValueError, "2行目"):
            self.run_paste("ok\n" + "x" * 513, emu, wait="none")
        self.assertEqual(emu.commands, [])

    def test_unknown_wait_is_rejected(self):
        with self.assertRaises(ValueError):
            self.run_paste("a", FakeEmulator(), wait="bogus")


if __name__ == "__main__":
    unittest.main()
