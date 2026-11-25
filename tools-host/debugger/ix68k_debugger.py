#!/usr/bin/env python3
"""ix68k external debugger（Issue #186）。

モデルとパーサはTk非依存に保つ。Tkビューはsocket I/Oをworker threadへ渡し、
widget更新をmain threadだけで行う。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
import argparse
import queue
import re
import sys
import threading
import time
import traceback
from typing import Iterable

_MCP_DIR = Path(__file__).resolve().parents[1] / "mcp"
if str(_MCP_DIR) not in sys.path:
    sys.path.insert(0, str(_MCP_DIR))

from ix68k_wire import (  # noqa: E402
    Request, Response, WireClient, WireError, WireTimeoutError, endpoint_from_environment,
)


class DebuggerState(str, Enum):
    OFF = "OFF"
    CONNECTING = "CONNECTING"
    FETCHING = "FETCHING"
    DISPLAYING = "DISPLAYING"
    RUNNING_POLL = "RUNNING_POLL"
    BREAK_POLL = "BREAK_POLL"


@dataclass(frozen=True)
class StopInfo:
    state: str
    fields: dict[str, str]

    @property
    def paused(self) -> bool:
        return self.state == "paused"

    @property
    def pc(self) -> int | None:
        try:
            return int(self.fields["PC"], 16)
        except (KeyError, ValueError):
            return None

    @property
    def cap(self) -> frozenset[str]:
        value = self.fields.get("CAP", "NONE")
        return frozenset() if value == "NONE" else frozenset(value.split(","))


@dataclass(frozen=True)
class RefreshPlan:
    generation: int
    requests: tuple[Request, ...]
    state: DebuggerState


@dataclass(frozen=True)
class DebuggerError:
    """ビューへ渡す通信失敗の詳細。例外型と本文を失わない。"""

    type_name: str
    message: str


@dataclass(frozen=True)
class CommandPlan:
    """状態変更コマンドの世代・一意token・wire要求を一組で渡す。"""

    generation: int
    token: int
    request: Request

    @property
    def command(self) -> str:
        return self.request.command


@dataclass(frozen=True)
class RingLine:
    raw: str
    pc: int | None = None
    kind: str | None = None


@dataclass(frozen=True)
class RingDump:
    raw: str
    lines: tuple[RingLine, ...]
    disabled: bool = False


VERSION = "1.0"
SNAPSHOT_SAVE = "SNAPSHOT SAVE"
SNAPSHOT_LOAD = "SNAPSHOT LOAD"
SNAPSHOT_STATUS = "SNAPSHOT STATUS"
# 通常の状態変更コマンドは0.5秒で打ち切る。SNAPSHOTは全メモリを複製するので長めにする。
UTILITY_TIMEOUT_S = 0.5
SNAPSHOT_TIMEOUT_S = 10.0
SNAPSHOT_LOAD_NOTICE = "SNAPSHOT LOAD: 復元しました（履歴リングは空になりました）"


def utility_timeout(command: str) -> float:
    """utility要求のtimeout秒。"""
    return SNAPSHOT_TIMEOUT_S if command.startswith("SNAPSHOT ") else UTILITY_TIMEOUT_S


def utility_notice(command: str, status: str) -> str:
    """utility応答を通知欄へ出す文字列にする。LOADの長いstopinfoは要約する。"""
    if command == SNAPSHOT_LOAD and status.startswith("OK STATE="):
        return SNAPSHOT_LOAD_NOTICE
    return f"{command}: {status}"


def parse_stopinfo(status: str) -> StopInfo:
    """``OK STATE=...`` の status を、未知フィールドを失わず解析する。"""
    fields = parse_fields(status)
    return StopInfo(fields.get("STATE", "unknown"), fields)


def parse_fields(status: str) -> dict[str, str]:
    """``OK K=V K=V ...`` の status を、出現順を保った辞書にする。``=`` の無い語は捨てる。"""
    tokens = status[3:].split() if status.startswith("OK ") else status.split()
    return dict(token.split("=", 1) for token in tokens if "=" in token)


# Registers / stopinfo ペインの表示。1行を左ペインに収まる幅に抑える。
REGISTER_COLUMN_WIDTH = 15
# この桁数以下の項目だけ2列に並べる。超える項目（STOP=BREAKPOINT、68030のCRP等）は1行で出す。
REGISTER_SHORT_ITEM = REGISTER_COLUMN_WIDTH - 2
# Traceペインのタブ。最後のRawは生テキスト（リング行のクリック追従なし）。
TRACE_RAW_TAB = "Raw"
TRACE_TABS = ("CALLRING", "PCRING", "REGRING", TRACE_RAW_TAB)
BREAKPOINT_TABS = ("BP", "WP")
# 整形後は18行。ペインの初期表示行数（足りなければ縦スクロール）。
REGISTER_PANE_LINES = 18
# D0〜D7 / A0〜A7 は横に並べる個数。1行は 11桁×4+区切り3 = 47桁。
REGISTERS_PER_ROW = 4
REGISTER_ROW_SEPARATOR = " "
REGISTER_DATA = tuple(f"D{n}" for n in range(8))
REGISTER_ADDRESS = tuple(f"A{n}" for n in range(8))
REGISTER_NAMES = frozenset(("PC", "SR", "USP", "SSP", *REGISTER_DATA, *REGISTER_ADDRESS))
# 実行状態の2行目に出す、停止理由以外の動作モード系フィールド。
MODE_FIELDS = ("MODE", "CAP", "EXEC", "WB")
# MMU GETの既知フィールドを、読みやすい組にして並べる（機種で出るフィールドが違う）。
MMU_GROUPS = (("CRP",), ("URP", "SRP"), ("TC",), ("DTT0", "DTT1"), ("ITT0", "ITT1"),
              ("TT0", "TT1"), ("MMUSR", "ACUSR"), ("AC0", "AC1"))
SR_TRACE_SHIFT, SR_TRACE_MASK = 14, 0b11
SR_SUPERVISOR_SHIFT = 13
SR_MASTER_SHIFT = 12
SR_IPL_SHIFT, SR_IPL_MASK = 8, 0b111
SR_CCR_BITS = ("X", "N", "Z", "V", "C")  # 上位ビットから順（bit4..bit0）


def _is_ok(response: Response | None) -> bool:
    return response is not None and response.status.startswith("OK")


def _pairs(items: list[str]) -> list[str]:
    """``K=V`` を2列に並べる。左列は固定幅にして列を揃える。どちらかが長い項目は1行で出す。"""
    lines = []
    index = 0
    while index < len(items):
        left = items[index]
        right = items[index + 1] if index + 1 < len(items) else None
        if right is not None and len(left) <= REGISTER_SHORT_ITEM and len(right) <= REGISTER_SHORT_ITEM:
            lines.append(left.ljust(REGISTER_COLUMN_WIDTH) + right)
            index += 2
        else:
            lines.append(left)
            index += 1
    return lines


def _rows(items: list[str], per_row: int) -> list[str]:
    """``K=V`` を per_row 個ずつ横に並べる（D0..D3 / D4..D7 の形）。"""
    return [REGISTER_ROW_SEPARATOR.join(items[index:index + per_row]) for index in range(0, len(items), per_row)]


def _field_items(fields: dict[str, str], names: Iterable[str]) -> list[str]:
    return [f"{name}={fields[name]}" for name in names if name in fields]


def format_sr_flags(value: str) -> list[str]:
    """SR（16進）をトレース・特権・マスタ・割り込みレベル・CCRに分解する。読めなければ空。"""
    try:
        sr = int(value, 16)
    except ValueError:
        return []
    trace = (sr >> SR_TRACE_SHIFT) & SR_TRACE_MASK
    supervisor = (sr >> SR_SUPERVISOR_SHIFT) & 1
    master = (sr >> SR_MASTER_SHIFT) & 1
    ipl = (sr >> SR_IPL_SHIFT) & SR_IPL_MASK
    ccr = "".join(str((sr >> bit) & 1) for bit in range(len(SR_CCR_BITS) - 1, -1, -1))
    return [f"  T={trace:02b} S={supervisor} M={master} IPL={ipl}", f"  {''.join(SR_CCR_BITS)}={ccr}"]


def format_registers_view(status: Response | None, registers: Response | None,
                          mmu: Response | None) -> str:
    """DBG STATUS / REGS / MMU GET の応答を、縦に読める複数行へ整形する。

    未知のフィールドは落とさず末尾へ ``K=V`` で出す。``ERR`` 等の応答は生の1行で出す。
    """
    lines: list[str] = []
    status_fields = parse_fields(status.status) if _is_ok(status) else {}
    if status is not None and not _is_ok(status):
        lines.append(status.status)
    headline = [f"{key}={value}" for key, value in status_fields.items()
                if key not in REGISTER_NAMES and key not in MODE_FIELDS]
    lines.extend(_pairs(headline))
    lines.extend(_pairs(_field_items(status_fields, MODE_FIELDS)))

    # 停止中のDBG STATUSもレジスタを含む。REGSがあればそれを優先する。
    register_fields = parse_fields(registers.status) if _is_ok(registers) else {
        key: value for key, value in status_fields.items() if key in REGISTER_NAMES}
    if registers is not None and not _is_ok(registers):
        lines.append(registers.status)
    if register_fields:
        if lines:
            lines.append("")
        lines.extend(_pairs(_field_items(register_fields, ("PC", "SR"))))
        if "SR" in register_fields:
            lines.extend(format_sr_flags(register_fields["SR"]))
        lines.extend(_rows(_field_items(register_fields, REGISTER_DATA), REGISTERS_PER_ROW))
        lines.extend(_rows(_field_items(register_fields, REGISTER_ADDRESS), REGISTERS_PER_ROW))
        lines.extend(_pairs(_field_items(register_fields, ("USP", "SSP"))))
        extra = [f"{key}={value}" for key, value in register_fields.items() if key not in REGISTER_NAMES]
        lines.extend(_pairs(extra))

    if mmu is not None:
        if lines:
            lines.append("")
        if _is_ok(mmu):
            mmu_fields = parse_fields(mmu.status)
            model = mmu_fields.pop("MODEL", None)
            lines.append(f"MMU  MODEL={model}" if model is not None else "MMU")
            for group in MMU_GROUPS:
                lines.extend(_pairs(_field_items(mmu_fields, group)))
            known = {name for group in MMU_GROUPS for name in group}
            lines.extend(_pairs([f"{key}={value}" for key, value in mmu_fields.items() if key not in known]))
        else:
            lines.append(f"MMU  {mmu.status}")
    return "\n".join(lines)


def parse_ring(text: str) -> RingDump:
    """core の出力を壊さず、クリック可能と断定できるPCだけを抽出する。"""
    disabled = text.startswith("PCRING disabled;")
    result: list[RingLine] = []
    for raw in text.splitlines():
        stripped = raw.strip()
        pc: int | None = None
        kind: str | None = None
        # PCRING: 一行には空白区切りの6〜8桁16進PCが最大16個ある（24bitを超えると8桁）。
        if stripped and all(_is_address_word(word) for word in stripped.split()):
            result.extend(RingLine(raw, int(word, 16), "PCRING") for word in stripped.split())
            continue
        # 途中で切れたREG/CALL行をクリック可能に見せないため、行全体を検証する。
        reg = re.fullmatch(
            r"pc=([0-9A-Fa-f]{6,8}) op=[0-9A-Fa-f]{4} sr=[0-9A-Fa-f]{4} "
            r"D=(?:[0-9A-Fa-f]{8} ){7}[0-9A-Fa-f]{8} "
            r"A=(?:[0-9A-Fa-f]{8} ){7}[0-9A-Fa-f]{8}",
            stripped,
        )
        call = re.fullmatch(r"(?:CALL|RET)\s+([0-9A-Fa-f]{6,8}) -> [0-9A-Fa-f]{6,8}", stripped)
        if reg:
            pc, kind = int(reg.group(1), 16), "REGRING"
        elif call:
            pc, kind = int(call.group(1), 16), "CALLRING"
        result.append(RingLine(raw, pc, kind))
    return RingDump(text, tuple(result), disabled)


def _is_hex(value: str) -> bool:
    return all("0" <= char <= "9" or "A" <= char <= "F" or "a" <= char <= "f" for char in value)


ADDRESS_BITS_24 = 24
ADDRESS_BITS_32 = 32
ADDRESS_DIGITS = {ADDRESS_BITS_24: 6, ADDRESS_BITS_32: 8}
# ringの行は、本体が出す値を読むだけなので表示設定によらず6〜8桁を受け付ける。
RING_ADDRESS_DIGITS = range(ADDRESS_DIGITS[ADDRESS_BITS_24], ADDRESS_DIGITS[ADDRESS_BITS_32] + 1)
# 24bitアドレスバスのCPU。MMU GETのMODEL=がこれ以外なら32bitを初期値にする。
MODELS_24BIT = frozenset({"68000", "68010"})
MMU_GET_COMMAND = "MMU GET"
MEM_PEEK_SUPER = "SUPER"


def _is_address_word(word: str) -> bool:
    return len(word) in RING_ADDRESS_DIGITS and _is_hex(word)


def address_mask(bits: int) -> int:
    return (1 << bits) - 1


def format_address(value: int, bits: int) -> str:
    """表示用アドレス。24bitなら6桁、32bitなら8桁に揃える。"""
    return f"{value & address_mask(bits):0{ADDRESS_DIGITS[bits]}X}"


def address_bits_from_mmu_reply(status: str) -> int | None:
    """``MMU GET`` 応答の ``MODEL=`` からアドレス幅を決める。判定できなければNone。"""
    match = re.match(r"OK\s+MODEL=(\w+)", status)
    if not match:
        return None
    return ADDRESS_BITS_24 if match.group(1) in MODELS_24BIT else ADDRESS_BITS_32


@dataclass
class DebuggerModel:
    """通信結果を世代単位で反映する、GUI非依存の状態機械。"""

    state: DebuggerState = DebuggerState.OFF
    refresh_enabled: bool = False
    generation: int = 0
    request_count: int = 0
    recent_request_count: int = 0
    memory_base: int = 0
    # 表示と入力のアドレス幅。接続先CPUで初期値を決め、利用者が選んだら以後は上書きしない。
    address_bits: int = ADDRESS_BITS_24
    address_bits_chosen: bool = False
    # MEM PEEKをスーパーバイザ空間で読む（ユーザーモード中でもカーネルを見る）。
    memory_super: bool = False
    cpu_probed: bool = False
    retry_ms: int = 0
    last_pc: int | None = None
    stopinfo: StopInfo | None = None
    panes: dict[str, Response] = field(default_factory=dict)
    error: DebuggerError | None = None
    break_pending: bool = False
    break_deadline_ms: int | None = None
    command_token: int = 0
    pending_command_token: int | None = None
    # 更新中(FETCHING)に戻るべき確定状態。更新中もボタン可否・操作受付をこの状態で判定する。
    # begin_refresh()が更新前の状態を、step()がDISPLAYING（Step後は停止中）を入れる。
    fetch_origin: DebuggerState = DebuggerState.OFF
    # 実行中はBP/WP LISTを取らないが、BP/WP操作や調査終了の直後は1回だけ取り直す。
    lists_stale: bool = False

    def enable_refresh(self) -> RefreshPlan:
        self.refresh_enabled = True
        return self.begin_refresh()

    def disable_refresh(self) -> None:
        # worker がすでに受け取った plan / command の完了結果も、この世代更新で
        # 無効になる。OFF後に通信や遅延応答が表示状態を復活させてはならない。
        self.generation += 1
        self.refresh_enabled = False
        self.state = DebuggerState.OFF
        self.retry_ms = 0
        self.break_pending = False
        self.break_deadline_ms = None
        self.pending_command_token = None

    def next_poll_delay_ms(self) -> int | None:
        """ビューが ``after()`` に渡す次回更新間隔。OFFでは予約しない。"""
        if not self.refresh_enabled:
            return None
        # Break は5秒の間、通信のbusy backoffとは独立して100msごとに状態を
        # 確認する。これにより即時応答後も停止到達を速やかに観測できる。
        if self.break_pending:
            return 100
        if self.retry_ms:
            return self.retry_ms
        return 250

    def reset_recent_request_count(self) -> None:
        """表示用の直近1秒カウンタをビューの1秒タイマでロールする。"""
        self.recent_request_count = 0

    def begin_refresh(self) -> RefreshPlan:
        if not self.refresh_enabled:
            return RefreshPlan(self.generation, (), self.state)
        previous_state = self.state
        if previous_state != DebuggerState.FETCHING:
            self.fetch_origin = previous_state
        # 世代が進むと保留中コマンドの応答は受理できないので、保留も解く（解かないと
        # 自動更新が保留待ちのまま止まる）。
        self.pending_command_token = None
        self.generation += 1
        self.state = DebuggerState.CONNECTING
        if self.state_before_fetch_is_running(previous_state):
            requests = (Request("DBG STATUS"),)
            if self.lists_stale:
                requests += (Request("BP LIST"), Request("WP LIST"))
        else:
            requests = self._paused_requests()
        if not self.cpu_probed and all(item.command != MMU_GET_COMMAND for item in requests):
            requests += (Request(MMU_GET_COMMAND),)
        self.state = DebuggerState.FETCHING
        self.request_count += len(requests)
        self.recent_request_count += len(requests)
        return RefreshPlan(self.generation, requests, self.state)

    def action_state(self) -> DebuggerState:
        """Run/Step/Breakの可否判定に使う状態。コマンド未保留の更新中は更新前の状態を返す。"""
        if self.state == DebuggerState.FETCHING and self.pending_command_token is None:
            return self.fetch_origin
        return self.state

    def can_run(self) -> bool:
        return self.refresh_enabled and self.action_state() == DebuggerState.DISPLAYING and self._is_paused()

    def can_break(self) -> bool:
        # Run直後のDBG CONT応答待ちの間は受け付けない（世代が進んでCONTが捨てられるため）。
        return (self.refresh_enabled and self.pending_command_token is None
                and self.action_state() == DebuggerState.RUNNING_POLL)

    def request_lists_once(self) -> None:
        """次の更新でBP/WP LISTを1回だけ取る（実行中にBP/WPを変えた後の表示ずれ防止）。"""
        self.lists_stale = True

    def poll_allowed(self) -> bool:
        """自動更新を今始めてよいか。コマンド応答待ちの間は始めない（世代が進んで応答が捨てられる）。"""
        return self.pending_command_token is None

    def _is_paused(self) -> bool:
        return bool(self.stopinfo and self.stopinfo.paused)

    def _plan_is_current(self, plan: RefreshPlan) -> bool:
        return self.refresh_enabled and plan.generation == self.generation

    def state_before_fetch_is_running(self, previous_state: DebuggerState) -> bool:
        return previous_state in {DebuggerState.RUNNING_POLL, DebuggerState.BREAK_POLL} or not (
            self.stopinfo and self.stopinfo.paused
        )

    def _paused_requests(self) -> tuple[Request, ...]:
        pc = self.last_pc if self.last_pc is not None else 0
        requests = [
            Request("DBG STATUS"), Request("REGS"), Request(f"DISASM {pc:06X} 32"),
            Request(self.peek_command(self.memory_base)), Request("BP LIST"), Request("WP LIST"),
            Request("RING CALLRING 200"), Request(MMU_GET_COMMAND),
        ]
        cap = self.stopinfo.cap if self.stopinfo else frozenset()
        if "PC" in cap:
            requests.append(Request("RING PCRING 200"))
        if "REG" in cap:
            requests.append(Request("RING REGRING 200"))
        return tuple(requests)

    def complete_refresh(self, generation: int, responses: Iterable[Response]) -> bool:
        """同世代だけを反映する。古い worker の完了結果は安全に捨てる。"""
        if generation != self.generation or not self.refresh_enabled:
            return False
        values = tuple(responses)
        status = next(
            (item for item in values if item.request and item.request.command == "DBG STATUS"),
            next((item for item in values if "STATE=" in item.status), None),
        )
        if status is not None:
            self.stopinfo = parse_stopinfo(status.status)
            if self.stopinfo.pc is not None:
                self.last_pc = self.stopinfo.pc
            if self.stopinfo.paused:
                self.break_pending = False
                self.break_deadline_ms = None
                self.state = DebuggerState.DISPLAYING
            else:
                self.state = DebuggerState.BREAK_POLL if self.break_pending else DebuggerState.RUNNING_POLL
        self.panes = {_pane_name(item): item for item in values}
        self._apply_cpu_probe(self.panes.get("mmu"))
        # 一覧を実際に受け取れた時だけ「取り直し待ち」を解く（timeoutや世代破棄では残す）。
        if "breakpoints" in self.panes and "watchpoints" in self.panes:
            self.lists_stale = False
        self.retry_ms = 0
        self.error = None
        return True

    def _apply_cpu_probe(self, response: Response | None) -> None:
        """MMU GETの応答でアドレス幅の初期値を決める。応答を得たら再問い合わせしない。"""
        if response is None:
            return
        self.cpu_probed = True
        bits = address_bits_from_mmu_reply(response.status)
        if bits is not None and not self.address_bits_chosen:
            self.address_bits = bits

    def peek_command(self, address: int) -> str:
        return f"MEM PEEK {address:06X} 100" + (f" {MEM_PEEK_SUPER}" if self.memory_super else "")

    def set_address_bits(self, bits: int) -> None:
        """利用者の選択。以後はCPU判定で上書きしない。"""
        self.address_bits = bits
        self.address_bits_chosen = True

    def execute_refresh(self, client: object, plan: RefreshPlan, timeout: float = 0.5) -> bool:
        """worker が呼ぶ一世代のwire処理。

        ``WireClient.pipeline()`` は接続、HELLO、tag照合、切断を一括して担当する。
        このモデルは通信完了時に限り同一世代の表示状態を更新する。
        """
        # disable_refresh() や次世代開始との競合を、送信前と完了時の二重検査で
        # 防ぐ。少なくとも無効化済み plan を新規接続へ送らない。
        if not plan.requests or not self._plan_is_current(plan):
            return False
        try:
            responses = client.pipeline(plan.requests, timeout=timeout).values()
        except WireError as error:
            return self.transport_error(plan.generation, error)
        return self.complete_refresh(plan.generation, responses)

    def transport_error(self, generation: int, error: Exception) -> bool:
        if generation != self.generation or not self.refresh_enabled:
            return False
        # 現世代の通信が失敗したので、コマンド応答待ちはここで終わる（更新保留を解く）。
        self.pending_command_token = None
        if isinstance(error, WireTimeoutError):
            # §2の busy 表示・再試行契約。Refresh ONを維持し、次の更新を待つ。
            self.retry_ms = _next_backoff(self.retry_ms)
            if self.break_pending: self.state = DebuggerState.BREAK_POLL
            elif self._is_paused() and self.fetch_origin not in {DebuggerState.RUNNING_POLL, DebuggerState.BREAK_POLL}: self.state = DebuggerState.DISPLAYING
            else: self.state = DebuggerState.RUNNING_POLL
        else:
            self.retry_ms = 0
            self.state = DebuggerState.OFF
            # connect/EOF/protocol異常は自動更新を止め、手動Refreshでのみ再接続する。
            self.refresh_enabled = False
            self.break_pending = False
            self.break_deadline_ms = None
        self.error = DebuggerError(type(error).__name__, str(error))
        return True

    def complete_command(self, response: Response, plan: CommandPlan) -> StopInfo | None:
        """Run/Break/Step の即時応答を反映する（次の更新はビューが予約する）。"""
        if plan.token != self.pending_command_token:
            return None
        # 世代が進んで応答を反映できない場合も、このコマンドの保留は終わらせる。
        self.pending_command_token = None
        if not self.refresh_enabled or plan.generation != self.generation:
            return None
        if "STATE=" not in response.status:
            return None
        self.stopinfo = parse_stopinfo(response.status)
        if self.stopinfo.pc is not None:
            self.last_pc = self.stopinfo.pc
        if self.stopinfo.paused:
            self.break_pending = False
            self.break_deadline_ms = None
            self.state = DebuggerState.DISPLAYING
        else:
            self.state = DebuggerState.BREAK_POLL if self.break_pending else DebuggerState.RUNNING_POLL
        return self.stopinfo

    def _command_plan(self, command: str) -> CommandPlan:
        self.command_token += 1
        self.pending_command_token = self.command_token
        # 更新中の古い結果が、コマンド後の状態を巻き戻さないよう捨てさせる。
        self.generation += 1
        return CommandPlan(self.generation, self.command_token, Request(command))

    def run(self) -> CommandPlan | None:
        if self.action_state() != DebuggerState.DISPLAYING:
            return None
        self.state = DebuggerState.RUNNING_POLL
        self.request_count += 1
        self.recent_request_count += 1
        return self._command_plan("DBG CONT")

    def step(self, count: int = 1) -> CommandPlan | None:
        if self.action_state() != DebuggerState.DISPLAYING or count not in {1, 10}:
            return None
        self.request_count += 1
        self.recent_request_count += 1
        # 応答を受けるまでStepの連打を受け付けない。
        self.fetch_origin = DebuggerState.DISPLAYING
        self.state = DebuggerState.FETCHING
        return self._command_plan(f"DBG STEP {count}")

    def break_execution(self, now_ms: int) -> CommandPlan | None:
        if not self.can_break():
            return None
        self.state = DebuggerState.BREAK_POLL
        self.break_pending = True
        self.break_deadline_ms = now_ms + 5000
        self.request_count += 1
        self.recent_request_count += 1
        return self._command_plan("DBG BREAK")

    def break_expired(self, now_ms: int) -> bool:
        if self.break_pending and self.break_deadline_ms is not None and now_ms >= self.break_deadline_ms:
            self.break_pending = False
            self.break_deadline_ms = None
            self.state = DebuggerState.RUNNING_POLL
            return True
        return False


def _next_backoff(previous: int) -> int:
    if previous == 0:
        return 100
    if previous == 100:
        return 250
    if previous == 250:
        return 500
    return 1000


def _pane_name(response: Response) -> str:
    """応答tagではなく、送った要求でビューの表示先を決める。"""
    command = response.request.command if response.request else ""
    if command == "DBG STATUS":
        return "status"
    if command == MMU_GET_COMMAND:
        return "mmu"
    if command == "REGS":
        return "registers"
    if command.startswith("DISASM "):
        return "disassembly"
    if command.startswith("MEM PEEK "):
        return "memory"
    if command == "BP LIST":
        return "breakpoints"
    if command == "WP LIST":
        return "watchpoints"
    if command.startswith("RING "):
        return "ring_" + command.split()[1].lower()
    return response.tag


def parse_address(value: str, bits: int = ADDRESS_BITS_24) -> int:
    """``$``/``0x``/裸16進のGUI入力を、``bits`` で指定した幅のアドレスへ正規化する。"""
    text = value.strip()
    if text.startswith("$"):
        text = text[1:]
    elif text.lower().startswith("0x"):
        text = text[2:]
    if not text or not _is_hex(text):
        raise ValueError("アドレスは16進数で入力してください")
    address = int(text, 16)
    if address > address_mask(bits):
        raise ValueError(f"アドレスは{bits}bitの範囲で入力してください")
    return address


def format_memory(data: bytes, base: int, bits: int = ADDRESS_BITS_24) -> str:
    """16 byte/line のhex + ASCII表示を作る。"""
    rows = []
    for offset in range(0, len(data), 16):
        row = data[offset:offset + 16]
        hex_part = " ".join(f"{item:02X}" for item in row).ljust(47)
        ascii_part = "".join(chr(item) if 32 <= item < 127 else "." for item in row)
        rows.append(f"{format_address(base + offset, bits)}  {hex_part}  |{ascii_part}|")
    return "\n".join(rows)


def parse_list_records(status: str) -> tuple[int | None, list[dict[str, str]]]:
    """BP/WP LISTの宣言件数とrecordを表示用に分ける。"""
    match = re.match(r"OK\s+(\d+)(?:\s+(.*))?$", status)
    if not match:
        return None, []
    records = []
    for record in (match.group(2) or "").split(";"):
        fields = dict(item.split("=", 1) for item in record.split(",") if "=" in item)
        if fields:
            records.append(fields)
    return int(match.group(1)), records


def memory_base_from_response(response: Response, fallback_command: str = "") -> int | None:
    """応答に対応する要求からMEM PEEK/READの基準アドレスを取り出す。"""
    command = response.request.command if response.request else fallback_command
    parts = command.split()
    peek_with_super = len(parts) == 5 and parts[:2] == ["MEM", "PEEK"] and parts[4] == MEM_PEEK_SUPER
    if (len(parts) == 4 or peek_with_super) and parts[:2] in (["MEM", "PEEK"], ["MEM", "READ"]):
        try:
            return int(parts[2], 16)
        except ValueError:
            return None
    return None


class DebuggerApplication:
    """Phase 3bのTkビュー。workerは結果だけをqueueへ置く。"""

    _REFRESH_DEFERRED_NOTICE = "コマンド応答待ちのため、更新は応答後に行います"
    _CLEANUP_COMMANDS = (
        "BP CLEAR", "WP CLEAR", "EXC SET 0", "WATCH OFF", "DIAG PCRING OFF",
        "DIAG REGRING OFF", "DIAG BADORI OFF", "WHITEBAND OFF", "DBG MODE AUTO",
    )

    def __init__(self, root: object, endpoint: str | tuple[str, int] | None = None) -> None:
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk, self.root = tk, ttk, root
        self.model = DebuggerModel()
        self.client = WireClient(endpoint if endpoint is not None else endpoint_from_environment()[0])
        # 操作ボタンと自動更新は別workerから発火し得る。WireClientのtag/PROTO
        # キャッシュを競合させず、ix68kの単一クライアント契約にも従う。
        self._client_lock = threading.Lock()
        self.events: queue.Queue[tuple[str, object, object]] = queue.Queue()
        self._poll_after: str | None = None
        self._closed = False
        self._cancel_epoch = 0
        self._frozen: dict[str, str] | None = None
        self._panes_current = False
        # 停止位置の自動選択を済ませた停止。更新のたびに利用者の選択を上書きしないため。
        self._stop_selected_key: tuple | None = None
        self.selected_memory_address: int | None = None
        self.memory_bytes = b""
        self._displayed_memory_base = 0
        self.address_var = tk.StringVar(value=format_address(0, ADDRESS_BITS_24))
        self.address_bits_var = tk.IntVar(value=ADDRESS_BITS_24)
        self.status_var = tk.StringVar(value="未接続 / Refresh: OFF")
        self.notice_var = tk.StringVar(value="")
        self.performance_var = tk.StringVar(value="CPU debug mode: —    Capture: —")
        self.counter_var = tk.StringVar(value="Requests: 0 total, 0 / sec")
        self._diag_vars: dict[str, object] = {}
        self._build()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(25, self._drain_events)
        root.after(1000, self._roll_counter)

    def _build(self) -> None:
        ttk = self.ttk
        self.root.title(f"ix68k-debugger {VERSION}")
        self.root.minsize(980, 680)
        bar = ttk.Frame(self.root, padding=6); bar.pack(fill="x")
        self.refresh_button = ttk.Button(bar, text="Refresh ON", command=self.manual_refresh); self.refresh_button.pack(side="left")
        ttk.Button(bar, text="Refresh OFF", command=self.disable_refresh).pack(side="left", padx=(4, 14))
        self.run_button = ttk.Button(bar, text="Run (F5)", command=self.run)
        self.break_button = ttk.Button(bar, text="Break (Shift+F5)", command=self.break_execution)
        self.step_button = ttk.Button(bar, text="Step (F10)", command=lambda: self.step(1))
        self.step10_button = ttk.Button(bar, text="Step ×10 (F11)", command=lambda: self.step(10))
        for button in (self.run_button, self.break_button, self.step_button, self.step10_button): button.pack(side="left", padx=2)
        # Save/Loadは停止中だけ有効（サーバも停止中以外はERR NOTREADYで拒否する）。Statusは実行中も照会できる。
        self.snapshot_save_button = ttk.Button(bar, text="Snapshot Save", command=self.snapshot_save)
        self.snapshot_load_button = ttk.Button(bar, text="Snapshot Load", command=self.snapshot_load)
        self.snapshot_status_button = ttk.Button(bar, text="Snapshot Status", command=self.snapshot_status)
        self.snapshot_save_button.pack(side="left", padx=(14, 2)); self.snapshot_load_button.pack(side="left", padx=2); self.snapshot_status_button.pack(side="left", padx=2)
        ttk.Button(bar, text="調査終了", command=self.cleanup).pack(side="right")
        info = ttk.Frame(self.root, padding=(8, 0, 8, 4)); info.pack(fill="x")
        for variable in (self.status_var, self.notice_var, self.performance_var, self.counter_var): ttk.Label(info, textvariable=variable).pack(anchor="w")
        main = ttk.Panedwindow(self.root, orient="horizontal"); main.pack(fill="both", expand=True, padx=6, pady=4)
        left = ttk.Panedwindow(main, orient="vertical"); right = ttk.Panedwindow(main, orient="vertical")
        main.add(left, weight=1); main.add(right, weight=2)
        left.add(self._register_pane(), weight=1); left.add(self._breakpoint_pane(), weight=1); left.add(self._diagnostics_pane(), weight=1)
        right.add(self._disassembly_pane(), weight=1); right.add(self._memory_pane(), weight=1); right.add(self._trace_pane(), weight=2)
        for sequence, action in (("<F5>", self.run), ("<Shift-F5>", self.break_execution), ("<F10>", lambda: self.step(1)), ("<F11>", lambda: self.step(10)), ("<Control-b>", self.add_breakpoint), ("<Control-g>", self.goto_address), ("<Control-l>", self.goto_pc)):
            self.root.bind(sequence, lambda event, callback=action: callback())
        # 削除キーは一覧にフォーカスがある時だけ効かせる（アドレス入力欄の編集と衝突させない）。
        # macOSのdeleteキーはBackSpaceを送る。
        for tree, action in ((self.bp_tree, self.delete_breakpoint), (self.wp_tree, self.delete_watchpoint)):
            for sequence in ("<Delete>", "<BackSpace>"): tree.bind(sequence, lambda event, callback=action: callback())
        self.root.bind("<Unmap>", self._on_unmap)
        self._refresh_widgets()

    def _text(self, parent: object, height: int, wrap: str = "none") -> object:
        text = self.tk.Text(parent, height=height, wrap=wrap, font="TkFixedFont")
        scroll = self.ttk.Scrollbar(parent, orient="vertical", command=text.yview)
        text.configure(yscrollcommand=scroll.set, state="disabled"); text.pack(side="left", fill="both", expand=True); scroll.pack(side="right", fill="y")
        return text

    def _register_pane(self) -> object:
        frame = self.ttk.Labelframe(self.root, text="Registers / stopinfo", padding=4); self.register_text = self._text(frame, REGISTER_PANE_LINES, wrap="word"); return frame

    def _breakpoint_pane(self) -> object:
        frame = self.ttk.Labelframe(self.root, text="Breakpoints / Watchpoints", padding=4)
        # Traceと同じ理由（macOSのNotebookは隣のタブの枠が重なる）で、ボタン行と重ねたページで作る。
        tabs = self.ttk.Frame(frame); tabs.pack(fill="x", pady=(0, 2))
        body = self.ttk.Frame(frame); body.pack(fill="both", expand=True)
        body.grid_rowconfigure(0, weight=1); body.grid_columnconfigure(0, weight=1)
        self.breakpoint_tab_var = self.tk.StringVar(value=BREAKPOINT_TABS[0]); self.breakpoint_pages = {}; trees = {}
        columns = {"BP": ("id", "addr", "en", "hits", "pass", "cond"), "WP": ("id", "addr", "size", "en", "hits", "last")}
        for name in BREAKPOINT_TABS:
            page = self.ttk.Frame(body); page.grid(row=0, column=0, sticky="nsew"); self.breakpoint_pages[name] = page
            self.ttk.Radiobutton(tabs, text=name, value=name, variable=self.breakpoint_tab_var, style="Toolbutton", command=lambda key=name: self.show_breakpoint_tab(key)).pack(side="left", padx=2)
            trees[name] = self._record_tree(page, columns[name])
        self.bp_tree, self.wp_tree = trees["BP"], trees["WP"]
        self.show_breakpoint_tab(BREAKPOINT_TABS[0])
        actions = self.ttk.Frame(frame); actions.pack(fill="x", pady=(4, 0))
        for label, command in (("Add BP", self.add_breakpoint), ("Delete BP", self.delete_breakpoint), ("Toggle BP", self.toggle_breakpoint), ("Add WP", self.add_watchpoint), ("Delete WP", self.delete_watchpoint)):
            self.ttk.Button(actions, text=label, command=command).pack(side="left", padx=2)
        return frame

    def _record_tree(self, parent: object, columns: tuple[str, ...]) -> object:
        tree = self.ttk.Treeview(parent, columns=columns, show="headings", height=5)
        for column in columns: tree.heading(column, text=column); tree.column(column, width=180 if column == "cond" else 65, stretch=column == "cond")
        tree.pack(fill="both", expand=True); return tree

    def _diagnostics_pane(self) -> object:
        frame = self.ttk.Labelframe(self.root, text="Diagnostics", padding=4)
        for kind in ("BADJUMP", "BADMOVEA", "BADADDR", "BADORI", "PCRING", "REGRING"):
            variable = self.tk.BooleanVar(value=False); self._diag_vars[kind] = variable
            self.ttk.Checkbutton(frame, text=kind, variable=variable, command=lambda key=kind: self.set_diagnostic(key)).pack(anchor="w")
        return frame

    def _disassembly_pane(self) -> object:
        frame = self.ttk.Labelframe(self.root, text="Disassembly (PC forward, 32 instructions)", padding=4); self.disasm_text = self._text(frame, 10); return frame

    def _memory_pane(self) -> object:
        frame = self.ttk.Labelframe(self.root, text="Memory (non-invasive PEEK)", padding=4)
        # 全部を1段に並べると、最小ウィンドウ幅（右ペイン約450px）で右端が見切れる（SUPERは消える）。
        # 1段目はアドレスと読み取りの設定、2段目は移動ボタンに分け、どちらも狭いペインに収める。
        controls = self.ttk.Frame(frame); controls.pack(fill="x")
        self.ttk.Entry(controls, textvariable=self.address_var, width=10).pack(side="left")
        for bits in (ADDRESS_BITS_24, ADDRESS_BITS_32): self.ttk.Radiobutton(controls, text=f"{bits}bit", value=bits, variable=self.address_bits_var, command=self.choose_address_bits).pack(side="left", padx=(4, 0))
        self.memory_super_var = self.tk.BooleanVar(value=False)
        self.ttk.Checkbutton(controls, text="SUPER", variable=self.memory_super_var, command=self.choose_memory_super).pack(side="left", padx=(8, 0))
        self.ttk.Button(controls, text="Read once", command=self.read_memory_once).pack(side="left", padx=(8, 0))
        jumps = self.ttk.Frame(frame); jumps.pack(fill="x", pady=(2, 0))
        for label, command in (("Go (Ctrl+G)", self.goto_address), ("PC", self.goto_pc), ("A7", lambda: self.goto_register("A7")), ("USP", lambda: self.goto_register("USP")), ("SSP", lambda: self.goto_register("SSP"))): self.ttk.Button(jumps, text=label, command=command).pack(side="left", padx=(0, 4))
        self.memory_text = self._text(frame, 10)
        self.memory_text.bind("<Button-1>", self.select_memory)
        self.memory_value_var = self.tk.StringVar(value="")
        self.ttk.Label(frame, textvariable=self.memory_value_var).pack(anchor="w")
        return frame

    def _trace_pane(self) -> object:
        frame = self.ttk.Labelframe(self.root, text="Trace", padding=4); actions = self.ttk.Frame(frame); actions.pack(fill="x")
        self.ttk.Button(actions, text="Freeze trace", command=self.freeze_trace).pack(side="left"); self.ttk.Button(actions, text="Export trace", command=self.export_trace).pack(side="left", padx=4)
        # macOSのNotebookはタブを1つの部品として描くため、隣のタブの枠が重なって見える。
        # 余白を足しても直らないので、独立したボタン行と、重ねて切り替えるページで作る。
        tabs = self.ttk.Frame(frame); tabs.pack(fill="x", pady=(4, 2))
        body = self.ttk.Frame(frame); body.pack(fill="both", expand=True)
        body.grid_rowconfigure(0, weight=1); body.grid_columnconfigure(0, weight=1)
        self.trace_tab_var = self.tk.StringVar(value=TRACE_TABS[0]); self.trace_pages = {}; self.trace_texts = {}
        for name in TRACE_TABS:
            page = self.ttk.Frame(body); page.grid(row=0, column=0, sticky="nsew"); self.trace_pages[name] = page
            self.ttk.Radiobutton(tabs, text=name, value=name, variable=self.trace_tab_var, style="Toolbutton", command=lambda key=name: self.show_trace_tab(key)).pack(side="left", padx=2)
            text = self._text(page, 10)
            if name == TRACE_RAW_TAB: self.raw_trace_text = text
            else: text.bind("<Button-1>", lambda event, key=name: self.follow_ring_pc(key, event)); self.trace_texts[name] = text
        self.show_trace_tab(TRACE_TABS[0])
        return frame

    def show_breakpoint_tab(self, name: str) -> None:
        """BP/WPのタブを切り替える。選ばれたページを最前面へ出す。"""
        self.breakpoint_tab_var.set(name); self.breakpoint_pages[name].tkraise()

    def show_trace_tab(self, name: str) -> None:
        """Traceのタブ（CALLRING/PCRING/REGRING/Raw）を切り替える。選ばれたページを最前面へ出す。"""
        self.trace_tab_var.set(name); self.trace_pages[name].tkraise()

    def _set_text(self, widget: object, value: str) -> None:
        widget.configure(state="normal"); widget.delete("1.0", "end"); widget.insert("1.0", value); widget.configure(state="disabled")

    def _worker(self, kind: str, payload: object, epoch: int) -> None:
        try:
            with self._client_lock:
                # lock待ちの間にOFF/closeされた要求は、接続も送信もしない。
                if self._closed or epoch != self._cancel_epoch:
                    return
                if kind in {"refresh", "command"} and payload.generation != self.model.generation:
                    return
                if kind == "refresh": result = tuple(self.client.pipeline(payload.requests, timeout=0.5).values())
                elif kind == "command": result = self.client.send(payload.command, timeout=0.5)
                elif kind == "utility": result = self.client.send(payload, timeout=utility_timeout(payload))
                elif kind == "cleanup": result = [(command, self.client.send(command, timeout=0.5)) for command in self._CLEANUP_COMMANDS]
                else: return
            self.events.put((kind, payload, result))
        except WireError as error:
            self.events.put((kind + "_error", payload, error))
        except Exception as error:
            self.events.put((kind + "_error", payload, error))

    def _start_worker(self, kind: str, payload: object) -> None:
        threading.Thread(target=self._worker, args=(kind, payload, self._cancel_epoch), daemon=True).start()

    def enable_refresh(self) -> None:
        self._cancel_poll()  # 予約済みの更新が残っていると、直後に無駄な更新が1回走る
        if self.model.refresh_enabled and not self.model.poll_allowed():
            # コマンド応答待ち。ここで更新を始めると世代が進んでコマンドが捨てられるので、
            # 応答後の更新（または次の予約）に任せる。
            self._schedule_poll(); return
        plan = self.model.enable_refresh() if not self.model.refresh_enabled else self.model.begin_refresh()
        self._panes_current = False
        if plan.requests: self._start_worker("refresh", plan)
        self._refresh_widgets()

    def manual_refresh(self) -> None:
        """Refresh ONボタン。コマンド応答待ちで更新を保留した時は、その旨を表示する。"""
        if self.model.refresh_enabled and not self.model.poll_allowed():
            self.notice_var.set(self._REFRESH_DEFERRED_NOTICE)
        self.enable_refresh()

    def disable_refresh(self) -> None:
        self._cancel_epoch += 1
        self.model.disable_refresh(); self._cancel_poll(); self._refresh_widgets()

    def _on_unmap(self, event: object) -> None:
        if event.widget is self.root and self.root.state() != "normal":
            self.disable_refresh()

    def _schedule_poll(self) -> None:
        self._cancel_poll(); delay = self.model.next_poll_delay_ms()
        if delay is not None and not self._closed: self._poll_after = self.root.after(delay, self._poll)

    def _poll(self) -> None:
        self._poll_after = None
        if self.model.break_expired(int(time.monotonic() * 1000)):
            self.notice_var.set("Break pending or unavailable")
        if not self.model.poll_allowed():
            self._schedule_poll(); return
        self.enable_refresh()

    def _cancel_poll(self) -> None:
        if self._poll_after is not None: self.root.after_cancel(self._poll_after); self._poll_after = None

    def _drain_events(self) -> None:
        while True:
            try: kind, payload, result = self.events.get_nowait()
            except queue.Empty: break
            try: self._handle_event(kind, payload, result)
            except Exception as error:
                # 描画中の例外で以後のイベント処理（after再予約）が止まらないようにする。
                # 通知欄は次のイベントで上書きされるため、本物のバグを隠さないようstderrにも出す。
                traceback.print_exc()
                self.notice_var.set(f"{kind}: 表示更新に失敗: {type(error).__name__}: {error}")
            self._refresh_widgets()
        if not self._closed: self.root.after(25, self._drain_events)

    def _handle_event(self, kind: str, payload: object, result: object) -> None:
        if kind == "refresh":
            if self.model.complete_refresh(payload.generation, result):
                self.address_bits_var.set(self.model.address_bits)
                self._schedule_poll()  # 描画が例外で中断しても自動更新は止めない
                self._render_panes()
        elif kind == "refresh_error":
            if self.model.transport_error(payload.generation, result): self._schedule_poll()
        elif kind == "command":
            current = self.model.refresh_enabled and payload.generation == self.model.generation
            self.model.complete_command(result, payload)
            # DBG CONT はstopinfoなしのOK即時応答である。Run/Break/Stepは
            # 応答本文の有無にかかわらず、現世代だけ次回更新を始める。
            if self.notice_var.get() == self._REFRESH_DEFERRED_NOTICE: self.notice_var.set("")  # 保留した更新を始めるので消す
            if current: self.enable_refresh()
        elif kind == "command_error":
            if self.model.transport_error(payload.generation, result): self._schedule_poll()
        elif kind == "utility":
            self.notice_var.set(utility_notice(payload, result.status))
            if payload == SNAPSHOT_LOAD: self._panes_current = False; self._stop_selected_key = None; self.model.request_lists_once()
            if payload.startswith(("MEM READ", "MEM PEEK")):
                self._render_memory_response(result, payload)
            if payload.startswith(("BP ", "WP ")): self.model.request_lists_once()
            if self.model.refresh_enabled: self.enable_refresh()
        elif kind == "utility_error":
            self.notice_var.set(f"{payload}: {result}")
            # 失敗（timeout等）でもサーバ側では適用済みのことがあるので、一覧は取り直す。
            if payload.startswith(("BP ", "WP ")): self.model.request_lists_once()
        elif kind == "cleanup":
            self.notice_var.set("調査終了: " + ", ".join(f"{command}={reply.status}" for command, reply in result))
            self.model.request_lists_once()
            if self.model.refresh_enabled: self.enable_refresh()
        elif kind == "cleanup_error":
            self.notice_var.set(f"調査終了に失敗: {result}")
            self.model.request_lists_once()  # 途中まで適用済みのことがある

    def _roll_counter(self) -> None:
        self.model.reset_recent_request_count(); self._refresh_widgets()
        if not self._closed: self.root.after(1000, self._roll_counter)

    def _refresh_widgets(self) -> None:
        fields = self.model.stopinfo.fields if self.model.stopinfo else {}; error = self.model.error
        suffix = (" / MCP busy — another client may be waiting" if isinstance(error, DebuggerError) and error.type_name == "WireTimeoutError" else (f" / {error.type_name}: {error.message}" if error else ""))
        self.status_var.set(f"{self.model.state.value} / Refresh: {'ON' if self.model.refresh_enabled else 'OFF'}{suffix}")
        self.performance_var.set(f"CPU debug mode: {fields.get('MODE', '—')} ({fields.get('EXEC', '—')})    Capture: {fields.get('CAP', '—')}")
        self.counter_var.set(f"Requests: {self.model.request_count} total, {self.model.recent_request_count} / sec")
        paused = self.model.can_run(); running = self.model.can_break()
        for button, enabled in ((self.run_button, paused), (self.step_button, paused), (self.step10_button, paused), (self.break_button, running),
                                (self.snapshot_save_button, paused), (self.snapshot_load_button, paused)): button.configure(state="normal" if enabled else "disabled")

    @staticmethod
    def _response_text(response: Response) -> str: return response.status if response.status.startswith("ERR") else (response.body.decode("utf-8", errors="replace") or response.status)

    def _render_panes(self) -> None:
        panes = self.model.panes
        self._set_text(self.register_text, format_registers_view(panes.get("status"), panes.get("registers"), panes.get("mmu")))
        for key, widget in (("disassembly", self.disasm_text),):
            if key in panes: self._set_text(widget, self._response_text(panes[key]))
        if "memory" in panes:
            self._render_memory_response(panes["memory"])
        self._render_records(self.bp_tree, panes.get("breakpoints"), ("id", "addr", "en", "hits", "pass", "cond")); self._render_records(self.wp_tree, panes.get("watchpoints"), ("id", "addr", "size", "en", "hits", "last"))
        self._select_stop_location()
        for name, key in (("CALLRING", "ring_callring"), ("PCRING", "ring_pcring"), ("REGRING", "ring_regring")):
            if key in panes: self._set_text(self.trace_texts[name], self._response_text(panes[key]))
        raw = "\n\n".join(f"=== {name} ===\n{self._response_text(panes[key])}" for name, key in (("CALLRING", "ring_callring"), ("PCRING", "ring_pcring"), ("REGRING", "ring_regring")) if key in panes)
        self._set_text(self.raw_trace_text, raw)
        self._panes_current = True

    def _render_memory_response(self, response: Response, fallback_command: str = "") -> None:
        if response.status.startswith("ERR"):
            self._set_text(self.memory_text, response.status)
            return
        base = memory_base_from_response(response, fallback_command)
        if base is None:
            self.notice_var.set("MEM応答の要求アドレスを特定できません")
            return
        self.memory_bytes = response.body
        self._set_text(self.memory_text, format_memory(response.body, base, self.model.address_bits))
        # 250msごとの更新で、利用者がクリックしたbyte選択を先頭へ戻さない。
        # 表示先頭が変わった（Go等）か範囲外になった時だけ先頭を選ぶ。
        kept = (self.selected_memory_address is not None and base == self._displayed_memory_base
                and base <= self.selected_memory_address < base + len(response.body))
        if not kept: self.selected_memory_address = base if response.body else None
        self._displayed_memory_base = base
        self._update_memory_value()

    def _render_records(self, tree: object, response: Response | None, columns: tuple[str, ...]) -> None:
        # 250msごとの更新で利用者の選択が消えないよう、同じidが残る限り復元する。
        # 実行中の更新はDBG STATUSだけでBP/WP LISTを取らない。その間は最後に取得した一覧を残す。
        if response is None: return
        selected = tree.selection(); focused = tree.focus()
        tree.delete(*tree.get_children())
        try: self._fill_records(tree, response, columns)
        finally:
            kept = [item for item in selected if tree.exists(item)]
            if kept: tree.selection_set(kept)
            if focused and tree.exists(focused): tree.focus(focused)

    def _fill_records(self, tree: object, response: Response, columns: tuple[str, ...]) -> None:
        if response.status.startswith("ERR"):
            tree.insert("", "end", values=(response.status,) + ("",) * (len(columns) - 1))
            return
        expected, records = parse_list_records(response.status)
        for record in records:
            if "cond" in record:
                pass_match = re.search(r"(?:\s*&&\s*)?#(\d+)$", record["cond"])
                if pass_match:
                    record["pass"] = pass_match.group(1)
                    record["cond"] = record["cond"][:pass_match.start()]
            tree.insert("", "end", iid=record.get("id", ""), values=tuple(record.get(column, "") for column in columns))
        if expected is not None and expected != len(records): self.notice_var.set(f"警告: LISTは{expected}件を宣言しましたが{len(records)}件だけ解析できました")

    def _select_stop_location(self) -> None:
        stop = self.model.stopinfo
        if stop is None or not stop.paused:
            self._stop_selected_key = None
            return
        if stop.pc is not None:
            self.disasm_text.tag_delete("current_pc")
            self.disasm_text.tag_configure("current_pc", background="#fff0a0", foreground="#000000")
            for line_number, line in enumerate(self.disasm_text.get("1.0", "end").splitlines(), 1):
                if re.match(rf"\s*{stop.pc:08X}(?:\s|$)", line, re.IGNORECASE):
                    self.disasm_text.tag_add("current_pc", f"{line_number}.0", f"{line_number}.end")
                    self.disasm_text.see(f"{line_number}.0")
                    break
        # 停止理由ごとに、選択する一覧とidのフィールド名を決める。USER/STEP/SNAPSHOT等は
        # BP/WPに対応しないので何も選択しない。
        selection = {"BREAKPOINT": (self.bp_tree, "BP"), "WATCHPOINT": (self.wp_tree, "WP")}.get(stop.fields.get("STOP"))
        if selection is None:
            return
        tree, key = selection
        identifier = stop.fields.get(key)
        stop_key = (stop.fields.get("STOP"), stop.pc, identifier)
        if stop_key == self._stop_selected_key:
            return
        if identifier and tree.exists(identifier):
            self._stop_selected_key = stop_key
            tree.selection_set(identifier)
            tree.focus(identifier)
            tree.see(identifier)

    def select_memory(self, event: object) -> None:
        index = self.memory_text.index(f"@{event.x},{event.y}")
        line, column = (int(value) for value in index.split("."))
        byte_index = (line - 1) * 16 + (column - 8) // 3
        if column < 8 or (column - 8) % 3 > 1 or not 0 <= byte_index < len(self.memory_bytes):
            return
        self.selected_memory_address = self._displayed_memory_base + byte_index
        # BP/WPの対象は入力欄が正本。クリックした byte のアドレスを入力欄へ反映する。
        self.address_var.set(self._format_address(self.selected_memory_address))
        self._update_memory_value()

    def _update_memory_value(self) -> None:
        if self.selected_memory_address is None:
            self.memory_value_var.set("")
            return
        offset = self.selected_memory_address - self._displayed_memory_base
        values = self.memory_bytes[offset:offset + 4]
        byte = f"{values[0]:02X}" if values else "—"
        word = f"{int.from_bytes(values[:2], 'big'):04X}" if len(values) >= 2 else "—"
        long_value = f"{int.from_bytes(values, 'big'):08X}" if len(values) >= 4 else "—"
        self.memory_value_var.set(f"{self._format_address(self.selected_memory_address)}: byte={byte} word={word} long={long_value}")

    def run(self) -> None:
        plan = self.model.run()
        if plan: self._panes_current = False; self._stop_selected_key = None; self._start_worker("command", plan)
        self._refresh_widgets()

    def step(self, count: int) -> None:
        plan = self.model.step(count)
        if plan: self._panes_current = False; self._stop_selected_key = None; self._start_worker("command", plan)
        self._refresh_widgets()

    def break_execution(self) -> None:
        plan = self.model.break_execution(int(time.monotonic() * 1000))
        if plan: self._panes_current = False; self._stop_selected_key = None; self._start_worker("command", plan)
        self._refresh_widgets()

    def _utility(self, command: str) -> None:
        self.model.request_count += 1; self.model.recent_request_count += 1; self._start_worker("utility", command); self._refresh_widgets()

    def snapshot_save(self) -> None:
        if self.model.can_run(): self._utility(SNAPSHOT_SAVE)

    def snapshot_load(self) -> None:
        from tkinter import messagebox
        if not self.model.can_run(): return
        text = "保存済みの状態へ復元します。現在のCPU・メモリ・周辺機器の状態は失われ、履歴リングは空になります。"
        if messagebox.askokcancel("Snapshot Load", text, parent=self.root): self._utility(SNAPSHOT_LOAD)

    def snapshot_status(self) -> None:
        self._utility(SNAPSHOT_STATUS)

    def choose_memory_super(self) -> None:
        """SUPERチェック。現在の表示位置をスーパーバイザ空間で読み直す。"""
        self.model.memory_super = self.memory_super_var.get()
        self._utility(self.model.peek_command(self.model.memory_base))

    def _format_address(self, value: int) -> str:
        return format_address(value, self.model.address_bits)

    def _parse_input_address(self) -> int:
        return parse_address(self.address_var.get(), self.model.address_bits)

    def choose_address_bits(self) -> None:
        """24/32bitラジオの操作。入力欄と表示中のメモリを新しい幅で整形し直す。"""
        self.model.set_address_bits(self.address_bits_var.get())
        # 24bitへ戻す時、範囲外の値は黙って切り詰めず、入力欄はそのままにする（Go時に範囲外として拒否される）。
        try: self.address_var.set(self._format_address(self._parse_input_address()))
        except ValueError: pass
        if self.memory_bytes: self._set_text(self.memory_text, format_memory(self.memory_bytes, self._displayed_memory_base, self.model.address_bits))
        self._update_memory_value()

    def goto_address(self) -> None:
        try: address = self._parse_input_address()
        except ValueError as error: self.notice_var.set(str(error)); return
        self.model.memory_base = address
        self.address_var.set(self._format_address(address)); self._utility(self.model.peek_command(address))

    def goto_pc(self) -> None:
        if self.model.last_pc is not None: self.address_var.set(self._format_address(self.model.last_pc)); self.goto_address()

    def goto_register(self, name: str) -> None:
        try: self.address_var.set(self._format_address(int(self.model.stopinfo.fields[name], 16)))
        except (AttributeError, KeyError, ValueError): self.notice_var.set(f"{name} はまだ取得されていません"); return
        self.goto_address()

    def read_memory_once(self) -> None:
        try: address = self._parse_input_address()
        except ValueError as error: self.notice_var.set(str(error)); return
        self.model.memory_base = address
        self._utility(f"MEM READ {address:06X} 100")

    def add_breakpoint(self) -> None:
        from tkinter import simpledialog
        try: address = self._parse_input_address()
        except ValueError as error: self.notice_var.set(str(error)); return
        if address & 1: self.notice_var.set("BPは偶数アドレスだけに設定できます"); return
        condition = simpledialog.askstring("Add BP", "condition（空欄で無条件）:", parent=self.root)
        if condition is None:
            return
        pass_count = simpledialog.askinteger("Add BP", "pass count（0以上）:", parent=self.root, initialvalue=0, minvalue=0)
        if pass_count is None:
            return
        suffix = condition.strip()
        if pass_count:
            suffix = f"{suffix} && #{pass_count}" if suffix else f"#{pass_count}"
        self._utility(f"BP ADD {address:06X}" + (f" {suffix}" if suffix else ""))

    def delete_breakpoint(self) -> None:
        selected = self.bp_tree.selection()
        if selected:
            self._utility(f"BP DEL {selected[0]}")
            self.bp_tree.selection_remove(selected)  # キーリピートで同じidへ連続送信しない

    def toggle_breakpoint(self) -> None:
        selected = self.bp_tree.selection()
        if selected:
            values = self.bp_tree.item(selected[0], "values"); self._utility(f"BP ENABLE {selected[0]} {'0' if len(values) > 2 and values[2] == '1' else '1'}")

    def add_watchpoint(self) -> None:
        from tkinter import simpledialog
        try: address = self._parse_input_address()
        except ValueError as error: self.notice_var.set(str(error)); return
        size = simpledialog.askinteger("Add WP", "size (1, 2, 4):", parent=self.root, minvalue=1, maxvalue=4)
        if size is not None and size in {1, 2, 4}: self._utility(f"WP ADD {address:06X} {size}")
        elif size is not None: self.notice_var.set("WP sizeは1、2、4だけです")

    def delete_watchpoint(self) -> None:
        selected = self.wp_tree.selection()
        if selected:
            self._utility(f"WP DEL {selected[0]}")
            self.wp_tree.selection_remove(selected)  # キーリピートで同じidへ連続送信しない

    def set_diagnostic(self, kind: str) -> None: self._utility(f"DIAG {kind} {'ON' if self._diag_vars[kind].get() else 'OFF'}")

    def freeze_trace(self) -> None:
        stop = self.model.stopinfo
        panes = self.model.panes
        required = {"status", "registers", "disassembly", "ring_callring"}
        if stop and "PC" in stop.cap:
            required.add("ring_pcring")
        if stop and "REG" in stop.cap:
            required.add("ring_regring")
        if self.model.state != DebuggerState.DISPLAYING or not self._panes_current or not stop or not stop.paused or not required.issubset(panes):
            self.notice_var.set("停止中の全traceペインが揃ってからFreezeしてください")
            return
        self._frozen = {key: self._response_text(value) for key, value in panes.items() if key in required}
        self.notice_var.set("traceをFreezeしました")

    def export_trace(self) -> None:
        from tkinter import filedialog
        if not self._frozen: self.notice_var.set("停止中のtraceをFreezeしてからexportしてください"); return
        path = filedialog.asksaveasfilename(parent=self.root, title="Export trace", defaultextension=".txt")
        if path:
            try:
                with open(path, "w", encoding="utf-8") as output:
                    for name, value in self._frozen.items(): output.write(f"=== {name} ===\n{value}\n\n")
            except OSError as error: self.notice_var.set(f"Export失敗: {error}")
            else: self.notice_var.set(f"Export: {path}")

    def follow_ring_pc(self, name: str, event: object) -> None:
        widget = self.trace_texts[name]
        index = widget.index(f"@{event.x},{event.y}")
        _, column = (int(value) for value in index.split("."))
        line = widget.get(f"{index} linestart", f"{index} lineend")
        parsed = parse_ring(line)
        verified = {item.pc for item in parsed.lines if item.pc is not None}
        targets = [(match.start(), match.end(), int(match.group(), 16)) for match in re.finditer(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{6,8}(?![0-9A-Fa-f])", line)]
        target = next((value for start, end, value in targets if start <= column < end and value in verified), None)
        if target is not None: self.address_var.set(self._format_address(target)); self.goto_address()

    def cleanup(self) -> None:
        from tkinter import messagebox
        text = "次の操作は他のMCPクライアントの設定にも影響します。\n\n" + "\n".join(self._CLEANUP_COMMANDS)
        if messagebox.askokcancel("調査終了", text, parent=self.root):
            self.model.request_count += len(self._CLEANUP_COMMANDS); self.model.recent_request_count += len(self._CLEANUP_COMMANDS); self._start_worker("cleanup", None); self._refresh_widgets()

    def close(self) -> None:
        self._closed = True; self.disable_refresh(); self.root.destroy()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="ix68k external MCP debugger"); parser.add_argument("--version", action="version", version=f"ix68k-debugger {VERSION}"); parser.add_argument("--socket", metavar="PATH"); parser.add_argument("--port", type=int)
    args = parser.parse_args(argv)
    if args.socket and args.port: parser.error("--socket と --port は同時に指定できません")
    endpoint: str | tuple[str, int] | None = args.socket if args.socket else (("127.0.0.1", args.port) if args.port is not None else None)
    try: import tkinter as tk
    except ImportError:
        print("Tkinter が必要です。macOS/Homebrewでは python-tk、Debian/Ubuntuでは python3-tk を導入してください。", file=sys.stderr); return 1
    root = tk.Tk(); DebuggerApplication(root, endpoint); root.mainloop(); return 0


if __name__ == "__main__":
    raise SystemExit(main())
