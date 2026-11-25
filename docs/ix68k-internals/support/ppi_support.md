# PPI (μPD8255) / ジョイスティック 実装状況

`src/bus/x68k_bus.c`（ポートA/B読み出し）+ `src/peripheral/adpcm.c`（ポートC・制御ワード）+
`src/platform/platform_sdl3_input.c`（ホスト入力）。ポートC下位4bitはADPCMのPAN/レート選択に
使われる（`docs/ix68k-internals/support/adpcm_support.md`参照）。

> **必須**: PPIのレジスタ・ジョイスティック入力経路を実装・変更したら、このファイルを必ず更新する。

---

## レジスタ単位の存在監査（2026-10-31）

原典: 『Inside X68000』「ジョイスティック」章・図36/37。

| レジスタ | アドレス | R/W | 実装状況 |
|---|---|---|---|
| ポートA（ジョイスティック#1） | $E9A001 | R | ✅ `joy_port[0]`をそのまま返す。初期値$FF（未接続） |
| ポートB（ジョイスティック#2） | $E9A003 | R | ✅ `joy_port[1]`をそのまま返す |
| ポートC | $E9A005 | R/W | 🔧 読み書き可。bit3-0（レート/PAN）・bit7-4（操作禁止・IOA6/IOA5）は使用 |
| 制御ワード（ビットセット/リセット） | $E9A007 | W | ✅ bit7=0 |
| 制御ワード（モード設定） | $E9A007 | W | ❌ bit7=1は無視（X68000は起動時固定のため実害低） |
| 制御ワード読み出し | $E9A007 | R | 常に0（8255は書き込み専用） |
| ポートA/B書き込み | $E9A001/3 | W | 無視（入力ポートのため実害低） |

### ポートA/Bのビット配置

| bit | 内容 | 実装 |
|---|---|---|
| 0-3 | 上/下/左/右（0=押下） | ✅ DPAD |
| 4 | 常に1 | ✅ |
| 5 | トリガA（0=押下） | ✅ ゲームパッドSOUTH |
| 6 | トリガB（0=押下） | ✅ ゲームパッドEAST |
| 7 | 常に1 | ✅ |

### ポートC上位4bit（ジョイスティック制御）

| bit | 内容 | 実装 |
|---|---|---|
| 4 | PC4: #1操作禁止（1=禁止） | ✅ 1の間ポートA読み出しは$FF |
| 5 | PC5: #2操作禁止（1=禁止） | ✅ 1の間ポートB読み出しは$FF |
| 6 | IOA5: #1トリガA信号をLowにする | ✅ 1の間ポートA bit5=0 |
| 7 | IOA6: #1トリガB信号をLowにする | ✅ 1の間ポートA bit6=0 |

## 既知の未実装

1. サイバースティック等のストローブ型拡張デバイス（PC4/PC6/PC7の駆動と読み出しタイミングの検証が未実施）
2. IOA5/IOA6とPC4（操作禁止）の優先関係は資料に記述が無く、禁止中でもIOAが勝つ実装にしている（実機/XEiJ未確認）
3. 8255モード設定コマンド（bit7=1）
4. IOCS `_A_JOYGET`（0xF2）は自前ハンドラ無し。ROMには実装が無く、AJOY.X（アナログジョイスティックドライバ）
   が常駐して追加するコール。機能番号0=データ読込、1=モード変更、2=通信速度変更
   （仕様の詳細は`iocs_support.md`の0xF2行）。ハード側の通信プロトコルは手元の資料に記述が無い
5. キーボードからのジョイスティック入力経路は未確認（ゲームパッドとMCP `JOY SET/RELEASE`のみ確認）

## 入力経路

- ホスト: `pal_joystick_read(index)`（SDL Gamepad。MCP `JOY SET`で注入した値が優先）
- `main.c`が毎フレーム`x68k_bus_set_joystick()`で`joy_port[]`へ流し込む
- IOCS `_JOYGET`（0x3B）はROM委譲で、ROMドライバがPPIを直接読む（d0.b=ポートA/Bの生値）

### MCP注入

- コマンド: `JOY SET <index> <hex>` / `JOY RELEASE <index>`（値はPPIポートA/B形式、アクティブロー）
- ツール: `joystick_set(index, up, down, left, right, trigger_a, trigger_b)` / `joystick_release(index)`
- 注入中は実コントローラより優先する（`pal_joystick_inject`/`pal_joystick_release`）
- テスト: `tests/joystick_ppi_test.c`（未接続時$FF・反映・範囲外index・ポートC非干渉）

### 物理パッド対応（SDL3）

- SELECT/START: 4方向+2ボタンのポートで表す慣例に従い、ゲームパッドの BACK(SELECT)を上下同時、START を左右同時としてポートA/Bへ出す（`pal_joystick_read`）。出典: [Qiita「ZUIKI X68000 Z XVI の操作方法」脚注](https://qiita.com/ht_deko/items/b3d9bf584921b79b9236)。TOWNSパッドは配線が異なる。方向レバー操作だけでSELECT/STARTを誤検出する不良パッド向けに、`ix68k.cfg` の `joystick_select_start = false` で合成を無効にできる（既定true）

- ゲームパッドの割り当て: ホストのゲームパッドは列挙順に最大8台まで開き、起動ログに `Gamepad#N connected: <名前>` と出す。既定は #1→Gamepad#1、#2→Gamepad#2。`ix68k.cfg` の `joy1` / `joy2`（0=自動、1〜8=N）で、X68000 のジョイスティック#1/#2 に使う Gamepad を指定できる。SDL の列挙順は接続順と一致しないことがある（macOS で VID 0x0810/PID 0x0001、SDL名 Twin USB Joystick の2台は、先に繋いだ側が Gamepad#2 になった）。起動時の列挙と ADDED イベントで同じデバイスを二重に開かない

- `SDL_HINT_JOYSTICK_MFI=0`: macOSのGameController.frameworkがHID記述子の非準拠パッドを専有し、
  SDLから見えなくなるのを回避する
- `extern/SDL_GameControllerDB`（mdqinc）を`pal_init()`で自動読込（`gamecontrollerdb.txt`を実行ファイル横へコピー）
- `gamecontrollerdb_user.txt`（実行ファイル横、任意、配布物に含めない）で同GUIDのマッピングを上書きできる
- `X68K_JOY_DEBUG=1`で生のボタン/軸/ハット入力をログ出力する（未知パッドの調査用）
- 注意: クローンチップのVID:PID `0810:0001`（Twin USB Joystick相当）は汎用IDで、配線の異なる
  個体が同じGUIDを共有する。DBのマッピングと実測がずれる場合は上記`user.txt`で上書きする
- 動作確認: XSP_010サンプル（`JOYGET(0)`で方向ビットを読みスプライト移動）でMCP注入により確認済み
