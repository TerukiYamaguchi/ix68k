/* ghosted.c */

/* GhostED (GED.X): MCPサーバから起動・操作するed風ラインエディタ（Issue #148）。
 * 設計: tools-x68k/ged/design_ghosted_editor.md
 *
 * Phase 1: バッファ構造体・ファイルロード/セーブ（w/q/q!）。
 * Phase 2: 画面レイアウト（ビューポート・モードライン・ステータス行・プロンプト行）
 * と l/g コマンド。
 * Phase 3: i/a（挿入モード）/d（削除）コマンド。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <doslib.h>

#define GED_MAX_LINES		8192		/* 行数上限 */
#define GED_MAX_LINE_LEN	1024		/* 1行の最大バイト数（超過分は切り詰め） */
#define GED_MAX_TOTAL_BYTES	(256 * 1024)	/* 全行内容の合計バイト数上限 */
#define GED_PATH_MAX		92		/* Human68k フルパス上限に合わせる */

#define GED_PROMPT_ROW		0	/* コマンド入力プロンプト（": "） */
#define GED_STATUS_ROW		1	/* 直前のコマンド結果（@GED OK/ERR ...） */
#define GED_MODE_ROW		2	/* モードライン（反転表示） */
#define GED_VIEW_START		3	/* バッファビューポート開始行 */
#define GED_VIEW_ROWS		28	/* バッファビューポート（行3～30） */
					/* 行31はHuman68k機能キー表示行、触らない。
					 * 入力域(プロンプト/ステータス/モードライン)を
					 * あえて画面先頭に置いている: 行31を除いた
					 * スクロール可能領域は行0～30で、fgetsのcooked
					 * 入力はEnterのたびに改行をエコーするため、
					 * カーソルがその領域の"最終行"にあるとHuman68k
					 * が自動スクロールしてしまう（実機で確認済みの
					 * 不具合）。プロンプトを最終行ではなく先頭行に
					 * 置くことで、境界行を踏まずに済む。 */


static char *lines[GED_MAX_LINES];	/* 行バッファ（固定配列、reallocしない） */
static int line_count;			/* 現在の行数 */
static long total_bytes;		/* lines[] が消費している合計バイト数 */
static int modified_flag;		/* =!0 なら未保存の変更あり */
static char cur_filename[GED_PATH_MAX];	/* 現在開いているファイル名 */
static int screen_cols;		/* 画面の桁数（96 または 64） */
static int cur_line;			/* カレント行（0-based） */
static int top_line;			/* ビューポート先頭行（0-based） */
static int insert_mode;		/* =!0 なら挿入モード中（単独の"."行で終了） */
static int insert_pos;			/* 挿入モード中、次に挿入する位置（0-based） */
static int insert_count;		/* 挿入モード中、このセッションで挿入した行数 */



/* カーソルを(row, col)（ともに0-based）へ移動する */
static void GedCursorTo (int row, int col)
{
	printf ("\x1b[%d;%dH", row + 1, col + 1);
}



/* 画面全体を消去する */
static void GedClearScreen (void)
{
	printf ("\x1b[2J");
}



/* カーソルのある行を消去する（カーソル位置は変わらない） */
static void GedClearLine (void)
{
	printf ("\x1b[2K");
}



/* モードライン用の水色帯のON/OFF（ハイライト・リバース水色、ESC[45m。
 * 文字を太字にする指定と水色リバースを1つの絶対値でまとめて設定する）。
 * GedClearLine()の前に呼ぶことで、消去範囲（行の右端まで）がそのまま
 * 水色の帯になる。 */
static void GedModeBarOn (void)
{
	printf ("\x1b[45m");
}

static void GedModeBarOff (void)
{
	printf ("\x1b[m");
}



/* 画面の桁数を判定する。DOSCALL _CONCTRL(MD=16)のラッパー_dos_c_width(-1)で
 * 現在の画面解像度モードを変更せず取得する。0/1(768x512)なら96桁、2～5
 * (512x512)なら64桁。ドキュメント外の値が返った場合は安全側(狭い方)へ
 * フォールバックする。 */
static void GedDetectScreenWidth (void)
{
	int mode = _dos_c_width (-1);

	if (mode == 0 || mode == 1)
		screen_cols = 96;
	else
		screen_cols = 64;	/* 2～5、またはドキュメント外の値のフォールバック */
}



/* カレントファイル名を更新する（Human68kフルパス上限に合わせて切り詰める） */
static void GedSetFilename (const char *path)
{
	strncpy (cur_filename, path, sizeof (cur_filename) - 1);
	cur_filename[sizeof (cur_filename) - 1] = '\0';
}



/* 1行分のテキストをバッファ末尾へ追加する。上限超過時は失敗する。 */
static int GedPushLine (const char *text, int len)
{
	char *p;

	if (line_count >= GED_MAX_LINES)
		return -1;
	if (len > GED_MAX_LINE_LEN)
		len = GED_MAX_LINE_LEN;	/* 超過分は切り詰める */
	if (total_bytes + len + 1 > GED_MAX_TOTAL_BYTES)
		return -1;

	p = malloc ((size_t) len + 1);
	if (p == NULL)
		return -1;

	memcpy (p, text, (size_t) len);
	p[len] = '\0';

	lines[line_count++] = p;
	total_bytes += len + 1;

	return 0;
}



/* pos（0-based）の位置へ1行挿入する。pos以降の既存行は後ろへずれる。
 * 上限超過時は失敗する（バッファは変更しない）。 */
static int GedInsertLineAt (int pos, const char *text)
{
	int len = (int) strlen (text);
	char *p;
	int i;

	if (line_count >= GED_MAX_LINES)
		return -1;
	if (len > GED_MAX_LINE_LEN)
		len = GED_MAX_LINE_LEN;	/* 超過分は切り詰める */
	if (total_bytes + len + 1 > GED_MAX_TOTAL_BYTES)
		return -1;

	p = malloc ((size_t) len + 1);
	if (p == NULL)
		return -1;
	memcpy (p, text, (size_t) len);
	p[len] = '\0';

	for (i = line_count; i > pos; i--)
		lines[i] = lines[i - 1];
	lines[pos] = p;
	line_count++;
	total_bytes += len + 1;
	modified_flag = !0;

	return 0;
}



/* from～to（0-based、両端含む）の行を削除する。範囲は呼び出し側が正規化済み
 * であることを前提とする。戻り値は削除した行数。 */
static int GedDeleteLines (int from, int to)
{
	int i;
	int count = to - from + 1;

	for (i = from; i <= to; i++) {
		total_bytes -= (long) strlen (lines[i]) + 1;
		free (lines[i]);
	}
	for (i = from; i + count < line_count; i++)
		lines[i] = lines[i + count];
	line_count -= count;
	modified_flag = !0;

	return count;
}



/* ファイルをロードしてバッファへ展開する。存在しなければ新規ファイル扱い（空
 * バッファ）とする。上限（行数／総バイト数）を超える場合は部分的に読み込まず
 * 即座にエラー終了する（プロセス終了によりHuman68kが確保済みメモリを回収する
 * ため、途中まで確保した行の後始末は行わない）。 */

/* Human68k EOF marker ($1A) at the end of a text file. */
static int GedHasTerminalEofMarker (void)
{
	return line_count > 0 && (unsigned char) lines[line_count - 1][0] == 0x1A &&
		lines[line_count - 1][1] == '\0';
}

static void GedLoad (const char *path)
{
	int fd;
	long size;
	char *buf;
	int n;
	long i;
	long line_start;

	GedSetFilename (path);

	cur_line = 0;
	top_line = 0;
	insert_mode = 0;

	fd = _dos_open (path, 0);
	if (fd < 0) {
		line_count = 0;
		total_bytes = 0;
		modified_flag = 0;
		return;		/* 新規ファイル扱い */
	}

	size = _dos_seek (fd, 0, 2);
	_dos_seek (fd, 0, 0);
	if (size < 0) {
		printf ("GED: ファイルサイズの取得に失敗しました\r\n");
		_dos_close (fd);
		_dos_exit2 (1);
	}
	if (size > GED_MAX_TOTAL_BYTES) {
		printf ("GED: ファイルが大きすぎます（上限 %d バイト）\r\n", GED_MAX_TOTAL_BYTES);
		_dos_close (fd);
		_dos_exit2 (1);
	}

	buf = malloc ((size_t) size + 1);
	if (buf == NULL) {
		printf ("GED: メモリ不足です\r\n");
		_dos_close (fd);
		_dos_exit2 (1);
	}

	n = _dos_read (fd, buf, (int) size);
	_dos_close (fd);
	if (n != (int) size) {
		printf ("GED: ファイルの読み込みに失敗しました\r\n");
		_dos_exit2 (1);
	}
	buf[size] = '\0';

	line_count = 0;
	total_bytes = 0;
	line_start = 0;
	for (i = 0; i <= size; i++) {
		int len;

		if (i != size && buf[i] != '\n')
			continue;

		len = (int) (i - line_start);
		if (len > 0 && buf[line_start + len - 1] == '\r')
			len--;

		if (i == size && len == 0 && line_start == i)
			break;	/* ファイル末尾の改行による余剰の空行は追加しない */

		if (GedPushLine (buf + line_start, len) < 0) {
			printf ("GED: ファイルが大きすぎます（上限 %d 行 / %d バイト）\r\n",
				GED_MAX_LINES, GED_MAX_TOTAL_BYTES);
			_dos_exit2 (1);
		}
		line_start = i + 1;
	}

	modified_flag = 0;
}



/* 開いているファイル名（またはpath指定）で保存する。戻り値は書き込みバイト数、
 * 失敗時は-1。 */
static long GedSave (const char *path)
{
	int fd;
	int i;
	long total;

	fd = _dos_create (path, 0);
	if (fd < 0)
		return -1;

	total = 0;
	for (i = 0; i < line_count; i++) {
		int len = (int) strlen (lines[i]);
		int n = _dos_write (fd, lines[i], len);

		if (n != len) {
			_dos_close (fd);
			return -1;
		}
		total += n;

		n = _dos_write (fd, "\r\n", 2);
		if (n != 2) {
			_dos_close (fd);
			return -1;
		}
		total += n;
	}

	_dos_close (fd);
	modified_flag = 0;

	return total;
}



/* fgetsで読んだ行末の改行文字(\r/\n)を取り除く */
static void GedTrimNewline (char *s)
{
	int len = (int) strlen (s);

	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
		s[len - 1] = '\0';
		len--;
	}
}



/* cur_line がビューポート内に収まるよう top_line を調整する */
static void GedClampScroll (void)
{
	if (cur_line < top_line)
		top_line = cur_line;
	if (cur_line >= top_line + GED_VIEW_ROWS)
		top_line = cur_line - GED_VIEW_ROWS + 1;
	if (top_line < 0)
		top_line = 0;
}



/* コマンド一覧（?コマンド用）。ビューポート領域へ1行ずつそのまま表示する。 */
static const char *const help_lines[] = {
	"l              redraw / show current line",
	"g <n>          go to line n",
	"i              insert before current line (end with '.')",
	"a              insert after current line (end with '.')",
	"d              delete current line",
	"d <n1> <n2>    delete lines n1-n2",
	"w [name]       save (optionally as name)",
	"q              quit (fails if unsaved changes)",
	"q!             quit and discard changes",
	"?              show this help",
};

#define GED_HELP_LINES ((int) (sizeof (help_lines) / sizeof (help_lines[0])))



/* モードライン（行2）を描画する。GedDrawScreen()・GedDrawHelp()の両方から呼ぶ。 */
static void GedDrawModeLine (void)
{
	int pct;
	char mode_buf[GED_PATH_MAX + 32];
	int mode_len;
	int pad;
	int i;

	pct = (line_count > 0) ? ((cur_line + 1) * 100 / line_count) : 0;

	mode_len = sprintf (mode_buf, "=%c=GhostED [%d%%] %s=",
			    modified_flag ? '*' : '=', pct, cur_filename);
	if (mode_len > screen_cols)
		mode_len = screen_cols;
	pad = screen_cols - mode_len;

	GedCursorTo (GED_MODE_ROW, 0);
	GedModeBarOn ();
	GedClearLine ();
	printf ("%.*s", mode_len, mode_buf);
	for (i = 0; i < pad; i++)
		putchar ('=');
	GedModeBarOff ();
}



/* 表示用に1行をcontent_width桁ぶんだけdstへ展開する。制御コード($00～$1F、
 * $7F)はCONSOLEドライバに解釈されてしまう（例: $1A=画面消去、$09=タブ移動）
 * ため、viの:set list慣習に倣い"^X"の2文字へ置き換える。SJIS2バイト目は
 * $40以上のためこの判定では壊れない。バッファ自体は書き換えない（保存内容は
 * 原文のまま）。 */
static void GedExpandLine (char *dst, const char *src, int width)
{
	int out = 0;
	int i;

	for (i = 0; src[i] != '\0'; i++) {
		unsigned char c = (unsigned char) src[i];

		if (c < 0x20 || c == 0x7f) {
			if (out + 2 > width)
				break;
			dst[out++] = '^';
			dst[out++] = (char) (c ^ 0x40);
		} else {
			if (out + 1 > width)
				break;
			dst[out++] = (char) c;
		}
	}
	dst[out] = '\0';
}



/* バッファのビューポート（行3～30）とモードライン（行2）を描画する。
 * ステータス行・プロンプト行はGedCmdLoop側で個別に扱う。 */
static void GedDrawScreen (void)
{
	int row;
	int content_width;

	content_width = screen_cols - 5 - 1;	/* "NNNN|" + "$" の分を引く */
	if (content_width < 1)
		content_width = 1;

	for (row = 0; row < GED_VIEW_ROWS; row++) {
		int idx = top_line + row;

		GedCursorTo (GED_VIEW_START + row, 0);
		GedClearLine ();

		if (idx < line_count) {
			char clipped[GED_MAX_LINE_LEN + 1];

			GedExpandLine (clipped, lines[idx], content_width);
			printf ("\x1b[31m%4d|\x1b[m%s\x1b[35m$\x1b[m", (idx + 1) % 10000, clipped);
		} else if (idx == line_count) {
			int i;

			for (i = 0; i < screen_cols; i++)
				putchar ('-');
		}
	}

	GedDrawModeLine ();
}



/* ?コマンド用のヘルプ画面をビューポートへ描画する（ファイル内容の代わりに
 * コマンド一覧を1行ずつ表示する）。モードラインは通常通り描画する。 */
static void GedDrawHelp (void)
{
	int row;

	for (row = 0; row < GED_VIEW_ROWS; row++) {
		GedCursorTo (GED_VIEW_START + row, 0);
		GedClearLine ();

		if (row < GED_HELP_LINES)
			fputs (help_lines[row], stdout);
	}

	GedDrawModeLine ();
}



/* コマンド入力ループ（l/g/i/a/d/w/q/q!/?）。
 * 各コマンドは処理結果をstatus_msgへ組み立てるだけにし、コマンド適用が
 * 終わった後で1回だけGedDrawScreen()し、ステータス行（行1）へstatus_msgを
 * 出す（適用前に描画すると結果が1コマンド遅れて表示されるバグになるため、
 * 描画は必ず「状態を更新し終えた後」に1回だけ行う）。プロンプト行（行0）には
 * ": "を出す。ステータス行とプロンプト行を別々の固定行に残すことで、
 * run_commandが「次のキー入力待ちに達した時点の画面」を取得しても結果が
 * 消えないようにする（tools-x68k/ged/design_ghosted_editor.md「行使用範囲・
 * モードライン・ステータス行・プロンプト行の配置」参照）。 */
static void GedCmdLoop (void)
{
	char cmdbuf[GED_MAX_LINE_LEN + 16];
	char status_msg[160];

	GedDrawScreen ();
	GedCursorTo (GED_STATUS_ROW, 0);
	GedClearLine ();
	printf ("@GED OK line=%d total=%d", cur_line + 1, line_count);

	for (;;) {
		int should_quit = 0;
		int show_help = 0;

		GedCursorTo (GED_PROMPT_ROW, 0);
		GedClearLine ();
		printf (": ");
		fflush (stdout);

		if (fgets (cmdbuf, sizeof (cmdbuf), stdin) == NULL)
			break;
		GedTrimNewline (cmdbuf);

		if (insert_mode) {
			if (strcmp (cmdbuf, ".") == 0) {
				insert_mode = 0;
				if (insert_count > 0)
					cur_line = insert_pos - 1;
				GedClampScroll ();
				sprintf (status_msg, "@GED OK inserted=%d total=%d", insert_count, line_count);
			} else if (GedInsertLineAt (insert_pos, cmdbuf) < 0) {
				sprintf (status_msg, "@GED ERR buffer full (max %d lines / %d bytes)",
					 GED_MAX_LINES, GED_MAX_TOTAL_BYTES);
			} else {
				insert_pos++;
				insert_count++;
				sprintf (status_msg, "@GED IN n=%d", insert_count);
			}
		} else if (strcmp (cmdbuf, "l") == 0) {
			sprintf (status_msg, "@GED OK line=%d total=%d", cur_line + 1, line_count);
		} else if (strcmp (cmdbuf, "?") == 0) {
			show_help = !0;
			sprintf (status_msg, "@GED OK help");
		} else if (cmdbuf[0] == 'g' && (cmdbuf[1] == '\0' || cmdbuf[1] == ' ')) {
			if (line_count == 0) {
				sprintf (status_msg, "@GED ERR empty buffer");
			} else {
				int n = (cmdbuf[1] == ' ') ? atoi (cmdbuf + 2) : cur_line + 1;

				if (n < 1)
					n = 1;
				if (n > line_count)
					n = line_count;
				cur_line = n - 1;
				GedClampScroll ();
				sprintf (status_msg, "@GED OK line=%d total=%d", cur_line + 1, line_count);
			}
		} else if (strcmp (cmdbuf, "i") == 0) {
			if (line_count >= GED_MAX_LINES) {
				sprintf (status_msg, "@GED ERR buffer full (max %d lines / %d bytes)",
					 GED_MAX_LINES, GED_MAX_TOTAL_BYTES);
			} else {
				insert_mode = !0;
				insert_pos = cur_line;
				insert_count = 0;
				sprintf (status_msg, "@GED OK insert mode (end with '.')");
			}
		} else if (strcmp (cmdbuf, "a") == 0) {
			if (line_count >= GED_MAX_LINES) {
				sprintf (status_msg, "@GED ERR buffer full (max %d lines / %d bytes)",
					 GED_MAX_LINES, GED_MAX_TOTAL_BYTES);
			} else {
				insert_mode = !0;
				insert_pos = (line_count == 0) ? 0 : cur_line + 1;
				if (GedHasTerminalEofMarker () && insert_pos == line_count)
					insert_pos = line_count - 1;
				insert_count = 0;
				sprintf (status_msg, "@GED OK insert mode (end with '.')");
			}
		} else if (cmdbuf[0] == 'd' && (cmdbuf[1] == '\0' || cmdbuf[1] == ' ')) {
			if (line_count == 0) {
				sprintf (status_msg, "@GED ERR empty buffer");
			} else {
				int n1, n2, deleted;

				if (cmdbuf[1] == '\0') {
					n1 = n2 = cur_line + 1;
				} else {
					char *endp;

					n1 = (int) strtol (cmdbuf + 2, &endp, 10);
					while (*endp == ' ')
						endp++;
					n2 = (*endp != '\0') ? (int) strtol (endp, NULL, 10) : n1;
				}

				if (n1 < 1)
					n1 = 1;
				if (n2 > line_count)
					n2 = line_count;

				if (n1 > n2) {
					sprintf (status_msg, "@GED ERR invalid range");
				} else {
					deleted = GedDeleteLines (n1 - 1, n2 - 1);

					if (cur_line >= line_count)
						cur_line = line_count - 1;
					if (cur_line < 0)
						cur_line = 0;
					GedClampScroll ();
					sprintf (status_msg, "@GED OK deleted=%d total=%d", deleted, line_count);
				}
			}
		} else if (strcmp (cmdbuf, "q") == 0) {
			if (modified_flag)
				sprintf (status_msg, "@GED ERR unsaved changes (use q!)");
			else
				should_quit = !0;
		} else if (strcmp (cmdbuf, "q!") == 0) {
			should_quit = !0;
		} else if (cmdbuf[0] == 'w' && (cmdbuf[1] == '\0' || cmdbuf[1] == ' ')) {
			const char *path = cur_filename;
			long n;

			if (cmdbuf[1] == ' ') {
				path = cmdbuf + 2;
				while (*path == ' ')
					path++;
			}

			n = GedSave (path);
			if (n < 0) {
				sprintf (status_msg, "@GED ERR save failed");
			} else {
				if (path != cur_filename)
					GedSetFilename (path);
				sprintf (status_msg, "@GED OK saved=%ld file=%s", n, cur_filename);
			}
		} else {
			sprintf (status_msg, "@GED ERR unknown command");
		}

		if (should_quit)
			break;

		if (show_help)
			GedDrawHelp ();
		else
			GedDrawScreen ();
		GedCursorTo (GED_STATUS_ROW, 0);
		GedClearLine ();
		printf ("%s", status_msg);
	}
}



int main (int argc, char **argv)
{
	if (argc != 2) {
		printf ("使い方: GED <ファイル名>\r\n");
		return 1;
	}

	GedLoad (argv[1]);
	GedDetectScreenWidth ();
	GedClearScreen ();

	GedCmdLoop ();

	GedClearScreen ();
	GedCursorTo (0, 0);

	return 0;
}
