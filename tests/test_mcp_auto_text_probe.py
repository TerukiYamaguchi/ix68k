#!/usr/bin/env python3
"""HIOCS.X / IOCS.X の出力採取点の自動有効化の契約テスト。

エミュレータの代わりに、メモリと TEXT PROBE の状態を持つ偽の _send_command を使う。"""

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
    loader = SourceFileLoader("ix68k_mcp_auto_text_probe_test", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


_server = _load_server()

DOS_PUTCHAR_ENTRY = 0x1808
ORIGINAL_HANDLER = 0x8888
HIOCS_HOOK = bytes.fromhex("207a24e40c100001")
HIOCS_BODY = bytes.fromhex("1038099066260cb8")
HIOCS_BODY_BRA = bytes.fromhex("6000ff5466260cb8")
IOCSX_HOOK = bytes.fromhex("207a05280c100001")
IOCSX_BODY = bytes.fromhex("41ed09901010662e")


class FakeEmulator:
    """MEM PEEK・CONFIG GET・TEXT PROBE だけを返す。"""

    def __init__(self, auto_option="OK TEXT_AUTO_PROBE=1"):
        self.auto_option = auto_option
        self.memory = {}
        self.probe_defs = []
        self.enabled = False
        self.commands = []
        self.set_vector(ORIGINAL_HANDLER)

    def set_vector(self, address):
        self.memory[DOS_PUTCHAR_ENTRY] = address.to_bytes(4, "big")

    def install(self, hook_address, hook, body_address, body):
        self.set_vector(hook_address)
        self.memory[hook_address] = hook
        self.memory[body_address] = body

    def reset(self):
        """エミュレータのリセット: プローブは無効に戻り、ドライバは未常駐に戻る。"""
        self.enabled = False
        self.memory = {}
        self.set_vector(ORIGINAL_HANDLER)

    def send(self, command, timeout=10.0, body=b""):
        self.commands.append(command)
        if command == "CONFIG GET TEXT_AUTO_PROBE":
            return self.auto_option, b""
        if command in ("RESET", "RESET FD0"):
            self.reset()
            return "OK", b""
        if command == "TEXT PROBE STATUS":
            return f"OK COUNT={len(self.probe_defs)} ENABLED={int(self.enabled)}", b""
        if command == "TEXT PROBE CLEAR":
            self.probe_defs, self.enabled = [], False
            return "OK", b""
        if command == "TEXT PROBE ENABLE":
            self.enabled = True
            return "OK", b""
        if command.startswith("TEXT PROBE ADD "):
            self.probe_defs.append(command[len("TEXT PROBE ADD "):])
            return "OK", b""
        if command.startswith("MEM PEEK "):
            parts = command.split()
            address, length = int(parts[2], 16), int(parts[3], 16)
            data = self.memory.get(address)
            if data is None or len(data) < length:
                return "ERR BADADDR", b""
            return "OK", data[:length]
        raise AssertionError(f"想定外のコマンド: {command}")

    def commands_starting(self, prefix):
        return [command for command in self.commands if command.startswith(prefix)]


class AutoTextProbeTest(unittest.TestCase):
    def setUp(self):
        _server._auto_text_reset_state()
        self.emulator = FakeEmulator()
        patcher = patch.object(_server, "_send_command", self.emulator.send)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tick(self):
        _server._auto_text_probe_tick()

    def assertArmed(self, source, pc):
        self.assertTrue(self.emulator.enabled)
        self.assertEqual(len(self.emulator.probe_defs), 1)
        self.assertEqual(
            self.emulator.probe_defs[0],
            f"0x{pc:X} CHAR D1 1 {source} B_PUTC_body shift-jis SUPER")
        self.assertEqual(_server._auto_text["source"], source)
        self.assertEqual(_server._auto_text["pc"], pc)

    def test_hiocs_is_armed_at_body_entry_derived_from_dos_vector(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x62142 - 0x30AC, HIOCS_BODY)
        self.tick()
        self.assertArmed("hiocs", 0x5F096)

    def test_hiocs_body_patched_with_bra_by_condrv_is_accepted(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY_BRA)
        self.tick()
        self.assertArmed("hiocs", 0x5F096)

    def test_iocsx_is_armed(self):
        self.emulator.install(0x5CBD2, IOCSX_HOOK, 0x5CBD2 - 0x2A9E, IOCSX_BODY)
        self.tick()
        self.assertArmed("iocsx", 0x5A134)

    def test_iocsx_body_with_bra_is_rejected(self):
        self.emulator.install(0x5CBD2, IOCSX_HOOK, 0x5A134, bytes.fromhex("6000ff541010662e"))
        self.tick()
        self.assertFalse(self.emulator.enabled)
        self.assertFalse(_server._auto_text["owned"])

    def test_not_armed_without_driver(self):
        self.tick()
        self.assertFalse(self.emulator.enabled)
        self.assertEqual(self.emulator.commands_starting("TEXT PROBE ADD"), [])

    def test_body_signature_mismatch_is_not_armed(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, bytes(8))
        self.tick()
        self.assertFalse(self.emulator.enabled)

    def test_option_off_does_nothing(self):
        self.emulator.auto_option = "OK TEXT_AUTO_PROBE=0"
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.assertFalse(self.emulator.enabled)
        self.assertEqual(self.emulator.commands_starting("TEXT PROBE"), [])

    def test_old_emulator_without_config_command_does_nothing(self):
        self.emulator.auto_option = "ERR UNKNOWN"
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.assertFalse(self.emulator.enabled)
        self.assertEqual(self.emulator.commands_starting("TEXT PROBE"), [])

    def test_second_tick_does_not_rearm(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.emulator.commands.clear()
        self.tick()
        self.assertEqual(self.emulator.commands_starting("TEXT PROBE ADD"), [])
        self.assertEqual(self.emulator.commands_starting("TEXT PROBE CLEAR"), [])

    def test_release_disables_the_probe_it_armed(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.emulator.set_vector(ORIGINAL_HANDLER)
        self.tick()
        self.assertFalse(self.emulator.enabled)
        self.assertFalse(_server._auto_text["owned"])

    def test_reinstall_at_another_address_rearms(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.emulator.install(0x16417C, HIOCS_HOOK, 0x16417C - 0x30AC, HIOCS_BODY)
        self.tick()
        self.assertArmed("hiocs", 0x16417C - 0x30AC)

    def test_user_clear_suppresses_until_reset(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        _server.text_probe(action="clear")
        self.assertFalse(self.emulator.enabled)
        self.tick()
        self.assertFalse(self.emulator.enabled)
        self.assertTrue(_server._auto_text["suppressed"])
        # リセットすると、次の常駐から再び自動で有効になる
        _server.reset()
        self.assertFalse(_server._auto_text["suppressed"])
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.assertArmed("hiocs", 0x5F096)

    def test_external_reset_clears_ownership_and_suppression(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.emulator.reset()  # GUI などツール外のリセット
        self.tick()
        self.assertFalse(_server._auto_text["owned"])
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.assertArmed("hiocs", 0x5F096)

    def test_manual_probe_is_not_touched(self):
        self.emulator.probe_defs = ["0x1000 CHAR D1 1 other manual shift-jis"]
        self.emulator.enabled = True
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.assertEqual(self.emulator.probe_defs, ["0x1000 CHAR D1 1 other manual shift-jis"])
        self.assertFalse(_server._auto_text["owned"])
        self.assertEqual(self.emulator.commands_starting("TEXT PROBE CLEAR"), [])

    def test_enable_failure_is_reported_in_status_and_retried(self):
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        original_send = self.emulator.send

        def failing_enable(command, timeout=10.0, body=b""):
            if command == "TEXT PROBE ENABLE":
                return "ERR NOTREADY entry probe is owned by PERF", b""
            return original_send(command, timeout, body)

        with patch.object(_server, "_send_command", failing_enable):
            self.tick()
            status = _server.text_probe(action="status")
        self.assertFalse(self.emulator.enabled)
        self.assertEqual(self.emulator.probe_defs, [])
        self.assertIn("AUTO=ERROR", status)
        self.tick()  # 失敗が解消すれば次のツール呼び出しで有効になる
        self.assertArmed("hiocs", 0x5F096)

    def test_status_reports_auto_state(self):
        self.assertTrue(_server.text_probe(action="status").endswith("AUTO=IDLE"))
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        self.tick()
        self.assertTrue(_server.text_probe(action="status").endswith("AUTO=hiocs@5F096"))

    def test_throttled_tick_skips_within_interval(self):
        _server._auto_text_last_tick = _server.time.monotonic()
        self.emulator.install(0x62142, HIOCS_HOOK, 0x5F096, HIOCS_BODY)
        _server._auto_text_probe_tick(60.0)
        self.assertEqual(self.emulator.commands, [])


if __name__ == "__main__":
    unittest.main()
