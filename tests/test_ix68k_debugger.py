#!/usr/bin/env python3
"""Tkなしで検証するデバッガモデルとring parser。"""

import sys
import unittest
import unittest.mock
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT / "tools" / "debugger"))
sys.path.insert(0, str(_ROOT / "tools" / "mcp"))
from ix68k_debugger import (  # noqa: E402
    ADDRESS_BITS_24, ADDRESS_BITS_32, CommandPlan, DebuggerModel, DebuggerState, address_bits_from_mmu_reply,
    format_address, format_memory, memory_base_from_response, parse_address, parse_list_records, parse_ring,
    SNAPSHOT_LOAD, SNAPSHOT_LOAD_NOTICE, SNAPSHOT_SAVE, SNAPSHOT_STATUS, SNAPSHOT_TIMEOUT_S, UTILITY_TIMEOUT_S,
    TRACE_TABS, format_registers_view, format_sr_flags, parse_fields, utility_notice, utility_timeout,
)
from ix68k_wire import (  # noqa: E402
    Request, Response, WireConnectionError, WireEOFError, WireProtocolVersionError, WireTimeoutError,
)


def response(tag, status):
    return Response(tag, status, b"")


class DebuggerModelTest(unittest.TestCase):
    def test_off_sends_nothing_and_refresh_sends_one_generation(self):
        model = DebuggerModel()
        self.assertEqual(model.begin_refresh().requests, ())
        plan = model.enable_refresh()
        self.assertEqual([item.command for item in plan.requests], ["DBG STATUS", "MMU GET"])
        self.assertEqual(model.request_count, 2)
        self.assertEqual(model.next_poll_delay_ms(), 250)
        model.reset_recent_request_count()
        self.assertEqual(model.recent_request_count, 0)

    def test_paused_result_expands_next_generation_and_uses_capture(self):
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused CAP=PC,REG PC=00FF00")])
        plan = model.begin_refresh()
        commands = [item.command for item in plan.requests]
        self.assertIn("DISASM 00FF00 32", commands)
        self.assertIn("MEM PEEK 000000 100", commands)
        self.assertIn("RING PCRING 200", commands)
        self.assertIn("RING REGRING 200", commands)
        self.assertIn("RING CALLRING 200", commands)

    def test_old_generation_is_discarded(self):
        model = DebuggerModel()
        old = model.enable_refresh()
        current = model.begin_refresh()
        self.assertFalse(model.complete_refresh(old.generation, [response("T1", "OK STATE=paused PC=000100")]))
        self.assertTrue(model.complete_refresh(current.generation, [response("T2", "OK STATE=running CAP=NONE")]))

    def test_execute_refresh_uses_one_wire_pipeline_per_generation(self):
        class Client:
            def __init__(self):
                self.calls = []

            def pipeline(self, requests, *, timeout):
                self.calls.append((tuple(requests), timeout))
                return {"T1": response("T1", "OK STATE=paused CAP=NONE PC=000200")}

        model = DebuggerModel(cpu_probed=True)
        plan = model.enable_refresh()
        client = Client()
        self.assertTrue(model.execute_refresh(client, plan))
        self.assertEqual([request.command for request in client.calls[0][0]], ["DBG STATUS"])
        self.assertEqual(client.calls[0][1], 0.5)
        self.assertEqual(model.state, DebuggerState.DISPLAYING)

    def test_timeout_backoff_and_break_deadline(self):
        model = DebuggerModel()
        plan = model.enable_refresh()
        for expected in (100, 250, 500, 1000, 1000, 1000):
            self.assertTrue(model.transport_error(plan.generation, WireTimeoutError("busy")))
            self.assertEqual(model.retry_ms, expected)
            self.assertEqual(model.next_poll_delay_ms(), expected)
        model.complete_refresh(plan.generation, [response("T1", "OK STATE=paused PC=000100")])
        run_plan = model.run()
        self.assertEqual(run_plan.command, "DBG CONT")
        self.assertIsNone(model.break_execution(10))  # CONTの応答待ちの間はBreakを受け付けない
        model.complete_command(response("TR", "OK"), run_plan)
        self.assertEqual(model.break_execution(10).command, "DBG BREAK")
        self.assertEqual(model.next_poll_delay_ms(), 100)
        self.assertFalse(model.break_expired(5009))
        self.assertTrue(model.break_expired(5010))
        self.assertEqual(model.state, DebuggerState.RUNNING_POLL)

    def test_break_wait_survives_immediate_reply_status_polls_and_timeouts(self):
        model = DebuggerModel(cpu_probed=True)
        initial = model.enable_refresh()
        model.complete_refresh(initial.generation, [response("T1", "OK STATE=paused PC=000100")])
        run_plan = model.run()
        model.complete_command(response("TR", "OK"), run_plan)
        break_plan = model.break_execution(10)
        self.assertEqual(break_plan.command, "DBG BREAK")
        self.assertEqual(model.complete_command(response("T2", "OK STATE=running"), break_plan), model.stopinfo)
        self.assertEqual(model.state, DebuggerState.BREAK_POLL)
        poll = model.begin_refresh()
        self.assertEqual([request.command for request in poll.requests], ["DBG STATUS"])
        model.transport_error(poll.generation, WireTimeoutError("busy"))
        self.assertEqual(model.state, DebuggerState.BREAK_POLL)
        self.assertEqual(model.next_poll_delay_ms(), 100)
        poll = model.begin_refresh()
        model.complete_refresh(poll.generation, [response("T3", "OK STATE=running")])
        self.assertEqual(model.state, DebuggerState.BREAK_POLL)
        poll = model.begin_refresh()
        model.complete_refresh(poll.generation, [response("T4", "OK STATE=paused PC=000102")])
        self.assertEqual(model.state, DebuggerState.DISPLAYING)

    def test_disable_cancels_created_refresh_plan_and_delayed_command(self):
        class Client:
            def pipeline(self, requests, *, timeout):
                raise AssertionError("disabled plan must not be sent")

        model = DebuggerModel()
        plan = model.enable_refresh()
        model.disable_refresh()
        self.assertFalse(model.execute_refresh(Client(), plan))

        plan = model.enable_refresh()
        model.complete_refresh(plan.generation, [response("T1", "OK STATE=paused PC=000100")])
        step = model.step()
        model.disable_refresh()
        self.assertIsNone(model.complete_command(response("T2", "OK STATE=paused PC=000102"), step))
        self.assertEqual(model.state, DebuggerState.OFF)

    def test_old_command_cannot_complete_after_off_on_and_new_command(self):
        model = DebuggerModel()
        initial = model.enable_refresh()
        model.complete_refresh(initial.generation, [response("T1", "OK STATE=paused PC=000100")])
        old_step = model.step()
        model.disable_refresh()
        fresh = model.enable_refresh()
        model.complete_refresh(fresh.generation, [response("T2", "OK STATE=paused PC=000200")])
        new_step = model.step()
        self.assertIsNone(model.complete_command(response("T3", "OK STATE=paused PC=000102"), old_step))
        self.assertEqual(model.complete_command(response("T4", "OK STATE=paused PC=000202"), new_step).pc, 0x202)

    def test_non_timeout_error_clears_break_wait_without_state_resurrection(self):
        model = DebuggerModel()
        initial = model.enable_refresh()
        model.complete_refresh(initial.generation, [response("T1", "OK STATE=paused PC=000100")])
        model.run()
        model.break_execution(0)
        plan = model.begin_refresh()
        model.transport_error(plan.generation, WireEOFError("切断"))
        self.assertFalse(model.break_pending)
        self.assertFalse(model.break_expired(5000))
        self.assertEqual(model.state, DebuggerState.OFF)

    def test_pipeline_response_uses_request_pane_when_out_of_order_and_err(self):
        class Client:
            def pipeline(self, requests, *, timeout):
                by_command = {request.command: request for request in requests}
                return {
                    "T3": Response("T3", "ERR unreadable", b"", by_command["REGS"]),
                    "T2": Response("T2", "OK STATE=paused PC=000100", b"", by_command["DBG STATUS"]),
                }

        model = DebuggerModel()
        initial = model.enable_refresh()
        model.complete_refresh(initial.generation, [response("T1", "OK STATE=paused PC=000100")])
        plan = model.begin_refresh()
        self.assertTrue(model.execute_refresh(Client(), plan))
        self.assertEqual(model.panes["registers"].status, "ERR unreadable")
        self.assertEqual(model.panes["status"].status, "OK STATE=paused PC=000100")

    def test_non_timeout_error_keeps_type_and_protocol_details(self):
        model = DebuggerModel()
        plan = model.enable_refresh()
        error = WireProtocolVersionError("版が違う（エミュレータ=3、ビルド=old）")
        self.assertTrue(model.transport_error(plan.generation, error))
        self.assertFalse(model.refresh_enabled)
        self.assertEqual(model.error.type_name, "WireProtocolVersionError")
        self.assertIn("ビルド=old", model.error.message)

        model = DebuggerModel()
        plan = model.enable_refresh()
        self.assertTrue(model.transport_error(plan.generation, WireEOFError("途中で切断")))
        self.assertEqual(model.error.type_name, "WireEOFError")

    def test_invalid_operations_emit_no_request(self):
        model = DebuggerModel()
        self.assertIsNone(model.run())
        self.assertIsNone(model.step())
        self.assertIsNone(model.break_execution(0))
        self.assertEqual(model.request_count, 0)

    def test_step_response_returns_to_paused_displaying_state(self):
        model = DebuggerModel()
        initial = model.enable_refresh()
        model.complete_refresh(initial.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        step = model.step(10)
        self.assertEqual(step.command, "DBG STEP 10")
        stop = model.complete_command(response("T2", "OK STATE=paused STOP=STEP PC=000102 CAP=NONE"), step)
        self.assertTrue(stop.paused)
        self.assertEqual(model.state, DebuggerState.DISPLAYING)
        self.assertEqual(model.last_pc, 0x102)

    def test_controls_stay_available_while_refresh_is_fetching(self):
        # 更新中(FETCHING)でもRun/Step/Breakの可否と受付は更新前の状態で判定する。
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        self.assertTrue(model.can_run())
        self.assertFalse(model.can_break())
        plan = model.begin_refresh()
        self.assertEqual(model.state, DebuggerState.FETCHING)
        self.assertTrue(model.can_run())
        self.assertFalse(model.can_break())
        model.complete_refresh(plan.generation, [response("T2", "OK STATE=running CAP=NONE")])
        self.assertFalse(model.can_run())
        self.assertTrue(model.can_break())
        model.begin_refresh()
        self.assertTrue(model.can_break())
        self.assertFalse(model.can_run())

    def test_no_controls_before_first_fetch_completes(self):
        model = DebuggerModel()
        model.enable_refresh()
        self.assertFalse(model.can_run())
        self.assertFalse(model.can_break())
        self.assertIsNone(model.run())

    def test_run_during_fetch_is_accepted_and_drops_the_stale_refresh(self):
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        fetch = model.begin_refresh()
        run = model.run()
        self.assertEqual(run.command, "DBG CONT")
        self.assertEqual(model.state, DebuggerState.RUNNING_POLL)
        # 更新中だった古い(paused)結果は、Run後の状態を巻き戻さない。
        self.assertFalse(model.complete_refresh(fetch.generation, [response("T2", "OK STATE=paused PC=000100 CAP=NONE")]))
        self.assertEqual(model.state, DebuggerState.RUNNING_POLL)

    def test_step_is_not_accepted_twice_while_command_is_pending(self):
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        self.assertIsNotNone(model.step())
        self.assertIsNone(model.step())
        self.assertFalse(model.can_run())

    def test_command_timeout_releases_pending_command(self):
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        step = model.step()
        self.assertIsNotNone(model.pending_command_token)
        model.transport_error(step.generation, WireTimeoutError("timeout"))
        self.assertIsNone(model.pending_command_token)

    def test_manual_refresh_after_command_does_not_leave_polling_blocked(self):
        # Opus B1: コマンド後に世代が進んでも、保留が残って自動更新が止まらない。
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        run = model.run()
        self.assertFalse(model.poll_allowed())
        refresh = model.begin_refresh()  # utility完了・手動Refresh等で世代が進む
        self.assertTrue(model.poll_allowed())
        self.assertIsNone(model.complete_command(response("T2", "OK"), run))  # 古い応答は反映しない
        self.assertTrue(model.complete_refresh(refresh.generation, [response("T3", "OK STATE=running CAP=NONE")]))
        self.assertTrue(model.poll_allowed())
        self.assertEqual(model.state, DebuggerState.RUNNING_POLL)

    def test_stale_command_response_still_releases_its_own_pending(self):
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        step = model.step()
        model.generation += 1  # 何らかの理由で世代が進んだ
        self.assertIsNone(model.complete_command(response("T2", "OK STATE=paused PC=000102"), step))
        self.assertIsNone(model.pending_command_token)

    def test_break_is_not_accepted_until_run_is_acknowledged(self):
        # Opus N1: DBG CONT応答前のBreakは世代を進めてCONTを捨てさせるので受け付けない。
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        run = model.run()
        self.assertFalse(model.can_break())
        self.assertIsNone(model.break_execution(0))
        model.complete_command(response("T2", "OK"), run)
        self.assertTrue(model.can_break())

    def test_step_pending_then_refresh_keeps_paused_origin(self):
        # Opus N2: モデル単体では、Step保留中にbegin_refreshされても戻り先はDISPLAYING。
        # （実際のビューは保留中にbegin_refreshしない。TkViewTestを参照）
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        model.step()
        model.begin_refresh()
        self.assertTrue(model.can_run())

    def test_timeout_while_paused_keeps_paused_state(self):
        # Opus N5: 停止中のbusy timeoutでRUNNING_POLLへ落ちず、Breakは有効にならない。
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        plan = model.begin_refresh()
        model.transport_error(plan.generation, WireTimeoutError("busy"))
        self.assertEqual(model.state, DebuggerState.DISPLAYING)
        self.assertFalse(model.can_break())
        self.assertTrue(model.can_run())

    def test_timeout_after_manual_refresh_on_while_paused_keeps_paused_state(self):
        # Opus N5残り: Refresh OFF→ONでfetch_originがOFFでも、停止中ならRUNNING_POLLへ落とさない。
        model = DebuggerModel()
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        model.disable_refresh()
        plan = model.enable_refresh()
        model.transport_error(plan.generation, WireTimeoutError("busy"))
        self.assertEqual(model.state, DebuggerState.DISPLAYING)
        self.assertFalse(model.can_break())

    def test_running_refresh_fetches_lists_once_after_bp_wp_change(self):
        # Opus N9: 実行中はDBG STATUSだけだが、BP/WP操作後は1回だけLISTを取り直す。
        model = DebuggerModel(cpu_probed=True)
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=running CAP=NONE")])
        self.assertEqual([r.command for r in model.begin_refresh().requests], ["DBG STATUS"])
        model.request_lists_once()
        plan = model.begin_refresh()
        self.assertEqual([r.command for r in plan.requests], ["DBG STATUS", "BP LIST", "WP LIST"])
        # 一覧を受け取れた時だけ取り直し待ちが解ける。
        model.complete_refresh(plan.generation, [
            Response("T1", "OK STATE=running CAP=NONE", b"", Request("DBG STATUS")),
            Response("T2", "OK 0 ", b"", Request("BP LIST")),
            Response("T3", "OK 0 ", b"", Request("WP LIST")),
        ])
        self.assertEqual([r.command for r in model.begin_refresh().requests], ["DBG STATUS"])

    def test_lists_refetch_survives_a_timed_out_refresh(self):
        # Opus NB1: 一覧つきの更新がtimeout/破棄されても、取り直し待ちは残る。
        model = DebuggerModel(cpu_probed=True)
        first = model.enable_refresh()
        model.complete_refresh(first.generation, [response("T1", "OK STATE=running CAP=NONE")])
        model.request_lists_once()
        plan = model.begin_refresh()
        model.transport_error(plan.generation, WireTimeoutError("busy"))
        self.assertEqual([r.command for r in model.begin_refresh().requests], ["DBG STATUS", "BP LIST", "WP LIST"])

    def test_selected_memory_base_is_used_by_paused_refresh_and_step_is_not_reentrant(self):
        model = DebuggerModel(memory_base=0xFF00)
        initial = model.enable_refresh()
        model.complete_refresh(initial.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])
        plan = model.begin_refresh()
        self.assertIn("MEM PEEK 00FF00 100", [request.command for request in plan.requests])
        model.complete_refresh(plan.generation, [response("T2", "OK STATE=paused PC=000100 CAP=NONE")])
        self.assertIsNotNone(model.step())
        self.assertIsNone(model.step())

    def test_running_poll_never_uses_stale_paused_stopinfo_for_full_refresh(self):
        model = DebuggerModel(cpu_probed=True)
        initial = model.enable_refresh()
        model.complete_refresh(initial.generation, [response("T1", "OK STATE=paused PC=000100 CAP=PC")])
        self.assertEqual(model.run().command, "DBG CONT")
        running = model.begin_refresh()
        self.assertEqual([request.command for request in running.requests], ["DBG STATUS"])

    def test_disconnect_stops_automatic_refresh_until_manual_refresh(self):
        model = DebuggerModel()
        plan = model.enable_refresh()
        self.assertTrue(model.transport_error(plan.generation, WireConnectionError("offline")))
        self.assertFalse(model.refresh_enabled)
        self.assertIsNone(model.next_poll_delay_ms())
        self.assertEqual(model.enable_refresh().requests[0].command, "DBG STATUS")


class RingParserTest(unittest.TestCase):
    fixtures = _ROOT / "tests" / "fixtures" / "mcp_ring"

    def text(self, name):
        return (self.fixtures / name).read_text()

    def test_valid_ring_kinds_have_clickable_pcs(self):
        self.assertEqual([line.pc for line in parse_ring(self.text("pc_valid.txt")).lines if line.pc is not None], [0xFF00, 0xFF02])
        self.assertEqual([line.pc for line in parse_ring(self.text("reg_valid.txt")).lines if line.pc is not None], [0xFF00])
        self.assertEqual([line.pc for line in parse_ring(self.text("call_valid.txt")).lines if line.pc is not None], [0xFF00, 0xFF24])

    def test_malformed_and_disabled_text_remain_raw(self):
        parsed = parse_ring(self.text("mixed_malformed.txt"))
        self.assertEqual(parsed.raw, self.text("mixed_malformed.txt"))
        self.assertEqual([line.pc for line in parsed.lines if line.pc is not None], [0xFF20])
        disabled = parse_ring(self.text("pc_disabled.txt"))
        self.assertTrue(disabled.disabled)
        self.assertEqual([line.pc for line in disabled.lines if line.pc is not None], [])


class AddressWidthModelTest(unittest.TestCase):
    def _probe(self, model, status):
        plan = model.enable_refresh()
        self.assertEqual(plan.requests[-1].command, "MMU GET")
        model.complete_refresh(plan.generation, [
            Response("1", "OK STATE=running MODE=AUTO CAP=NONE EXEC=NORMAL WB=OFF", b"", request=Request("DBG STATUS")),
            Response("2", status, b"", request=Request("MMU GET")),
        ])

    def test_default_is_24bit_and_probe_selects_32bit_for_68030(self):
        model = DebuggerModel()
        self.assertEqual(model.address_bits, ADDRESS_BITS_24)
        self._probe(model, "OK MODEL=68030 TC=00000000")
        self.assertEqual(model.address_bits, ADDRESS_BITS_32)
        self.assertNotIn("MMU GET", [r.command for r in model.begin_refresh().requests])

    def test_probe_keeps_24bit_for_68000(self):
        model = DebuggerModel()
        self._probe(model, "OK MODEL=68000 MMU=UNSUPPORTED")
        self.assertEqual(model.address_bits, ADDRESS_BITS_24)

    def test_user_choice_is_not_overwritten_by_probe(self):
        model = DebuggerModel()
        model.set_address_bits(ADDRESS_BITS_24)
        self._probe(model, "OK MODEL=68060 URP=0")
        self.assertEqual(model.address_bits, ADDRESS_BITS_24)


class MemorySuperAndMmuTest(unittest.TestCase):
    def _paused_commands(self, model):
        model.stopinfo = None
        model.last_pc = 0x1000
        return [r.command for r in model._paused_requests()]

    def test_peek_command_adds_super_only_when_selected(self):
        model = DebuggerModel()
        self.assertEqual(model.peek_command(0x10000000), "MEM PEEK 10000000 100")
        model.memory_super = True
        self.assertEqual(model.peek_command(0x1000), "MEM PEEK 001000 100 SUPER")

    def test_paused_refresh_uses_super_and_fetches_mmu_every_time(self):
        model = DebuggerModel(cpu_probed=True, memory_super=True)
        commands = self._paused_commands(model)
        self.assertIn("MEM PEEK 000000 100 SUPER", commands)
        self.assertEqual(commands.count("MMU GET"), 1)

    def test_memory_base_is_read_from_super_peek_command(self):
        reply = Response("T", "OK", b"A", Request("MEM PEEK 10000000 100 SUPER"))
        self.assertEqual(memory_base_from_response(reply), 0x10000000)
        self.assertIsNone(memory_base_from_response(Response("T", "OK", b"A", Request("MEM PEEK 1000 100 BOGUS"))))


class DebuggerViewDataTest(unittest.TestCase):
    """画面なしCIでも検証できる、ビューが使う入力・整形契約。"""

    def test_address_input_accepts_documented_hex_forms_and_rejects_invalid_range(self):
        self.assertEqual(parse_address("$ff00"), 0xFF00)
        self.assertEqual(parse_address("0xFF00"), 0xFF00)
        self.assertEqual(parse_address("FF00"), 0xFF00)
        with self.assertRaises(ValueError):
            parse_address("1000000")
        with self.assertRaises(ValueError):
            parse_address("not-an-address")

    def test_address_input_range_follows_selected_width(self):
        self.assertEqual(parse_address("$10000000", ADDRESS_BITS_32), 0x10000000)
        self.assertEqual(parse_address("FFFFFFFF", ADDRESS_BITS_32), 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            parse_address("100000000", ADDRESS_BITS_32)
        with self.assertRaises(ValueError):
            parse_address("10000000", ADDRESS_BITS_24)

    def test_address_format_is_fixed_width_per_mode(self):
        self.assertEqual(format_address(0xFF00, ADDRESS_BITS_24), "00FF00")
        self.assertEqual(format_address(0xFF00, ADDRESS_BITS_32), "0000FF00")
        self.assertEqual(format_address(0x10000000, ADDRESS_BITS_32), "10000000")
        self.assertEqual(format_memory(b"A", 0x10000000, ADDRESS_BITS_32).split()[0], "10000000")

    def test_address_bits_come_from_mmu_get_model(self):
        self.assertEqual(address_bits_from_mmu_reply("OK MODEL=68000 MMU=UNSUPPORTED"), ADDRESS_BITS_24)
        self.assertEqual(address_bits_from_mmu_reply("OK MODEL=68010 MMU=UNSUPPORTED"), ADDRESS_BITS_24)
        for model in ("68020", "68EC030", "68030", "68040", "68060"):
            self.assertEqual(address_bits_from_mmu_reply(f"OK MODEL={model} TC=0"), ADDRESS_BITS_32, model)
        self.assertIsNone(address_bits_from_mmu_reply("ERR NOCPU no cpu"))

    def test_ring_accepts_mixed_six_and_eight_digit_addresses(self):
        pc = parse_ring("00FF904C 10000010 FF0000")
        self.assertEqual([item.pc for item in pc.lines], [0xFF904C, 0x10000010, 0xFF0000])
        call = parse_ring("CALL 10000010 -> 10000100")
        self.assertEqual((call.lines[0].pc, call.lines[0].kind), (0x10000010, "CALLRING"))

    def test_memory_display_is_sixteen_bytes_per_line_with_ascii(self):
        rendered = format_memory(b"ABC\x00" + bytes(range(12)), 0xFF00)
        self.assertEqual(rendered.splitlines()[0][:6], "00FF00")
        self.assertIn("41 42 43 00", rendered)
        self.assertIn("|ABC.", rendered)

    def test_list_records_preserves_declared_count_for_truncation_warning(self):
        count, records = parse_list_records("OK 2 id=0,addr=00FF00,en=1,hits=3,cond=;id=1,addr=00FF10,en=0,hits=0,cond=X")
        self.assertEqual(count, 2)
        self.assertEqual([record["id"] for record in records], ["0", "1"])
        self.assertEqual(parse_list_records("ERR NOCPU unavailable"), (None, []))

    def test_memory_response_uses_its_request_not_the_current_gui_address(self):
        request = Request("MEM PEEK 000000 100")
        reply = Response("T1", "OK", b"\0", request)
        self.assertEqual(memory_base_from_response(reply), 0)
        self.assertEqual(memory_base_from_response(Response("T2", "OK", b"", None), "MEM READ 00FF00 100"), 0xFF00)
        self.assertIsNone(memory_base_from_response(Response("T3", "OK", b"", None)))




REGS_REPLY = ("OK D0=20800000 D1=00000020 D2=00000002 D3=00000200 D4=000BA800 D5=00000000 D6=00000000 "
              "D7=00000005 A0=00013D30 A1=0005E89C A2=0008E126 A3=0008DA78 A4=0008D377 A5=00008264 "
              "A6=000082DE A7=00008258 PC=0002F87E SR=2000 USP=000B3408 SSP=00008364")
MMU_REPLY = ("OK MODEL=68060 URP=00BD8000 SRP=00BD8000 TC=0000C210 DTT0=00000000 DTT1=00000000 "
             "ITT0=00000000 ITT1=00000000")
PAUSED_REPLY = "OK STATE=paused STOP=SNAPSHOT MODE=AUTO CAP=NONE EXEC=NORMAL WB=OFF RINGS=CLEARED PC=00FF8EC2 D0=00000001 SR=270C"
# 左ペインに収まる目安（TkFixedFontで約50桁）。横スクロールを出さないための上限。
MAX_VIEW_LINE_WIDTH = 50
# メモリ操作の1段が収めるべき幅(px)。最小ウィンドウ幅(980px)での右ペインの幅（macOSで実測444）。
MEMORY_ROW_MAX_WIDTH = 450
MEMORY_ROW_PAD_PER_WIDGET = 8  # packのpadx等の見積もり（実測は1個あたり約5px）


class RegistersViewTest(unittest.TestCase):
    def view(self, status=PAUSED_REPLY, registers=REGS_REPLY, mmu=MMU_REPLY):
        return format_registers_view(*(Response("T", text, b"") if text else None for text in (status, registers, mmu)))

    def test_no_line_needs_horizontal_scrolling(self):
        for line in self.view().splitlines():
            self.assertLessEqual(len(line), MAX_VIEW_LINE_WIDTH, line)

    def test_data_and_address_registers_are_four_per_row(self):
        lines = self.view().splitlines()
        self.assertIn("D0=20800000 D1=00000020 D2=00000002 D3=00000200", lines)
        self.assertIn("D4=000BA800 D5=00000000 D6=00000000 D7=00000005", lines)
        self.assertIn("A0=00013D30 A1=0005E89C A2=0008E126 A3=0008DA78", lines)
        self.assertIn("A4=0008D377 A5=00008264 A6=000082DE A7=00008258", lines)

    def test_other_registers_use_two_aligned_columns(self):
        lines = self.view().splitlines()
        self.assertIn("PC=0002F87E    SR=2000", lines)
        self.assertIn("USP=000B3408   SSP=00008364", lines)


    def test_status_lines_keep_unknown_fields_and_split_mode_fields(self):
        lines = self.view().splitlines()
        self.assertEqual(lines[:4], ["STATE=paused   STOP=SNAPSHOT", "RINGS=CLEARED",
                                     "MODE=AUTO      CAP=NONE", "EXEC=NORMAL    WB=OFF"])

    def test_mmu_block_shows_model_then_pairs(self):
        text = self.view()
        self.assertIn("MMU  MODEL=68060", text)
        self.assertIn("URP=00BD8000   SRP=00BD8000", text)
        self.assertIn("TC=0000C210", text.splitlines())
        self.assertIn("DTT0=00000000  DTT1=00000000", text)
        self.assertIn("ITT0=00000000  ITT1=00000000", text)

    def test_68030_mmu_long_registers_get_their_own_lines(self):
        text = self.view(mmu="OK MODEL=68030 TC=80F04445 CRP=0000000000000001 SRP=0000000000000002 "
                             "TT0=00000000 TT1=00000000 MMUSR=0000")
        lines = text.splitlines()
        self.assertIn("CRP=0000000000000001", lines)
        self.assertIn("SRP=0000000000000002", lines)
        self.assertIn("TC=80F04445", lines)
        self.assertIn("TT0=00000000   TT1=00000000", lines)
        self.assertIn("MMUSR=0000", lines)

    def test_long_stop_name_never_runs_into_the_next_column(self):
        lines = self.view(status="OK STATE=paused STOP=BREAKPOINT BP=0 MODE=AUTO CAP=NONE EXEC=NORMAL WB=OFF").splitlines()
        self.assertIn("STOP=BREAKPOINT", lines)
        self.assertIn("STATE=paused", lines)

    def test_running_status_shows_only_state_and_mode(self):
        text = self.view(status="OK STATE=running MODE=AUTO CAP=NONE EXEC=NORMAL WB=OFF", registers=None, mmu=None)
        self.assertEqual(text.splitlines(), ["STATE=running", "MODE=AUTO      CAP=NONE", "EXEC=NORMAL    WB=OFF"])

    def test_paused_status_registers_are_used_when_regs_is_missing(self):
        text = self.view(registers=None, mmu=None)
        self.assertIn("PC=00FF8EC2    SR=270C", text)
        self.assertIn("D0=00000001", text)

    def test_err_replies_are_shown_raw_and_do_not_hide_other_panes(self):
        text = self.view(registers="ERR NOTREADY", mmu="ERR UNSUPPORTED")
        self.assertIn("ERR NOTREADY", text)
        self.assertIn("MMU  ERR UNSUPPORTED", text)
        self.assertIn("STATE=paused", text)

    def test_partial_register_set_is_not_dropped(self):
        text = self.view(status=None, registers="OK D0=00000001 D1=00000002 D2=00000003 PC=00001000", mmu=None)
        for expected in ("D0=00000001", "D1=00000002", "D2=00000003", "PC=00001000"):
            self.assertIn(expected, text)

    def test_unknown_register_fields_are_appended(self):
        text = self.view(status=None, registers=REGS_REPLY + " CACR=80008000", mmu=None)
        self.assertIn("CACR=80008000", text)

    def test_nothing_to_show_gives_empty_text(self):
        self.assertEqual(self.view(status=None, registers=None, mmu=None), "")


class SrFlagsTest(unittest.TestCase):
    def test_supervisor_with_ipl_and_ccr(self):
        self.assertEqual(format_sr_flags("270C"), ["  T=00 S=1 M=0 IPL=7", "  XNZVC=01100"])

    def test_user_mode_trace_and_master_bits(self):
        self.assertEqual(format_sr_flags("C000"), ["  T=11 S=0 M=0 IPL=0", "  XNZVC=00000"])
        self.assertEqual(format_sr_flags("3010"), ["  T=00 S=1 M=1 IPL=0", "  XNZVC=10000"])

    def test_unreadable_value_gives_no_flags(self):
        self.assertEqual(format_sr_flags("zz"), [])

    def test_parse_fields_keeps_order_and_ignores_bare_words(self):
        self.assertEqual(list(parse_fields("OK B=2 A=1 junk")), ["B", "A"])


class SnapshotHelperTest(unittest.TestCase):
    def test_snapshot_commands_use_the_long_timeout_only(self):
        for command in (SNAPSHOT_SAVE, SNAPSHOT_LOAD, SNAPSHOT_STATUS):
            self.assertEqual(utility_timeout(command), SNAPSHOT_TIMEOUT_S)
        for command in ("BP ADD 001000", "MEM READ 001000 100", "DIAG PCRING ON"):
            self.assertEqual(utility_timeout(command), UTILITY_TIMEOUT_S)

    def test_load_notice_summarizes_stopinfo_and_keeps_errors_verbatim(self):
        self.assertEqual(utility_notice(SNAPSHOT_LOAD, "OK STATE=paused STOP=SNAPSHOT RINGS=CLEARED PC=00FF8EBC"),
                         SNAPSHOT_LOAD_NOTICE)
        self.assertEqual(utility_notice(SNAPSHOT_LOAD, "ERR NOTREADY NO_SLOT"), "SNAPSHOT LOAD: ERR NOTREADY NO_SLOT")
        self.assertEqual(utility_notice(SNAPSHOT_SAVE, "OK SLOT=VALID"), "SNAPSHOT SAVE: OK SLOT=VALID")
        self.assertEqual(utility_notice(SNAPSHOT_STATUS, "OK SLOT=EMPTY"), "SNAPSHOT STATUS: OK SLOT=EMPTY")


def _make_app():
    """表示が使えない環境ではNoneを返す（Tkビューのテストをスキップするため）。"""
    import tkinter as tk
    from ix68k_debugger import DebuggerApplication
    try:
        root = tk.Tk()
    except tk.TclError:  # 表示が無い環境だけスキップ。importの失敗等は握りつぶさない
        return None, None
    return root, DebuggerApplication(root, "/tmp/ix68k_test_none.sock")


class TkViewTest(unittest.TestCase):
    def setUp(self):
        self.root, self.app = _make_app()
        if self.root is None:
            self.skipTest("Tkの表示が使えない")
        self.addCleanup(self.root.destroy)

    def _paused_app(self):
        first = self.app.model.enable_refresh()
        self.app.model.complete_refresh(first.generation, [response("T1", "OK STATE=paused PC=000100 CAP=NONE")])

    def test_render_exception_does_not_stop_polling(self):
        # Opus B2: BP LISTのid重複でTclErrorになっても、次の更新は予約される。
        first = self.app.model.enable_refresh()
        bad = Response("T2", "OK 2 id=0,addr=001000,en=1,hits=0,cond=;id=0,addr=002000,en=1,hits=0,cond=", b"",
                       Request("BP LIST"))
        status = Response("T1", "OK STATE=paused PC=000100 CAP=NONE", b"", Request("DBG STATUS"))
        self.app.events.put(("refresh", first, (status, bad)))
        with unittest.mock.patch("traceback.print_exc") as print_exc:
            self.app._drain_events()
        print_exc.assert_called_once()  # 例外を握り潰さずstderrへ出す
        self.assertIsNotNone(self.app._poll_after)

    def test_list_selection_survives_rerender_and_running_keeps_list(self):
        cols = ("id", "addr", "en", "hits", "pass", "cond")
        text = "OK 2 id=0,addr=001000,en=1,hits=0,cond=;id=1,addr=002000,en=1,hits=0,cond="
        self.app._render_records(self.app.bp_tree, response("T", text), cols)
        self.app.bp_tree.selection_set("1")
        self.app._render_records(self.app.bp_tree, response("T", text), cols)
        self.assertEqual(self.app.bp_tree.selection(), ("1",))
        self.app._render_records(self.app.bp_tree, None, cols)
        self.assertEqual(len(self.app.bp_tree.get_children()), 2)

    def test_memory_selection_kept_until_base_changes(self):
        body = bytes(range(256))
        self.app._render_memory_response(Response("T", "OK", body), "MEM PEEK 001000 100")
        self.app.selected_memory_address = 0x1010
        self.app._render_memory_response(Response("T", "OK", body), "MEM PEEK 001000 100")
        self.assertEqual(self.app.selected_memory_address, 0x1010)
        self.app._render_memory_response(Response("T", "OK", body), "MEM PEEK 003000 100")
        self.assertEqual(self.app.selected_memory_address, 0x3000)

    def test_address_radio_reformats_entry_and_memory_without_truncating(self):
        self.app._render_memory_response(Response("T", "OK", b"A"), "MEM PEEK 001000 100")
        self.app.address_var.set("001000")
        self.app.address_bits_var.set(ADDRESS_BITS_32)
        self.app.choose_address_bits()
        self.assertEqual(self.app.address_var.get(), "00001000")
        self.assertTrue(self.app.memory_text.get("1.0", "1.8").startswith("00001000"))
        self.app.address_var.set("10000000")
        self.app.address_bits_var.set(ADDRESS_BITS_24)
        self.app.choose_address_bits()
        self.assertEqual(self.app.address_var.get(), "10000000")  # 範囲外は切り詰めない

    def test_mmu_reply_is_shown_in_register_pane_and_super_check_resends_peek(self):
        self.app.model.panes = {"mmu": response("T", "OK MODEL=68030 TC=00000000")}
        self.app._render_panes()
        self.assertIn("MODEL=68030", self.app.register_text.get("1.0", "end"))
        sent = []
        self.app._utility = sent.append
        self.app.memory_super_var.set(True)
        self.app.choose_memory_super()
        self.assertEqual(sent, ["MEM PEEK 000000 100 SUPER"])

    def test_delete_keys_send_once_and_clear_selection(self):
        cols = ("id", "addr", "en", "hits", "pass", "cond")
        self.app._render_records(self.app.bp_tree, response("T", "OK 1 id=1,addr=002000,en=1,hits=0,cond="), cols)
        self.app.bp_tree.selection_set("1")
        sent = []
        self.app._utility = sent.append
        # キー入力そのものはウィンドウのフォーカスに依存して不安定なので、bind登録と動作を分けて確認する。
        for tree in (self.app.bp_tree, self.app.wp_tree):
            for sequence in ("<Delete>", "<BackSpace>"):
                self.assertTrue(tree.bind(sequence), (tree, sequence))
        self.assertFalse(self.root.bind("<Delete>"))  # 入力欄の編集と衝突させない
        for _ in range(2):  # キーリピート相当
            self.app.delete_breakpoint()
        self.assertEqual(sent, ["BP DEL 1"])

    def _running_app(self):
        first = self.app.model.enable_refresh()
        self.app.model.complete_refresh(first.generation, [response("T1", "OK STATE=running CAP=NONE")])
        plans = []
        self.app._start_worker = lambda kind, payload: plans.append(payload)
        return plans

    def test_bp_add_or_cleanup_completion_refetches_lists_while_running(self):
        # Opus NB2/NB4: 実行中のBP操作・調査終了の完了後は、一覧を取り直す。
        # 成功時はその場で更新を始め、失敗時（サーバ側は適用済みのことがある）は取り直し待ちを立てる。
        ok = (("utility", "BP ADD 001000", response("T", "OK 0")),
              ("cleanup", None, [("BP CLEAR", response("T", "OK"))]))
        failed = (("utility_error", "BP ADD 001000", WireTimeoutError("busy")),
                  ("cleanup_error", None, WireTimeoutError("busy")))
        for event in ok:
            with self.subTest(kind=event[0]):
                self.app.model = DebuggerModel()
                plans = self._running_app()
                self.app.events.put(event)
                self.app._drain_events()
                self.assertIn("BP LIST", [r.command for plan in plans for r in plan.requests])
        for event in failed:
            with self.subTest(kind=event[0]):
                self.app.model = DebuggerModel()
                self._running_app()
                self.app.events.put(event)
                self.app._drain_events()
                self.assertTrue(self.app.model.lists_stale)

    def test_refresh_is_not_started_while_command_is_pending(self):
        # Opus N14/N15: コマンド応答待ちにutility完了や手動Refreshで更新を始めない。
        self._paused_app()
        started = []
        self.app._start_worker = lambda kind, payload: started.append(kind)
        self.app.step(1)
        self.assertEqual(started, ["command"])
        self.app.enable_refresh()
        self.assertEqual(started, ["command"])
        self.assertFalse(self.app.model.can_run())

    def test_manual_refresh_notifies_deferral_and_clears_it_after_command(self):
        # Opus NB1/NB2: Refresh ONボタンはmanual_refreshに結び付き、保留通知は応答後に消える。
        self._paused_app()
        started = []
        self.app._start_worker = lambda kind, payload: started.append(kind)
        self.assertIn("manual_refresh", str(self.app.refresh_button.cget("command")))
        self.app.step(1)
        plan = CommandPlan(self.app.model.generation, self.app.model.pending_command_token, Request("DBG STEP 1"))
        self.app.manual_refresh()
        self.assertEqual(started, ["command"])
        self.assertEqual(self.app.notice_var.get(), self.app._REFRESH_DEFERRED_NOTICE)
        self.app.events.put(("command", plan, response("T3", "OK STATE=paused PC=000102 CAP=NONE")))
        self.app._drain_events()
        self.assertNotEqual(self.app.notice_var.get(), self.app._REFRESH_DEFERRED_NOTICE)
        self.assertEqual(started, ["command", "refresh"])

    def test_memory_control_rows_fit_the_narrowest_pane(self):
        # 全部を1段に並べると右端のRead once/SUPERが見切れる（合計で約730px）。最小ウィンドウ幅での
        # 右ペインに収まる幅の2段に分けていること。macOSのTkは、同じプロセスで作り直したルートで
        # update系を呼ぶと落ちるため、各部品の要求幅の合計（+並べる余白の見積もり）で確かめる。
        pane = self.app.memory_text.master
        rows = [widget for widget in pane.winfo_children() if widget.winfo_class() == "TFrame"]
        self.assertEqual(len(rows), 2)
        texts = set()
        for row in rows:
            leaves = row.winfo_children()
            width = sum(leaf.winfo_reqwidth() for leaf in leaves) + len(leaves) * MEMORY_ROW_PAD_PER_WIDGET
            self.assertLessEqual(width, MEMORY_ROW_MAX_WIDTH, [leaf.winfo_class() for leaf in leaves])
            texts |= {leaf.cget("text") for leaf in leaves if leaf.winfo_class() != "TEntry"}
        self.assertEqual(texts, {"24bit", "32bit", "SUPER", "Read once", "Go (Ctrl+G)", "PC", "A7", "USP", "SSP"})

    def test_trace_tabs_are_separate_buttons_that_raise_their_page(self):
        self.assertEqual(tuple(self.app.trace_pages), TRACE_TABS)
        self.assertEqual(set(self.app.trace_texts), {"CALLRING", "PCRING", "REGRING"})  # Rawはリング行追従の対象外
        self.assertEqual(self.app.trace_tab_var.get(), "CALLRING")
        for name in TRACE_TABS:
            self.app.show_trace_tab(name)
            page = self.app.trace_pages[name]
            self.assertIs(page.master.winfo_children()[-1], page)  # 最前面（重なり順の最後）
            self.assertEqual(self.app.trace_tab_var.get(), name)

    def test_trace_text_is_still_written_to_each_tab(self):
        self.app._set_text(self.app.trace_texts["PCRING"], "pc ring")
        self.app._set_text(self.app.raw_trace_text, "raw ring")
        self.assertIn("pc ring", self.app.trace_texts["PCRING"].get("1.0", "end"))
        self.assertIn("raw ring", self.app.raw_trace_text.get("1.0", "end"))

    def _stop_with(self, stop_fields):
        """BP id=0 / WP id=0 を載せた一覧で、指定の停止情報を表示して選択状態を返す。"""
        self.app._render_records(self.app.bp_tree, response("T", "OK 1 id=0,addr=001000,en=1,hits=0,cond="),
                                 ("id", "addr", "en", "hits", "pass", "cond"))
        self.app._render_records(self.app.wp_tree, response("T", "OK 1 id=0,addr=002000,size=2,en=1,hits=0,last="),
                                 ("id", "addr", "size", "en", "hits", "last"))
        first = self.app.model.enable_refresh()
        self.app.model.complete_refresh(first.generation, [response("T1", f"OK STATE=paused PC=000100 CAP=NONE {stop_fields}")])
        self.app._stop_selected_key = None
        self.app._select_stop_location()
        return self.app.bp_tree.selection(), self.app.wp_tree.selection()

    def test_breakpoint_stop_selects_only_the_bp_row(self):
        self.assertEqual(self._stop_with("STOP=BREAKPOINT BP=0"), (("0",), ()))

    def test_watchpoint_stop_selects_only_the_wp_row(self):
        self.assertEqual(self._stop_with("STOP=WATCHPOINT WP=0"), ((), ("0",)))

    def test_other_stops_select_nothing_even_with_a_stray_wp_field(self):
        for stop in ("STOP=SNAPSHOT RINGS=CLEARED", "STOP=USER", "STOP=STEP", "STOP=SNAPSHOT WP=0"):
            with self.subTest(stop=stop):
                self.assertEqual(self._stop_with(stop), ((), ()))
                self.assertIsNone(self.app._stop_selected_key)

    def test_snapshot_save_load_need_pause_but_status_does_not(self):
        sent = []
        self.app._start_worker = lambda kind, payload: sent.append((kind, payload))
        self.app._refresh_widgets()
        self.assertEqual(str(self.app.snapshot_save_button.cget("state")), "disabled")
        self.assertEqual(str(self.app.snapshot_load_button.cget("state")), "disabled")
        self.app.snapshot_save()
        self.assertEqual(sent, [])  # 停止中でなければ送らない
        self._paused_app()
        self.app._refresh_widgets()
        self.assertEqual(str(self.app.snapshot_save_button.cget("state")), "normal")
        self.assertEqual(str(self.app.snapshot_load_button.cget("state")), "normal")
        self.app.snapshot_save()
        self.app.snapshot_status()
        self.assertEqual(sent, [("utility", SNAPSHOT_SAVE), ("utility", SNAPSHOT_STATUS)])

    def test_snapshot_load_asks_confirmation_and_resets_panes_on_reply(self):
        sent = []
        self.app._start_worker = lambda kind, payload: sent.append((kind, payload))
        self._paused_app()
        with unittest.mock.patch("tkinter.messagebox.askokcancel", return_value=False) as ask:
            self.app.snapshot_load()
        ask.assert_called_once()
        self.assertEqual(sent, [])  # キャンセルなら送らない
        with unittest.mock.patch("tkinter.messagebox.askokcancel", return_value=True):
            self.app.snapshot_load()
        self.assertEqual(sent, [("utility", SNAPSHOT_LOAD)])
        self.app._panes_current = True
        self.app._stop_selected_key = ("SNAPSHOT", 0xFF8EBC, None)
        self.app.events.put(("utility", SNAPSHOT_LOAD, response("T9", "OK STATE=paused STOP=SNAPSHOT RINGS=CLEARED PC=00FF8EBC")))
        self.app._drain_events()
        self.assertEqual(self.app.notice_var.get(), SNAPSHOT_LOAD_NOTICE)
        self.assertFalse(self.app._panes_current)
        self.assertIsNone(self.app._stop_selected_key)
        self.assertTrue(self.app.model.lists_stale)


if __name__ == "__main__":
    unittest.main()
