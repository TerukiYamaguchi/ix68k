# 68060 実行ループ高速化 設計

## 1. 目的と対象範囲

- この文書が定める対象:
  - 68060 モデルで IC/DC を有効にした実行時に、出力を変えずに 1 命令あたりのホスト処理量を減らす変更
  - 対象は `extern/cmc68000` の `execute_body` と、命令フェッチ高速経路
- 対象外・非目標:
  - サイクル計上、例外フレーム、バスエラーの再実行、IRQ の受付タイミング、診断機能の出力を変える最適化
  - 68000 / 68010 / 68020 / 68030 / 68040 の実行経路の変更
  - 060 のサイクル計算（`timing_060.c`）。コンパイラが呼び出しをすでに展開しており、出力を変えずに削れる処理が残らない
  - D キャッシュの 4 ウェイ探索の最適化（効果が測定誤差内のため行わない）
  - x68000 側（バス・周辺デバイス・描画・音声）の最適化。本設計で x68000 側が変わるのは `extern/cmc68000` の submodule pin の更新のみ
- 利用者または開発者に見える制約:
  - 変更の前後で、同一入力に対するゲストの観測可能な挙動とサイクル値が一致する
  - 削減量は精度を落とす案の合計より小さい。hard-overload の解消は保証しない

## 2. 実装状態

| 項目 | 状態 | 補足 |
| --- | --- | --- |
| 1. `execute_body` の固定費整理 | ✅ | mc68000 で実装済み |
| 2. 命令フェッチ高速経路の世代判定化 | ✅ | mc68000 で実装済み |
| 3. `cyc060_instruction` の簡略化 | ❌ | 実施しない（3 章） |
| x68000 側の submodule pin 更新 | ✅ | 変更 1・2 を含む mc68000 のコミットへ更新済み |
| 起動の回帰（NetBSD・Human68k）と BENCH の出力一致 | ✅ | NetBSD は 68060/50MHz のソースビルド版でログインプロンプトまで到達。BENCH の `GHOSTED.X` は 27358 バイト |

## 3. 設計の要点

- **同じ結果を安く得る変更だけを行う。** 出力が変わりうる省略は採らない。
- 実装順は 1 → 2 とし、1 つ実装するごとに測定して採否を決める。
- 世代判定（変更 2）は、無効化イベントの漏れが正しさを壊す。そのため、(a) 無効化を 1 つの関数に集約する、(b) 呼び出し元が多く漏れやすい比較（FC の S ビット）は世代に含めず毎回比較する、の 2 点で構造的に漏れを防ぐ。
- 採らない変更:

| 変更 | 採らない理由 |
| --- | --- |
| `irq_level` を 16 命令に 1 回だけ判定する | IRQ の受付が最大 15 命令遅れる |
| IRQ 判定関数のインライン可能化 | `x68k_bus_ioi_pending_irq` / `x68k_bus_dmac2_pending_irq` / `x68k_bus_dmac3_pending_irq` は `x68k_bus_irq_level` と同じ翻訳単位（`src/bus/x68k_bus.c`）にある。SCC・MFP は要求ビットを先に調べてから呼んでおり、削れるコストの根拠がない |
| `cyc060_instruction` の 3 段呼び出しの 1 段化 | `-O2` では `cyc060_instruction_raw` と、`cyc060_jump` / `cyc060_movem` / `cyc060_fpu` / `cyc060_fpu_movem` 以外の `cyc060_group*` が `cyc060_instruction` に展開されている。1 段化しても結果が変わらない |
| `cyc060_instruction` の opcode 表引き | サイクル値が `t060_ea_extra` などの実行時の状態に依存しない opcode は、グループ 7（MOVEQ）だけで、これは `switch` で即座に返っている。それ以外を表にするには分類表が要り、複雑さに見合わない |
| `cyc060_instruction` を opcode ごとにメモ化する | 分岐の成否などの状態に依存する命令でサイクル値がずれる |
| フェッチ高速経路で `cpu_note_access` と ATC `history` の更新を省く | `history` は ATC の置換順に効き、フォルト情報の記録も変わる |
| 診断フラグ判定やレジスタ退避を一括で外す | バスエラー再実行と診断出力が変わりうる |
| D キャッシュ読出しの MRU ウェイ先読み | 効果が測定誤差内 |

## 4. 構成とデータモデル

- 主な構造体・状態:
  - `mc68k_state_t`（`extern/cmc68000/src/cpu/cpu_internal.h`）。機種判定に使う `model`、`atc060_ifast`（フェッチ高速経路の検証値）、`atc060_i`、`icache040`、`icache040_last`、`atc060_clock`
  - 新設: フェッチ高速経路の世代カウンタ `ifast_gen`（`uint64_t`）と、`atc060_ifast` が検証時の世代を覚える `gen`
  - 新設: 世代カウンタを進める内部関数 `cpu_ifetch_fast_invalidate`
- 所有関係: すべて CPU コア（mc68000）が所有する。
- 更新タイミング:
  - 機種判定の結果（`is_68000` / `is_68040` / `is_68060` / `has_restart_isa` / `has_trace_t0`）: `execute_body` の冒頭でローカル定数として 1 回だけ評価する。モデルは生成後に変えられない（`mc68k_set_model` は存在しない）
  - 世代カウンタ: 5 章の無効化イベントで `cpu_ifetch_fast_invalidate` が呼ばれたとき。値は単調に増加し、戻さない
- サブシステム間の境界: 公開 API（`include/mc68000/`）は変えない。変更は `../cmc68000/` で行い、x68000 は submodule pin を更新する。

## 5. インターフェース／レジスタ／入力

公開 API・設定ファイル・MCP コマンドの変更はない。

世代カウンタを進める無効化イベントは、すべて `cpu_ifetch_fast_invalidate` を呼ぶ。

| イベント | 実装箇所 | 影響する前提 |
| --- | --- | --- |
| `MOVEC` による制御レジスタの書き込み（TC / ITT0 / ITT1 を含む） | `ops_system_ext.c` の `op_movec`（060 の書き込み側） | TC・ITT の比較 |
| ATC エントリの挿入・置換（PLPA を含む） | `ops_68060_mmu.c` のエントリ登録 | `atc060_i` のエントリ |
| PFLUSH 系（PFLUSH / PFLUSHN / PFLUSHA / PFLUSHAN） | `ops_68060_mmu.c` の `op_060_pflush` | `atc060_i` のエントリ |
| リセット | `mc68k_reset`（TC・ITT を更新する。ATC は変えない） | TC・ITT の比較 |
| スナップショット復元 | `mc68k_snapshot_commit` | すべて |

次は世代の対象にしない。フェッチごとに従来どおり確認するため、無効化イベントにならない。

- FC の S ビット。
- CACR の IE ビット（`cpu->cacr` をフェッチごとに読む）。
- I キャッシュラインの `valid` / `loaded` / `physical_line`。CINV / CPUSH やラインの置換は、この確認で検出される。
- 68060 に PTEST 命令はない（`ops_68060_mmu.c` の冒頭コメントのとおり）。

FC の S ビットについて: `cpu_060_ifetch_fast` は、`access->fc` と `atc060_ifast.fc_s` の比較を毎フェッチで行う。`cpu->sr` を `cpu_set_sr` を通さずに直接更新する経路（`cpu_exception_frame_origin_rte_commit`）があっても、S の変化は次のフェッチの比較で検出される。

## 6. 動作仕様

### 6.1 `execute_body` の固定費整理（変更 1）

- 機種判定（`cpu_is_68000` / `cpu_is_68040` / `cpu_is_68060` / `cpu_has_68010_isa` / `cpu_has_t0_trace`）は、命令ごとに評価せず、4 章のローカル定数を参照する。値は従来の判定と常に一致する。
- 68040 以外では結果が変わらない処理は、`is_68040` の条件の中に入れる。
  - `wb2.valid` を立てるのは `cpu_translate_write_access` だけで、`cpu_is_68040` のときに限る。`wb2_defer_ok` を true にするのは `execute_body` だけで、68040 のときに限る。68040 以外では、どちらも常に false である。
  - 対象は、`wb2.valid` のリセット、`wb2_defer_ok` の設定・解除、命令完了後の `wb2.valid` の判定。
- `cpu_prefetch_complete_instruction` は 68000 のときだけ呼ぶ。他の機種では `prefetch_flush = false` のみを行う関数なので、同じ値を直接書く。
- `call_ring` の BSR / JSR / RTS の判定は、opcode の上位 4 ビットが `$4` か `$6` のときだけ行う。記録の有無と内容は変わらない。
- 次は**省かない**。
  - 68010+ のレジスタ退避（`restart_d` / `restart_a` / `restart_sr`）。バスエラー再実行の巻き戻しが使う。
  - 診断リング（`pc_ring` / `pctrace1024` / `reg_ring` / `badori` / `badjump`）の判定と記録。
  - `call_ring` の常時記録。

### 6.2 命令フェッチ高速経路の世代判定化（変更 2）

- 変更前の `cpu_060_ifetch_fast` は、毎フェッチで次を比較する。
  - 論理ページ・`valid`・FC の S ビット
  - TC・ITT0・ITT1
  - ATC エントリの `valid`・`fc`・`page_shift`・`logical_page`・`cache_mode`
- このうち、**FC の S ビット、PC の論理ページ一致、CACR の IE ビット、ATC エントリの `cache_mode`、および I キャッシュラインの確認（`valid` / `loaded` / `physical_line`）は毎フェッチ行う**。残りの比較（TC・ITT0/1・ATC エントリの `valid` / `fc` / `page_shift` / `logical_page`）を、前回の検証時の世代と現在の世代の**一致 1 回**に置き換える。
  - `cpu_mmu060_ifast_prime` が `atc060_ifast` を作るとき、その時点の世代を `gen` に保存する。
  - 次のフェッチで世代が一致すれば、残りの比較を省いてエントリを直接使う。
  - 不一致なら従来の比較を行い、通ったら `gen` を現在の世代に更新する。通らなければ高速経路を使わない。
- 次の副作用は、従来の経路と同じ回数・同じ順序で行う。
  - `cpu_note_access`
  - ATC エントリの `history` 更新（`++atc060_clock`）
  - `access_cache_mode` と `access_tt_hit` の設定
  - IE=0 のときのバス読出しと待ち時間の計上、バスエラー
  - `icache040_last` の更新（同じセットの別ウェイに当たったとき）
- 5 章の無効化イベントのどれかが `cpu_ifetch_fast_invalidate` を呼ばないと、古いエントリで取り出す。無効化イベントの網羅が本変更の正しさの条件である。
- ATC エントリや TC などの内部状態を、ゲスト命令を介さずに直接書き換えても、世代は進まない。そのような書き換えは製品コードに存在せず、試験コードだけが行う。試験は 9 章のとおり `cpu_ifetch_fast_invalidate` を明示的に呼ぶ。
- 命令側 ATC のエントリを書くのは、`ops_68060_mmu.c` のエントリ登録と `op_060_pflush` の 2 箇所だけである。エントリ登録の直後には、命令フェッチの変換なら `cpu_mmu060_ifast_prime` が高速経路の記憶を作り直す。エントリ登録時の `cpu_ifetch_fast_invalidate` は、`prime` の対象にならない命令アクセス（alternate・RMW）で挿入が起きる場合のための安全策である。

### 6.3 スナップショットの復元

- 世代カウンタ `ifast_gen` を `snapshot_excludes` に追加し、復元で巻き戻さない。`snapshot_excludes` は offset 順に並べる。
- `model` は `snapshot_excludes` に含まれており、復元されない。`execute_body` のローカル定数は復元の影響を受けない。
- `atc060_ifast` はスナップショットの内容で上書きされる。`mc68k_snapshot_commit` は、`copy_except` の後に `cpu_ifetch_fast_invalidate` を呼び、復元後の最初のフェッチで必ず再検証させる。

## 7. 実装上の対応

| 役割 | 主な実装箇所 | 責務 |
| --- | --- | --- |
| 実行ループ | `extern/cmc68000/src/cpu/m68000.c` の `execute_body` | 機種判定のローカル定数化、68040 以外で不要な処理のスキップ |
| 命令フェッチ | `extern/cmc68000/src/cpu/cpu_internal.h` の `cpu_060_ifetch_fast`、`cpu_ifetch_fast_invalidate` | 世代判定化、無効化 |
| 高速経路の記憶 | `extern/cmc68000/src/cpu/ops_68060_mmu.c` の `cpu_mmu060_ifast_prime` | 検証時の世代の保存 |
| 無効化 | `ops_system_ext.c` の `op_movec`、`ops_68060_mmu.c` のエントリ登録と `op_060_pflush`、`m68000.c` の `mc68k_reset` | 世代カウンタの更新 |
| スナップショット | `extern/cmc68000/src/cpu/snapshot.c` の `snapshot_excludes`、`mc68k_snapshot_commit` | 世代の除外、復元後の無効化 |
| 分岐サイト一覧 | `extern/cmc68000/tests/data/cpu/mc68060_model_branch_sites.csv` | `execute_body` の機種判定の変更に合わせて再生成する |

変更は cmc68000 リポジトリ（`../cmc68000/`）で行い、x68000 は submodule pin を更新する。

## 8. 互換性・既知の制約

- 実機との差異: 本設計は挙動を変えないため、差異は増減しない。
- 参照実装との比較方針: 命令フェッチ高速経路は、既存の `atc060_ifast_disable` で高速経路を止めた実行と比較する。本番コードに二経路は持ち込まない。
- 削減量: macOS の IC/DC ON の GCC.X BENCH で、ホスト CPU 秒が変更前に対して約 3.7% 減る（変更 1 で約 1.8%、変更 2 で約 1.9%）。精度を落とす案も含めた合計の約 14% より小さい。hard-overload が消えるかは確認していない。
- Windows（MSVC）: コード配置とインライン化の変化で結果が大きく動くことが分かっている。macOS の結果を Windows の結果として扱わない。
- 後方互換性への注意:
  - スナップショットの形式は変えない。世代カウンタは `snapshot_excludes` に加えるため、スナップショットの内容には含まれない。
  - 試験が `atc060_i` や `mmu060_tc` などを直接書き換える場合は、書き換えの後に `cpu_ifetch_fast_invalidate` を呼ぶ必要がある。
  - `execute_body` の機種判定を変えると、分岐サイト一覧の CSV が現行の src と一致しなくなる。`extern/cmc68000/scripts/extract_model_branch_sites.py` で再生成する。

## 9. テストと受入条件

| 観点 | テスト | 期待結果 |
| --- | --- | --- |
| フェッチ経路の同一性と無効化（変更 2） | `test_68060_ifetch_fast`。各場面を高速経路あり・なしで実行し、PC・D0・サイクル・例外ベクタ・ATC の内容・`atc060_clock` を比較する。ゲスト命令で TC / ITT0 / ITT1 を変える場面（MOVEC）、PFLUSHA、TC.E の off→on、S の切替、reset を含む | すべて一致。古い割付を使わない |
| スナップショット（変更 2） | `test_68060_ifetch_fast` の復元後の比較。復元後の結果が高速経路あり・なしで一致し、`ifast_gen` が復元で後退しないこと | 一致。世代は進む |
| 既存のフェッチ回帰 | `test_68060_ifetch_line`、`test_68060_ifetch_fast` の、ATC エントリ・TC・ITT を直接書き換える場面は、書き換えの後に `cpu_ifetch_fast_invalidate` を呼ぶ。期待値と assert は変えない | 全件成功。テストの削除・スキップなし |
| 既存回帰 | mc68000 の全テスト。`-DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g"` で assert を有効にして実行する | 全件成功 |
| 分岐サイト一覧 | `test_design_060_traceability` | CSV が HEAD の src と一致する |
| 変異試験 | 無効化の呼び出しの削除（MOVEC・PFLUSH・reset・スナップショット復元）、`snapshot_excludes` からの除外取り消し、S 比較・`cache_mode` 確認・再検証時の `valid` 確認の削除、世代判定を常に一致にする変異 | 試験で検出される。エントリ登録時の無効化の削除は、6.2 のとおり通常の経路では結果が変わらないため検出されない |
| 起動の回帰 | 68060 / 50MHz で NetBSD（ソースビルド版）と Human68k の起動 | 変更前と同じ |
| 出力の一致 | IC/DC ON の GCC.X BENCH で `GHOSTED.X` が 27358 バイト | 変更前と同じ |
| 速度 | macOS、IC/DC ON の GCC.X BENCH。変更前と変更後のバイナリを別ビルドにし、プロセスを起動し直して交互に 3 回ずつ | ホスト CPU 秒の差と hard-overload を記録する。揺らぎは約 ±1% |

既存のフェッチ回帰で、直接書き換えの後に明示的な無効化を呼ぶ変更は、「内部状態を直接書き換えても毎フェッチで再検証される」ことの検証を、「無効化イベントが通知されたら再検証される」ことの検証に変える。直接書き換えの検出という性質は失われるため、ゲスト命令経由で各イベントを起こす場面で補う。

RTE が S を変える経路（`cpu_exception_frame_origin_rte_commit`）の専用テストは置かない。S は世代に含めず毎フェッチ比較するため、`cpu_set_sr` を通るかどうかに依らない。S 比較の削除は、`test_68060_ifetch_fast`・`test_68060_ifetch_line`・`test_68060_mmu` が検出する。

受入条件:

1. 上表のフェッチ経路・スナップショットの各テスト、既存回帰、分岐サイト一覧がすべて成功する。
2. BENCH の出力が変更前と一致し、起動の回帰が変更前と同じである。
3. 速度の測定結果を記録する。hard-overload が消えなかった場合は、削減量と残りの原因を記録し、本設計の範囲外の案（精度の一部を落とす案）は別途承認を得て検討する。

## 10. 根拠資料

- 一次資料: *MC68060 User's Manual* 第 4 章（MMU）、第 5 章（キャッシュ）
- ROM実測: なし
- 参照実装: 変更前のコア（`atc060_ifast_disable` による高速経路なしの実行）

## 11. 更新履歴

| 日付 | 変更内容 |
| --- | --- |
| 2026-11-12 | 設計の初版 |
| 2026-11-12 | 変更 1・2 を実装。変更 3 は実施しない方針に変更 |

---

## 記述方針

この文書は現行仕様の正本として、調査経緯・旧案・修正前後の比較を含めない。
