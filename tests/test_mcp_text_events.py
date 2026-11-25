#!/usr/bin/env python3
"""read_text_events() の圧縮・検索契約テスト。"""

import importlib.util
import sys
import types
import unittest
from importlib.machinery import SourceFileLoader
from pathlib import Path
from unittest.mock import patch


_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT / "tools" / "mcp"))


def _install_fastmcp_stub():
    """MCP Pythonパッケージ無しでもサーバーの純粋関数を検証できるようにする。"""
    if "mcp.server.fastmcp" in sys.modules:
        return
    mcp = types.ModuleType("mcp")
    server = types.ModuleType("mcp.server")
    fastmcp = types.ModuleType("mcp.server.fastmcp")

    class FastMCP:
        def __init__(self, _name):
            pass

        @staticmethod
        def tool():
            return lambda function: function

    fastmcp.FastMCP = FastMCP
    fastmcp.Image = object
    sys.modules.update({"mcp": mcp, "mcp.server": server, "mcp.server.fastmcp": fastmcp})


def _load_server():
    _install_fastmcp_stub()
    path = _ROOT / "tools" / "mcp" / "ix68k_mcp_server.py"
    loader = SourceFileLoader("ix68k_mcp_text_events_test", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


_server = _load_server()


def _field(value: str, size: int) -> bytes:
    return value.encode("ascii").ljust(size, b"\0")


def _event(seq: int, raw: bytes, *, probe: str = "console", source: str = "iocscall",
           encoding: str = "shift-jis") -> bytes:
    return (seq.to_bytes(8, "big") + (1).to_bytes(8, "big") + (0x1000).to_bytes(4, "big")
            + (0).to_bytes(4, "big", signed=True) * 3 + (0).to_bytes(2, "big")
            + _field(source, 32) + _field(probe, 32) + _field(encoding, 16)
            + len(raw).to_bytes(4, "big") + raw)


def _response(*events: bytes, dropped: int = 0, epoch: int = 1) -> bytes:
    return (b"TEV1" + len(events).to_bytes(4, "big") + dropped.to_bytes(4, "big")
            + epoch.to_bytes(8, "big") + b"".join(events))


class ReadTextEventsTest(unittest.TestCase):
    def setUp(self):
        _server._text_event_buffer.clear()
        _server._text_event_buffer_size = 0
        _server._text_event_epoch = None
        _server._text_event_contiguous_from = None
        _server._text_event_generation = 0
        _server._wait_cursor = None

    def responses(self, *bodies):
        values = iter(bodies)
        return patch.object(_server, "_send_command", side_effect=lambda *_args, **_kwargs: ("OK", next(values)))

    def test_text_mode_combines_raw_bytes_before_shift_jis_decode(self):
        body = _response(_event(1, b"\x82"), _event(2, b"\xa0"))
        with self.responses(body):
            result = _server.read_text_events()
        self.assertEqual(result["streams"], [{"probe": "console", "text": "あ"}])
        self.assertEqual(result["next_seq"], 3)
        self.assertEqual(result["available_from_seq"], 1)

    def test_filters_cursor_and_selected_event_fields(self):
        body = _response(
            _event(10, b"A", source="first"),
            _event(11, b"B", source="second"),
            _event(12, b"C", source="first"),
        )
        with self.responses(body):
            result = _server.read_text_events(source="first", max_events=1,
                                              detail="events", fields=["seq", "raw_hex"])
        self.assertEqual(result["events"], [{"seq": 10, "raw_hex": "41"}])
        self.assertEqual(result["next_seq"], 11)

    def test_match_can_cross_two_drains_and_combines_after_until(self):
        first = _response(_event(1, b"boot log"))
        second = _response(_event(2, b"in: root\n# tail"))
        with self.responses(first, second):
            _server.read_text_events()
            result = _server.read_text_events(after="login:", until="# ", include_match=False)
        self.assertEqual(result["streams"], [{"probe": "console", "text": " root\n"}])

    def test_contains_and_regex_filter_streams(self):
        body = _response(_event(1, b"panic: fault"), _event(2, b"\n"))
        with self.responses(body):
            result = _server.read_text_events(contains=r"panic: \w+", regex=True)
        self.assertEqual(result["streams"][0]["text"], "panic: fault\n")

    def test_buffer_limit_discards_oldest_whole_event(self):
        old_limit = _server._TEXT_EVENT_BUFFER_LIMIT
        _server._TEXT_EVENT_BUFFER_LIMIT = 3
        try:
            body = _response(_event(1, b"ab"), _event(2, b"cd"), dropped=2)
            with self.responses(body):
                result = _server.read_text_events()
        finally:
            _server._TEXT_EVENT_BUFFER_LIMIT = old_limit
        self.assertEqual(result["available_from_seq"], 2)
        self.assertEqual(result["streams"], [{"probe": "console", "text": "cd"}])
        self.assertEqual(result["dropped"], 2)

    def test_dropped_is_per_read_not_cumulative(self):
        first = _response(_event(5, b"a"), dropped=4)
        second = _response(_event(6, b"b"))
        third = _response(_event(20, b"c"), dropped=13)
        with self.responses(first, second, third):
            self.assertEqual(_server.read_text_events()["dropped"], 4)
            self.assertNotIn("dropped", _server.read_text_events())
            self.assertEqual(_server.read_text_events()["dropped"], 13)

    def test_available_from_seq_moves_past_a_gap(self):
        first = _response(_event(1, b"a"), _event(2, b"b"))
        second = _response(_event(10, b"c"), dropped=7)
        with self.responses(first, second):
            self.assertEqual(_server.read_text_events()["available_from_seq"], 1)
            result = _server.read_text_events()
        self.assertEqual(result["available_from_seq"], 10)
        self.assertEqual(result["next_seq"], 11)
        self.assertEqual(result["streams"][0]["text"], "abc")

    def test_contiguous_drains_keep_available_from_seq(self):
        with self.responses(_response(_event(1, b"a")), _response(_event(2, b"b"))):
            _server.read_text_events()
            self.assertEqual(_server.read_text_events()["available_from_seq"], 1)

    def test_epoch_change_discards_old_buffer(self):
        first = _response(_event(100, b"old"), epoch=1)
        second = _response(_event(100, b"new"), epoch=2)
        with self.responses(first, second):
            _server.read_text_events()
            result = _server.read_text_events()
        self.assertEqual(result["streams"][0]["text"], "new")
        self.assertEqual(result["available_from_seq"], 100)

    def test_seq_rewind_with_same_epoch_discards_old_buffer(self):
        first = _response(_event(500, b"old"), _event(501, b"x"))
        second = _response(_event(0, b"new"))
        with self.responses(first, second):
            _server.read_text_events()
            result = _server.read_text_events()
        self.assertEqual(result["streams"][0]["text"], "new")
        self.assertEqual(result["available_from_seq"], 0)
        self.assertEqual(result["next_seq"], 1)

    def test_event_count_limit_discards_oldest_events(self):
        old_limit = _server._TEXT_EVENT_BUFFER_MAX_EVENTS
        _server._TEXT_EVENT_BUFFER_MAX_EVENTS = 2
        try:
            body = _response(_event(1, b"a"), _event(2, b"b"), _event(3, b"c"))
            with self.responses(body):
                result = _server.read_text_events()
        finally:
            _server._TEXT_EVENT_BUFFER_MAX_EVENTS = old_limit
        self.assertEqual(result["available_from_seq"], 2)
        self.assertEqual(result["streams"][0]["text"], "bc")

    def test_generation_increments_only_when_buffer_is_discarded(self):
        bodies = (_response(_event(1, b"a"), epoch=1), _response(_event(2, b"b"), epoch=1),
                  _response(_event(3, b"c"), epoch=2), _response(_event(0, b"d"), epoch=2))
        with self.responses(*bodies):
            _server.read_text_events()
            _server.read_text_events()
            self.assertEqual(_server._text_event_generation, 0)
            _server.read_text_events()
            self.assertEqual(_server._text_event_generation, 1)
            _server.read_text_events()
            self.assertEqual(_server._text_event_generation, 2)


class FakeClock:
    """sleep で進む疑似時計。実時間待ちを避ける。"""

    def __init__(self):
        self.now = 1000.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class WaitForTest(ReadTextEventsTest):
    def setUp(self):
        super().setUp()
        self.clock = FakeClock()
        patches = (patch.object(_server.time, "monotonic", self.clock.monotonic),
                   patch.object(_server.time, "sleep", self.clock.sleep))
        for item in patches:
            item.start()
            self.addCleanup(item.stop)

    def feed(self, *bodies):
        """drain のたびに次の応答を返し、尽きたら空応答を返す。"""
        values = iter(bodies)
        return patch.object(
            _server, "_send_command",
            side_effect=lambda *_a, **_k: ("OK", next(values, _response())))

    def test_matches_immediately_from_server_start_position(self):
        with self.feed(_response(_event(1, b"old login: ")), _response(_event(2, b"new login: "))):
            _server._wait_cursor = None
            result = _server.wait_for("login:", timeout_ms=1000, since_seq=2)
        self.assertTrue(result["matched"])
        self.assertEqual(result["text"], "new login:")
        self.assertEqual(result["next_seq"], 3)
        self.assertEqual(result["since_seq_used"], 2)

    def test_first_call_without_cursor_ignores_existing_output(self):
        _server._text_event_buffer.append({"seq": 4, "epoch": 1, "pc": 0, "x": 0, "y": 0,
                                           "attr": 0, "flags": 0, "source": "iocscall",
                                           "probe": "console", "encoding": "shift-jis",
                                           "raw_bytes": b"login: "})
        with self.feed(_response(), _response(_event(5, b"login: "))):
            result = _server.wait_for("login:", timeout_ms=1000)
        self.assertEqual(result["since_seq_used"], 5)
        self.assertEqual(result["next_seq"], 6)

    def test_timeout_without_cursor_pins_start_for_retry(self):
        with self.feed(_response(_event(1, b"boot")), _response(_event(2, b"log")),
                       _response(_event(3, b"in:"))):
            first = _server.wait_for("login:", timeout_ms=0)
            second = _server.wait_for("login:", timeout_ms=1000)
        self.assertFalse(first["matched"])
        self.assertEqual(second["since_seq_used"], first["since_seq_used"])
        self.assertTrue(second["matched"])
        self.assertEqual(second["text"], "login:")

    def test_match_appears_after_several_polls_and_crosses_boundary(self):
        bodies = (_response(), _response(_event(1, b"boot... log")), _response(_event(2, b"in: ")))
        with self.feed(*bodies):
            result = _server.wait_for("login:", timeout_ms=5000, since_seq=0)
        self.assertTrue(result["matched"])
        self.assertEqual(result["text"], "boot... login:")
        self.assertEqual(result["next_seq"], 3)
        self.assertGreater(result["elapsed_ms"], 0)

    def test_timeout_returns_unmatched_and_keeps_start(self):
        with self.feed(_response(_event(1, b"booting"))):
            result = _server.wait_for("login:", timeout_ms=200, since_seq=0)
        self.assertFalse(result["matched"])
        self.assertEqual(result["text"], "booting")
        self.assertEqual(result["next_seq"], 0)
        self.assertEqual(result["probe"], "console")

    def test_timeout_retry_without_args_catches_split_pattern(self):
        _server._wait_cursor = (0, 0, _server._text_event_generation)
        with self.feed(_response(_event(1, b"log")), _response(_event(2, b"in:"))):
            first = _server.wait_for("login:", timeout_ms=0)
            self.assertFalse(first["matched"])
            second = _server.wait_for("login:", timeout_ms=1000)
        self.assertTrue(second["matched"])
        self.assertEqual(second["text"], "login:")

    def test_consecutive_calls_do_not_match_same_occurrence(self):
        _server._wait_cursor = (0, 0, _server._text_event_generation)
        body = _response(_event(1, b"$ one\n$ two\n"))
        with self.feed(body):
            first = _server.wait_for("$ ", timeout_ms=100)
            second = _server.wait_for("$ ", timeout_ms=100)
        self.assertEqual(first["text"], "$ ")
        self.assertEqual(second["text"], "one\n$ ")

    def test_two_patterns_inside_one_event_are_found_in_order(self):
        _server._wait_cursor = (0, 0, _server._text_event_generation)
        with self.feed(_response(_event(7, b"alpha beta"))):
            first = _server.wait_for("alpha", timeout_ms=100)
            second = _server.wait_for("beta", timeout_ms=100)
        self.assertEqual(first["text"], "alpha")
        self.assertEqual(second["text"], " beta")

    def test_regex_include_match_false_and_max_chars_keep_match(self):
        body = _response(_event(1, b"x" * 50 + b"panic: fault"))
        with self.feed(body):
            result = _server.wait_for(r"panic: \w+", timeout_ms=100, regex=True, since_seq=0,
                                      max_chars=5)
        self.assertEqual(result["text"], "panic: fault")
        self.assertTrue(result["truncated"])
        with self.feed(_response(_event(1, b"ab login: "))):
            result = _server.wait_for("login:", timeout_ms=100, since_seq=0, include_match=False)
        self.assertEqual(result["text"], "ab ")

    def test_probe_and_source_filters_and_smallest_seq_wins(self):
        body = _response(_event(1, b"ok", probe="a", source="s1"),
                         _event(2, b"ok", probe="b", source="s2"))
        with self.feed(body):
            result = _server.wait_for("ok", timeout_ms=100, since_seq=0)
        self.assertEqual(result["probe"], "a")
        with self.feed(body):
            _server._text_event_buffer.clear()
            result = _server.wait_for("ok", timeout_ms=100, since_seq=0, source="s2")
        self.assertEqual(result["probe"], "b")

    def test_shift_jis_split_across_polls(self):
        with self.feed(_response(_event(1, b"\x82")), _response(_event(2, b"\xa0"))):
            result = _server.wait_for("あ", timeout_ms=1000, since_seq=0)
        self.assertTrue(result["matched"])
        self.assertEqual(result["next_seq"], 3)

    def test_dropped_is_summed_and_warns(self):
        with self.feed(_response(dropped=3), _response(_event(1, b"x"), dropped=4)):
            result = _server.wait_for("zzz", timeout_ms=100, since_seq=0)
        self.assertEqual(result["dropped"], 7)
        self.assertIn("取りこぼし", result["warning"])

    def test_start_before_available_range_warns(self):
        with self.feed(_response(_event(10, b"a"), dropped=9)):
            result = _server.wait_for("a", timeout_ms=100, since_seq=2)
        self.assertIn("保持範囲", result["warning"])

    def test_cursor_set_on_empty_buffer_does_not_warn_about_gap(self):
        _server._wait_cursor = (0, 0, _server._text_event_generation)
        with self.feed(_response(_event(1093, b"login: "))):
            result = _server.wait_for("login:", timeout_ms=100)
        self.assertTrue(result["matched"])
        self.assertNotIn("warning", result)

    def test_restart_with_rewound_seq_matches_with_stale_since_seq(self):
        _server._wait_cursor = (500, 0, _server._text_event_generation)
        first = _response(_event(499, b"old"), epoch=1)
        second = _response(_event(0, b"login: "), epoch=9)
        with self.feed(first, second):
            _server._drain_text_events()
            result = _server.wait_for("login:", timeout_ms=1000)
        self.assertTrue(result["matched"])
        self.assertTrue(result["restarted"])

    def test_input_tool_mark_moves_cursor_to_end(self):
        with self.feed(_response(_event(1, b"old login: "))):
            _server._mark_wait_cursor()
        self.assertEqual(_server._wait_cursor[:2], (2, 0))
        with self.feed(_response(_event(2, b"login: ")), ):
            result = _server.wait_for("login:", timeout_ms=100)
        self.assertEqual(result["text"], "login:")

    def test_mark_failure_invalidates_cursor_without_raising(self):
        _server._wait_cursor = (1, 0, 0)
        with patch.object(_server, "_send_command", side_effect=RuntimeError("down")):
            _server._mark_wait_cursor()
        self.assertIsNone(_server._wait_cursor)

    def test_connection_errors_are_retried_until_deadline(self):
        with patch.object(_server, "_send_command",
                          side_effect=_server.WireConnectionError("down")):
            result = _server.wait_for("x", timeout_ms=200, since_seq=0)
        self.assertFalse(result["matched"])
        self.assertIn("通信", result["warning"])

    def test_poll_socket_timeout_reflects_remaining_time(self):
        calls = []
        def record(_cmd, timeout=10.0, **_kwargs):
            calls.append(timeout)
            return "OK", _response()
        with patch.object(_server, "_send_command", side_effect=record):
            _server.wait_for("x", timeout_ms=300, since_seq=0)
            _server.wait_for("x", timeout_ms=999999, since_seq=0)
        self.assertLessEqual(calls[0], 1.3)
        self.assertLessEqual(max(calls), _server._WAIT_FOR_POLL_TIMEOUT_S)

    def test_invalid_arguments_raise_before_polling(self):
        with patch.object(_server, "_send_command", side_effect=AssertionError("polled")):
            with self.assertRaises(ValueError):
                _server.wait_for("(", regex=True)
            with self.assertRaises(ValueError):
                _server.wait_for("", since_seq=0)
            with self.assertRaises(ValueError):
                _server.wait_for("x", since_seq=-1)
            with self.assertRaises(ValueError):
                _server.wait_for("x", max_chars=0)

    def test_many_single_char_events_scan_only_new_events(self):
        events = [_event(i, b"a") for i in range(1, 2001)]
        with self.feed(_response(*events[:1000]), _response(*events[1000:], _event(2001, b"Z"))):
            result = _server.wait_for("Z", timeout_ms=1000, since_seq=0)
        self.assertTrue(result["matched"])
        self.assertEqual(result["next_seq"], 2002)


if __name__ == "__main__":
    unittest.main()
