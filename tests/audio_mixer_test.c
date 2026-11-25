/*
 * audio_mixer.c の単体テスト（リサンプル方式の切替に対応）。SDL/バス非依存の純ロジックのみを検証する。
 */
#include "audio/audio_mixer.h"
#include <assert.h>
#ifdef _WIN32
#define _USE_MATH_DEFINES /* MSVCのmath.hはM_PIを既定で定義しないため */
#endif
#include <math.h>
#include <stdio.h>

/* NEAREST/LINEAR/CUBIC 共通で成立すべき不変条件（フレーム数レンジ・
 * パン・クリップ等）を全方式に対して検証する。 */
static void test_mode(audio_resample_mode_t mode) {
    int16_t in[65];
    for (int i = 0; i < 65; i++)
        in[i] = (int16_t)(1000 + i);
    int16_t out[2048];

    /* 65サンプル@3900Hz -> 44100Hzへアップサンプル: 44100/3900*65 ≈ 735フレーム */
    audio_resample_state_t st = {0};
    int frames = audio_mix_resample_pan(in, 65, 3900u, 44100u, 1, 1, out, 1024, mode, &st);
    assert(frames > 700 && frames < 760);

    /* 両チャンネルON: L/Rが同じ値 */
    for (int i = 0; i < frames; i++)
        assert(out[i * 2] == out[i * 2 + 1]);

    /* 左のみON: 右は無音（独立したstate） */
    st = (audio_resample_state_t){0};
    int frames_l = audio_mix_resample_pan(in, 65, 3900u, 44100u, 1, 0, out, 1024, mode, &st);
    assert(frames_l == frames);
    for (int i = 0; i < frames_l; i++) {
        assert(out[i * 2] != 0);
        assert(out[i * 2 + 1] == 0);
    }

    /* 両方OFF: 完全無音 */
    st = (audio_resample_state_t){0};
    int frames_off = audio_mix_resample_pan(in, 65, 3900u, 44100u, 0, 0, out, 1024, mode, &st);
    for (int i = 0; i < frames_off; i++) {
        assert(out[i * 2] == 0);
        assert(out[i * 2 + 1] == 0);
    }

    /* out_capacity_framesで頭打ちになる */
    st = (audio_resample_state_t){0};
    int frames_capped = audio_mix_resample_pan(in, 65, 3900u, 44100u, 1, 1, out, 10, mode, &st);
    assert(frames_capped == 10);

    /* stateがフレームをまたいで連続し、フレーム数が一致すること（2026-07-16
     * 修正の回帰確認）。65サンプルを2回に分けて連続処理した結果は、同じ65
     * サンプルを1回で処理した結果とほぼ一致するはず。
     * NEARESTは前後参照が不要なため完全一致するが、LINEAR/CUBICは分割点
     * 直前の出力サンプルがp1/p2として次コールの値を参照できず末尾値で
     * クランプされる近似のため、分割点付近の数サンプルだけ小さな差が出得る
     * （1/60秒ごとの本番呼び出しでも同じ近似が生じるが、影響は数サンプル/
     * 約1000サンプル中と可聴域外）。値は許容差付きで比較する。 */
    {
        audio_resample_state_t st_once = {0};
        int16_t out_once[2048];
        int frames_once =
            audio_mix_resample_pan(in, 65, 3900u, 44100u, 1, 1, out_once, 2048, mode, &st_once);

        audio_resample_state_t st_split = {0};
        int16_t out_split[2048];
        int f1 = audio_mix_resample_pan(in, 33, 3900u, 44100u, 1, 1, out_split, 2048, mode,
                                        &st_split);
        int f2 = audio_mix_resample_pan(in + 33, 65 - 33, 3900u, 44100u, 1, 1, out_split + f1 * 2,
                                        2048 - f1, mode, &st_split);
        assert(f1 + f2 == frames_once);
        const int tol = mode == AUDIO_RESAMPLE_NEAREST ? 0 : 3;
        for (int i = 0; i < frames_once; i++) {
            int d0 = out_once[i * 2] - out_split[i * 2];
            int d1 = out_once[i * 2 + 1] - out_split[i * 2 + 1];
            assert(d0 >= -tol && d0 <= tol);
            assert(d1 >= -tol && d1 <= tol);
        }
    }

    /* in_count<=0 / rate==0 は安全に0を返す */
    st = (audio_resample_state_t){0};
    assert(audio_mix_resample_pan(in, 0, 3900u, 44100u, 1, 1, out, 1024, mode, &st) == 0);
    assert(audio_mix_resample_pan(in, 65, 0u, 44100u, 1, 1, out, 1024, mode, &st) == 0);
    assert(audio_mix_resample_pan(in, 65, 3900u, 0u, 1, 1, out, 1024, mode, &st) == 0);

    /* audio_resample_mono: ダウンサンプル方向（マイクcapture 44100Hz ->
     * ADPCMネイティブレート 15625Hz 相当）。735サンプル(44100Hzで1/60秒分)
     * -> 44100/15625*735 の逆数、すなわち 735*15625/44100 ≈ 260 サンプル。 */
    int16_t mic[735];
    for (int i = 0; i < 735; i++)
        mic[i] = (int16_t)(2000 + i);
    int16_t down[512];
    st = (audio_resample_state_t){0};
    int down_n = audio_resample_mono(mic, 735, 44100u, 15625u, down, 512, mode, &st);
    assert(down_n > 240 && down_n < 280);

    /* アップサンプル方向も同じ関数で動作する（対称性確認） */
    int16_t up[4096];
    st = (audio_resample_state_t){0};
    int up_n = audio_resample_mono(down, down_n, 15625u, 44100u, up, 4096, mode, &st);
    assert(up_n > 700 && up_n < 760);

    /* in_count<=0 / rate==0 は安全に0を返す */
    st = (audio_resample_state_t){0};
    assert(audio_resample_mono(mic, 0, 44100u, 15625u, down, 512, mode, &st) == 0);
    assert(audio_resample_mono(mic, 735, 0u, 15625u, down, 512, mode, &st) == 0);
    assert(audio_resample_mono(mic, 735, 44100u, 0u, down, 512, mode, &st) == 0);

    /* audio_resample_stereo_i32: 62500Hz(OPM固定) -> 44100Hzのダウンサンプル。
     *1フレーム分(1042サンプル)でフレーム数≈735。 */
    {
        int32_t opm_in[1042 * 2];
        for (int i = 0; i < 1042; i++) {
            opm_in[i * 2 + 0] = 1000 + i;    /* L */
            opm_in[i * 2 + 1] = -(1000 + i); /* R (L/R独立確認用に符号を変える) */
        }
        int32_t opm_out[2048 * 2];
        audio_resample_state_t st64 = {0};
        int fr = audio_resample_stereo_i32(opm_in, 1042, 1042, 62500u, 44100u, opm_out, 2048, mode,
                                           &st64);
        assert(fr > 700 && fr < 760);
        /* L/Rが独立に変換されていることの確認。NEARESTは入力の複製なので
         * 完全一致するが、LINEAR/CUBICは負値の右シフト丸め（floor方向）が
         * 正負非対称なため±1LSBの誤差が生じ得る（XEiJの実装も同じ丸め方向、
         * 可聴域外の誤差）。許容差1で判定する。 */
        for (int i = 0; i < fr; i++) {
            int32_t sum = opm_out[i * 2 + 0] + opm_out[i * 2 + 1];
            assert(sum >= -1 && sum <= 1);
        }
    }

    /* フレーム境界での先読み(in_valid_frames)有無による末尾出力の違い
     * （実地検証で判明したバグの回帰テスト）。手計算で検証できる
     * 小さな例: in_rate=3,out_rate=2(step=1.5サンプル/出力)、in_frames=2
     * だと2番目(最後)の出力がちょうどidx=in_frames-1・frac=0.5になり、
     * LINEARはp1、CUBICはp1・p2が境界を跨ぐ。先読み無しでは末尾サンプルの
     * 複製(clamp)になるが、先読み有りでは次フレームの先頭サンプルへ向けて
     * 正しく補間される。NEARESTは先読みを使わないため両者が完全一致する。 */
    {
        int32_t buf[4 * 2] = {0, 0, 1, -1, 99, -99, 199, -199};
        int32_t out_noaux[8 * 2];
        audio_resample_state_t st_noaux = {0};
        int fr_noaux = audio_resample_stereo_i32(buf, 2, 2, 3u, 2u, out_noaux, 8, mode, &st_noaux);

        int32_t out_aux[8 * 2];
        audio_resample_state_t st_aux = {0};
        int fr_aux = audio_resample_stereo_i32(buf, 2, 4, 3u, 2u, out_aux, 8, mode, &st_aux);

        assert(fr_noaux == 2 && fr_aux == 2);
        if (mode == AUDIO_RESAMPLE_NEAREST) {
            assert(out_noaux[1 * 2 + 0] == out_aux[1 * 2 + 0]);
            assert(out_noaux[1 * 2 + 1] == out_aux[1 * 2 + 1]);
        } else {
            /* Lは増加列(0,1,99,199)なので先読み有りの方が末尾複製より大きい
             * 値へ、Rはその符号反転なので逆に小さい値へ補間されるはず */
            assert(out_aux[1 * 2 + 0] > out_noaux[1 * 2 + 0]);
            assert(out_aux[1 * 2 + 1] < out_noaux[1 * 2 + 1]);
        }
    }

    /* audio_mix_resample_pan_i32: パンOFF側は常に0。 */
    {
        int16_t in16[65];
        for (int i = 0; i < 65; i++)
            in16[i] = (int16_t)(1000 + i);
        int32_t out32[1024 * 2];
        audio_resample_state_t st64 = {0};
        int fr = audio_mix_resample_pan_i32(in16, 65, 65, 3900u, 44100u, 1, 0, out32, 1024, mode,
                                            &st64);
        assert(fr > 700 && fr < 760);
        for (int i = 0; i < fr; i++) {
            assert(out32[i * 2 + 0] != 0);
            assert(out32[i * 2 + 1] == 0);
        }
    }
}

int main(void) {
    test_mode(AUDIO_RESAMPLE_NEAREST);
    test_mode(AUDIO_RESAMPLE_LINEAR);
    test_mode(AUDIO_RESAMPLE_CUBIC);

    /* audio_mix_sum_clip_stereo: マスタースケール0.5倍(XEiJ
     * sndTargetScale既定値準拠)適用後にクリップ・相殺が正しく働くこと。
     * 方式に依存しないため1回だけ検証。 */
    {
        int32_t a[4 * 2] = {100000, -100000, 70000, -70000, 20000, 20000, 10000, -10000};
        int32_t b[4 * 2] = {100000, -100000, 0, 0, -20000, -20000, 10000, -10000};
        int16_t out[4 * 2];
        int frames = audio_mix_sum_clip_stereo(a, b, 4, out);
        assert(frames == 4);
        /* 200000/-200000 -> 0.5倍で100000/-100000 -> クリップ */
        assert(out[0] == 32767 && out[1] == -32768);
        /* 70000/-70000 -> 0.5倍で35000/-35000 -> クリップ */
        assert(out[2] == 32767 && out[3] == -32768);
        /* 正+負で相殺（スケールに関わらず0） */
        assert(out[4] == 0 && out[5] == 0);
        /* 20000/-20000 -> 0.5倍で10000/-10000 -> クリップされず、スケールが
         * 実際に半分になっていることを確認 */
        assert(out[6] == 10000 && out[7] == -10000);
    }

    /* audio_dc_block_stereo: 定数DC入力が減衰し、十分なサンプル数後には
     * ほぼ0へ収束すること（回帰テスト）。 */
    {
        audio_dc_block_state_t st = {0};
        int32_t buf[4000 * 2];
        for (int i = 0; i < 4000; i++) {
            buf[i * 2 + 0] = 10000;  /* L: 定数DC */
            buf[i * 2 + 1] = -5000;  /* R: 定数DC(符号違い) */
        }
        audio_dc_block_stereo(buf, 4000, &st);
        /* 先頭出力はステップ応答: y[0]=x[0]-x1(=0)+R*y1(=0)=x[0] */
        assert(buf[0] == 10000);
        assert(buf[1] == -5000);
        /* 時定数1/(1-R)≈993サンプル。4000サンプル(≈4τ)後はほぼ収束する */
        assert(buf[(4000 - 1) * 2 + 0] > -200 && buf[(4000 - 1) * 2 + 0] < 200);
        assert(buf[(4000 - 1) * 2 + 1] > -200 && buf[(4000 - 1) * 2 + 1] < 200);
    }

    /* audio_dc_block_stereo: フィルタの遮断周波数(≈7Hz)より十分高い交流
     * 成分(100Hz)はほぼ減衰しないこと。 */
    {
        audio_dc_block_state_t st = {0};
        const int n = 4410; /* 100Hz@44100Hzで10周期分、過渡応答収束の猶予を確保 */
        int32_t buf[4410 * 2];
        for (int i = 0; i < n; i++) {
            int32_t v = (int32_t)(10000.0 * sin(2.0 * M_PI * 100.0 * (double)i / 44100.0));
            buf[i * 2 + 0] = v;
            buf[i * 2 + 1] = v;
        }
        audio_dc_block_stereo(buf, n, &st);
        /* 過渡応答が収まった後半(最後の1周期=441サンプル)でピーク振幅が
         * ほぼ保たれることを確認(9割以上残っていればよい)。 */
        int32_t peak = 0;
        for (int i = n - 441; i < n; i++) {
            int32_t a = buf[i * 2 + 0] > 0 ? buf[i * 2 + 0] : -buf[i * 2 + 0];
            if (a > peak)
                peak = a;
        }
        assert(peak > 9000);
    }

    printf("audio_mixer_test: OK\n");
    return 0;
}
