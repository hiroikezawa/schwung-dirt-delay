/* Schwung audio FX v2 adapter for the Dirt Delay engine. */
#include "host/audio_fx_api_v2.h"
#include "dirt_engine.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "metadata.h"
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Instance pool requires lock-free atomics");
/* No allocation even in create/destroy: these run on the SPI callback too.
   Atomic slot ownership also permits concurrent bus-worker construction. */
#define INSTANCE_COUNT 16
typedef struct { atomic_int used; dd_engine fx; } instance;
static instance instances[INSTANCE_COUNT];
static const char *keys[DD_PARAM_COUNT] = {
    "mix", "gain", "drive", "time", "feedback", "crush", "fx1", "fx2"
};
static const char *fx_options[] = {"Off", "OD", "BC", NULL};
/* Keys older states carry that v0.3 no longer has; accepted and ignored. */
static const char *obsolete_keys[] = {"cutoff", "res", "boost", "match", NULL};

static void *create_instance(const char *dir, const char *config) {
    (void)dir; (void)config;
    for (int i = 0; i < INSTANCE_COUNT; ++i) {
        int unused = 0;
        if (atomic_compare_exchange_strong(&instances[i].used, &unused, 1)) {
            dd_init(&instances[i].fx);
            return &instances[i];
        }
    }
    return NULL;
}
static void destroy_instance(void *ptr) {
    if (ptr) atomic_store(&((instance *)ptr)->used, 0);
}
static int param_index(const char *key) {
    for (int i = 0; i < DD_PARAM_COUNT; ++i) if (!strcmp(key, keys[i])) return i;
    return -1;
}
static const char **options_for(int idx) {
    return idx >= DD_FIRST_ENUM ? fx_options : NULL;
}
static const char *skip_space(const char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}
/* An enum accepts its index or its option name; the UI learns index format
   from get_param, which always reports the index. */
static int parse_value(int idx, const char *val, float *out) {
    const char **opts = options_for(idx);
    if (opts) {
        const char *p = skip_space(val);
        for (int i = 0; opts[i]; ++i) {
            size_t n = strlen(opts[i]);
            if (!strncasecmp(p, opts[i], n) && !*skip_space(p + n)) { *out = (float)i; return 1; }
        }
    }
    char *end;
    float v = strtof(val, &end);
    if (end == val || *skip_space(end) || !isfinite(v)) return 0;
    if (opts && v != floorf(v)) return 0;
    *out = v;
    return 1;
}
static int obsolete_index(const char *key) {
    for (int i = 0; obsolete_keys[i]; ++i) if (!strcmp(key, obsolete_keys[i])) return i;
    return -1;
}
/* Flat, numeric, versioned JSON only. Bounded parsing, transactional restore:
   every key exactly once, nothing unknown, every value in range.
   Version 3 is v0.3's own. Versions 1 and 2 (v0.1 / v0.2) are read too: the
   keys v0.3 dropped are ignored, Input Gain takes its default (0 dB), and a
   Drive below the new 0.3 floor is raised to it rather than refused, so a
   slot saved by an older build still loads. */
static int restore_state(instance *inst, const char *text) {
    if (strlen(text) > 1024) return 0;
    const char *p = skip_space(text);
    if (*p++ != '{') return 0;
    float next[DD_PARAM_COUNT];
    unsigned seen = 0, seen_obsolete = 0;
    int version = 0;
    enum { MAX_FIELDS = DD_PARAM_COUNT + 5 };
    for (int field = 0; ; ++field) {
        if (field >= MAX_FIELDS) return 0;
        p = skip_space(p);
        if (*p++ != '"') return 0;
        char key[24]; int n = 0;
        while (*p && *p != '"' && n < (int)sizeof(key) - 1) key[n++] = *p++;
        key[n] = 0;
        if (*p++ != '"') return 0;
        p = skip_space(p);
        if (*p++ != ':') return 0;
        p = skip_space(p);
        /* JSON numeric grammar (strtof alone also accepts NaN, hex and '+'). */
        const char *start = p;
        if (*p == '-') ++p;
        if (*p == '0') ++p;
        else {
            if (*p < '1' || *p > '9') return 0;
            do { ++p; } while (isdigit((unsigned char)*p));
        }
        if (*p == '.') {
            ++p; if (!isdigit((unsigned char)*p)) return 0;
            while (isdigit((unsigned char)*p)) ++p;
        }
        if (*p == 'e' || *p == 'E') {
            ++p; if (*p == '+' || *p == '-') ++p;
            if (!isdigit((unsigned char)*p)) return 0;
            while (isdigit((unsigned char)*p)) ++p;
        }
        char *end;
        float v = strtof(start, &end);
        if (end != p || !isfinite(v)) return 0;
        int idx = param_index(key), old = obsolete_index(key);
        if (!strcmp(key, "version")) {
            if (version || v < 1 || v > 3 || v != floorf(v)) return 0;
            version = (int)v;
        } else if (old >= 0) {
            if (seen_obsolete & (1u << old)) return 0;
            seen_obsolete |= 1u << old;
        } else {
            if (idx < 0 || (seen & (1u << idx))) return 0;
            seen |= 1u << idx;
            if (options_for(idx) && v != floorf(v)) return 0;
            next[idx] = v;
        }
        p = skip_space(p);
        if (*p == '}') break;
        if (*p++ != ',') return 0;
    }
    if (*p++ != '}' || *skip_space(p)) return 0;
    const unsigned all = (1u << DD_PARAM_COUNT) - 1;
    if (version == 3) {
        if (seen != all || seen_obsolete) return 0;
    } else if (version == 1 || version == 2) {
        const unsigned need_old = version == 1 ? 0x7u : 0xfu; /* cutoff res boost (+match) */
        if (seen != (all & ~(1u << DD_GAIN)) || seen_obsolete != need_old) return 0;
        next[DD_GAIN] = dd_param_default[DD_GAIN];
        if (next[DD_DRIVE] >= 0.f && next[DD_DRIVE] < dd_param_min[DD_DRIVE])
            next[DD_DRIVE] = dd_param_min[DD_DRIVE];
    } else return 0;
    for (int i = 0; i < DD_PARAM_COUNT; ++i)
        if (next[i] < dd_param_min[i] || next[i] > dd_param_max[i]) return 0;
    for (int i = 0; i < DD_PARAM_COUNT; ++i) dd_set_param(&inst->fx, i, next[i]);
    dd_snap(&inst->fx);
    return 1;
}
static void set_param(void *ptr, const char *key, const char *val) {
    if (!ptr || !key || !val) return;
    instance *inst = ptr;
    if (!strcmp(key, "state")) { (void)restore_state(inst, val); return; }
    int idx = param_index(key);
    float v;
    if (idx < 0 || !parse_value(idx, val, &v)) return;
    dd_set_param(&inst->fx, idx, v);
}
static int copy_text(char *buf, int size, const char *text) {
    size_t n = strlen(text);
    if (n >= (size_t)size) { buf[0] = 0; return -1; }
    memcpy(buf, text, n + 1);
    return (int)n;
}
static int get_param(void *ptr, const char *key, char *buf, int size) {
    if (!ptr || !key || !buf || size <= 0) return -1;
    buf[0] = 0;
    const float *v = ((instance *)ptr)->fx.target.value;
    int idx = param_index(key);
    int n = -1;
    if (idx >= DD_FIRST_ENUM) n = snprintf(buf, size, "%d", (int)v[idx]);
    else if (idx >= 0) n = snprintf(buf, size, "%.6g", v[idx]); /* state keeps %.9g */
    else if (!strcmp(key, "name")) return copy_text(buf, size, "Dirt Delay");
    else if (!strcmp(key, "chain_params")) return copy_text(buf, size, chain_params);
    else if (!strcmp(key, "ui_hierarchy")) return copy_text(buf, size, ui_hierarchy);
    else if (!strcmp(key, "state")) {
        n = snprintf(buf, size,
            "{\"version\":3,\"mix\":%.9g,\"gain\":%.9g,\"drive\":%.9g,\"time\":%.9g,"
            "\"feedback\":%.9g,\"crush\":%.9g,\"fx1\":%d,\"fx2\":%d}",
            v[DD_MIX], v[DD_GAIN], v[DD_DRIVE], v[DD_TIME], v[DD_FEEDBACK], v[DD_CRUSH],
            (int)v[DD_FX1], (int)v[DD_FX2]);
    }
    if (n < 0 || n >= size) { buf[0] = 0; return -1; }
    return n;
}
static void process_block(void *ptr, int16_t *audio, int frames) {
    if (!ptr || !audio || frames <= 0) return; /* in-place: leave audio as is */
    dd_engine *fx = &((instance *)ptr)->fx;
    float buf[MOVE_FRAMES_PER_BLOCK * 2];
    for (int done = 0; done < frames; ) {
        int n = frames - done;
        if (n > MOVE_FRAMES_PER_BLOCK) n = MOVE_FRAMES_PER_BLOCK;
        int16_t *io = audio + done * 2;
        for (int i = 0; i < n * 2; ++i) buf[i] = io[i] / 32768.0f;
        dd_process(fx, buf, n);
        /* x32768 mirrors the input scaling, so the dry share is bit-exact. */
        for (int i = 0; i < n * 2; ++i) {
            float x = buf[i] * 32768.0f;
            if (!isfinite(x)) x = 0;
            io[i] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, x));
        }
        done += n;
    }
}
static audio_fx_api_v2_t api = {
    .api_version = AUDIO_FX_API_VERSION_2, .create_instance = create_instance,
    .destroy_instance = destroy_instance, .process_block = process_block,
    .set_param = set_param, .get_param = get_param, .on_midi = NULL
};
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    /* The engine is tuned for 44.1 kHz; refuse a host that says otherwise. */
    if (host && host->sample_rate && host->sample_rate != MOVE_SAMPLE_RATE) return NULL;
    return &api;
}
