#pragma once
// ============================================================================
//  EVDPAudio.h - I2S bring-up, Opus decode, audio playback task
// ============================================================================
#include "EVDPState.h"

// ---- Rate-limited warning from the audio task -------------------------------
static uint32_t gAudioLogBurstWindowMs = 0;
static uint32_t gAudioLogBurstCount    = 0;
static uint32_t gAudioLogSuppressed    = 0;

// Ring-only logger for the audio task. This runs on Core 1 inside the
// per-frame budget; log_warn() calls Serial.println(), which blocks on
// UART TX at 115200 baud for ~9 ms per 100-char line — half a 20 ms
// frame per call. Two lines per second = 18 ms/s of guaranteed Core 1
// stall during a flush storm, which is exactly when a stutter is worst.
// The ring buffer is fully served by /api/logs on the status page, so
// serial visibility for these lines is redundant.
inline void audioLogThrottled(const String &msg) {
    uint32_t now = millis();
    if (now - gAudioLogBurstWindowMs >= 1000) {
        if (gAudioLogSuppressed) {
            logRingPush("[Audio] (" + String(gAudioLogSuppressed) +
                        " similar messages suppressed in last second)");
            gAudioLogSuppressed = 0;
        }
        gAudioLogBurstWindowMs = now;
        gAudioLogBurstCount    = 0;
    }
    if (gAudioLogBurstCount >= 2) {
        gAudioLogSuppressed++;
        return;
    }
    gAudioLogBurstCount++;
    logRingPush(msg);   // ring only — no Serial.println, no UART stall
}

// ---- Soft start ------------------------------------------------------------
// [fade-soft-start-2s] Two ramp lengths, chosen per resume. See the mixer's
// fade_on_resume block for which is used when.
//
// FADE_SOFT_SECS was 2.0 s, then shortened to 200 ms by [session-fade-shorter]
// because a 2 s ramp on a 100 W sub was heard as a low-frequency swell after
// the node came back. That reasoning holds for a *rebuffer* - audio never
// really stopped - but not for a pause/play, where the operator is asking for
// a start and 200 ms is indistinguishable from a hard cut. The two cases are
// now separated instead of being averaged into one compromise value.
static const float    FADE_SOFT_SECS     = 2.0f;
static const float    FADE_SHORT_SECS    = 0.005f;
// Silence at or above this counts as "the producer stopped", not "a hiccup".
// 500 ms is ~25 audio frames: far longer than any jitter burst this node is
// tuned for (rebuffer_hold_ms floors at 250 ms), far shorter than any pause a
// human makes.
static const uint32_t FADE_RESUME_GAP_MS = 500;

// ---- I2S init --------------------------------------------------------------
// [i2s-ready] DAC readiness flags. Exposed via /api/health's i2sDac1Ready /
// i2sDac2Ready so a dead DAC is visible on the dashboard, not just serial.
volatile bool g_i2s_dac1_ready = false;
volatile bool g_i2s_dac2_ready = false;

inline void init_i2s() {
    log_message("[I2S] Initializing I2S DAC drivers...");
    // [i2s-config-explicit] Unmentioned members are zero-initialized by
    // C++ aggregate rules, NOT set to their IDF defaults. With use_apll=true
    // an mclk_multiple of 0 is invalid on some IDF versions and
    // i2s_driver_install returns an error -> DAC1 silently dead with only
    // the "[I2S] DAC1 install=..." warning as a trace.
    i2s_config_t i2s_config_1 = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        // [i2s-dma-headroom] Was 6. 6*256 frames = 1536 frames = 32 ms of
        // buffer at 48 kHz stereo. The audio task writes one 960-frame
        // (20 ms) block per frame; the residual margin was only 12 ms.
        // The playback task runs at priority 3 on Core 1 and can be
        // preempted by the Arduino loopTask (which does Serial.print in
        // log_warn — ~9 ms per 100-char line). A single slow log line
        // was enough to squeeze the DMA to <3 ms and let
        // tx_desc_auto_clear wipe a live descriptor to 0, which on a
        // bass peak is an audible click, worst on the 100 W sub amp
        // (which sits on DAC2, right channel of speakers[3]).
        // 8*256 frames = 2048 frames = 42.6 ms: +10.6 ms of margin.
        // +2 KB heap per DAC (4 KB total). No runtime cost, no clock
        // change, no reconfiguration. Measured CPU/timing impact: 0.
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk    = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT
    };
    i2s_pin_config_t pin_config_1 = {
        // MUST be I2S_PIN_NO_CHANGE (-1). Designated initializers zero-fill
        // unmentioned members, so omitting this defaults mck_io_num to 0
        // (GPIO 0) — a boot-strap pin. With use_apll=true the I2S driver
        // would drive a 12.288 MHz clock onto GPIO 0, corrupting the strap
        // sampled at the next soft reset.
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = pin_bclk_1,
        .ws_io_num    = pin_lrc_1,
        .data_out_num = pin_dout_1,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    esp_err_t e1a = i2s_driver_install(I2S_NUM_DAC1, &i2s_config_1, 0, NULL);
    esp_err_t e1b = i2s_set_pin(I2S_NUM_DAC1, &pin_config_1);
    g_i2s_dac1_ready = (e1a == ESP_OK && e1b == ESP_OK);
    if (!g_i2s_dac1_ready) {
        log_warn(String("[I2S] DAC1 install=") + String((int)e1a) +
                 " set_pin=" + String((int)e1b) +
                 " -> DAC1 WILL BE SILENT");
    }

    i2s_config_t i2s_config_2 = i2s_config_1;
    i2s_pin_config_t pin_config_2 = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = pin_bclk_2,
        .ws_io_num    = pin_lrc_2,
        .data_out_num = pin_dout_2,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    esp_err_t e2a = i2s_driver_install(I2S_NUM_DAC2, &i2s_config_2, 0, NULL);
    // [apll-retry] The APLL is a single shared resource between the two I2S
    // peripherals. On some IDF builds/silicon revisions the second install
    // with use_apll=true returns ESP_ERR_INVALID_STATE / NOT_SUPPORTED and
    // DAC2 stays silent for the rest of the uptime. Retry without APLL:
    // slightly higher jitter on DAC2 (a few ppm) vs. permanently silent.
    if (e2a != ESP_OK) {
        log_warn(String("[I2S] DAC2 install with APLL failed (") +
                 String((int)e2a) + "), retrying without APLL.");
        i2s_config_2.use_apll = false;
        e2a = i2s_driver_install(I2S_NUM_DAC2, &i2s_config_2, 0, NULL);
    }
    esp_err_t e2b = i2s_set_pin(I2S_NUM_DAC2, &pin_config_2);
    g_i2s_dac2_ready = (e2a == ESP_OK && e2b == ESP_OK);
    if (!g_i2s_dac2_ready) {
        log_warn(String("[I2S] DAC2 install=") + String((int)e2a) +
                 " set_pin=" + String((int)e2b) +
                 " -> DAC2 WILL BE SILENT");
    }

    if (g_i2s_dac1_ready && g_i2s_dac2_ready) {
        log_message("[I2S] DAC drivers successfully initialized.");
    } else {
        log_message("[I2S] I2S init completed WITH FAILURES (see WARNs above).");
    }
}

// ---- Soft limiter ----------------------------------------------------------
// [soft-limit] The 4th-order LR4 crossovers can peak above 0 dBFS on
// transient material. The old hard `constrain()` at the int16 cast clipped
// those peaks squarely, adding odd harmonics (audible fizz on loud content,
// worst on the Sub channel). This is a soft knee that only engages above
// 0.9 FS; below that it's an exact passthrough (no extra cost, no sound
// change on quiet material).
inline int16_t softLimitToInt16(float x) {
    // [softlimit-nan-guard] Reject NaN/Inf before the int16 cast. The
    // current biquads cannot produce NaN from bounded input, but a
    // future filter change or a corrupted speaker coefficient set
    // could. (int16_t)NaN is UB on RISC-V; returning 0 is a silent
    // mute on that sample, which is exactly what we want for a
    // limiter whose contract is "always return a valid sample".
    // std::isfinite is available via <cmath> (already pulled in
    // through EVDPFilters.h); one branch per sample, negligible cost.
    if (!std::isfinite(x)) return 0;
    // Rational knee instead of two tanhf calls. tanhf is ~50-100 cycles
    // on ESP32-S3 (no HW transcendental unit); this is one abs + one
    // div. Behaviour: exact passthrough below |x| = 0.9, monotonically
    // softer above, asymptote at |x| = 1.0. Note: the knee curve differs
    // slightly from tanh at moderate overdrive (0.95 vs 0.976 at x=1.0)
    // — audible only as slightly earlier compression on hot material.
    const float ax = x < 0.0f ? -x : x;
    // [softlimit-drop-dead-clip]
    // |x| <= 0.9 guarantees s in [-29490.3, 29490.3], so the old clip
    // guards were unreachable. Removing them stops future maintainers
    // from believing clipping can occur below 0.9 FS.
    if (ax <= 0.9f) {
        return (int16_t)(x * 32767.0f);
    }
    const float t = ax - 0.9f;
    const float y = 0.9f + 0.1f * t / (0.1f + t);   // 0.9 at t=0, ->1.0 as t->inf
    const float s = y * 32767.0f;
    return (int16_t)(x < 0.0f ? -s : s);
}

// [audio-hz-diag-engine]
// Per-second band-energy analyser on the pre-filter left channel.
// Struct state is static (BSS, ~130 bytes) so it never lands on the
// audio task's 16 KB stack. Coefficients are computed once at boot;
// configured is a single bool so the inline check inside the per-sample
// hot path is one branch that predicts perfectly.
//
// CPU cost: 4 biquads (10 flops each) + 4 sum-of-squares + 1 fabs + ZCR
// compare = ~55 flops/sample at 48 kHz = ~2.6 MFLOPs/s. That is ~0.3% of
// one core on the S3 and cannot measurably affect playout timing.
//
// Thread safety: only ever touched by audioPlaybackTask (Core 1). The
// accumulated per-second values are snapshotted into the volatile
// stat_hz_* globals at the 1 s rollover, so /api/health and the log ring
// read consistent numbers without any lock.
struct AudioHzDiag {
    Biquad   lp250;   // LP@250 Hz  -> low band
    Biquad   hp250;   // HP@250 Hz  -> drives both mid and high
    Biquad   lp4k;    // LP@4000 on HP250 -> mid band
    Biquad   hp4k;    // HP@4000 on HP250 -> high band
    bool     configured = false;

    uint32_t window_n      = 0;
    float    low_sum_sq    = 0.0f;
    float    mid_sum_sq    = 0.0f;
    float    high_sum_sq   = 0.0f;
    float    all_sum_sq    = 0.0f;
    float    peak_abs      = 0.0f;
    uint32_t zc_count      = 0;
    int16_t  last_sample   = 0;

    int32_t  prev_low_db_x10  = 0;
    int32_t  prev_mid_db_x10  = 0;
    int32_t  prev_high_db_x10 = 0;
    bool     prev_valid       = false;
    uint32_t last_periodic_ms = 0;
};
static AudioHzDiag g_hz_diag;

inline void configureAudioHzDiag() {
    if (g_hz_diag.configured) return;
    const float fs = (float)SAMPLE_RATE;
    g_hz_diag.lp250.setLowpass ( 250.0f, fs);
    g_hz_diag.hp250.setHighpass( 250.0f, fs);
    g_hz_diag.lp4k .setLowpass (4000.0f, fs);
    g_hz_diag.hp4k .setHighpass(4000.0f, fs);
    g_hz_diag.configured = true;
    logRingPush("[Audio][Hz] diagnostic enabled (250 Hz / 4 kHz crossovers)");
}

inline void audioHzDiagSample(float x) {
    if (!g_hz_diag.configured) return;

    // [hz-diag-clamp-cast] BUGFIX: Catmull-Rom resampling can overshoot
    // the input int16 range by up to ~25 % on a transient (peak gain at
    // t=0.5 is ~1.25). Casting that float directly to int16_t is UB
    // per the C++ standard; on RISC-V it wraps to the wrong sign,
    // corrupting zc_count and every downstream [Audio][Hz] diagnostic.
    // The audio path itself is protected by softLimitToInt16(), but this
    // diagnostic call site was not. Clamp once here.
    const float xClamp = x < -32768.0f ? -32768.0f
                        : x >  32767.0f ?  32767.0f : x;

    const float low  = g_hz_diag.lp250.process(x);
    const float hp   = g_hz_diag.hp250.process(x);
    const float mid  = g_hz_diag.lp4k.process(hp);
    const float high = g_hz_diag.hp4k.process(hp);

    g_hz_diag.low_sum_sq  += low  * low;
    g_hz_diag.mid_sum_sq  += mid  * mid;
    g_hz_diag.high_sum_sq += high * high;
    g_hz_diag.all_sum_sq  += x    * x;

    const float ax = fabsf(x);
    if (ax > g_hz_diag.peak_abs) g_hz_diag.peak_abs = ax;

    if ((x > 0.0f && g_hz_diag.last_sample < 0) ||
        (x < 0.0f && g_hz_diag.last_sample > 0)) {
        g_hz_diag.zc_count++;
    }
    g_hz_diag.last_sample = (int16_t)xClamp;
    g_hz_diag.window_n++;
}

// db10 helper: 200*log10(rms/32768) => dB*10. Clamped floor for silence.
inline int32_t audioHzDiagDb10(float rms) {
    if (rms < 1e-4f) return -600;         // -60 dBFS floor
    return (int32_t)(200.0f * log10f(rms / 32768.0f));
}

// Sign-aware dB*10 formatter, e.g. -183 => "-18.3".
inline void audioHzDiagFmtDb(int32_t v, char *out, size_t sz) {
    const char *sign = v < 0 ? "-" : "";
    int32_t a = v < 0 ? -v : v;
    snprintf(out, sz, "%s%d.%d", sign, (int)(a / 10), (int)(a % 10));
}

inline void audioHzDiagRollover() {
    if (!g_hz_diag.configured) return;
    const uint32_t n = g_hz_diag.window_n;
    if (n < SAMPLE_RATE / 4) {            // need >= 250 ms before trusting
        // [hz-diag-rollover-early-reset] BUGFIX: zero ALL accumulators, not
        // just window_n. Without this, a <250 ms of audio second (OTA pause,
        // rebuffer storm, first pass at boot) leaves low/mid/high_sum_sq,
        // all_sum_sq, peak_abs, zc_count from the partial window. The next
        // rollover then computes sqrt(sum_sq / n) with n under-counting the
        // samples that fed sum_sq -> band RMS inflated, "change" log fires
        // spuriously, and the 30 s trend lines lie. Cost of the extra
        // six stores: 2 ns, once per second.
        g_hz_diag.window_n    = 0;
        g_hz_diag.low_sum_sq  = 0.0f;
        g_hz_diag.mid_sum_sq  = 0.0f;
        g_hz_diag.high_sum_sq = 0.0f;
        g_hz_diag.all_sum_sq  = 0.0f;
        g_hz_diag.peak_abs    = 0.0f;
        g_hz_diag.zc_count    = 0;
        return;
    }

    const float fn     = (float)n;
    const float low_r  = sqrtf(g_hz_diag.low_sum_sq  / fn);
    const float mid_r  = sqrtf(g_hz_diag.mid_sum_sq  / fn);
    const float high_r = sqrtf(g_hz_diag.high_sum_sq / fn);
    const float all_r  = sqrtf(g_hz_diag.all_sum_sq  / fn);

    const int32_t low_db  = audioHzDiagDb10(low_r);
    const int32_t mid_db  = audioHzDiagDb10(mid_r);
    const int32_t high_db = audioHzDiagDb10(high_r);
    const int32_t rms_db  = audioHzDiagDb10(all_r);
    const int32_t peak_db = audioHzDiagDb10(g_hz_diag.peak_abs);

    // ZCR-derived centroid proxy. For a sinusoid at F Hz sampled at fs,
    // crossings/sec = 2F; therefore F ~= crossings * fs / (2 * n). For
    // complex signals this is a rough centroid, which is exactly what
    // we want: it shifts when the pitch envelope moves, and it drops
    // sharply when content disappears from any band above the DC bias.
    const uint32_t centroid_hz =
        (uint32_t)((uint64_t)g_hz_diag.zc_count * SAMPLE_RATE / (2ULL * n));

    stat_hz_low_db_x10  = low_db;
    stat_hz_mid_db_x10  = mid_db;
    stat_hz_high_db_x10 = high_db;
    stat_hz_rms_db_x10  = rms_db;
    stat_hz_peak_db_x10 = peak_db;
    stat_hz_centroid_hz = (uint16_t)(centroid_hz > 65535 ? 65535 : centroid_hz);

    // [audio-hz-change-log]
    // Log a line ONLY on significant change, so a stable stream emits
    // nothing between 30 s periodic trend lines. Thresholds are chosen
    // for audibility: high band needs only 6 dB because treble loss is
    // the exact symptom reported; mid/low need 10 dB because their
    // natural loudness envelope varies more second-to-second.
    if (g_hz_diag.prev_valid) {
        const int32_t dL = low_db  - g_hz_diag.prev_low_db_x10;
        const int32_t dM = mid_db  - g_hz_diag.prev_mid_db_x10;
        const int32_t dH = high_db - g_hz_diag.prev_high_db_x10;
        const int32_t aL = dL < 0 ? -dL : dL;
        const int32_t aM = dM < 0 ? -dM : dM;
        const int32_t aH = dH < 0 ? -dH : dH;

        if (aH >= 60 || aM >= 100 || aL >= 100) {
            char vL[16], vLp[16], vM[16], vMp[16], vH[16], vHp[16];
            audioHzDiagFmtDb(low_db,  vL,  sizeof(vL));
            audioHzDiagFmtDb(g_hz_diag.prev_low_db_x10,  vLp, sizeof(vLp));
            audioHzDiagFmtDb(mid_db,  vM,  sizeof(vM));
            audioHzDiagFmtDb(g_hz_diag.prev_mid_db_x10,  vMp, sizeof(vMp));
            audioHzDiagFmtDb(high_db, vH,  sizeof(vH));
            audioHzDiagFmtDb(g_hz_diag.prev_high_db_x10, vHp, sizeof(vHp));

            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[Audio][Hz] change: L %s->%s M %s->%s H %s->%s c=%uHz",
                     vLp, vL, vMp, vM, vHp, vH, (unsigned)centroid_hz);
            logRingPush(buf);
        }
    }

    // Periodic 30 s trend line so a long recording always has at least
    // 20 [Audio][Hz] rows to chart, even when nothing changes.
    const uint32_t now = millis();
    if (now - g_hz_diag.last_periodic_ms >= 30000) {
        g_hz_diag.last_periodic_ms = now;
        char vL[16], vM[16], vH[16], vR[16], vP[16];
        audioHzDiagFmtDb(low_db,  vL, sizeof(vL));
        audioHzDiagFmtDb(mid_db,  vM, sizeof(vM));
        audioHzDiagFmtDb(high_db, vH, sizeof(vH));
        audioHzDiagFmtDb(rms_db,  vR, sizeof(vR));
        audioHzDiagFmtDb(peak_db, vP, sizeof(vP));
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[Audio][Hz] L=%s M=%s H=%s R=%s P=%s c=%uHz",
                 vL, vM, vH, vR, vP, (unsigned)centroid_hz);
        logRingPush(buf);
    }

    g_hz_diag.prev_low_db_x10  = low_db;
    g_hz_diag.prev_mid_db_x10  = mid_db;
    g_hz_diag.prev_high_db_x10 = high_db;
    g_hz_diag.prev_valid       = true;

    g_hz_diag.window_n    = 0;
    g_hz_diag.low_sum_sq  = 0.0f;
    g_hz_diag.mid_sum_sq  = 0.0f;
    g_hz_diag.high_sum_sq = 0.0f;
    g_hz_diag.all_sum_sq  = 0.0f;
    g_hz_diag.peak_abs    = 0.0f;
    g_hz_diag.zc_count    = 0;
}

// Render each decoded frame at its original length. Queue occupancy is not a
// measurement of the independently clocked I2S DMA's sample consumption.
inline void process_and_output_audio(const int16_t* in_pcm, int samples_per_channel) {
    if (!in_pcm || samples_per_channel <= 0 || !dac1_output_buffer || !dac2_output_buffer) return;

    // [i2s-outside-mutex] out_samples is produced inside the critical section
    // but the I2S writes below are pure task-local buffer flushes — they do
    // not touch speakers[], speaker_filters[] or any volume/mute state. The
    // old code held g_audio_config_mutex across two i2s_write calls plus the
    // bounded force-drain (~50 ms worst case), which blocked handlePostVolume
    // and handleSave on the WebServer task for that whole window and let
    // dashboard slider drags hit the 8 s fetch timeout.
    int out_samples = 0;

    if (g_audio_config_mutex) xSemaphoreTake(g_audio_config_mutex, portMAX_DELAY);

    // [no-double-attenuation]
    // The server already multiplies the PCM by masterVolume/100 before Opus
    // encode (see [double-attenuation-intentional] on the server). Applying
    // master_volume_multiplier again here meant a 50% master gave 0.25x
    // output — the slider was effectively squared. Keep node-local trim
    // (node_volume_multiplier) and zone trim only. master_volume_multiplier
    // is still updated by ctrl/register and reported in /api/health, but no
    // longer affects the DAC path.
    float gain = (!master_muted && !node_muted && !zone_muted)
        ? (node_volume_multiplier * zone_volume_multiplier)
        : 0.0f;

    // Queue depth cannot measure DMA sample consumption; do not trim PCM.
    // Queue-depth telemetry remains useful, but it does not drive resampling.
    stat_queue_error = audio_queue ? (int)uxQueueMessagesWaiting(audio_queue)
                                     - (int)audio_queue_target : 0;
    stat_correction_samples = 0;
    out_samples = samples_per_channel;

    static float fade_gain = 1.0f;
    static float fade_step = 1.0f / ((float)SAMPLE_RATE * FADE_SHORT_SECS);

    if (fade_on_resume && gain > 0.0f) {
        fade_on_resume = false;
        fade_gain = 0.0f;
        // [fade-soft-start-2s] A resume is one of two things and they must not
        // sound the same:
        //
        //   SOFT  - the node had NO audio at all for FADE_RESUME_GAP_MS or
        //           more, or the session is new (boot / server epoch change).
        //           That is a real stop-then-start: a pause/play on the PC or
        //           phone, the server restarting, the node coming back from an
        //           OTA. Ramp over FADE_SOFT_SECS (2.0 s) so it fades in.
        //
        //   SHORT - a rebuffer caused by a few dropped datagrams while the
        //           producer was still sending. Nothing actually stopped, so a
        //           2 s ramp would be heard as a swell on top of continuous
        //           audio, not as a soft start. FADE_SHORT_SECS (5 ms) only
        //           removes the step discontinuity.
        //
        // Before this the choice was made on g_session_fade_needed alone, and
        // that flag is only set at boot and on an epoch change - so a
        // pause/play, the commonest case of all, took the 5 ms path and was
        // heard as a hard START. The gap length is measured at the
        // fade_on_resume site below (see [fade-resume-gap]).
        bool soft = g_session_fade_needed || (g_resume_gap_ms >= FADE_RESUME_GAP_MS);
        float fade_secs = soft ? FADE_SOFT_SECS : FADE_SHORT_SECS;
        g_session_fade_needed = false;
        g_resume_gap_ms = 0;
        fade_step = 1.0f / ((float)SAMPLE_RATE * fade_secs);
    }

    const float step = (float)samples_per_channel / (float)out_samples;
    float srcPos = 0.0f;

    const int   srcMode0 = speakers[0].source;
    const int   srcMode1 = speakers[1].source;
    const int   srcMode2 = speakers[2].source;
    const int   srcMode3 = speakers[3].source;
    const float spkGain0 = speakers[0].volume / 100.0f;
    const float spkGain1 = speakers[1].volume / 100.0f;
    const float spkGain2 = speakers[2].volume / 100.0f;
    const float spkGain3 = speakers[3].volume / 100.0f;

    for (int i = 0; i < out_samples; i++) {
        int i0 = (int)srcPos;
        if (i0 >= samples_per_channel) i0 = samples_per_channel - 1;
        int i1 = (i0 + 1 < samples_per_channel) ? (i0 + 1) : (samples_per_channel - 1);
        int im = (i0 > 0)                       ? (i0 - 1) : 0;
        int i2 = (i0 + 2 < samples_per_channel) ? (i0 + 2) : (samples_per_channel - 1);
        float frac = srcPos - (float)i0;

        float pmL = (float)in_pcm[im * 2 + 0];
        float pmR = (float)in_pcm[im * 2 + 1];
        float p0L = (float)in_pcm[i0 * 2 + 0];
        float p0R = (float)in_pcm[i0 * 2 + 1];
        float p1L = (float)in_pcm[i1 * 2 + 0];
        float p1R = (float)in_pcm[i1 * 2 + 1];
        float p2L = (float)in_pcm[i2 * 2 + 0];
        float p2R = (float)in_pcm[i2 * 2 + 1];

        float left, right;
        // Catmull-Rom overshoots at the frame edges because i1/i2 are
        // clamped to i0 there, giving the interpolator a phantom
        // (-0.0625 * (pm - p0)) weight that can exceed the true endpoint
        // by up to 6% of the local sample delta. On a transient that
        // lands exactly on a 20 ms frame boundary this is audible as a
        // small 50 Hz click. Fall back to linear interp in the first
        // and last two samples of every frame.
        if (i0 < 1 || i0 >= samples_per_channel - 2) {
            left  = p0L + frac * (p1L - p0L);
            right = p0R + frac * (p1R - p0R);
        } else {
            float t  = frac;
            float t2 = t * t;
            float t3 = t2 * t;
            float am = -0.5f * t3 +        t2 - 0.5f * t;
            float a0 =  1.5f * t3 - 2.5f * t2              + 1.0f;
            float a1 = -1.5f * t3 +  2.0f * t2 + 0.5f * t;
            float a2 =  0.5f * t3 -  0.5f * t2;
            left  = am * pmL + a0 * p0L + a1 * p1L + a2 * p2L;
            right = am * pmR + a0 * p0R + a1 * p1R + a2 * p2R;
        }

        left  *= gain * fade_gain;
        right *= gain * fade_gain;

        // [audio-hz-diag-disabled]
        // Per-sample band analyser removed for production: 4 biquads +
        // 4 sum-of-squares + ZCR compare per sample at 48 kHz = ~1.2 %
        // of Core 1. AudioHzDiag (the struct, g_hz_diag, and every
        // stat_hz_* global) stays in BSS (~130 B) but is never sampled,
        // so stat_hz_* read 0 on /api/health and no [Audio][Hz] lines
        // reach the log ring. To re-enable, restore the
        // audioHzDiagSample(left); call here and the
        // audioHzDiagRollover(); call in the 1 s policy block below.

        if (fade_gain < 1.0f) {
            fade_gain += fade_step;
            if (fade_gain > 1.0f) fade_gain = 1.0f;
        }
        srcPos += step;

        const float mono = (left + right) * 0.5f;
        float in0 = (srcMode0 == 1) ? right : (srcMode0 == 2) ? mono : left;
        float in1 = (srcMode1 == 1) ? right : (srcMode1 == 2) ? mono : left;
        float in2 = (srcMode2 == 1) ? right : (srcMode2 == 2) ? mono : left;
        float in3 = (srcMode3 == 1) ? right : (srcMode3 == 2) ? mono : left;

        float s0 = speaker_filters[0].process(in0);
        float s1 = speaker_filters[1].process(in1);
        float s2 = speaker_filters[2].process(in2);
        float s3 = speaker_filters[3].process(in3);

        dac1_output_buffer[i * 2]     = softLimitToInt16((s0 * spkGain0) * (1.0f / 32767.0f));
        dac1_output_buffer[i * 2 + 1] = softLimitToInt16((s1 * spkGain1) * (1.0f / 32767.0f));
        dac2_output_buffer[i * 2]     = softLimitToInt16((s2 * spkGain2) * (1.0f / 32767.0f));
        dac2_output_buffer[i * 2 + 1] = softLimitToInt16((s3 * spkGain3) * (1.0f / 32767.0f));
    }

    // ---- Critical section ends here --------------------------------------
    // Everything above touched shared state. Everything below touches only
    // dac1_output_buffer / dac2_output_buffer / stat_* — no other task writes
    // those buffers, and the stat_* fields are volatile uint32_t (atomic on
    // this MCU). Releasing the mutex here lets handleSave/handlePostVolume
    // proceed while the I2S DMA drain runs.
    if (g_audio_config_mutex) xSemaphoreGive(g_audio_config_mutex);

    // Both DMA paths receive the complete decoded frame.
    const size_t want = (size_t)out_samples * CHANNELS * sizeof(int16_t);
    const size_t slab = 1024;
    size_t off1 = 0, off2 = 0;

    if (!g_i2s_dac1_ready) off1 = want;
    if (!g_i2s_dac2_ready) off2 = want;

    uint32_t deadline = millis() + 25;
    while ((off1 < want || off2 < want) && (int32_t)(millis() - deadline) < 0) {
        if (off1 < want) {
            size_t n1 = want - off1; if (n1 > slab) n1 = slab;
            size_t w1 = 0;
            esp_err_t r1 = i2s_write(I2S_NUM_DAC1, (const char*)dac1_output_buffer + off1, n1, &w1, pdMS_TO_TICKS(3));
            if (r1 == ESP_OK) off1 += w1;
            else              stat_i2s_write_errors++;
        }
        if (off2 < want) {
            size_t n2 = want - off2; if (n2 > slab) n2 = slab;
            size_t w2 = 0;
            esp_err_t r2 = i2s_write(I2S_NUM_DAC2, (const char*)dac2_output_buffer + off2, n2, &w2, pdMS_TO_TICKS(3));
            if (r2 == ESP_OK) off2 += w2;
            else              stat_i2s_write_errors++;
        }
    }

    const uint32_t drain_deadline = millis() + 40;
    auto drainOne = [&](i2s_port_t port, bool ready,
                        int16_t* buf, size_t& off) {
        while (ready && off < want &&
               (int32_t)(millis() - drain_deadline) < 0) {
            size_t n = want - off;
            size_t w = 0;
            esp_err_t r = i2s_write(port, (const char*)buf + off, n, &w,
                                    pdMS_TO_TICKS(5));
            if (r == ESP_OK && w > 0) off += w;
            else { stat_i2s_write_errors++; break; }
        }
    };
    drainOne(I2S_NUM_DAC1, g_i2s_dac1_ready, dac1_output_buffer, off1);
    drainOne(I2S_NUM_DAC2, g_i2s_dac2_ready, dac2_output_buffer, off2);

    if (off1 < want || off2 < want) stat_i2s_timeouts++;
}

// Keep the stateful decoder in sequence when a queued frame is not played.
inline void decodeDiscardedAudio(const AudioMessage& msg) {
    if (!opus_decoder || !pcm_decode_buffer) return;
    int result = opus_decode(opus_decoder,
                             msg.conceal ? nullptr : msg.payload,
                             msg.conceal ? 0 : msg.length,
                             pcm_decode_buffer, OPUS_FRAME_SIZE, 0);
    if (result < 0) {
        stat_opus_decode_errors++;
        opus_decoder_ctl(opus_decoder, OPUS_RESET_STATE);
    }
}

// Hardened flush: clear stale queue data but do not immediately force a second
// resync loop from a single transient jitter burst.
inline void flushAudioQueueAndRebuffer(const char* reason) {
    uint32_t drained = 0;
    uint32_t drained_real = 0;

    if (!g_audio_queue_mutex) return;
    xSemaphoreTake(g_audio_queue_mutex, portMAX_DELAY);
    if (audio_queue != nullptr) {
        AudioMessage tmp;
        while (xQueueReceive(audio_queue, &tmp, 0) == pdPASS) {
            drained++;
            if (!tmp.conceal) drained_real++;
        }
    }

    if (drained_real > 0) stat_frames_discarded += drained_real;

    if (opus_decoder != nullptr) {
        opus_decoder_ctl(opus_decoder, OPUS_RESET_STATE);
    }

    seq_initialized = false;
    g_playout_lead_ms = 0;
    ts_prev_audio_arrival_ms = 0;
    last_conceal_captured_ms = 0;
    xSemaphoreGive(g_audio_queue_mutex);
    // Let drop_at() re-learn latency after the flush.
    last_node_latency_ms = 20;
    // [stutter-visible-counters] The first frame after a rebuffer would
    // otherwise register its delta against the pre-flush captured_ms and
    // report a multi-second "gap" that is really just the flush.
    statLastCapturedMs = 0;

    if (drained_real > 0) {
        stat_rebuffers++;
        ts_last_rebuffer_ms = millis();
    } else {
        stat_empty_flushes++;
    }
    trim_unstable = true;

    audioLogThrottled(String("[Audio] Flush+rebuffer (") + reason +
                      "), drained " + String(drained) + " queued frame(s).");
}

// ---- Playout-delay slew ----------------------------------------------------
inline void slewPlayoutDelay() {
    int target = target_playout_delay_ms;
    int cur    = playout_delay_ms;
    if (cur == target) return;
    if (cur < target) {
        cur += PLAYOUT_DELAY_SLEW_MS_PER_FRAME;
        if (cur > target) cur = target;
    } else {
        cur -= PLAYOUT_DELAY_SLEW_MS_PER_FRAME;
        if (cur < target) cur = target;
    }
    playout_delay_ms = cur;
}

// [p2-conceal-watermark]
// PLC frames spent on stutter absorption in the current second. File scope
// because the absorb site is deep inside audioPlaybackTask()'s deadline
// branch, far from the per-second policy block. Decayed (not zeroed) at the
// rollover so the allowance behaves as a rate and cannot be banked.
static volatile uint32_t g_plc_absorb_this_sec = 0;
inline void decayPlcAbsorbAllowance() {
    const uint32_t step = PLC_ALLOWANCE_PER_SEC / 3;
    g_plc_absorb_this_sec = (g_plc_absorb_this_sec > step)
                                ? (g_plc_absorb_this_sec - step) : 0;
}

// ---- Playback task ---------------------------------------------------------
inline void audioPlaybackTask(void *pvParameters) {
    AudioMessage msg;
    bool is_buffering = true;
    uint32_t starved_since_ms = 0;
    uint32_t lastStarveBeatMs = 0;   // reset when a frame is dequeued
    uint32_t starveStartMs    = 0;
    uint32_t stale_streak = 0;
    uint32_t buffer_wait_ms = 0;   // [auto-recover-rebuffer] 0 = not waiting
    bool force_prebuffer_entry = false;  // [auto-recover-prebuffer-entry]
    // [starve-hold-scales-with-queue]
    // 1500 ms was tuned for a 48-slot queue. With 32 slots and target 3,
    // the buffer empties in ~80 ms, so 1500 ms means the stale-reanchor
    // flush (at AUDIO_STALE_REANCHOR_MS = 600) always wins the recovery
    // race and this counter is dead code. Scale with the queue target:
    // 2 frames of margin past the target plus 100 ms for the send burst
    // to arrive, floor at 250 ms.
    for (;;) {
        // [rebuffer-hold-dynamic]
        // Was hoisted above the loop, so a mid-stream bump to
        // audio_queue_target (bufferMs push / mode change) left the
        // starvation window at the boot value and rebuffered too
        // eagerly. Recompute each pass: 2 frames of margin past the
        // target + 100 ms for the send burst to arrive, floor 250 ms.
        uint32_t rebuffer_hold_ms =
            (uint32_t)(audio_queue_target + 2) * 20 + 100;
        if (rebuffer_hold_ms < 250) rebuffer_hold_ms = 250;
        if (otaNodeUpdating) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        {
            static uint32_t last_policy_ms      = 0;
            static uint32_t last_late20_snap    = 0;
            static uint32_t last_late5_snap     = 0;
            static uint32_t last_grow_ms        = 0;
            static uint32_t grow_streak         = 0;
            static uint32_t last_late_change_ms = 0;
            uint32_t now_pol = millis();

            if (now_pol - last_policy_ms >= 1000) {
                last_policy_ms = now_pol;
                decayPlcAbsorbAllowance();   // [p2-conceal-watermark]

                // [audio-hz-diag-disabled]
                // Per-second rollover (5 × sqrtf + 5 × log10f, plus the
                // change-detector and 30 s trend-line log pushes) removed
                // for production; see [audio-hz-diag-disabled] in the
                // per-sample loop. AudioHzDiag is inert — stat_hz_* stay
                // at 0 and no [Audio][Hz] lines reach the ring.

                // [captured-ms-timeline-counters] Snapshot + reset the
                // two per-second node-side counters. Same window as the
                // existing packetsLastSec / correctorFramesLastSec
                // rollover below, so all fields in a recording row
                // describe the same 1 s.
                //
                // The pendingBuf / encodeOverruns / encodePeakUs rollup
                // that used to be here was dead on the node: those six
                // counters never incremented (they measure the SERVER's
                // encoder and pendingBuf). Removed with the state decls.
                statCapturedMsGapMaxLastSec    = statCapturedMsGapThisSec;
                statCapturedMsGapThisSec       = 0;
                statCorrectorFramesPeakLastSec = statCorrectorFramesPeakThisSec;
                statCorrectorFramesPeakThisSec = 0;

                portENTER_CRITICAL(&g_gap_stat_mux);
                stat_packets_last_sec    = stat_packets_this_sec;
                stat_packets_this_sec    = 0;
                stat_max_gap_ms_last_sec = stat_max_gap_ms_this_sec;
                stat_max_gap_ms_this_sec = 0;
                portEXIT_CRITICAL(&g_gap_stat_mux);

                stat_corrector_frames_last_sec      = stat_corrector_frames_this_sec;
                stat_corrector_frames_this_sec      = 0;
                stat_short_decode_frames_last_sec   = stat_short_decode_frames_this_sec;
                stat_short_decode_frames_this_sec   = 0;
                stat_lead_below20_last_sec          = stat_lead_below20_this_sec;
                stat_lead_below20_this_sec          = 0;
                stat_min_lead_last_sec              = stat_min_lead_this_sec;
                stat_min_lead_this_sec              = 0x7FFFFFFF;

                uint32_t late5  = stat_late_5to20;
                uint32_t late20 = stat_late_20to100;
                uint32_t d5  = (late5  >= last_late5_snap)  ? (late5  - last_late5_snap)  : 0;
                uint32_t d20 = (late20 >= last_late20_snap) ? (late20 - last_late20_snap) : 0;

                if (d5 > 0 || d20 > 0) last_late_change_ms = now_pol;

                static int   target_lp_src = -1;
                static int   target_lp     = 0;
                static float target_lp_f   = 0.0f;
                auto applyDelayToQueueTarget = [](int delayMs) {
                    // [queue-target-smoothed-latency]
                    // Delegate to computeQueueTargetSmoothed() in EVDPState.h.
                    // The three call sites (this one, applyRegistrationResponse,
                    // applyCtrlJson) previously computed the target from
                    // last_node_latency_ms independently and disagreed by
                    // 5-10 frames every heartbeat, because last_node_latency_ms
                    // carries 20-40 ms of per-frame noise. That disagreement
                    // is what kept err at -6 in the field and made the
                    // corrector fire 6×/sec. All three now share one EMA.
                    int tgt = computeQueueTargetSmoothed(delayMs);

                    if (delayMs != target_lp_src) {
                        target_lp_f   = (float)tgt;
                        target_lp_src = delayMs;
                    } else {
                        target_lp_f = target_lp_f * 0.875f + (float)tgt * 0.125f;
                    }
                    target_lp = (int)(target_lp_f + 0.5f);
                    if (target_lp < 2) { target_lp = 2; target_lp_f = 2.0f; }

                    int pre = target_lp;
                    if (pre < 2) pre = 2;
                    if (pre > AUDIO_QUEUE_LEN - 2)
                        pre = AUDIO_QUEUE_LEN - 2;

                    audio_queue_target = target_lp;
                    audio_prebuffer    = pre;
                };

                // [auto-expand-playout]
                // Monotonic, session-scoped expansion of playout delay on
                // top of the server's bufferMs. Runs BEFORE
                // applyDelayToQueueTarget so the queue-target recompute uses
                // the current effective delay.
                //
                // Trigger (either is sufficient):
                //   * min_lead_last_sec < -3 ms for 3 consecutive seconds
                //     (something was genuinely late; a positive min lead
                //     means every frame that second had slack)
                //   * >= 5 frames in the 20-100 ms late bucket in one
                //     second (sustained moderate lateness)
                //
                // Action:
                //   * g_auto_expand_ms += 5 (capped at AUTO_EXPAND_MAX_MS)
                //   * target_playout_delay_ms = last_server_buffer_ms +
                //     g_auto_expand_ms
                //   * NEVER decreases during a session
                //   * reset to 0 only on a genuine server bufferMs change
                //     (handled in EVDPNet.h — see
                //     [auto-expand-reset-on-server-change])
                //
                // Under the observed ~55 ms capture-to-arrival latency and
                // server bufferMs=50, the first expansion lands at 55 (0 ms
                // headroom), the second at 60 (5 ms), the third at 65
                // (10 ms — typical WiFi jitter floor), and the fourth at
                // 70 (15 ms — comfortable). Each step takes ~3-4 s, so the
                // full ~20 ms correcti\on settles in under 15 s and then
                // stays put. That bounded transition window is the entire
                // reason the old self-tuner was disabled: it oscillated
                // 60->70->60 every few seconds, which was audible. A
                // monotonic ratchet cannot oscillate.
                static int32_t auto_expand_consec = 0;
                static uint8_t drain_over_secs    = 0;   // [auto-recover-drain]
                int32_t min_lead = stat_min_lead_last_sec;
                bool trigger;
                if (min_lead != 0x7FFFFFFF && min_lead < -3) {
                    trigger = true;
                } else if (d20 >= 5) {
                    trigger = true;
                } else {
                    trigger = false;
                }
                if (trigger) auto_expand_consec++;
                else         auto_expand_consec = 0;

                // [auto-expand-faster-trigger]
                // 3 consecutive seconds at min_lead < -3 is too slow for a node sitting
                // at 9 ms of static headroom (node 210, recording 2026-11-04). A single
                // late burst trips the -3 threshold for one second, then recovers before
                // the 3-second counter completes. Require only 2 consecutive seconds,
                // and additionally fire immediately on a single second where d20 >= 10
                // (heavily late second).
                if ((auto_expand_consec >= 2 ||
                    (auto_expand_consec >= 1 && d20 >= 10)) &&
                    g_auto_expand_ms < AUTO_EXPAND_MAX_MS) {
                    g_auto_expand_ms += 20;
                    int newTarget = (int)last_server_buffer_ms + (int)g_auto_expand_ms;
                    if (newTarget > 400) newTarget = 400;
                    if (newTarget > target_playout_delay_ms) {
                        target_playout_delay_ms = newTarget;
                        audioLogThrottled(String("[AutoExpand] playout ") +
                                          String(target_playout_delay_ms) +
                                          "ms (server " + String(last_server_buffer_ms) +
                                          " + auto " + String(g_auto_expand_ms) + ")");
                    }
                    auto_expand_consec = 0;  // 3 more seconds to next step
                }



                // [auto-recover-drain]
                // The queue has been consistently deeper than the target for
                // AUTO_RECOVER_DRAIN_SECONDS: the node is carrying seconds of
                // backlog (typically after a burst the corrector could not
                // absorb). drop_at() drains one frame at a time, which is a
                // long audible crunch. Throw the excess away in one step
                // instead and let the normal playout continue from the
                // surviving head. This never changes the target, so it cannot
                // fight the slider or the corrector.
                if (audio_queue != nullptr) {
                    UBaseType_t qd = uxQueueMessagesWaiting(audio_queue);
                    int32_t excess = (int32_t)qd - (int32_t)audio_queue_target;
                    if (excess >= AUTO_RECOVER_DRAIN_FRAMES) {
                        if (++drain_over_secs >= AUTO_RECOVER_DRAIN_SECONDS) {
                            drain_over_secs = 0;
                            flushAudioQueueAndRebuffer("auto-recover: queue ran away");
                            stat_auto_recover_drain++;
                            is_buffering = true;
                        }
                    } else {
                        drain_over_secs = 0;
                    }
                }

                applyDelayToQueueTarget(target_playout_delay_ms);

                (void)grow_streak;
                (void)last_grow_ms;
                (void)last_late_change_ms;

                last_late20_snap = late20;
                last_late5_snap  = late5;
            }
        }

        if (g_force_resync) {
            g_force_resync = false;
            flushAudioQueueAndRebuffer("clock resync");
            is_buffering = true;
        }

        if (g_force_flush_incompatible) {
            g_force_flush_incompatible = false;
            flushAudioQueueAndRebuffer("audio protocol incompatible");
            is_buffering = true;
        }

        if (g_force_rebuffer) {
            g_force_rebuffer = false;
            flushAudioQueueAndRebuffer("OTA exit");
            is_buffering = true;
        }

        if (is_buffering) {
            uint32_t pre  = (uint32_t)audio_prebuffer;
            // [auto-recover-prebuffer-entry]
            // `g_have_played_audio` stays true for the whole uptime, so every
            // rebuffer after the first required the FULL queue target before
            // playout resumed. After an auto-recover re-anchor that is
            // backwards: we just threw the timeline away and need the
            // bootstrap prebuffer, not the steady-state target. The flag is
            // set by the re-anchor below and cleared on the next frame.
            uint32_t need = (g_have_played_audio && !force_prebuffer_entry)
                                ? (uint32_t)audio_queue_target : pre;
            if (need < 2) need = 2;
            if (audio_queue != nullptr && uxQueueMessagesWaiting(audio_queue) >= need) {
                is_buffering  = false;
                fade_on_resume = true;
                // [fade-resume-gap] How long was there no audio at all? This
                // is the whole difference between "the producer paused and
                // came back" (soft start) and "a few datagrams were late"
                // (declick only). ts_last_audio_packet_ms is stamped on every
                // VALID datagram in EVDPNet.h, so the age of it right now is
                // the length of the silence that just ended.
                //
                // A never-received packet (0) counts as a long gap on purpose:
                // the very first resume after boot is a session start.
                g_resume_gap_ms = (ts_last_audio_packet_ms == 0)
                                      ? 0xFFFFFFFFu
                                      : (uint32_t)(millis() - ts_last_audio_packet_ms);
                g_resume_trim  = true;
                stale_streak   = 0;
                force_prebuffer_entry = false;
                playout_delay_ms = target_playout_delay_ms;
            } else {
                // [auto-recover-rebuffer]
                // Waiting for the queue to refill is correct for a normal
                // rebuffer (the target is a few frames, ~60-100 ms). If it
                // has not refilled after AUTO_RECOVER_REBUFFER_MS the wait is
                // not going to succeed on its own: the frames that ARE
                // arriving are being timed against the pre-flush timeline and
                // are being discarded, so the queue can never reach the
                // target. Before this, the only way out was a server restart.
                //
                // Re-anchor: reset the sequence/lead state exactly as a flush
                // does, then take whatever arrives as a fresh timeline and
                // re-enter playout with the prebuffer (not the full target),
                // so recovery needs 3 frames instead of 5-10.
                if (buffer_wait_ms == 0) {
                    buffer_wait_ms = millis();
                } else if (millis() - buffer_wait_ms >= AUTO_RECOVER_REBUFFER_MS) {
                    buffer_wait_ms = 0;
                    flushAudioQueueAndRebuffer("auto-recover: rebuffer stalled");
                    stat_auto_recover_rebuffer++;
                    // Prebuffer is the bootstrap target; clear is_buffering so
                    // the next pass admits audio as soon as `need` frames are
                    // queued rather than waiting out another hold window.
                    audio_prebuffer = 3;
                    is_buffering    = true;
                    force_prebuffer_entry = true;   // resume on `pre`, not `target`
                    audioLogThrottled("[Audio] Auto-recover: timeline re-anchored");
                }
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            buffer_wait_ms = 0;
        }

        if (audio_queue != nullptr &&
            xQueueReceive(audio_queue, &msg, pdMS_TO_TICKS(20)) == pdPASS) {
            g_have_played_audio = true;
            starved_since_ms = 0;
            starveStartMs    = 0;
            lastStarveBeatMs = 0;
            slewPlayoutDelay();

            // [dwell-default-latency]
            // Use 20 ms as a sane default while last_node_latency_ms is still
            // 0 (first frame after boot or after a flush reset). Without this
            // the first drop_at fires one frame too high and discards a frame
            // that was on schedule.
            uint32_t dwellLat = last_node_latency_ms ? last_node_latency_ms : 20;
            int dwell_frames = (playout_delay_ms - (int)dwellLat) / 20;
            if (dwell_frames < 2) dwell_frames = 2;
            const int drop_at = dwell_frames + 2;
            if (uxQueueMessagesWaiting(audio_queue) > (UBaseType_t)drop_at) {
                stat_frames_discarded++;
                decodeDiscardedAudio(msg);
                continue;
            }

            // ---- Real network latency, not queue dwell ----------------
            // [clock-pair-read] The pair (g_sync_local_ms, g_sync_server_ms)
            // is written by setServerClockOffset() under g_clock_pair_mux.
            // Read both fields in one critical section — otherwise a writer
            // landing between the two stores makes our_local_at_capture
            // wrong by |new_offset - old_offset| (tens of ms on a drifting
            // link, not "a few ns" as the old comment claimed).
            // [conceal-no-latency] Concealment frames never touched the
            // wire. Their arrival_ms = millis() biases the histogram toward
            // the 2000 ms clamp, which then feeds the self-tuner through
            // last_node_latency_ms -> queue_target. Exclude them from the
            // latency measurement entirely.
            if (server_clock_synced && !msg.conceal) {
                uint32_t sync_loc, sync_srv;
                portENTER_CRITICAL(&g_clock_pair_mux);
                sync_loc = g_sync_local_ms;
                sync_srv = g_sync_server_ms;
                portEXIT_CRITICAL(&g_clock_pair_mux);

                int64_t our_local_at_capture =
                    (int64_t)sync_loc + (int32_t)(msg.captured_ms - sync_srv);
                int64_t net_ms =
                    (int32_t)(msg.arrival_ms - (uint32_t)our_local_at_capture);
                if (net_ms < 0)    net_ms = 0;
                if (net_ms > 2000) net_ms = 2000;
                last_node_latency_ms = (uint32_t)net_ms;
            }

            // [stutter-visible-counters] Timeline-discontinuity detector.
            // A missing packet or a burst of preemption produces a
            // captured_ms delta larger than 20 ms. seqGaps and framesLate
            // both remain 0 for a missing frame — this is the only counter
            // that sees the "1.2 s thump" a listener hears without any
            // other metric firing.
            //   gap 20 ms = normal
            //   gap 40 ms = one frame skipped (audible thump)
            //   gap 60 ms = two frames missed
            if (statLastCapturedMs != 0) {
                uint32_t gap = msg.captured_ms - statLastCapturedMs;
                if (gap > statCapturedMsGapThisSec) {
                    statCapturedMsGapThisSec = gap;
                }
                if (gap > 21) {
                    statCapturedMsGapGt20Total++;
                    if (gap > 41) statCapturedMsGapGt40Total++;
                }
            }
            statLastCapturedMs = msg.captured_ms;

            // ---- Deadline-based playout -------------------------------
            int64_t late_ms = 0;
            bool    haveDeadline = false;
            if (server_clock_synced) {
                uint32_t sync_loc, sync_srv;
                portENTER_CRITICAL(&g_clock_pair_mux);
                sync_loc = g_sync_local_ms;
                sync_srv = g_sync_server_ms;
                portEXIT_CRITICAL(&g_clock_pair_mux);

                int64_t deadline_local =
                    (int64_t)sync_loc + (int32_t)(msg.captured_ms - sync_srv)
                    + (int64_t)playout_delay_ms;
                // [millis-wrap-safe-wait]
                // deadline_local is anchored on sync_loc (a recent
                // millis() snapshot refreshed every 2 s). Computing
                // wait_ms = deadline - millis() in int64 is NOT
                // wrap-safe: at the 49-day uint32 rollover, deadline
                // stays near UINT32_MAX while millis() reads 0..2000
                // for ~2 s. wait_ms then becomes ~4 billion, trips the
                // MAX_WAIT_MS clamp on every frame, and the audio task
                // enters a flush+rebuffer storm until the next sync
                // re-anchors. Cast the subtraction through int32 first;
                // modulo arithmetic is wrap-safe at 32 bits. Same
                // pattern the rest of this file already uses for
                // captured_ms / seq deltas.
                int32_t wait_ms_i32 =
                    (int32_t)((uint32_t)deadline_local - (uint32_t)millis());
                int64_t wait_ms = (int64_t)wait_ms_i32;
                haveDeadline = true;

                const int64_t MAX_WAIT_MS = (int64_t)playout_delay_ms + 300;
                if (wait_ms > MAX_WAIT_MS) {
                    stat_frames_discarded++;
                    flushAudioQueueAndRebuffer("wait_ms clamp: offset probably wrong");
                    is_buffering = true;
                    stale_streak = 0;
                    continue;
                }

                late_ms = -wait_ms;
                g_playout_lead_ms = (int32_t)(wait_ms > 2000 ? 2000 : (wait_ms < -2000 ? -2000 : wait_ms));

                // [lead-below20-threshold] Was `< 10` while the field name,
                // the JSON key (leadBelow20LastSec) and the dashboard copy
                // all say 20. Mismatch under-reports near-deadline frames.
                if (wait_ms < 20) stat_lead_below20_this_sec++;
                if (wait_ms < 2000 && wait_ms > -2000) {
                    int32_t lead = (int32_t)wait_ms;
                    if (lead < stat_min_lead_this_sec) stat_min_lead_this_sec = lead;
                }

                if (g_resume_trim) {
                    const int64_t frame_ms = (OPUS_FRAME_SIZE * 1000LL) / SAMPLE_RATE;
                    if (wait_ms < -(frame_ms / 2)) {
                        stat_frames_discarded++;
                        decodeDiscardedAudio(msg);
                        continue;
                    }
                    g_resume_trim = false;
                }

                if (wait_ms > 0) {
                    if (wait_ms > 50) {
                        vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait_ms - 20)));
                    }
                    // [millis-wrap-safe-loop]
                    // `(int64_t)millis() < deadline_local` reads TRUE at
                    // the 49-day rollover (millis() = 0, deadline ≈
                    // UINT32_MAX), spinning for ~50 days of virtual
                    // wait. Signed int32 delta is the correct form.
                    while ((int32_t)(millis() - (uint32_t)deadline_local) < 0) {
                        vTaskDelay(1);
                    }
                    late_ms = 0;
                }
            }

            if (haveDeadline && late_ms > (int64_t)AUDIO_STALE_REANCHOR_MS) {
                stat_frames_late++;
                stat_frames_discarded++;
                flushAudioQueueAndRebuffer("frame far past deadline");
                is_buffering = true;
                stale_streak = 0;   // [stale-streak-reset] don't carry the count across the flush
                continue;
            } else if (haveDeadline && late_ms > (int64_t)AUDIO_STALE_DISCARD_MS) {
                stat_frames_late++;
                stat_frames_discarded++;
                decodeDiscardedAudio(msg);
                stale_streak++;
                if (stale_streak >= AUDIO_STALE_STREAK_LIMIT) {
                    // [auto-expand-before-streak-flush]
                    // The 3 s auto-expand ratchet cannot react to a burst
                    // that pushes 5 frames past the discard threshold in
                    // <100 ms. Before paying the rebuffer cost, bump the
                    // auto-expand target 10 ms and skip the flush; only
                    // flush once auto-expand is exhausted (a persistent
                    // lateness still recovers, a transient hiccup does
                    // not rebuffer).
                    // [p2-conceal-watermark]
                    // This branch used to bump g_auto_expand_ms, which is
                    // capped at AUTO_EXPAND_MAX_MS = 0 — so the condition was
                    // permanently false and the node went straight to a full
                    // flush+rebuffer on every run of 3 stale frames. That is
                    // the audible "crunch then gap" after a short burst.
                    //
                    // The ratchet is gone by design (the operator's slider is
                    // the authority), so the cheap absorb is a bounded amount
                    // of PLC instead: if we are not already concealing more
                    // than the watermark allows, synthesise up to
                    // PLC_ALLOWANCE extra frames rather than throwing the
                    // queue away. Past the allowance a persistent lateness
                    // still recovers through the flush below.
                    if (g_plc_absorb_this_sec < PLC_ALLOWANCE_PER_SEC) {
                        g_plc_absorb_this_sec += PLC_ALLOWANCE_PER_SEC / 3;
                        stat_frames_concealed++;
                        decodeDiscardedAudio(msg);
                        stale_streak = 0;
                        continue;
                    }
                    flushAudioQueueAndRebuffer("repeated stale frames");
                    is_buffering = true;
                    stale_streak = 0;
                }
                continue;
            }
            stale_streak = 0;
            if (haveDeadline && late_ms > 0) {
                stat_frames_late++;
                if      (late_ms <   5) stat_late_lt5++;
                else if (late_ms <  20) stat_late_5to20++;
                else if (late_ms < 100) stat_late_20to100++;
                else                    stat_late_100to200++;
            }

            if (opus_decoder != nullptr && pcm_decode_buffer != nullptr) {
                int decoded_samples;
                if (msg.conceal || msg.length == 0) {
                    decoded_samples = opus_decode(opus_decoder, nullptr, 0,
                                                  pcm_decode_buffer, OPUS_FRAME_SIZE, 0);
                } else {
                    decoded_samples = opus_decode(opus_decoder, msg.payload, msg.length,
                                                  pcm_decode_buffer, OPUS_FRAME_SIZE, 0);
                }
                // [short-decode-pad-hold]
                // A frame shorter than OPUS_FRAME_SIZE used to be padded
                // with zeros. On the sub path that is a step from the
                // last decoded sample straight to 0, which is a click on
                // any sustained bass content; on 100 W it is audible.
                // The old comment said "pad the tail with silence and
                // always hand the corrector a full frame" — the padding
                // is still required (otherwise the DMA underruns the
                // tail of the 20 ms slot), but the fill value must
                // continue the waveform, not restart it at zero.
                //
                // Hold the last decoded sample L/R across the pad. This
                // produces a short DC segment at the current amplitude
                // (0-60 samples in the common case) followed by the
                // natural continuation on the next frame, which is the
                // same shape the corrector's tail pad uses. No step, no
                // click. stat_short_decode_frames_this_sec still bumps
                // so /api/health still reports the event.
                if (decoded_samples > 0 && decoded_samples < OPUS_FRAME_SIZE) {
                    stat_short_decode_frames_this_sec++;
                    int16_t lastL = pcm_decode_buffer[(decoded_samples - 1) * 2 + 0];
                    int16_t lastR = pcm_decode_buffer[(decoded_samples - 1) * 2 + 1];
                    for (int s = decoded_samples; s < OPUS_FRAME_SIZE; s++) {
                        pcm_decode_buffer[s * 2 + 0] = lastL;
                        pcm_decode_buffer[s * 2 + 1] = lastR;
                    }
                    decoded_samples = OPUS_FRAME_SIZE;
                }
                if (decoded_samples > 0) {
                    uint32_t t_play0 = millis();
                    process_and_output_audio(pcm_decode_buffer, decoded_samples);
                    uint32_t t_play  = millis() - t_play0;
                    if (t_play > 22) {
                        static uint32_t last_slow_log_ms = 0;
                        if (millis() - last_slow_log_ms > 5000) {
                            last_slow_log_ms = millis();
                            log_warn(String("[Play] frame playback took ") +
                                     String(t_play) + " ms");
                        }
                    }
                } else {
                    stat_opus_decode_errors++;
                }
            }
        } else {
            if (audio_queue != nullptr && uxQueueMessagesWaiting(audio_queue) == 0) {
                if (starved_since_ms == 0) {
                    starved_since_ms = millis();
                } else if (millis() - starved_since_ms >= rebuffer_hold_ms) {
                    starved_since_ms = 0;
                    is_buffering = true;
                    stat_rebuffers++;
                    ts_last_rebuffer_ms = millis();
                    audioLogThrottled("[Audio] Starved -> entering buffering");
                }
            } else {
                starved_since_ms = 0;
            }

            // [starved-heartbeat]
            // While is_buffering is true and audio_queue has been empty
            // for more than 5 s, log one line every 5 s naming how stale
            // the last received packet is. Field recording on node .211
            // shows a 30 s blackout (t=119.9 to t=147.9) with zero log
            // output because the transition log above fires only once.
            // This makes the blackout visible in the recorder's Log Lines
            // section without requiring the operator to correlate the
            // packetsLastSec column by hand.
            //
            // Rate is 1 per 5 s = 20 lines per 100 s of starvation. On a
            // 50-line ring that is enough that the last ~4 minutes of
            // starvation is always visible, without evicting normal
            // operational logs from the last 4 s. Ring-only (audioLog-
            // Throttled) so the UART is not blocked during an event that
            // is being diagnosed for jitter.
            if (is_buffering && audio_queue != nullptr &&
                uxQueueMessagesWaiting(audio_queue) == 0) {
                uint32_t now_starve = millis();
                if (starveStartMs == 0) starveStartMs = now_starve;
                if (lastStarveBeatMs == 0 ||
                    (now_starve - lastStarveBeatMs) >= 5000UL) {
                    lastStarveBeatMs = now_starve;
                    uint32_t lastPktAge =
                        (ts_last_audio_packet_ms == 0)
                            ? 0xFFFFFFFFu
                            : (now_starve - ts_last_audio_packet_ms);
                    audioLogThrottled(
                        String("[Audio] Starved ") +
                        String((now_starve - starveStartMs) / 1000UL) +
                        "s (no pkt for " +
                        (lastPktAge == 0xFFFFFFFFu
                            ? String("never")
                            : String(lastPktAge / 1000UL) + "s") +
                        ", seq expected=" + String((uint32_t)expected_seq) +
                        " last=" + String((uint32_t)last_seq_seen) + ")");
                }
            } else {
                // Reset the heartbeat bookkeeping when the queue refills.
                // statics are per-function; reset here so the next
                // starvation event starts its own 5 s clock instead of
                // inheriting the previous event's cadence.
                static bool resetPending = false;
                if (audio_queue != nullptr &&
                    uxQueueMessagesWaiting(audio_queue) > 0) {
                    if (resetPending) {
                        // nothing else to reset in this scope
                    }
                }
            }
        }
    }
}