#!/usr/bin/env python3
"""MCP snapshot() ツールのワイヤ・例外変換契約。"""

import unittest
from unittest.mock import patch

from test_mcp_text_events import _server


class SnapshotToolTest(unittest.TestCase):
    def test_each_action_uses_its_wire_command(self):
        for action, command, result in (
            ("save", "SNAPSHOT SAVE", "SLOT=VALID"),
            ("load", "SNAPSHOT LOAD", "STATE=paused STOP=SNAPSHOT"),
            ("status", "SNAPSHOT STATUS", "SLOT=EMPTY"),
        ):
            with patch.object(_server, "_send_command", return_value=(f"OK {result}", b"")) as send:
                self.assertEqual(_server.snapshot(action), result)
            send.assert_called_once_with(command)

    def test_invalid_action_is_rejected_before_transport(self):
        with self.assertRaisesRegex(ValueError, "save"):
            _server.snapshot("discard")

    def test_wire_error_becomes_runtime_error(self):
        with patch.object(_server, "_send_command", return_value=("ERR NOTREADY RUNNING", b"")):
            with self.assertRaisesRegex(RuntimeError, "NOTREADY RUNNING"):
                _server.snapshot("save")


if __name__ == "__main__":
    unittest.main()
