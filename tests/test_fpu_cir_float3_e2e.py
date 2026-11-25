#!/usr/bin/env python3
"""FLOAT3.X を実際に常駐させる FPU-CIR の E2E テスト（headless ix68k + MCP ワイヤ）。

disk/FLOAT3.XDF（tools-host/dev/make_float3_xdf.sh が disk/HUMAN302.XDF から生成。CONFIG.SYS が
\\SYS\\FLOAT3.X を常駐させる）で起動し、次を確認する。

  - ボードあり (-fpu on): FLOAT3.X が登録され、IOCS $FF のフック（$07FC）が
    `move.w #0,$E9E006` の reset ハンドラ（L001698）を指し、白帯の「中止」(_ABORTJOB) で
    そのハンドラに停止して、2 命令後に D0=0（null frame の readback）になる。
    BASIC の SIN・乗算・SQR・EXP・LOG・COS・TAN・ATAN が FLOAT3.X の CIR 経由ルーチン
    （FSIN: L0036be、FMUL.D: L003d30、FSQRT: L00384a 等）に届き、正しい値を返す
  - ボードなし: FLOAT3.X が登録されない（画面から確認できるのは「\\SYS\\FLOAT3.X」の登録失敗まで。
    漢字は TEXT BITMAP で読めないため、「数値演算プロセッサがありません」の文言そのものは見ない）

前提が無い環境（headless バイナリ・ROM・sram.dat・disk/FLOAT3.XDF の欠落）では終了コード 77（ctest の SKIP）で
終わる。実行ファイルは環境変数 IX68K_BIN、ROM は IX68K_ROM、CGROM は IX68K_CGROM で上書きできる。
"""

import os
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
BIN = Path(os.environ.get("IX68K_BIN", _ROOT / "build-nogui" / "ix68k-nogui"))
ROM = Path(os.environ.get("IX68K_ROM", _ROOT / "rom" / "IPLROM.DAT"))
CGROM = Path(os.environ.get("IX68K_CGROM", _ROOT / "rom" / "CGROM.DAT"))
SRAM = _ROOT / "sram.dat"
XDF = _ROOT / "disk" / "FLOAT3.XDF"
ASM_BUILD = _ROOT / "tools" / "develop" / "asmtest" / "build.sh"
ASM_SOURCE = _ROOT / "tools" / "develop" / "asmtest" / "float3_funcs_test.s"
ASM_TOOLCHAIN = (Path("/opt/homebrew/bin/m68k-xelf-gcc"), Path("/opt/homebrew/bin/m68k-xelf-as"),
                 Path("/opt/homebrew/opt/elf2x68k/bin/elf2x68k.py"),
                 _ROOT / "tools" / "host" / "hu68disk" / "hu68disk")
ASM_PROGRAM = "FLOAT3_FUNCS_TEST"  # float3_funcs_test.s を build.sh が書き込む実行ファイル名
ASM_CHECKS = ("FSINH", "FCOSH", "FTANH", "FATANH", "FASIN", "FACOS", "FLOG10", "FLOG2", "FPIMUL")

BOOT_TIMEOUT_S = 40.0
POLL_S = 0.2
SCAN_A = 0x1E61  # 'a'（kcode<<8 | char。MCP の key("press", "a") と同じ値）
IOCS_ABORTJOB_VECTOR = 0x07FC  # $0400 + 0xFF * 4
RESET_HANDLER_SIGNATURE = bytes.fromhex("33FC000000E9E006")  # move.w #0,$E9E006
RESET_HANDLER_OFFSET = 0x1698  # FLOAT3.S の L001698
FSIN_ROUTINE_OFFSET = 0x36BE  # FLOAT3.S の L0036be（FSIN.D の command $540E を書く）
FMUL_ROUTINE_OFFSET = 0x3D30  # FLOAT3.S の L003d30（FMUL.D、register 版）
FTAN_ROUTINE_OFFSET = 0x3632  # FLOAT3.S の L003632（FTAN.D の command $540F）
FATAN_ROUTINE_OFFSET = 0x367A  # FLOAT3.S の L00367a（FATAN.D の command $540A）
FCOS_ROUTINE_OFFSET = 0x3706  # FLOAT3.S の L003706（FCOS.D の command $541D）
FLOGN_ROUTINE_OFFSET = 0x374C  # FLOAT3.S の L00374c（FLOGN.D の command $5414）
FETOX_ROUTINE_OFFSET = 0x37DA  # FLOAT3.S の L0037da（FETOX.D の command $5410）
FSQRT_ROUTINE_OFFSET = 0x384A  # FLOAT3.S の L00384a（FSQRT.D の command $5404）
ORIGINAL_HANDLER_STEPS = 2  # move.w #0,$E9E006 / move.w $E9E006,d0 の後、jmp の手前
JMP_ABSOLUTE_LONG_OFFSET = 6  # 2 命令（8 + 6 byte）の後ろの jmp の位置は handler + 14
FLOAT3_BANNER = "Co-PROCESSOR DRIVER"
FLOAT3_REGISTER_ERROR = "\\SYS\\FLOAT3.X"  # 「が登録できませんでした」の前半（ASCII 部分）
SIN_1 = "0.8414709848079"
PRODUCT_1_5_2_5 = "3.75"
SQRT_2 = "1.4142135623731"
EXP_1 = "2.718281828459"
LOG_2 = "0.69314718055995"
COS_1 = "0.54030230586814"
TAN_1 = "1.5574077246549"
ATAN_1 = "0.78539816339745"


def _missing_prerequisite():
    for path in (BIN, ROM, SRAM, XDF):
        if not path.exists():
            return f"{path} がありません"
    return None


class Emulator:
    def __init__(self, fpu: str, xdf: Path = XDF):
        self.fpu = fpu
        self.xdf = xdf
        self.tmp = Path(tempfile.mkdtemp(prefix="ix68k_fpu_e2e_"))
        self.sock = self.tmp / "mcp.sock"
        self.proc = None
        self.client = WireClient(str(self.sock))

    def start(self):
        sram = self.tmp / "sram.dat"
        shutil.copy(SRAM, sram)
        args = [str(BIN), "-no-config", "-rom", str(ROM), "-sram", str(sram),
                "-fd0", str(self.xdf), "-boot", "fdd", "-fpu", self.fpu]
        if CGROM.exists():
            args += ["-cgrom", str(CGROM)]
        env = dict(os.environ, IX68K_MCP_SOCK=str(self.sock))
        self.proc = subprocess.Popen(args, env=env, cwd=_ROOT,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + BOOT_TIMEOUT_S
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("ix68k が起動直後に終了しました")
            if self.sock.exists() and "Command version" in self.text():
                return
            time.sleep(POLL_S)
        raise RuntimeError("Human68k の起動を待てませんでした")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def send(self, command, timeout=10.0):
        response = self.client.send(command, timeout)
        return response.status, response.body

    def text(self):
        try:
            status, body = self.send("TEXT BITMAP")
        except Exception:
            return ""
        return body.decode("shift_jis", errors="replace") if status == "OK" else ""

    def peek(self, address, length):
        status, body = self.send(f"MEM PEEK {address:X} {length:X}")
        assert status == "OK", status
        return body

    def peek_long(self, address):
        return int.from_bytes(self.peek(address, 4), "big")

    def type_line(self, line, timeout_ms=15000):
        """行入力待ちを待ってから 1 行貼り付ける"""
        self.send("KEY CLEAR")
        status, _ = self.send(f"KEY WAIT {timeout_ms}", timeout_ms / 1000.0 + 5.0)
        assert status.startswith("OK"), status
        raw = (line + "\n").encode("shift_jis")
        self.send(f"PASTE {raw.hex().upper()}")

    def wait_text(self, needle, timeout_s=15.0):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if needle in self.text():
                return True
            time.sleep(POLL_S)
        return False

    def wait_paused(self, timeout_s=10.0):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            status, _ = self.send("DBG STATUS")
            if "STATE=paused" in status:
                return status
            time.sleep(POLL_S)
        return None

    def press_a(self):
        self.send(f"KEY SCAN {SCAN_A:04X}")


@unittest.skipIf(_missing_prerequisite(), str(_missing_prerequisite()))
class Float3BoardPresent(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.emu = Emulator(fpu="on")
        cls.addClassCleanup(cls.emu.stop)  # start() が失敗しても後始末する
        cls.emu.start()
        cls.handler = cls.emu.peek_long(IOCS_ABORTJOB_VECTOR)
        cls.base = cls.handler - RESET_HANDLER_OFFSET

    def tearDown(self):
        # 失敗しても BP と停止状態を次のテストへ持ち越さない
        self.emu.send("BP CLEAR")
        self.emu.send("WHITEBAND OFF")
        status, _ = self.emu.send("DBG STATUS")
        if "STATE=paused" in status:
            self.emu.send("DBG CONT")

    def test_01_driver_registered(self):
        text = self.emu.text()
        self.assertIn(FLOAT3_BANNER, text)
        self.assertNotIn(FLOAT3_REGISTER_ERROR, text)

    def test_02_iocs_abort_hook_points_at_reset_handler(self):
        self.assertEqual(self.emu.peek(self.handler, len(RESET_HANDLER_SIGNATURE)),
                         RESET_HANDLER_SIGNATURE)

    def test_03_abort_job_reaches_reset_handler(self):
        status, _ = self.emu.send(f"BP ADD {self.handler:X}")
        self.assertTrue(status.startswith("OK"), status)
        # 空のドライブ → 白帯「中止＜Ａ＞」。白帯検出で停止させ、表示が出たことを確定してから A を押す
        self.emu.send("WHITEBAND ON")
        self.emu.type_line("DIR B:")
        stop = self.emu.wait_paused()
        self.assertIsNotNone(stop, "白帯が出ませんでした")
        self.assertIn("STOP=WHITEBAND", stop)
        self.emu.send("WHITEBAND OFF")
        self.emu.send("DBG CONT")
        self.emu.press_a()
        stop = self.emu.wait_paused()
        self.assertIsNotNone(stop, "_ABORTJOB のハンドラで停止しませんでした")
        self.assertIn("STOP=BREAKPOINT", stop)
        self.assertIn(f"PC={self.handler:08X}", stop)
        self.assertIn("D0=000000FF", stop)  # IOCS _ABORTJOB
        # ハンドラの 2 命令（$E9E006 への書込みと読出し）の後、D0 は null frame の readback の 0
        status, _ = self.emu.send(f"DBG STEP {ORIGINAL_HANDLER_STEPS}")
        self.assertIn("D0=00000000", status)
        jmp = self.handler + len(RESET_HANDLER_SIGNATURE) + JMP_ABSOLUTE_LONG_OFFSET
        self.assertIn(f"PC={jmp:08X}", status)

    def _start_basic(self):
        self.emu.type_line("BASIC2\\BASIC")
        self.assertTrue(self.emu.wait_text("Ok"), "BASIC が起動しませんでした")

    def _ensure_basic(self):
        if "Ok" not in self.emu.text():  # 前のテストが失敗して BASIC が動いていない場合
            self._start_basic()

    def _run_through_routine(self, statement, routine, expected_regs, expected_text):
        address = self.base + routine
        status, _ = self.emu.send(f"BP ADD {address:X}")
        self.assertTrue(status.startswith("OK"), status)
        self.emu.type_line(statement)
        stop = self.emu.wait_paused()
        self.assertIsNotNone(stop, f"{statement} が FLOAT3.X のルーチンに到達しませんでした")
        self.assertIn(f"PC={address:08X}", stop)
        for reg in expected_regs:
            self.assertIn(reg, stop)
        self.emu.send("BP CLEAR")
        self.emu.send("DBG CONT")
        self.assertTrue(self.emu.wait_text(expected_text), f"{statement} の結果が表示されませんでした")

    def test_04_basic_sin_runs_through_fsin_routine(self):
        self._start_basic()
        self._run_through_routine("PRINT SIN(1)", FSIN_ROUTINE_OFFSET,
                                  ["D0=3FF00000"], SIN_1)  # 引数 1.0 の上位 long

    def test_05_basic_multiply_runs_through_fmul_routine(self):
        self._ensure_basic()
        self._run_through_routine("PRINT 1.5*2.5", FMUL_ROUTINE_OFFSET,
                                  ["D0=3FF80000", "D2=40040000"], PRODUCT_1_5_2_5)  # 1.5 と 2.5

    def test_06_basic_sqr_runs_through_fsqrt_routine(self):
        self._ensure_basic()
        self._run_through_routine("PRINT SQR(2)", FSQRT_ROUTINE_OFFSET,
                                  ["D0=40000000"], SQRT_2)  # 引数 2.0 の上位 long

    def test_07_basic_exp_runs_through_fetox_routine(self):
        self._ensure_basic()
        self._run_through_routine("PRINT EXP(1)", FETOX_ROUTINE_OFFSET,
                                  ["D0=3FF00000"], EXP_1)

    def test_08_basic_log_runs_through_flogn_routine(self):
        self._ensure_basic()
        self._run_through_routine("PRINT LOG(2)", FLOGN_ROUTINE_OFFSET,
                                  ["D0=40000000"], LOG_2)

    def test_09_basic_cos_runs_through_fcos_routine(self):
        self._ensure_basic()
        self._run_through_routine("PRINT COS(1)", FCOS_ROUTINE_OFFSET,
                                  ["D0=3FF00000"], COS_1)

    def test_10_basic_tan_runs_through_ftan_routine(self):
        self._ensure_basic()
        self._run_through_routine("PRINT TAN(1)", FTAN_ROUTINE_OFFSET,
                                  ["D0=3FF00000"], TAN_1)

    def test_11_basic_atan_runs_through_fatan_routine(self):
        self._ensure_basic()
        self._run_through_routine("PRINT ATAN(1)", FATAN_ROUTINE_OFFSET,
                                  ["D0=3FF00000"], ATAN_1)


def _missing_asm_prerequisite():
    for path in ASM_TOOLCHAIN:
        if not path.exists():
            return f"{path} がありません"
    return None


@unittest.skipIf(_missing_prerequisite() or _missing_asm_prerequisite(),
                 str(_missing_prerequisite() or _missing_asm_prerequisite()))
class Float3FunctionsFromAssembly(unittest.TestCase):
    """BASIC から届かない関数を、Line-F の FExx を直接呼ぶ .X（float3_funcs_test.s）で確認する"""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="ix68k_float3_asm_"))
        cls.addClassCleanup(shutil.rmtree, cls.tmp, ignore_errors=True)
        xdf = cls.tmp / "FLOAT3_ASM.XDF"
        shutil.copy(XDF, xdf)
        subprocess.run(["bash", str(ASM_BUILD), str(ASM_SOURCE), str(xdf)], cwd=_ROOT, check=True,
                       stdout=subprocess.DEVNULL)
        cls.emu = Emulator(fpu="on", xdf=xdf)
        cls.addClassCleanup(cls.emu.stop)
        cls.emu.start()

    def test_functions_return_expected_values(self):
        self.emu.type_line(ASM_PROGRAM)
        self.assertTrue(self.emu.wait_text(f"{ASM_CHECKS[-1]} "), "テストプログラムが終わりませんでした")
        text = self.emu.text()
        for name in ASM_CHECKS:
            self.assertIn(f"{name} OK", text)
        self.assertNotIn(" NG", text)


@unittest.skipIf(_missing_prerequisite(), str(_missing_prerequisite()))
class Float3BoardAbsent(unittest.TestCase):
    def test_register_fails_without_board(self):
        emu = Emulator(fpu="off")
        try:
            emu.start()
            text = emu.text()
            self.assertIn(FLOAT3_REGISTER_ERROR, text)
            self.assertNotIn(FLOAT3_BANNER, text)
        finally:
            emu.stop()


if __name__ == "__main__":
    if _missing_prerequisite():
        print(f"skip: {_missing_prerequisite()}")
        sys.exit(SKIP_EXIT_CODE)
    unittest.main(verbosity=2)
