# FPU-CIR（CZ-6BP1/CZ-6BP1A, MC68881）実装状況

`src/bus/fpu_cir.c`/`fpu_cir.h`（CIR adapter）+ `src/bus/x68k_bus.c`（window デコードと
word/long の振り分け）。FPU の状態機械・演算・state frame は mc68000 の公開 API
（`extern/cmc68000/include/mc68000/fpu.h`）が所有し、ix68k は MMIO と API の間を変換するだけ。
アドレスマップ全体は `docs/ix68k-internals/addrmap.md`。

> **必須**: CIR の振り分け・応答符号化・転送規則を実装・変更したら、このファイルと
> 設計書 §5 の該当箇所を必ず更新します。

## 有効化

既定は `fpu = off` で、`$E9E000-$E9FFFF` 全域が CPU バスエラーになります。68000 で `fpu = on` を指定すると
primary の CZ-6BP1 を接続します。`fpu = off` のときは
`$E9E000-$E9FFFF` 全域が CPU バスエラーになります。

| 設定 | 設定ファイル | CLI | 既定 |
|---|---|---|---|
| FPU | `fpu = off\|on\|68881\|68881-secondary` | `-fpu off\|on\|68881\|68881-secondary` | off |

`on` と `68881` は primary（`$E9E000-$E9E01F`）、`68881-secondary` は secondary
（`$E9E080-$E9E09F`、構成・境界テスト専用）を選びます。CLI は設定ファイルを上書きします。
MC68881 の生成に失敗したら起動を失敗させます。
有効化は `x68k_bus_create()` と CPU 配線の間に一度だけ行います。選択外の XB1 は有効時もバスエラー。

## アクセスの扱い

- 偶数アドレスの word と long（long は offset と offset+2 の二つの word cycle）だけが CIR に届きます。
  `+$1E` からの long は 2 cycle 目が window 外（`+$20`）でバスエラー（1 cycle 目は完了済み）。
- byte アクセスと奇数アドレスの word/long は FPU API を呼ばず、read は全1、write は無視。
- 裏口（MCP・デバッガ・IOCS の `x68k_bus_read8/peek8/write8`）も FPU API を呼ばず、read は全1。
- ユーザモードのスーパーバイザ保護違反でバスエラーになる cycle では、デバイスを呼びません。
- 方向違い・予約位置は bus error にしない（read は全1、write は無視）。register select CIR への
  write だけは protocol violation になります。

## CIR 単位の実装状況

| offset | 方向 | 動作 |
|---|---|---|
| `+$00` | R | 応答 primitive。command/condition write 直後の最初の read だけ `$8900`（busy）、以降は `read_response()` を符号化 |
| `+$02` | W | 値を問わず ABORT。部分入力・転送記述子・busy を破棄 |
| `+$04` | R | save。`begin_save()` の format word。有効な frame なら body を operand CIR の word ストリームで読む |
| `+$06` | R/W | restore。W は `begin_restore()`（`$0000` は null frame の reset）。R は直近の readback（未開始は全1） |
| `+$0A` | W | command word |
| `+$0E` | W | condition predicate（下位6bit） |
| `+$10`/`+$12` | R/W | operand。同一 CIR の word ストリーム（B/W は 1 word、他は 2 word を 1 単位） |
| `+$14` | R | register select mask |
| `+$18`/`+$1A` | W | instruction address。上位→下位の 2 word で 1 回 |
| `+$08,$0C,$16,$1C,$1E` | - | 予約。read 全1、write 無視 |

応答 primitive は 881UM Table 7-7 / Figure 7-7 の符号化（NULL、EA transfer、MP register、FP registers、
take pre/mid exception）。転送記述子は残り byte 数だけを持ち、転送 response・save・restore が設定します。
部分入力の破棄規則（control/restore/有効 save/別 CIR access/方向切替）は設計書 §5。

## reset 経路

`$E9E006` への `$0000` 書込み（null frame restore）が `FPn/FPCR/FPSR/FPIAR` を reset します。`_B_INTVCS` の
D1=`$1FF` で登録されるドライバのハンドラも同じ書込みを行うだけで、ix68k は D1=`$1FF` を特別扱いしません。
`FLOAT3.X` 常駐時は IOCS ベクタ `$07FC` がドライバ内の reset ハンドラ（`move.w #0,$E9E006` →
`move.w $E9E006,d0` → 元の `_ABORTJOB` ハンドラへ `jmp`）を指し、白帯の「中止」で到達する
（E2E テストで確認済み。ハンドラの 2 命令後、D0 は null frame の readback の 0）。
CPU の `RESET` 命令と CZ-6BP1 の RESET 端子の結線は未確認のため、FPU の reset には接続していません。

## 検証

| テスト | 内容 |
|---|---|
| `fpu_cir_window_test` | 無効/primary/secondary の window 境界、裏口、生成失敗 |
| `config_test` | 設定ファイル/CLI の解決 |
| `fpu_cir_test` | 実 MC68881 モデルでの応答 raw word、busy、operand ストリーム、save/restore、FMOVEM、部分入力の破棄、byte/奇数アクセス、ユーザモード保護 |
| `fpu_cir_float3_test` | 純正 `FLOAT3.X` の静的逆アセンブル由来の CIR 列（変換、加減乗、FPSR の `$A800` → `and.w #$00D0`/`#$00F0` → CCR の経路、倍精度の単項関数 15 ルーチン（TAN/ATAN/SIN/COS/LOGN/ETOX/SQRT/SINH/COSH/TANH/ATANH/ASIN/ACOS/LOG10/LOG2）、`FMOVECR #$00`（π）× x、引数の特殊値で結果を FPU を呼ばず決める経路の reset）、ボードなしの FPU 検出（CPU バスエラー）、2 つの reset 経路 |
| `test_fpu_cir_float3_e2e.py`（ctest `fpu_cir_float3_e2e`） | headless ix68k で `disk/FLOAT3.XDF` から起動し、FLOAT3.X の常駐、`$07FC` の reset ハンドラと白帯の中止での連鎖（D0=`$FF` で停止、2 命令後に D0=0）、BASIC の `SIN(1)`・`1.5*2.5`・`SQR(2)`・`EXP(1)`・`LOG(2)`・`COS(1)`・`TAN(1)`・`ATAN(1)` が FLOAT3.X の対応ルーチン（FSIN／FMUL.D／FSQRT／FETOX／FLOGN／FCOS／FTAN／FATAN）を経て正しい値を返すこと、BASIC に無い SINH/COSH/TANH/ATANH/ASIN/ACOS/LOG10/LOG2 と `FMOVECR #$00`（π）を、Line-F の FExx を直接呼ぶ `.X`（`tools-x68k/asmtest/float3_funcs_test.s`）で確認すること（`m68k-xelf-gcc`・`elf2x68k`・`hu68disk` が無ければこの項目だけ SKIP）、ボードなしで `¥SYS¥FLOAT3.X` の登録に失敗すること（漢字の文言は画面テキストから読めないため見ない）を確認します。`disk/FLOAT3.XDF`・ROM・`sram.dat`・ヘッドレスのビルドが無ければ SKIP |

`disk/FLOAT3.XDF` は `tools-host/dev/make_float3_xdf.sh` が `disk/HUMAN302.XDF` から生成する（CONFIG.SYS の
`\SYS\FLOAT2.X` を `\SYS\FLOAT3.X` に置換するだけで、FLOAT3.X は HUMAN302.XDF 内のものを使う）。
`disk/` は git 管理外なので、必要になったら再生成します。

## 未確認・制限

- FLOAT3.X が使う `FMOVECR` の定数は `$00`（π）と `$32`（1.0）だけで、どちらも検証済み。
- BASIC2 には SINH/COSH/TANH/ATANH/ASIN/ACOS/LOG10/LOG2 に当たる関数が無い（構文エラー）。これらは
  FExx を直接呼ぶ `.X` で、exact に表せる値（0・1・2・3・π/2）の結果だけを確認しています。
  exact でない引数（例: `SINH(1)`）の結果は未確認です。

- 実機のバイト lane、busy の回数・時間、物理 RESET 端子の結線は未測定。`busy_once` は純正ドライバの
  ポーリング契約を満たす互換モデルで、物理遅延を再現しません。
- MC68882、X68030 の FPCP、CZ-6BP2、`$E9E080` の純正ドライバ回帰は対象外です。
- save 拒否（転送中の入れ子 save）の判定は、format word の上位 byte が `$02` かつ `body_bytes==0` である
  （下位 byte は使わない）。モデルが拒否時に返す内部規約（mc68000 の `$0200` と body 0）に依存し、
  公開ヘッダには無い。
