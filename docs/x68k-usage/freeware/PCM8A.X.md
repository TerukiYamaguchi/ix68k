---
exe: PCM8A.X
種別: freeware
作者: philly さん
版: version 1.02（`pcm8a102.lzh`収録、69958 bytes、1997-09-02）
出典: 配布元 http://retropc.net/x68000/software/sound/adpcm/pcm8a/ （X68000 LIBRARY）／同梱 `PCM8A.DOC`
判別: -（配布アーカイブ pcm8a102.lzh 収録版のみ確認）
検証状態: ✅ SYSTEM_HD.HDSの`\BIN`へ導入・AUTOEXEC.BAT登録・起動時の常駐をix68k確認済み
  （バナー "X68k PCM8A polyphonic ADPCM driver v1.02 (c)1993-97 philly"）。個別スイッチは未検証
最終更新: 2026-08-19
---
# PCM8A.X

> リアルタイムADPCM多重再生ドライバ。`PCM8.X`相当の多重再生に`MPCM.X`相当のチャンネル毎の音程・音量指定を加えた常駐型ドライバで、68030以上のMPU（040turbo/060turbo含む）にも対応します。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式:

```
PCM8A [スイッチ]
```

スイッチは以下（`PCM8A.DOC`「使用方法」節参照、未検証）。誤ったスイッチを与えると
ヘルプを表示します。

常駐後に単体で実行して状態を切り替えるもの:

| コマンド | 効果 |
|---------|------|
| `PCM8A ON` | 多重再生を有効化する（既定状態） |
| `PCM8A OFF` | 多重再生を無効化する |
| `PCM8A FNC` | `trap #2`のファンクションコール経由のみ多重再生を許可し、IOCS経由は単音再生に制限する |
| `PCM8A CLR` | 動作モードの設定は変えずに全チャンネルをクリアする |
| `PCM8A -R` | 常駐を解除します。他の常駐アプリが解除を禁止していると失敗します。`CONFIG.SYS`側で組み込んだ場合はメモリ上には残ったまま解除状態になる |

表示・出力の切り替え（`num`省略時は現在値を反転）:

| スイッチ | 意味 |
|---------|------|
| `-D[num]` | 動作状態の常時表示。`0`=非表示（既定）、`1`=表示 |
| `-V[num]` | 音量の扱い。`0`=固定（原音の音量のまま出力）、`1`=可変（既定） |

チャンネル・組み込みに関する指定:

| スイッチ | 意味 |
|---------|------|
| `-Inum` | IOCS経由で使えるチャンネル数の上限（`0`〜`9`、既定`9`）。`0`にするとIOCS出力を止める |
| `-N` | `DEVICE=`でドライバとして組み込む際のデバイス名を`@PCM`にする（コマンドライン常駐時は無視される） |

リソース割り当て（値は10進、`$`または`X`を頭に付けると16進として解釈）:

| スイッチ | 意味 |
|---------|------|
| `-S[n1],[n2],[n3],[n4]` | n1=最大チャンネル数（1〜250、既定25）、n2=処理バイト数を12で割った値（1〜12、既定4）、n3=音量下限（0〜15または`$40`〜`$A0`、既定`$40`）、n4=音量上限（同、既定`$A0`）。値を省略して`-S`単体で指定すると現在の設定値を表示する |
| `-Wnum` | 常駐用のバッファ・テーブル領域をMALLOCで確保します。`num`のビット0〜4で対象を選ぶ（下表）。コマンドライン常駐時のみ有効で、Xellent30ローカルSRAMや16MB超のハイメモリ上に常駐させたい場合に使う |
| `-Mnum` | ADPCMのクロックアップモードを指定する（値の詳細は`PCM8AVUP.DOC`） |
| `-Fnum` | 動作基準周波数を指定する（値の詳細は`PCM8AVUP.DOC`） |

`-W`のビットが指すMALLOC対象:

| ビット | 対象 | 必要バイト数の目安 |
|-------|------|------------------|
| 0 | PCMバッファ | 処理バイト数 × 16 |
| 1 | ADPCMバッファ | 処理バイト数 × 6 |
| 2 | チャンネルバッファ | 全チャンネル数 × 128 |
| 3 | PCM→ADPCM変換テーブル | 103168（固定） |
| 4 | ADPCM→PCM変換テーブル | (音量設定数 + 97) × 1K |

Xellent30ローカルSRAMや16MB超のハイメモリに常駐させる構成では、ADPCMバッファ
（ビット1）は必ずMALLOC確保側に含める必要がある（そうしないと常駐に失敗する）。

CONFIG.SYSから組み込む場合は`DEVICE=`指定が可能で、デバイス名は`PCM`（`-N`指定時は`@PCM`）
になる（📖 `PCM8A.DOC`「使用方法」）。

### 使用上の制約・トレードオフ

`PCM8A.DOC`の記述を踏まえた実運用上の注意点:

- 処理を単純化して高速化した分、`PCM8`（v0.48系）より音質は劣る。音質を優先するなら
  `PCM8`または`MPCM`を選ぶ
- MPCM互換の音程変換モードはCPU負荷が高く、無印68000機ではまともに動きません。負荷を
  下げたい場合は「`-S`で処理単位を大きくする／チャンネル数を絞る」「`-F`で出力周波数を
  下げる」「`-V0`で音量演算を止める」のいずれかを試す
- `-S`は常駐後にも再指定できるが、常駐時に確保した領域より広いワークエリアを要求する
  変更（チャンネル数を増やす・処理単位を増やす・音量下限を下げる・音量上限を上げる）は
  できません。`-S`を省略して常駐した場合は`-S32,12,$40,$A0`相当の領域が確保されます。
  常駐後もチャンネル数を25未満には減らせない
- PCM8向けの制御コマンド（`PCM8 OFF`・`PCM8 -R`等）はPCM8A常駐中でもそのまま通る
  （両ドライバの制御インターフェースが共通なため）
- 10MHz機ではラスター割り込みと同時に使うと不具合が出やすいので避けたほうがよい

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/sound/adpcm/pcm8a/ から `pcm8a102.lzh`（93.0KB）。
収録物は `PCM8A.X` / `PCM8A.DOC` / `PCM8ATEC.DOC`（テクニカル資料）/ `PCM8AVUP.DOC`
（クロックアップ・動作基準周波数）/ `PCM8A102.HAS`（ソース）。

**`arc/retropc.net/x68000/software/sound/adpcm/pcm8a/pcm8a102.lzh` にローカル保存済み**
（2026-08-18取得）。再インストール・再検証時はretropc.netへ再アクセスせずこちらを使います。

### インストール（SYSTEM_HD.HDSへの導入手順）

```sh
# 1. ダウンロード（arc/ に無い場合のみ。あるならこの手順は飛ばす）
curl --insecure -L --create-dirs \
  -o "arc/retropc.net/x68000/software/sound/adpcm/pcm8a/pcm8a102.lzh" \
  "http://retropc.net/x68000/software/sound/adpcm/pcm8a/pcm8a102.lzh"

# 2. 展開（unar、要 brew install unar）
unar -e SHIFT-JIS -o ./pcm8a_extracted \
  "arc/retropc.net/x68000/software/sound/adpcm/pcm8a/pcm8a102.lzh"
# → ./pcm8a_extracted/pcm8a102/{PCM8A.X, PCM8A.DOC, PCM8ATEC.DOC, PCM8AVUP.DOC, PCM8A102.HAS}

# 3. 本体をHDSの\BINへコピー（hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./pcm8a_extracted/pcm8a102/PCM8A.X /BIN

# 4. ドキュメントを\DOC\PCM8A\へ配置（ソース PCM8A102.HAS は除く）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/PCM8A
for f in PCM8A.DOC PCM8ATEC.DOC PCM8AVUP.DOC; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "./pcm8a_extracted/pcm8a102/$f" /DOC/PCM8A
done

# 5. AUTOEXEC.BATへ常駐登録（オプションなし。ZMUSIC.Xより先に常駐させるため
#    "PCM8A" は "ZMUSIC" より前の行に置く）
tools-host/hu68disk/hu68disk extract disk/SYSTEM_HD.HDS /AUTOEXEC.BAT ./AUTOEXEC.BAT.tmp
# 末尾（EOFマーカー$1Aの直前）に "PCM8A\r\n" を追記してから書き戻す（改行はCRLF）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./AUTOEXEC.BAT.tmp /AUTOEXEC.BAT
```

AUTOEXEC.BATにはオプションなしで`PCM8A`→`ZMUSIC`の順に登録している（[ZMUSIC.X](ZMUSIC.X.md)が
ADPCMを多重発声するにはPCM8系ドライバが先に常駐している必要があるため）。常駐サイズは標準
370〜400KBと大きい点に注意（`-S`でチャンネル数・処理バイト数を絞ると縮む）。
バンドル導入手順は[../README.md](../README.md)「音源ドライバ一式」を参照。

### 検証メモ

- 2026-08-18: `pcm8a102.lzh`を`arc/`へ取得し、`PCM8A.X`を`disk/SYSTEM_HD.HDS`の`\BIN`へ、
  ドキュメント3点を`\DOC\PCM8A\`へ配置（`hu68disk list`で存在確認済み）。同日、AUTOEXEC.BAT
  末尾（EOFマーカー直前）に`PCM8A`／`ZMUSIC`の2行をCRLFで追記（484→499 bytes）。
- 2026-08-18: エミュレータ起動でAUTOEXEC.BAT経由の常駐を確認。`PCM8A`のバナー
  "X68k PCM8A polyphonic ADPCM driver v1.02 (c)1993-97 philly" 表示後、続く`ZMUSIC`が
  "ADPCM is managed by PCM8.X (C)H.ETOH" と表示し、PCM8AをPCM8互換ドライバとして
  認識していることを確認（`C:¥>`プロンプトまで正常到達、エラーなし）。
- **未検証**: 各スイッチ（`-S`/`-V`/`-I`/`-W`/`-M`/`-F`等）の動作、実際のADPCM多重再生。
