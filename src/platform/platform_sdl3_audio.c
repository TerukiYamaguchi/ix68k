/* glibc で posix_openpt 系を宣言させるため、ヘッダより前に定義する。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform/platform_sdl3_internal.h"

#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

SDL_AudioStream *g_audio_stream = NULL; /* ADPCM音声出力 */
int g_audio_channels = 2;
int g_audio_started = 0;
SDL_AudioStream *g_capture_stream = NULL; /* マイク入力(録音) */
int g_serial_fd = -1;                     /* RS-232C(SCC ch A)ホスト経路 */

/* SDL_INIT_AUDIO + SDL_AudioStream(SDL_OpenAudioDeviceStream, callback=NULL)
 * によるADPCM音声出力。callback未登録のストリームはSDL_PutAudioStreamDataで
 * 外部からプッシュするモデルになり、SDL2のSDL_QueueAudioと同様メインループの
 * フレームペーシングとそのまま親和する。 */
int pal_audio_init(int sample_rate_hz, int channels) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        pal_log("SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = sample_rate_hz;
    spec.format = SDL_AUDIO_S16;
    spec.channels = channels;
    /* SDL3のSDL_AudioSpecにはsamplesフィールドが無いため、デバイスの
     * バッファサイズ（レイテンシ/アンダーラン特性）はヒントで指定する。
     * SDL_OpenAudioDeviceStreamより前に設定しないと効かない。 */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
    g_audio_stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_audio_stream) {
        pal_log("SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return 0;
    }
    g_audio_channels = channels;
    g_audio_started = 0; /* デバイスは開いた直後pausedのまま維持する。以下参照 */
    return 1;
}

int pal_audio_available(void) {
    return g_audio_stream != NULL;
}

int pal_audio_queued_frames(void) {
    if (!g_audio_stream || g_audio_channels <= 0)
        return -1;
    int queued = SDL_GetAudioStreamQueued(g_audio_stream);
    if (queued < 0)
        return -1;
    return queued / (g_audio_channels * (int)sizeof(int16_t));
}

/* 事前バッファリング目標: 約200ms分（44100Hz・2ch・int16）。メインループは
 * 論理フレーム(60Hz)ごとにその時点で生成された分だけをpal_audio_queue()へ
 * 渡す設計のため、生成量と消費量がほぼ1:1で足並みが揃う。この状態ですぐに
 * 再生を開始すると、ホスト側のわずかなタイミング揺らぎ（スケジューリング
 * 遅延等）だけでSDLの内部キューが枯渇し「ぶつぶつ」というアンダーラン
 * （無音の途切れ）が発生する。このため、キューが目標値に
 * 達するまで再生開始(resume)を遅らせ、揺らぎを吸収するクッションを持たせる。 */
#define AUDIO_PREBUFFER_BYTES 35280u /* 44100Hz * 0.2sec * 2ch * 2byte */

/* 出力キューの滞留上限: 約0.5秒分。ホスト側でメインループが長時間止まる、
 * あるいはオーディオデバイスがキューを消費しない期間（ウィンドウ操作・
 * スリープ・出力デバイス切り替え等）があると、その間もループ復帰後に
 * 60Hz分の生成が積み上がってSDLのキューに滞留が残る。SDLは滞留を
 * 捨てないため、滞留量がそのまま恒久的な「音の遅延」になる（実測で
 * 147秒分=約26MBまで成長し、ZMUSICの再生開始・停止が同じだけ遅れた）。
 * 上限を超えたらキューを捨てて事前バッファリングからやり直す。
 * AUDIO_PREBUFFER_BYTES より十分大きくしないと事前バッファ完了直後に
 * 毎回破棄が走って発振するため、2倍以上の余裕を取る。 */
#define AUDIO_MAX_QUEUE_BYTES   88200u  /* 44100Hz * 0.5sec * 2ch * 2byte */
#define AUDIO_OUT_BYTES_PER_SEC 176400u /* 44100Hz * 2ch * 2byte */

/* 音声診断ダンプ: X68K_AUDIO_DUMP=<path> 指定時、
 * SDLへ渡す直前の最終ミックス済みPCM(44100Hz, interleaved s16le)を生ファイル
 * へ書き出す。波形そのものにクリップ/欠落/周期性アーティファクトが無いかを
 * オフラインで実測できる（他のX68K_*診断環境変数と同様、既定オフ）。
 * SDL2/SDL3間の比較検証でも、この出力のバイト一致が
 * 「生成側の同一性」の判定基準になる。 */
static FILE *s_audio_dump = NULL;
static int s_audio_dump_checked = 0;
static pal_audio_stats_t s_audio_stats = {0};

void pal_audio_get_stats(pal_audio_stats_t *out) {
    if (out)
        *out = s_audio_stats;
}

void pal_audio_queue(const int16_t *samples, int frame_count) {
    if (!g_audio_stream || frame_count <= 0)
        return;
    if (!s_audio_dump_checked) {
        s_audio_dump_checked = 1;
        const char *path = getenv("X68K_AUDIO_DUMP");
        if (path) {
            s_audio_dump = fopen(path, "wb");
            if (s_audio_dump)
                pal_log("X68K_AUDIO_DUMP: %s へPCM(44100Hz s16le stereo)を書き出します\n",
                        path);
        }
    }
    if (s_audio_dump)
        fwrite(samples, sizeof(int16_t), (size_t)(frame_count * g_audio_channels),
               s_audio_dump);
    int queued = SDL_GetAudioStreamQueued(g_audio_stream);
    /* 再生開始後に次のPCMを積む直前の残量が0なら、前回から今回までに
     * デバイス側が枯渇したことを意味する。このままresume状態を保つと
     * 以後の一瞬の揺らぎでも連続して無音を出すので、一度pauseして
     * 事前バッファリングから回復する。CPU/OPM/ADPCMの論理時刻はここで
     * 一切進めない・水増ししない。 */
    if (queued == 0 && g_audio_started) {
        pal_audio_reset();
        s_audio_stats.underflow_count++;
        pal_log("audio: 出力キューの枯渇を検出。事前バッファリングから再開します"
                " (count=%llu)\n",
                (unsigned long long)s_audio_stats.underflow_count);
        queued = 0;
    }
    if (queued >= 0 && (Uint32)queued > AUDIO_MAX_QUEUE_BYTES) {
        pal_audio_reset();
        s_audio_stats.queue_limit_count++;
        pal_log("audio: 出力キューの滞留 %d バイト(約%.1f秒)を破棄しました\n", queued,
                (double)queued / (double)(AUDIO_OUT_BYTES_PER_SEC));
    }
    SDL_PutAudioStreamData(g_audio_stream, samples,
                           frame_count * g_audio_channels * (int)sizeof(int16_t));
    if (!g_audio_started) {
        int queued = SDL_GetAudioStreamQueued(g_audio_stream);
        if (queued >= 0 && (Uint32)queued >= AUDIO_PREBUFFER_BYTES) {
            SDL_ResumeAudioStreamDevice(g_audio_stream);
            g_audio_started = 1;
        }
    }
}

/* 滞留を捨てて遅延をリセットする。捨てた直後は再生を止め、事前
 * バッファリングが溜まってから resume し直す（そのまま再生し続けると
 * キューが空の状態から始まりアンダーランする）。 */
void pal_audio_reset(void) {
    if (!g_audio_stream)
        return;
    s_audio_stats.reset_count++;
    SDL_ClearAudioStream(g_audio_stream);
    SDL_PauseAudioStreamDevice(g_audio_stream);
    g_audio_started = 0;
}

void pal_audio_quit(void) {
    if (g_audio_stream) {
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
    }
}

/* マイク入力(録音経路、_ADPCMINP等)。SDL_OpenAudioDeviceStreamの録音デバイス
 * 版で、macOS では CoreAudio 経由でマイクを取得する。TCC
 * 権限拒否時は SDL_OpenAudioDeviceStream 自体が失敗するため、呼び出し側は
 * pal_audio_capture_available()==0 で自然に無音入力へフォールバックする。 */
int pal_audio_capture_init(int sample_rate_hz, int channels) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        pal_log("SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = sample_rate_hz;
    spec.format = SDL_AUDIO_S16;
    spec.channels = channels;
    g_capture_stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL);
    if (!g_capture_stream) {
        pal_log("SDL_OpenAudioDeviceStream(capture) failed: %s\n", SDL_GetError());
        return 0;
    }
    /* 開いた直後はpaused状態のまま維持する（OSのマイク使用インジケータを
     * 点灯させないため）。実際の録音はゲストが録音を開始したときだけ
     * pal_audio_capture_set_active(1) で有効化する。 */
    return 1;
}

int pal_audio_capture_available(void) {
    return g_capture_stream != NULL;
}

void pal_audio_capture_set_active(int active) {
    if (!g_capture_stream)
        return;
    if (active)
        SDL_ResumeAudioStreamDevice(g_capture_stream);
    else
        SDL_PauseAudioStreamDevice(g_capture_stream);
}

int pal_audio_capture_dequeue(int16_t *out, int max_samples) {
    if (!g_capture_stream || max_samples <= 0)
        return 0;
    int got = SDL_GetAudioStreamData(g_capture_stream, out, max_samples * (int)sizeof(int16_t));
    if (got < 0)
        return 0;
    return got / (int)sizeof(int16_t);
}

void pal_audio_capture_quit(void) {
    if (g_capture_stream) {
        SDL_DestroyAudioStream(g_capture_stream);
        g_capture_stream = NULL;
    }
}

#ifndef _WIN32
/* RS-232C(SCC ch A)のホスト側データ経路。
 * X68K_RS232_DEV=<path>指定時はそのパスを直接openする。未指定時は
 * X68K_RS232_PTY=1 のときのみ posix_openpt でPTYマスタを開く。 */
int pal_serial_init(void) {
    const char *dev_path = getenv("X68K_RS232_DEV");
    if (dev_path) {
        g_serial_fd = open(dev_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (g_serial_fd < 0) {
            pal_log("RS-232C: %s のopenに失敗しました\n", dev_path);
            return 0;
        }
        pal_log("RS-232C: %s に接続しました\n", dev_path);
        return 1;
    }
    if (!getenv("X68K_RS232_PTY"))
        return 0;
    g_serial_fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_serial_fd < 0) {
        pal_log("RS-232C: posix_openpt に失敗しました\n");
        return 0;
    }
    if (grantpt(g_serial_fd) < 0 || unlockpt(g_serial_fd) < 0) {
        pal_log("RS-232C: grantpt/unlockpt に失敗しました\n");
        close(g_serial_fd);
        g_serial_fd = -1;
        return 0;
    }
    const char *slave_path = ptsname(g_serial_fd);
    if (!slave_path) {
        pal_log("RS-232C: ptsname に失敗しました\n");
        close(g_serial_fd);
        g_serial_fd = -1;
        return 0;
    }
    pal_log("RS-232C PTY slave: %s\n", slave_path);
    return 1;
}

int pal_serial_available(void) {
    return g_serial_fd >= 0;
}

int pal_serial_read(uint8_t *out, int max) {
    if (g_serial_fd < 0 || max <= 0)
        return 0;
    ssize_t got = read(g_serial_fd, out, (size_t)max);
    if (got < 0)
        return 0; /* EAGAIN等: データなし */
    return (int)got;
}

int pal_serial_write(const uint8_t *buf, int n) {
    if (g_serial_fd < 0 || n <= 0)
        return 0;
    ssize_t wrote = write(g_serial_fd, buf, (size_t)n);
    if (wrote < 0)
        return 0;
    return (int)wrote;
}

void pal_serial_quit(void) {
    if (g_serial_fd >= 0) {
        close(g_serial_fd);
        g_serial_fd = -1;
    }
}
#else
int pal_serial_init(void) {
    return 0;
}

int pal_serial_available(void) {
    return 0;
}

int pal_serial_read(uint8_t *out, int max) {
    (void)out;
    (void)max;
    return 0;
}

int pal_serial_write(const uint8_t *buf, int n) {
    (void)buf;
    (void)n;
    return 0;
}

void pal_serial_quit(void) {
}
#endif
