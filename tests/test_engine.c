/* Engine behaviour (v0.3), native with ASan/UBSan. */
#include "../dsp/dirt_engine.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static dd_engine e;
static double bass_tilt, stereo_corr;
static float buf[2 * 44100];

static void setup(dd_engine *x, float mix, float drive, float time_ms, float fb) {
    dd_init(x);
    dd_set_param(x, DD_MIX, mix);
    dd_set_param(x, DD_DRIVE, drive);
    dd_set_param(x, DD_TIME, time_ms);
    dd_set_param(x, DD_FEEDBACK, fb);
    dd_snap(x);
}
static void run(dd_engine *x, float *lr, int frames) {
    for (int i = 0; i < frames; i += 128) dd_process(x, lr + i * 2, frames - i < 128 ? frames - i : 128);
}
static int first_nonzero_ch(const float *lr, int frames, int ch) {
    for (int i = 0; i < frames; ++i) if (fabsf(lr[i * 2 + ch]) > 1e-9f) return i;
    return -1;
}
/* The right channel runs at Time itself (the left at 3/4 of it). */
static int first_nonzero(const float *lr, int frames) { return first_nonzero_ch(lr, frames, 1); }
static float noise(void) { return (float)rand() / RAND_MAX * 2.f - 1.f; }
static double rms_db(const float *lr, int frames) {
    double acc = 0;
    for (int i = 0; i < 2 * frames; ++i) acc += (double)lr[i] * lr[i];
    return 10 * log10(acc / (2 * frames) + 1e-30);
}
/* Plucked notes every 500 ms (220-440 Hz, 150 ms decay, peak 0.5): the
   spiky material that once tripped the ceiling on the wet side alone. */
static void plucks(float *lr, long t0, int frames) {
    for (int i = 0; i < frames; ++i) {
        long t = t0 + i;
        float ph = (float)(t % 22050) / 44100.f;
        float hz = 220.f * powf(2.f, (float)((t / 22050) % 5) / 5.f);
        lr[2 * i] = lr[2 * i + 1] = 0.5f * expf(-ph / 0.15f) * sinf(6.2831853f * hz * ph);
    }
}
/* Feed `seconds` of noise at `amp`; return output level minus input level
   over the last second, in dB. */
static double level_change(dd_engine *x, float amp, int seconds) {
    double in_db = 0, out_db = 0;
    for (int s = 0; s < seconds; ++s) {
        for (int i = 0; i < 2 * 44100; ++i) buf[i] = amp * noise();
        in_db = rms_db(buf, 44100);
        run(x, buf, 44100);
        out_db = rms_db(buf, 44100);
    }
    return out_db - in_db;
}

/* Distortion of a 110 Hz sine through the wet path alone: harmonics 2..9
   against the fundamental, over the last half second. */
static double thd(dd_engine *x) {
    const float hz = 110.f;
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < 44100; ++i)
            buf[2 * i] = buf[2 * i + 1] = 0.25f * sinf(6.2831853f * hz * (float)(s * 44100 + i) / 44100.f);
        run(x, buf, 44100);
    }
    double h[10] = {0}, hs = 0;
    for (int k = 1; k < 10; ++k) {
        double re = 0, im = 0;
        for (int i = 22050; i < 44100; ++i) {
            double ph = 6.283185307 * k * hz * i / 44100;
            re += buf[2 * i] * cos(ph); im += buf[2 * i] * sin(ph);
        }
        h[k] = sqrt(re * re + im * im);
    }
    for (int k = 2; k < 10; ++k) hs += h[k] * h[k];
    return sqrt(hs) / h[1];
}

int main(void) {
    /* Silence in, silence out, in every switch combination and at full gain. */
    for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
        setup(&e, 1, 0.8f, 30, 0.9f);
        dd_set_param(&e, DD_FX1, a); dd_set_param(&e, DD_FX2, b); dd_set_param(&e, DD_GAIN, 24);
        memset(buf, 0, sizeof(buf));
        run(&e, buf, 44100);
        for (int i = 0; i < 2 * 44100; ++i) assert(buf[i] == 0);
    }
    /* Mix 0 is exactly the dry input, whatever Input Gain says. */
    setup(&e, 0, 1, 50, 0.9f);
    dd_set_param(&e, DD_GAIN, 24); dd_snap(&e);
    for (int i = 0; i < 2 * 4096; ++i) buf[i] = noise();
    static float copy[2 * 4096]; memcpy(copy, buf, sizeof(copy));
    run(&e, buf, 4096);
    assert(!memcmp(copy, buf, sizeof(copy)));
    /* The wet path is two delays in series and nothing else: an impulse first
       returns at exactly 2*D (no filter to smear it). */
    int times[] = {10, 125, 250};
    for (int t = 0; t < 3; ++t) {
        setup(&e, 1, 0.5f, (float)times[t], 0);
        memset(buf, 0, sizeof(buf)); buf[0] = buf[1] = 0.5f;
        run(&e, buf, 44100);
        int d = (int)(times[t] * 0.001f * DD_SAMPLE_RATE);
        assert(first_nonzero(buf, 44100) == 2 * d);
        /* v0.5: the left echo comes first, at about 2 x 3/4 x Time. */
        int dl = (int)(times[t] * 0.001f * DD_SAMPLE_RATE * DD_LEFT_TIME_RATIO);
        int l0 = first_nonzero_ch(buf, 44100, 0);
        assert(l0 >= 2 * dl - 1 && l0 <= 2 * dl + 2 && l0 < 2 * d);
    }
    /* DEVIATION pin: Time 0 is a one-sample delay, not DelayLine's 12000. */
    setup(&e, 1, 0.5f, 0, 0);
    memset(buf, 0, sizeof(buf)); buf[0] = buf[1] = 0.5f;
    run(&e, buf, 128);
    assert(first_nonzero(buf, 128) == 2);
    /* Drive's floor is 0.3, so the wet path can no longer go silent. */
    dd_init(&e);
    dd_set_param(&e, DD_DRIVE, 0); assert(e.target.value[DD_DRIVE] == 0.3f);
    setup(&e, 1, 0, 50, 0.5f);
    for (int i = 0; i < 2 * 44100; ++i) buf[i] = 0.3f * noise();
    run(&e, buf, 44100);
    assert(rms_db(buf + 44100, 22050) > -30);
    /* Faithful quirk: FX 2's OD has no drive of its own. With FX 1 not on OD
       nothing ever sets the shared overdrive, so it stays at its 0.5. */
    setup(&e, 1, 0.9f, 5, 0);
    dd_set_param(&e, DD_FX2, DD_FX_OD);
    memset(buf, 0, sizeof(buf)); run(&e, buf, 128);
    assert(e.overdrive.drive == 0.5f);
    dd_set_param(&e, DD_FX1, DD_FX_OD); run(&e, buf, 128);
    assert(e.overdrive.drive == 0.9f);
    /* Each switch position changes the sound. */
    static float ref[2 * 8192];
    for (int a = 0; a < 3; ++a) {
        setup(&e, 1, 0.7f, 20, 0.6f);
        dd_set_param(&e, DD_FX1, a);
        srand(1);
        for (int i = 0; i < 2 * 8192; ++i) buf[i] = 0.4f * noise();
        run(&e, buf, 8192);
        if (a) assert(memcmp(ref, buf, sizeof(ref)));
        else memcpy(ref, buf, sizeof(ref));
    }
    /* Input Gain is what drives the distortion: at Drive 0.3, +24 dB of gain
       turns a nearly clean sine into a heavily distorted one -- and Level
       Match keeps it at the same volume. */
    double thd0, thd24, lvl0, lvl24;
    setup(&e, 1, 0.3f, 50, 0); thd0 = thd(&e); lvl0 = rms_db(buf + 44100, 22050);
    setup(&e, 1, 0.3f, 50, 0); dd_set_param(&e, DD_GAIN, 24); dd_snap(&e);
    thd24 = thd(&e); lvl24 = rms_db(buf + 44100, 22050);
    assert(thd0 < 0.05 && thd24 > 0.25 && fabs(lvl24 - lvl0) < 1.5);
    /* v0.4 fixed bass boost: 55 Hz + 880 Hz in, the low tone comes out at
       least 5 dB ahead of the high one (it was level before). */
    {
        static float inb[2 * 44100];
        setup(&e, 1, 0.5f, 50, 0);
        for (int s2 = 0; s2 < 3; ++s2) {
            for (int i = 0; i < 44100; ++i) {
                double tt = (s2 * 44100 + i) / 44100.0;
                inb[2 * i] = inb[2 * i + 1] = buf[2 * i] = buf[2 * i + 1] =
                    0.25f * (float)(sin(6.283185307 * 55 * tt) + sin(6.283185307 * 880 * tt));
            }
            run(&e, buf, 44100);
        }
        double rel[2]; const double hzs[2] = {55, 880};
        for (int k = 0; k < 2; ++k) {
            double ro = 0, io = 0, ri = 0, ii = 0;
            for (int i = 22050; i < 44100; ++i) {
                double ph = 6.283185307 * hzs[k] * i / 44100;
                ro += buf[2 * i] * cos(ph); io += buf[2 * i] * sin(ph);
                ri += inb[2 * i] * cos(ph); ii += inb[2 * i] * sin(ph);
            }
            rel[k] = 10 * log10((ro * ro + io * io) / (ri * ri + ii * ii));
        }
        bass_tilt = rel[0] - rel[1];
        assert(bass_tilt > 5);
    }
    /* v0.5 stereo: a MONO source comes out with left and right wet that no
       longer match (correlation well below 1; v0.4 was exactly 1). */
    {
        setup(&e, 1, 0.5f, 125, 0.4f);
        srand(3);
        for (int s2 = 0; s2 < 3; ++s2) {
            for (int i = 0; i < 44100; ++i) buf[2 * i] = buf[2 * i + 1] = 0.3f * noise();
            run(&e, buf, 44100);
        }
        double lr_ = 0, ll = 0, rr = 0;
        for (int i = 0; i < 44100; ++i) {
            lr_ += (double)buf[2 * i] * buf[2 * i + 1];
            ll += (double)buf[2 * i] * buf[2 * i]; rr += (double)buf[2 * i + 1] * buf[2 * i + 1];
        }
        stereo_corr = lr_ / sqrt(ll * rr);
        assert(stereo_corr < 0.5);
    }
    /* Worst case for 60 s: full-scale noise, every stage hot. Finite, bounded. */
    setup(&e, 1, 1, 7, 0.9f);
    dd_set_param(&e, DD_GAIN, 24); dd_set_param(&e, DD_FX1, DD_FX_OD);
    dd_set_param(&e, DD_FX2, DD_FX_BC); dd_set_param(&e, DD_CRUSH, 0.3f);
    float peak = 0;
    for (int s = 0; s < 60; ++s) {
        dd_set_param(&e, DD_FX2, s % 3);
        for (int i = 0; i < 2 * 44100; ++i) buf[i] = noise();
        run(&e, buf, 44100);
        for (int i = 0; i < 2 * 44100; ++i) { assert(isfinite(buf[i])); peak = fmaxf(peak, fabsf(buf[i])); }
    }
    assert(peak < 100);
    /* Parameter validation. */
    dd_init(&e);
    assert(!dd_set_param(&e, DD_MIX, NAN) && !dd_set_param(&e, -1, 0) && !dd_set_param(&e, DD_PARAM_COUNT, 0));
    dd_set_param(&e, DD_FEEDBACK, 5); assert(e.target.value[DD_FEEDBACK] == 0.9f);
    dd_set_param(&e, DD_GAIN, 99); assert(e.target.value[DD_GAIN] == 24);
    dd_set_param(&e, DD_FX1, 1.6f); assert(e.target.value[DD_FX1] == 2);
    /* Level Match: across Mix and the settings that change the wet path's
       loudness, the output stays within 1.5 dB of the input. */
    struct { float drive, fb, gain; int fx1, fx2; } cfg[] = {
        {0.5f, 0.4f, 0, 0, 0}, {0.3f, 0.0f, 0, 0, 0}, {0.9f, 0.8f, 0, 1, 1},
        {0.6f, 0.6f, 6, 2, 2}, {0.3f, 0.4f, 24, 0, 1}, {0.35f, 0.9f, 12, 1, 2},
    };
    float mixes[] = {0, 0.25f, 0.5f, 0.75f, 1};
    double worst = 0;
    for (unsigned c = 0; c < sizeof(cfg) / sizeof(cfg[0]); ++c) {
        setup(&e, 1, cfg[c].drive, 60, cfg[c].fb);
        dd_set_param(&e, DD_FX1, cfg[c].fx1); dd_set_param(&e, DD_FX2, cfg[c].fx2);
        dd_set_param(&e, DD_GAIN, cfg[c].gain); dd_snap(&e);
        srand(100 + c);
        level_change(&e, 0.3f, 6); /* let the match settle once */
        for (int m = 0; m < 5; ++m) {
            dd_set_param(&e, DD_MIX, mixes[m]);
            double d = level_change(&e, 0.3f, 2);
            if (fabs(d) > fabs(worst)) worst = d;
            if (fabs(d) > 1.5) { printf("level %u mix %.2f: %+.2f dB\n", c, mixes[m], d); return 1; }
        }
    }
    float pluck_drive[] = {0.3f, 0.5f, 0.9f};
    for (int d = 0; d < 3; ++d) {
        setup(&e, 1, pluck_drive[d], 125, 0.4f);
        long t0 = 0;
        for (int s = 0; s < 5; ++s, t0 += 44100) { plucks(buf, t0, 44100); run(&e, buf, 44100); }
        for (int m = 0; m < 5; ++m) {
            dd_set_param(&e, DD_MIX, mixes[m]);
            plucks(buf, t0, 44100); run(&e, buf, 44100); t0 += 44100;
            static float in1[2 * 44100];
            plucks(in1, t0, 44100); memcpy(buf, in1, sizeof(in1)); run(&e, buf, 44100); t0 += 44100;
            double dd = rms_db(buf, 44100) - rms_db(in1, 44100);
            if (fabs(dd) > fabs(worst)) worst = dd;
            if (fabs(dd) > 1.5) { printf("plucks drive %.1f mix %.2f: %+.2f dB\n", pluck_drive[d], mixes[m], dd); return 1; }
        }
    }
    /* A tail rings out untouched: silence in front of it teaches nothing. */
    setup(&e, 1, 0.5f, 100, 0.7f);
    level_change(&e, 0.3f, 4);
    memset(buf, 0, sizeof(buf)); run(&e, buf, 22050); /* 2 x Time still compares */
    const float held_dry = e.p_dry, held_wet = e.p_wet;
    double tail_db = rms_db(buf, 22050);
    memset(buf, 0, sizeof(buf)); run(&e, buf, 44100);
    assert(e.p_dry == held_dry && e.p_wet == held_wet); /* the target is frozen */
    assert(tail_db > -40 && rms_db(buf, 44100) > -90);
    /* Sudden jumps are caught within milliseconds: no 10 ms window of the
       next second may be 6 dB over the input. */
    const float step_case[][3] = {{DD_DRIVE, 1, 0.3f}, {DD_GAIN, 24, 0.3f}, {DD_FEEDBACK, 0.9f, 0.9f}};
    double jump_worst = -99;
    for (int j = 0; j < 3; ++j) {
        setup(&e, 1, step_case[j][2], 30, j == 2 ? 0 : 0.4f);
        srand(9);
        level_change(&e, 0.3f, 4);
        dd_set_param(&e, (int)step_case[j][0], step_case[j][1]);
        for (int i = 0; i < 2 * 44100; ++i) buf[i] = 0.3f * noise();
        double in_db = rms_db(buf, 44100);
        run(&e, buf, 44100);
        for (int w0 = 0; w0 + 441 <= 44100; w0 += 441) {
            double d = rms_db(buf + 2 * w0, 441) - in_db;
            if (d > jump_worst) jump_worst = d;
        }
    }
    if (jump_worst > 6) { printf("jump peak %+.2f dB\n", jump_worst); return 1; }
    /* Cost: one second of the heaviest configuration. */
    setup(&e, 1, 0.7f, 100, 0.8f);
    dd_set_param(&e, DD_FX1, DD_FX_BC); dd_set_param(&e, DD_FX2, DD_FX_OD); dd_set_param(&e, DD_GAIN, 12);
    for (int i = 0; i < 2 * 44100; ++i) buf[i] = noise();
    clock_t c0 = clock(); run(&e, buf, 44100);
    double ms = 1000.0 * (clock() - c0) / CLOCKS_PER_SEC;
    printf("PASS: engine (silence, dry identity, 2xD echo, drive floor, quirk, THD %.0f%% -> %.0f%% at +24 dB, "
           "bass +%.1f dB over 880 Hz, L/R corr %.2f from mono, level match worst %+.2f dB, jump peak %+.1f dB, 60 s peak %.2f, %.1f ms per audio second native)\n",
           100 * thd0, 100 * thd24, bass_tilt, stereo_corr, worst, jump_worst, peak, ms);
    return 0;
}
