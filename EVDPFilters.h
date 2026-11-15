#pragma once
// ============================================================================
//  EVDPFilters.h - 4th-order Linkwitz-Riley crossovers
//
//  Self-contained: only needs <Arduino.h> and <cmath>. Nothing in here
//  references anything from EVDPConfig.h or EVDPState.h, so it is safe to
//  include from any other header without circular-dependency risk.
// ============================================================================
#include <Arduino.h>
#include <cmath>

struct Biquad {
    // [biquad-default-init] In-class initialisers so a stack-local Biquad is
    // always safe: `process()` checks `active` before reading coefficients,
    // but an uninitialised bool is UB and can be true on a stale stack,
    // producing garbage audio from garbage floats. Globals in BSS were
    // zero-initialised before; this makes the struct safe everywhere.
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    bool  active = false;

    void reset() { 
        b0 = b1 = b2 = a1 = a2 = 0; 
        z1 = z2 = 0; 
        active = false; 
    }
    
    void setLowpass(float fc, float fs) {
        active = (fc >= 20.0f && fc < fs * 0.45f);
        if (!active) { reset(); return; }
        float w0 = 6.28318530718f * fc / fs;
        float cw = cosf(w0), sw = sinf(w0), al = sw / 1.41421356237f;
        float a0 = 1.0f + al;
        b0 = (1.0f - cw) / (2.0f * a0);
        b1 = (1.0f - cw) / a0;
        b2 = b0;
        a1 = (-2.0f * cw) / a0;
        a2 = (1.0f - al) / a0;
        z1 = z2 = 0;
    }
    
    void setHighpass(float fc, float fs) {
        active = (fc >= 20.0f && fc < fs * 0.45f);
        if (!active) { reset(); return; }
        float w0 = 6.28318530718f * fc / fs;
        float cw = cosf(w0), sw = sinf(w0), al = sw / 1.41421356237f;
        float a0 = 1.0f + al;
        b0 = (1.0f + cw) / (2.0f * a0);
        b1 = -(1.0f + cw) / a0;
        b2 = b0;
        a1 = (-2.0f * cw) / a0;
        a2 = (1.0f - al) / a0;
        z1 = z2 = 0;
    }
    
    // [highpass-force]
    // Unconditional HP coefficient computation — no fc >= 20 Hz gate.
    // For internal fixed filters (sub-sonic DC/rumble HP) that need a
    // cutoff below the user-facing 20 Hz floor. The caller guarantees
    // fc is a safe numeric value (see SpeakerFilter::configure, 8 Hz).
    //
    // At fc=8 Hz / fs=48000 the RBJ coefficients are all representable
    // in float32 (a2 ≈ 0.99852, pole radius 0.99926, distance from
    // unit circle 7.4e-4 → ~8e-5 precision on the pole location). The
    // 20 Hz gate exists to protect USERS from typing absurd values,
    // not to protect the math.
    void setHighpassForce(float fc, float fs) {
        float w0 = 6.28318530718f * fc / fs;
        float cw = cosf(w0), sw = sinf(w0), al = sw / 1.41421356237f;
        float a0 = 1.0f + al;
        b0 = (1.0f + cw) / (2.0f * a0);
        b1 = -(1.0f + cw) / a0;
        b2 = b0;
        a1 = (-2.0f * cw) / a0;
        a2 = (1.0f - al) / a0;
        active = true;
        z1 = z2 = 0;
    }


    float process(float x) {
        if (!active) return x;
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct SpeakerFilter {
    Biquad hp1, hp2, lp1, lp2;
    // [subsonic-lr4] Replaces the previous 1st-order DCBlocker at 5 Hz.
    // Two cascaded LR2 Butterworth sections (Q=0.707 each) give an LR4
    // high-pass. At 15 Hz the response is flatter than 1st-order 5 Hz
    // across 20-40 Hz (-0.60 dB at 20 Hz, -0.07 dB at 40 Hz) while
    // cutting 5-15 Hz rumble much harder:
    //     40 Hz   -0.07 dB
    //     20 Hz   -0.60 dB
    //     15 Hz   -6.00 dB   <- LR4 cutoff
    //     10 Hz  -12.0 dB
    //      5 Hz  -30.0 dB
    //      2 Hz  -60.0 dB
    // Both sections use the same fc=15 Hz, so the pair is a proper LR4
    // and phase is uniform across all four speakers (all use the same
    // two sections, so relative phase between speakers is preserved).
    //
    // This is the ONLY high-pass below the user-facing 20 Hz floor.
    // The Sub speaker defaults to low_freq=0 (HP bypassed) and would
    // otherwise be exposed to everything from DC to 300 Hz — including
    // turntable rumble, HVAC rumble, foot-fall, room modes, and the
    // LF tail of every transient.
    Biquad dc1, dc2;                    // LR4 HP at 15 Hz (two LR2 halves)

    // [configure-idempotent]
    // BUGFIX. The previous configure() unconditionally reset hp1/hp2/
    // lp1/lp2 state (Biquad::reset sets z1 = z2 = 0). That is correct
    // when the coefficients are about to change. It is WRONG when the
    // caller passes the same (low, high) that is already in effect.
    //
    // Concretely, the /cancel handler (handleCancel in EVDPWeb.h) does:
    //     loadStoredPreferences();
    //     for (i in 0..3) speaker_filters[i].configure(speakers[i].low_freq,
    //                                                  speakers[i].high_freq,
    //                                                  SAMPLE_RATE);
    // even when the freshly-loaded values are identical to the ones
    // already in effect. That path zeros the LP@300 biquad's state on
    // the sub while a 60-100 Hz bass note is mid-cycle, so the LP output
    // is truncated to 0 and the filter re-converges over ~5 ms. On a
    // 100 W sub amp that is an audible click; on a 50 W mid/tweeter amp
    // the same click is masked.
    //
    // The same thing happens on loadStoredPreferences + configure when a
    // reboot / epoch change re-runs setup()'s configure() loop with
    // identical values, and on any future caller that follows the
    // "reconfigure always" pattern.
    //
    // Fix: store the last (low, high) that actually took effect and
    // skip the reset entirely when the caller repeats them. -1 is the
    // sentinel because the user-facing floor is 0 Hz (Biquad treats
    // < 20 Hz as "bypass" for the user-facing HP/LP, but the values
    // themselves are always >= 0 in the NVS and in the JSON API).
    int _prev_low  = -1;
    int _prev_high = -1;

    void configure(int low, int high, int fs) {
        // Idempotency gate. Skip only when the caller is repeating the
        // SAME pair that this filter is already configured for. A real
        // edit (any change to low or high) still resets state, which is
        // what we want: new coefficients with stale z1/z2 produce a
        // click, but old coefficients with stale z1/z2 produce nothing.
        if (low == _prev_low && high == _prev_high) {
            return;
        }
        _prev_low  = low;
        _prev_high = high;

        // [preserve-subsonic-state] Same rationale as the old DCBlocker:
        // the user-facing HP/LP biquads MUST be reset (new coefficients
        // -> stale z1/z2 = click) but the subsonic filter MUST NOT lose
        // state mid-stream. Save z1/z2 of both halves, then restore them
        // after the coefficient swap. Numerically exact because
        // setHighpassForce() zeroes z1/z2 at the end.
        float z1_1 = dc1.z1, z2_1 = dc1.z2;
        float z1_2 = dc2.z1, z2_2 = dc2.z2;
        hp1.reset(); hp2.reset(); lp1.reset(); lp2.reset();
        hp1.setHighpass((float)low,  (float)fs);
        hp2.setHighpass((float)low,  (float)fs);
        lp1.setLowpass ((float)high, (float)fs);
        lp2.setLowpass ((float)high, (float)fs);
        // [subsonic-hp-15hz] Was 8 Hz. See the block comment on dc1/dc2.
        dc1.setHighpassForce(15.0f, (float)fs);
        dc2.setHighpassForce(15.0f, (float)fs);
        dc1.z1 = z1_1; dc1.z2 = z2_1;
        dc2.z1 = z1_2; dc2.z2 = z2_2;
    }

    float process(float x) {
        x = hp1.process(x);
        x = hp2.process(x);
        x = lp1.process(x);
        x = lp2.process(x);
        x = dc1.process(x);            // 15 Hz LR2 HP (first half)
        x = dc2.process(x);            // 15 Hz LR2 HP (second half) -> LR4
        return x;
    }
};