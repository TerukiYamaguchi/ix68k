/* SDL出力キューの枯渇後、再生を続けず事前バッファリングへ戻ることを検証する。 */
#include "platform/platform.h"
#include "platform/platform_sdl3_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* setenv() 相当 */
#endif

#define SAMPLE_RATE 44100
#define CHANNELS 2
#define PREBUFFER_FRAMES (SAMPLE_RATE / 5)

int main(void) {
    /* 物理デバイスを使わずに、SDLのキュー状態だけを検証する。 */
    setenv("SDL_AUDIODRIVER", "dummy", 1);

    assert(pal_audio_init(SAMPLE_RATE, CHANNELS) == 1);
    assert(pal_audio_queued_frames() == 0);

    int16_t pcm[PREBUFFER_FRAMES * CHANNELS] = {0};
    pal_audio_queue(pcm, PREBUFFER_FRAMES);
    assert(g_audio_started == 1);
    assert(pal_audio_queued_frames() >= PREBUFFER_FRAMES);

    /* 再生済みとしてキューを枯渇させ、次回のqueueで回復状態へ入ることを確認する。 */
    SDL_ClearAudioStream(g_audio_stream);
    pal_audio_queue(pcm, 1);
    assert(g_audio_started == 0);
    assert(pal_audio_queued_frames() == 1);
    pal_audio_stats_t stats;
    pal_audio_get_stats(&stats);
    assert(stats.reset_count == 1);
    assert(stats.underflow_count == 1);
    assert(stats.queue_limit_count == 0);

    pal_audio_queue(pcm, PREBUFFER_FRAMES - 1);
    assert(g_audio_started == 1);

    pal_audio_quit();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    printf("test_audio_recovery: OK\n");
    return 0;
}
