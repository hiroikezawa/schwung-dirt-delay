/* Dirt Delay engine: grown from a port of a Daisy Versio patch (OD -> OD/BC
   -> delay -> OD/BC -> delay, dry/wet). The DaisySP objects it uses
   (Overdrive, Decimator, DelayLine) are re-implemented here in C with the same
   arithmetic. v0.3 dropped the patch's two low-passes and its tap boost, and
   added an input gain and Level Match; v0.4 a fixed bass boost; v0.5 offset left/right delay times. No allocation, no I/O. */
#ifndef DIRT_ENGINE_H
#define DIRT_ENGINE_H
#include <stddef.h>
#include <stdint.h>

#define DD_SAMPLE_RATE 44100.0f
/* DelayLine<float, 12000> in the original; 0.25 s at 44.1 kHz is 11025. */
#define DD_DELAY_SIZE 12000
#define DD_MAX_DELAY_S 0.25f
/* Dry block powers kept for Level Match: 2 x 250 ms is 173 blocks of 128. */
#define DD_HIST 256

/* Order is the knob order (knobs 1-8). */
enum {
    DD_MIX, DD_GAIN, DD_DRIVE, DD_TIME, DD_FEEDBACK, DD_CRUSH,
    DD_FX1, DD_FX2, DD_PARAM_COUNT
};
#define DD_FIRST_ENUM DD_FX1
/* Versio switch positions, in the order the original's switch cases use. */
enum { DD_FX_OFF = 0, DD_FX_OD = 1, DD_FX_BC = 2 };

typedef struct { float value[DD_PARAM_COUNT]; } dd_params;

typedef struct { float drive, pre_gain, post_gain; } dd_overdrive;
typedef struct {
    float downsample_factor, bitcrush_factor, downsampled;
    uint32_t bits_to_crush, inc, threshold;
} dd_decimator;
typedef struct { float line[DD_DELAY_SIZE]; size_t write_ptr, delay; float frac; } dd_delay;
/* Fixed stereo offset (v0.5): the left delays run at this fraction of Time. */
#define DD_LEFT_TIME_RATIO 0.75f
/* Fixed low shelf (v0.4): RBJ biquad, transposed direct form II. */
#define DD_BASS_HZ 100.0f
#define DD_BASS_DB 6.0f
typedef struct { float b0, b1, b2, a1, a2; } dd_biquad_coef;
typedef struct { float z1, z2; } dd_biquad;

typedef struct {
    dd_params target;                  /* what the host set */
    float smooth[DD_FIRST_ENUM];       /* continuous values actually used */
    float k_sample, k_time;            /* per-sample smoothing coefficients */
    /* Overdrive holds no signal state, only gains, and _l/_r are always set
       to the same drive -- so one gain set stands for each L/R pair. */
    dd_overdrive od;        /* od_l / od_r: the always-on input drive */
    dd_overdrive overdrive; /* overdrive_l / _r: SHARED by FX 1 and FX 2 */
    dd_decimator bc1[2], bc2[2];
    dd_delay d1[2], d2[2];
    dd_biquad_coef bass_coef;
    dd_biquad bass[2];      /* before the input drive, wet path only */
    /* Level Match (not in the original, always on): equal-power crossfade,
       and the wet path's gain following dry loudness. */
    float xf_mix, dry_gain, wet_gain;  /* crossfade cached for xf_mix */
    float p_dry, p_wet, p_dry_fast, p_wet_fast; /* power followers */
    float gain;                        /* applied wet gain */
    float dry_hist[DD_HIST];
    unsigned hist_pos;
} dd_engine;

/* Range of each parameter, in host units (gain dB, time ms). */
extern const float dd_param_min[DD_PARAM_COUNT];
extern const float dd_param_max[DD_PARAM_COUNT];
extern const float dd_param_default[DD_PARAM_COUNT];

void dd_init(dd_engine *e);
/* Returns 0 if the index is unknown or the value is not finite. Clamps. */
int dd_set_param(dd_engine *e, int idx, float value);
/* Jump the smoothed values to their targets (used after a state restore). */
void dd_snap(dd_engine *e);
/* In-place, interleaved stereo float, nominal +-1. */
void dd_process(dd_engine *e, float *lr, int frames);

#endif
