#!/usr/bin/env python3
"""PGO 計装ビルドの学習: GCC.X BENCH を 1 回実行し、.profdata にまとめる。

使い方: python3 tools-host/pgo/train.py INSTRUMENTED_IX68K OUTPUT.profdata
手順書は tools-host/pgo/README.md、設計は docs/ix68k-internals/design/pgo_design.md。

Windows の違い（Issue #268）:
  - MCP は TCP（127.0.0.1:IX68K_MCP_PORT、既定 26800）。Unix ソケットは使わない
  - SIGTERM が無いので、`taskkill /PID`（/F なし）でウィンドウを閉じる要求を送り、
    正常終了の経路で .profraw を書かせる
  - llvm-profdata は PATH か `C:\\Program Files\\LLVM\\bin` から探す
GCC.X のあるホストドライブ（tests/work/gccbench）が無い環境では、disk/GCCBENCH.XDF の
コピーを FD0 に挿して A: から実行する。
"""
import glob
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools-host", "mcp"))
from ix68k_wire import WireClient, endpoint_from_environment  # noqa: E402

IS_WIN = sys.platform == "win32"
ENDPOINT, _USE_TCP = endpoint_from_environment()
CONFIGS = ["local/profiles/disk.cfg", "local/profiles/x68060.cfg"]
HOST_BENCH_DIR = os.path.join(REPO, "tests", "work", "gccbench")
BENCH_XDF = os.path.join(REPO, "disk", "GCCBENCH.XDF")
SOCK_WAIT_TRIES = 100
SOCK_WAIT_SEC = 0.2
BOOT_TIMEOUT_SEC = 60
BENCH_TIMEOUT_SEC = 600
EXIT_TIMEOUT_SEC = 30
PROMPT_TIMEOUT_MS_PAD = 5.0


def find_profdata_cmd():
    if sys.platform == "darwin":
        return ["xcrun", "llvm-profdata"]
    found = shutil.which("llvm-profdata")
    if found:
        return [found]
    if IS_WIN:
        for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
            cand = os.path.join(base or "", "LLVM", "bin", "llvm-profdata.exe")
            if os.path.exists(cand):
                return [cand]
    sys.exit("ERROR: llvm-profdata が見つからない（Windows は winget install LLVM.LLVM）")


def run(w, command, timeout):
    """プロンプトを待ってから command を貼り付け、次のプロンプトまで待つ。"""
    w.send("KEY CLEAR", 10.0, b"")
    r = w.send(f"KEY WAIT {int(timeout * 1000)}", timeout + PROMPT_TIMEOUT_MS_PAD, b"")
    if not r.status.startswith("OK"):
        raise RuntimeError(f"no prompt before {command!r} ({r.status})")
    w.send("PASTE " + (command + "\n").encode("shift_jis").hex().upper(), 10.0, b"")
    r = w.send(f"KEY WAIT {int(timeout * 1000)}", timeout + PROMPT_TIMEOUT_MS_PAD, b"")
    if not r.status.startswith("OK"):
        raise RuntimeError(f"{command!r} did not finish ({r.status})")


def endpoint_ready(w):
    if _USE_TCP:
        try:
            w.ensure_protocol(2.0)
            return True
        except Exception:
            return False
    return os.path.exists(ENDPOINT)


def prepare_bench(w, work_dir):
    """BENCH を実行できる場所へ移る。ホストドライブに無ければ XDF のコピーを FD0 に挿す。"""
    if os.path.isdir(HOST_BENCH_DIR):
        run(w, "G:", BOOT_TIMEOUT_SEC)
        run(w, r"CD \gccbench", BOOT_TIMEOUT_SEC)
        return
    if not os.path.exists(BENCH_XDF):
        raise RuntimeError(f"BENCH が見つからない: {HOST_BENCH_DIR} も {BENCH_XDF} も無い")
    xdf = os.path.join(work_dir, "GCCBENCH.XDF")  # BENCH は書き込むので元は汚さない
    shutil.copyfile(BENCH_XDF, xdf)
    run(w, "C:", BOOT_TIMEOUT_SEC)  # 起動完了待ち
    r = w.send(f"DISK INSERT FD 0 {xdf}", 10.0, b"")
    if not r.status.startswith("OK"):
        raise RuntimeError(f"DISK INSERT FD 0 が失敗 ({r.status})")
    run(w, "A:", BOOT_TIMEOUT_SEC)
    run(w, r"CD \gccbench", BOOT_TIMEOUT_SEC)


def stop(proc):
    """計装ビルドのハンドラ/終了処理に .profraw を書かせて終了させる。"""
    if IS_WIN:
        subprocess.run(["taskkill", "/PID", str(proc.pid)], capture_output=True)
    else:
        proc.send_signal(signal.SIGTERM)  # 計装ビルドのハンドラが .profraw を書く
    try:
        proc.wait(timeout=EXIT_TIMEOUT_SEC)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise RuntimeError("計装ビルドが終了しなかった（.profraw が書かれていない可能性）")


def collect(binary, raw_dir):
    env = dict(os.environ, LLVM_PROFILE_FILE=os.path.join(raw_dir, "p-%p.profraw"))
    w = WireClient(ENDPOINT)
    if _USE_TCP:
        if endpoint_ready(w):
            raise RuntimeError(f"MCP {ENDPOINT} に既に応答がある（ix68k が動いている）。終了してから再実行")
    elif os.path.exists(ENDPOINT):
        os.unlink(ENDPOINT)
    cmd = [binary]
    for cfg in CONFIGS:
        cmd += ["-config", cfg]
    proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)
    try:
        for _ in range(SOCK_WAIT_TRIES):
            if endpoint_ready(w):
                break
            time.sleep(SOCK_WAIT_SEC)
        else:
            raise RuntimeError("MCP エンドポイントが現れなかった")
        w.ensure_protocol()
        with tempfile.TemporaryDirectory() as work_dir:
            prepare_bench(w, work_dir)
            run(w, "BENCH", BENCH_TIMEOUT_SEC)
    except BaseException:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        raise
    stop(proc)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary, output = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    profdata_cmd = find_profdata_cmd()
    with tempfile.TemporaryDirectory() as raw_dir:
        collect(binary, raw_dir)
        raws = glob.glob(os.path.join(raw_dir, "*.profraw"))
        if not raws:
            sys.exit("ERROR: .profraw が書き出されていない（計装ビルドか確認）")
        subprocess.run([*profdata_cmd, "merge", f"-output={output}", *raws], check=True)
    print(f"wrote {output} ({os.path.getsize(output)} bytes)")


if __name__ == "__main__":
    main()
