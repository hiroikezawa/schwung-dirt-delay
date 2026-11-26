/* Dirt Delay engine. Each dd_* block below is the DaisySP object of the same
   name (Source/Effects/overdrive.cpp, decimator.cpp, Utility/delayline.h,
   MIT, Electrosmith / Emilie Gillet), kept arithmetic-for-arithmetic. Where
   this departs from the Versio patch it is marked DEVIATION and listed in
   README.md. */
#include "dirt_engine.h"
#include <math.h>
#include <string.h>

#define PI_F 3.1415927410125732421875f

/* Drive starts at 0.3: below it DaisySP's Overdrive has so little pre-gain
   that the always-on input stage turned the wet path down to silence. */
const float dd_param_min[DD_PARAM_COUNT] = {0, 0, 0.3f, 0, 0, 0, 0, 0};
/* Feedback tops out at 0.9: the original clamps its knob with min(k, 0.9f). */
const float dd_param_max[DD_PARAM_COUNT] = {1, 24, 1, 250, 0.9f, 1, 2, 2};
const float dd_param_default[DD_PARAM_COUNT] = {0.5f, 0, 0.5f, 125, 0.4f, 0.5f, 0, 0};

/* Level Match. The gain is clamped to -36..+24 dB so a near-silent wet path
   cannot ask for infinity. A block only teaches the followers while both
   sides carry signal -- otherwise a ringing tail would be "corrected" toward
   the silence in front of it.
   The target comes from slow, symmetric followers (accurate: a fast attack
   biases whichever side is spikier). A pair of fast followers, one per side
   and shaped alike so plucked material reads the same on both, is only a
   CEILING at +3 dB: turning Drive or Input Gain up makes the wet path louder
   at once, and the slow target alone would let that through at full level
   for half a second. The gain falls fast and rises slowly. */
#define MATCH_GATE 1e-6f        /* -60 dBFS mean power */
#define MATCH_GAIN_MIN 0.0158489f
#define MATCH_GAIN_MAX 15.848932f
#define MATCH_ATTACK_TAU 0.03f  /* fast followers rising, gain falling */
#define MATCH_CEILING 2.0f      /* +3 dB, in power */
#define MATCH_POWER_TAU 0.4f
#define MATCH_GAIN_TAU 0.3f

static float fclamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---- dsp.h ---- */
static float soft_limit(float x) { return x * (27.f + x * x) / (27.f + 9.f * x * x); }
static float soft_clip(float x) { return x < -3.f ? -1.f : x > 3.f ? 1.f : soft_limit(x); }

/* ---- Overdrive ---- */
static void od_set_drive(dd_overdrive *o, float drive) {
    drive = fclamp(drive, 0.f, 1.f);
    if (drive == o->drive) return; /* pure function of drive; skip the divide */
    o->drive = drive;
    const float d = 2.f * drive;
    const float d2 = d * d;
    const float pre_a = d * 0.5f;
    const float pre_b = d2 * d2 * d * 24.0f;
    o->pre_gain = pre_a + (pre_b - pre_a) * d2;
    const float squashed = d * (2.0f - d);
    o->post_gain = 1.0f / soft_clip(0.33f + squashed * (o->pre_gain - 0.33f));
}
static float od_process(const dd_overdrive *o, float in) {
    return soft_clip(o->pre_gain * in) * o->post_gain;
}
static void od_init(dd_overdrive *o) { o->drive = -1.f; od_set_drive(o, .5f); }

/* ---- Decimator (smooth crushing off, as the original never enables it) ---- */
static void bc_init(dd_decimator *b) {
    memset(b, 0, sizeof(*b));
    b->downsample_factor = 1.0f;
}
static void bc_set(dd_decimator *b, float crush, float downsample) {
    b->bitcrush_factor = crush;
    b->bits_to_crush = (uint32_t)(crush * 16);
    b->downsample_factor = downsample;
}
static float bc_process(dd_decimator *b, float input) {
    b->threshold = (uint32_t)((b->downsample_factor * b->downsample_factor) * 96.0f);
    b->inc += 1;
    if (b->inc > b->threshold) {
        b->inc = 0;
        b->downsampled = input;
    }
    /* DEVIATION (UB guard only): clamp before the float->int conversion, and
       shift left as unsigned -- C leaves both undefined where C++/GCC on the
       Daisy happened to wrap. Unreachable for any signal this chain carries. */
    int32_t t = (int32_t)(fclamp(b->downsampled, -32767.f, 32767.f) * 65536.0f);
    t >>= b->bits_to_crush;
    t = (int32_t)((uint32_t)t << b->bits_to_crush);
    return (float)t / 65536.0f;
}

/* ---- DelayLine<float, 12000> ---- */
static void delay_init(dd_delay *d) { memset(d, 0, sizeof(*d)); d->delay = 1; }
static void delay_set(dd_delay *d, float delay) {
    int32_t i = (int32_t)delay;
    d->frac = delay - (float)i;
    d->delay = (size_t)i < DD_DELAY_SIZE ? (size_t)i : DD_DELAY_SIZE - 1;
}
static float delay_read(const dd_delay *d) {
    float a = d->line[(d->write_ptr + d->delay) % DD_DELAY_SIZE];
    float b = d->line[(d->write_ptr + d->delay + 1) % DD_DELAY_SIZE];
    return a + (b - a) * d->frac;
}
/* Denormal guard, inaudible (-300 dB): a decaying feedback tail otherwise
   ends its life in subnormals. */
static float flush(float x) { return fabsf(x) < 1e-15f ? 0.f : x; }
static void delay_write(dd_delay *d, float s) {
    d->line[d->write_ptr] = flush(s);
    d->write_ptr = (d->write_ptr - 1 + DD_DELAY_SIZE) % DD_DELAY_SIZE;
}

/* ---- Fixed bass boost: RBJ low shelf, shelf slope S = 1 ---- */
static void shelf_init(dd_biquad_coef *k, float hz, float db) {
    const float A = powf(10.f, db / 40.f);
    const float w = 2.f * PI_F * hz / DD_SAMPLE_RATE;
    const float cw = cosf(w), sw = sinf(w);
    const float alpha = sw / 2.f * sqrtf(2.f); /* S = 1 */
    const float sa = 2.f * sqrtf(A) * alpha;
    const float a0 = (A + 1) + (A - 1) * cw + sa;
    k->b0 = A * ((A + 1) - (A - 1) * cw + sa) / a0;
    k->b1 = 2 * A * ((A - 1) - (A + 1) * cw) / a0;
    k->b2 = A * ((A + 1) - (A - 1) * cw - sa) / a0;
    k->a1 = -2 * ((A - 1) + (A + 1) * cw) / a0;
    k->a2 = ((A + 1) + (A - 1) * cw - sa) / a0;
}
static float biquad_process(const dd_biquad_coef *k, dd_biquad *q, float x) {
    const float y = k->b0 * x + q->z1;
    q->z1 = flush(k->b1 * x - k->a1 * y + q->z2);
    q->z2 = flush(k->b2 * x - k->a2 * y);
    return y;
}

/* ---- engine ---- */
static float db_to_gain(float db) { return powf(10.f, db * 0.05f); }

void dd_init(dd_engine *e) {
    memset(e, 0, sizeof(*e));
    for (int i = 0; i < DD_PARAM_COUNT; ++i) e->target.value[i] = dd_param_default[i];
    /* DEVIATION (control rate): Versio pots are continuous; Move's encoders
       step, so values glide (10 ms; 60 ms for delay time, which the fractional
       read turns into a tape-like pitch bend instead of a click). */
    e->k_sample = 1.f - expf(-1.f / (0.010f * DD_SAMPLE_RATE));
    e->k_time = 1.f - expf(-1.f / (0.060f * DD_SAMPLE_RATE));
    od_init(&e->od);
    od_init(&e->overdrive);
    shelf_init(&e->bass_coef, DD_BASS_HZ, DD_BASS_DB);
    for (int c = 0; c < 2; ++c) {
        bc_init(&e->bc1[c]); bc_init(&e->bc2[c]);
        delay_init(&e->d1[c]); delay_init(&e->d2[c]);
    }
    e->xf_mix = -1.f;
    e->gain = 1.f;
    dd_snap(e);
}

int dd_set_param(dd_engine *e, int idx, float v) {
    if (idx < 0 || idx >= DD_PARAM_COUNT || !isfinite(v)) return 0;
    v = fclamp(v, dd_param_min[idx], dd_param_max[idx]);
    if (idx >= DD_FIRST_ENUM) v = floorf(v + 0.5f);
    e->target.value[idx] = v;
    return 1;
}

/* smooth[DD_GAIN] holds the LINEAR input gain; every other slot is in host units. */
void dd_snap(dd_engine *e) {
    for (int i = 0; i < DD_FIRST_ENUM; ++i) e->smooth[i] = e->target.value[i];
    e->smooth[DD_GAIN] = db_to_gain(e->target.value[DD_GAIN]);
}

/* One-pole glide that LANDS: an exponential approach never reaches its target,
   which would leave Mix 0 not quite dry and the tail in denormals. */
static float glide(float s, float t, float k) {
    s += k * (t - s);
    return fabsf(t - s) < 1e-6f ? t : s;
}

/* Up to DD_BLOCK frames: the wet path is rendered first, so Level Match can
   judge this block's wet level before any of it is mixed. */
#define DD_BLOCK 128
static void process_block(dd_engine *e, float *lr, int frames) {
    float *s = e->smooth;
    const float *t = e->target.value;
    s[DD_CRUSH] = t[DD_CRUSH]; /* quantised to whole bits anyway */
    const float gain_target = db_to_gain(t[DD_GAIN]);
    const int fx1 = (int)t[DD_FX1], fx2 = (int)t[DD_FX2];
    const float crush = s[DD_CRUSH];

    /* ---- pass 1: the wet path ---- */
    float wet[DD_BLOCK * 2], mix[DD_BLOCK];
    float sum_dry = 0.f, sum_wet = 0.f, dsamp = 1.f;
    for (int i = 0; i < frames; ++i) {
        s[DD_MIX] = glide(s[DD_MIX], t[DD_MIX], e->k_sample);
        s[DD_GAIN] = glide(s[DD_GAIN], gain_target, e->k_sample);
        s[DD_DRIVE] = glide(s[DD_DRIVE], t[DD_DRIVE], e->k_sample);
        s[DD_FEEDBACK] = glide(s[DD_FEEDBACK], t[DD_FEEDBACK], e->k_sample);
        s[DD_TIME] = glide(s[DD_TIME], t[DD_TIME], e->k_time);
        mix[i] = s[DD_MIX];
        const float in_gain = s[DD_GAIN];
        const float od_drive = s[DD_DRIVE];
        const float bc_downsample = s[DD_DRIVE]; /* KNOB_2 feeds both, as in the original */
        const float feedback = s[DD_FEEDBACK];
        /* DEVIATION: a DelayLine set to 0 reads the slot written 12000 samples
           ago (0.27 s), so the fully-down knob jumped to the LONGEST delay.
           One sample is the floor. */
        dsamp = s[DD_TIME] * 0.001f * DD_SAMPLE_RATE;
        if (dsamp < 1.f) dsamp = 1.f;

        od_set_drive(&e->od, od_drive);
        /* v0.5 stereo: the LEFT pair of delays runs at 3/4 of Time, the right
           at Time, so the two sides' echoes land apart and the wet path is
           wide even from a mono source. Both keep the one-sample floor. */
        const float dsamp_l = fmaxf(1.f, dsamp * DD_LEFT_TIME_RATIO);
        delay_set(&e->d1[0], dsamp_l);
        delay_set(&e->d2[0], dsamp_l);
        delay_set(&e->d1[1], dsamp);
        delay_set(&e->d2[1], dsamp);
        for (int c = 0; c < 2; ++c) {
            const float in = lr[i * 2 + c];
            /* Input Gain feeds the wet path only: it decides how hard the
               drive is hit, and Level Match then brings the result back to
               the (ungained) dry level. The fixed bass boost sits before the
               drive too, so the low end is what gets pushed into it. */
            float x = biquad_process(&e->bass_coef, &e->bass[c], in * in_gain);
            x = od_process(&e->od, x);

            /* FX 1 (switch 1). overdrive is shared with FX 2. */
            if (fx1 == DD_FX_OD) {
                od_set_drive(&e->overdrive, od_drive);
                x = od_process(&e->overdrive, x);
            } else if (fx1 == DD_FX_BC) {
                bc_set(&e->bc1[c], crush, bc_downsample);
                x = bc_process(&e->bc1[c], x);
            }

            /* The original low-passed this at 2 kHz; v0.3 does not. */
            float d1 = delay_read(&e->d1[c]);
            delay_write(&e->d1[c], x + d1 * feedback);
            x = d1;

            /* FX 2 (switch 2): the OD case uses whatever drive the shared
               overdrive was last given -- FX 1's drive, or the 0.5 it was
               initialised with. Faithful to the original. */
            if (fx2 == DD_FX_OD) {
                x = od_process(&e->overdrive, x);
            } else if (fx2 == DD_FX_BC) {
                bc_set(&e->bc2[c], crush, bc_downsample);
                x = bc_process(&e->bc2[c], x);
            }

            /* ... and ran a resonant low-pass here; v0.3 does not. */
            float d2 = delay_read(&e->d2[c]);
            delay_write(&e->d2[c], x + d2 * feedback);
            x = d2;

            wet[i * 2 + c] = x;
            sum_dry += in * in;
            sum_wet += x * x;
        }
    }

    /* ---- Level Match: learn, then choose this block's gain ---- */
    /* The first echo of a sound leaves 2 x Time after it went in (1.5 x on
       the left), so the wet block is compared with the dry block from that
       long ago -- the midpoint of the two sides. */
    const float bp_dry = sum_dry / (float)(2 * frames);
    const float bp_wet = sum_wet / (float)(2 * frames);
    e->dry_hist[e->hist_pos] = bp_dry;
    unsigned lag = (unsigned)((1.f + DD_LEFT_TIME_RATIO) * dsamp / (float)frames + 0.5f);
    if (lag > DD_HIST - 1) lag = DD_HIST - 1;
    const float then = e->dry_hist[(e->hist_pos + DD_HIST - lag) % DD_HIST];
    e->hist_pos = (e->hist_pos + 1) % DD_HIST;
    if (then > MATCH_GATE && bp_wet > MATCH_GATE * 1e-3f) {
        const float k_att = 1.f - expf(-(float)frames / (MATCH_ATTACK_TAU * DD_SAMPLE_RATE));
        const float k_rel = 1.f - expf(-(float)frames / (MATCH_POWER_TAU * DD_SAMPLE_RATE));
        e->p_dry += k_rel * (then - e->p_dry);
        e->p_wet += k_rel * (bp_wet - e->p_wet);
        e->p_wet_fast += (bp_wet > e->p_wet_fast ? k_att : k_rel) * (bp_wet - e->p_wet_fast);
        e->p_dry_fast += (then > e->p_dry_fast ? k_att : k_rel) * (then - e->p_dry_fast);
    }
    const float g0 = e->gain;
    float g1 = g0;
    int ceiling = 0;
    if (e->p_wet > 1e-12f && e->p_dry > 0.f) {
        float target = sqrtf(e->p_dry / e->p_wet);
        /* The ceiling judges THIS block, so a jump is caught before it is heard. */
        const float fast = fmaxf(e->p_wet_fast, bp_wet);
        const float limit = sqrtf(MATCH_CEILING * fmaxf(e->p_dry_fast, e->p_dry) / fmaxf(fast, 1e-12f));
        if (limit < target) target = limit;
        target = fclamp(target, MATCH_GAIN_MIN, MATCH_GAIN_MAX);
        if (target < g0 && g0 > limit) { g1 = target; ceiling = 1; }
        else {
            const float tau = target < g0 ? MATCH_ATTACK_TAU : MATCH_GAIN_TAU;
            g1 = g0 + (1.f - expf(-(float)frames / (tau * DD_SAMPLE_RATE))) * (target - g0);
        }
    }
    e->gain = g1;

    /* ---- pass 2: equal-power mix. A ceiling drop ramps over 16 samples. ---- */
    const float ramp = ceiling ? (frames < 16 ? (float)frames : 16.f) : (float)frames;
    for (int i = 0; i < frames; ++i) {
        const float drywet = mix[i];
        if (drywet != e->xf_mix) { /* dry and wet are uncorrelated */
            e->xf_mix = drywet;
            e->dry_gain = drywet >= 1.f ? 0.f : cosf(drywet * (PI_F * 0.5f));
            e->wet_gain = sinf(drywet * (PI_F * 0.5f));
        }
        const float a = fminf(1.f, (float)(i + 1) / ramp);
        const float wet_gain = e->wet_gain * (g0 + (g1 - g0) * a);
        for (int c = 0; c < 2; ++c)
            lr[i * 2 + c] = lr[i * 2 + c] * e->dry_gain + wet[i * 2 + c] * wet_gain;
    }
}

void dd_process(dd_engine *e, float *lr, int frames) {
    for (int done = 0; done < frames; done += DD_BLOCK)
        process_block(e, lr + done * 2, frames - done < DD_BLOCK ? frames - done : DD_BLOCK);
}
