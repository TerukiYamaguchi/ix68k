#!/usr/bin/env python3
"""IPLROM30 + 純正 FLOAT4.X を使う X68030 FPCP のE2E回帰。"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT / "tools" / "mcp"))
from ix68k_wire import WireClient  # noqa: E402

SKIP_EXIT_CODE = 77
MAGIC = 0x46345453  # F4TS
FEFN = 0x4645666E
RESULT_SIZE = 56
BIN = Path(os.environ.get("IX68K_BIN", _ROOT / "build-nogui" / "ix68k-nogui"))
ROM = Path(os.environ.get("IX68K_ROM", _ROOT / "rom" / "IPLROM30.DAT"))
CGROM = Path(os.environ.get("IX68K_CGROM", _ROOT / "rom" / "CGROM.DAT"))
SRAM = _ROOT / "sram.dat"
XDF = _ROOT / "disk" / "FLOAT4.XDF"
MAKE_XDF = _ROOT / "tools" / "dev" / "make_float4_xdf.sh"
PREREQUISITES = (BIN, ROM, CGROM, SRAM, MAKE_XDF, _ROOT / "disk" / "HUMAN302.XDF",
                 _ROOT / "tools" / "host" / "hu68disk" / "hu68disk",
                 Path("/opt/homebrew/bin/m68k-xelf-gcc"), Path("/opt/homebrew/bin/m68k-xelf-as"),
                 Path("/opt/homebrew/opt/elf2x68k/bin/elf2x68k.py"))


def _missing_prerequisite():
    for path in PREREQUISITES:
        if not path.exists():
            return f"{path} がありません"
    return None


class Emulator:
    def __init__(self, cpu_model, fpu):
        self.tmp = Path(tempfile.mkdtemp(prefix="ix68k_float4_e2e_"))
        self.sock = self.tmp / "mcp.sock"
        self.cpu_model = cpu_model
        self.fpu = fpu
        self.proc = None
        self.client = WireClient(str(self.sock))

    def send(self, command, timeout=10.0):
        response = self.client.send(command, timeout)
        return response.status, response.body

    def start(self):
        sram = self.tmp / "sram.dat"
        shutil.copy(SRAM, sram)
        args = [str(BIN), "-no-config", "-rom", str(ROM), "-cgrom", str(CGROM),
                "-cpu-model", self.cpu_model, "-cpu-clock", "25", "-series", "x68030",
                "-fpu", self.fpu, "-sram", str(sram),
                "-fd0", str(XDF), "-boot", "fdd"]
        env = dict(os.environ, IX68K_MCP_SOCK=str(self.sock))
        self.log = self.tmp / "ix68k.log"
        with self.log.open("wb") as output:
            self.proc = subprocess.Popen(args, env=env, cwd=_ROOT, stdout=output, stderr=output)
        deadline = time.time() + 15.0
        last_error = ""
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("ix68k が起動直後に終了しました")
            if self.sock.exists():
                try:
                    status, _ = self.send("DBG STATUS")
                    if status:
                        return
                except Exception as exc:
                    last_error = str(exc)
            time.sleep(0.1)
        raise RuntimeError(f"MCP ソケットを待てませんでした: {last_error}; {self.log.read_text(errors='replace')}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def peek(self, address, length):
        status, body = self.send(f"MEM PEEK {address:X} {length:X}")
        if status != "OK":
            raise RuntimeError(status)
        return body

    def peek_long(self, address):
        return int.from_bytes(self.peek(address, 4), "big")

    def result_stop(self):
        status, _ = self.send("EXC SET 0000000000000010")  # vector 4
        if status != "OK":
            raise RuntimeError(status)
        status, _ = self.send("RESET FD0")
        if status != "OK":
            raise RuntimeError(status)
        deadline = time.time() + 15.0
        last_status = ""
        while time.time() < deadline:
            status, _ = self.send("DBG STATUS")
            last_status = status
            if "STATE=paused" in status:
                d1 = int(re.search(r"D1=([0-9A-F]{8})", status).group(1), 16)
                if d1 == MAGIC:
                    a0 = int(re.search(r"A0=([0-9A-F]{8})", status).group(1), 16)
                    size = int(re.search(r"D0=([0-9A-F]{8})", status).group(1), 16)
                    self.send("EXC SET 0")
                    return self.peek(a0, size)
                self.send("DBG CONT")  # IPLROM30 が意図的に起こす ILLEGAL は通す。
            time.sleep(0.05)
        raise RuntimeError(f"FLOAT4TEST.X の停止を待てませんでした: {last_status}")


@unittest.skipIf(_missing_prerequisite(), str(_missing_prerequisite()))
class Float4FpcpE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["bash", str(MAKE_XDF)], cwd=_ROOT, check=True, stdout=subprocess.DEVNULL)

    def _run(self, cpu_model, fpu):
        emu = Emulator(cpu_model, fpu)
        try:
            emu.start()
            return emu, emu.result_stop()
        except Exception:
            emu.stop()
            raise

    def _assert_present(self, cpu_model):
        emu, raw = self._run(cpu_model, "on")
        try:
            words = [int.from_bytes(raw[i:i + 4], "big") for i in range(0, RESULT_SIZE, 4)]
            self.assertEqual(words[0], MAGIC)
            self.assertEqual(words[1], 1, "FLOAT4.X が常駐していません")
            self.assertEqual(words[4] >> 24, 0xff, "IPLROM30 の FPU 検出が失敗しました")
            self.assertEqual(emu.peek_long(words[2] - 4), FEFN)
            self.assertEqual(emu.peek_long(words[3] - 4), FEFN)
            # DCMP/DFIX はCCRまたはFLOAT4内部ABIで結果を返す。ここでは呼出し後に
            # F-line例外へ落ちず、続くSINまで完走したことを停止点で確認する。
            self.assertEqual(words[6:10], [0x40100000, 0, 0x400E0000, 0])
            self.assertEqual(words[12:14], [0x3FEAED54, 0x8F090CEE])
        finally:
            emu.stop()

    def test_68ec030_float4_uses_68882(self):
        self._assert_present("68ec030")

    def test_68030_float4_uses_68882(self):
        self._assert_present("68030")

    def test_fpu_none_does_not_install_float4(self):
        emu, raw = self._run("68ec030", "off")
        try:
            words = [int.from_bytes(raw[i:i + 4], "big") for i in range(0, RESULT_SIZE, 4)]
            self.assertEqual(words[0], MAGIC)
            self.assertEqual(words[1], 0)
            self.assertNotEqual(emu.peek_long(words[2] - 4), FEFN)
            self.assertEqual(emu.peek_long(0xcbd) >> 24, 0)
        finally:
            emu.stop()


if __name__ == "__main__":
    if _missing_prerequisite():
        print(f"skip: {_missing_prerequisite()}")
        sys.exit(SKIP_EXIT_CODE)
    unittest.main(verbosity=2)
