#ifndef DUREN_MENU_SYNTH_H
#define DUREN_MENU_SYNTH_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * Tiny generated menu ambient. No PCM music data and no sample playback.
 * 8 kHz fixed-point synth: 2 drone oscillators + 1 sparse bell voice +
 * a tiny 290 ms 8-bit circular delay. Sequence length: 36 seconds.
 * Approx state cost: 2.4 KiB RAM; wavetable + sequence < 400 bytes ROM.
 */
#define MENU_SYNTH_SR              8000u
#define MENU_SYNTH_LOOP_SAMPLES  288000u
#define MENU_SYNTH_BELL_SAMPLES   44000u
#define MENU_SYNTH_DELAY_SAMPLES   2320u
#define MENU_SYNTH_DELAY_TAP1      1160u
#define MENU_SYNTH_MUSIC_GAIN        30u

static const int8_t menu_synth_sine[256] = {
       0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
      49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
      90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
     117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
     127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
     117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
      90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
      49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
     -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
     -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
     -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
     -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

typedef struct {
    bool active;
    uint8_t event_index;
    uint8_t decay_tick;
    uint16_t bell_attack;
    uint16_t delay_pos;
    uint32_t loop_pos;
    uint32_t drone_phase1;
    uint32_t drone_phase2;
    uint32_t lfo_phase;
    uint32_t bell_phase1;
    uint32_t bell_phase2;
    uint32_t bell_phase3;
    uint32_t bell_step;
    uint32_t bell_left;
    uint32_t bell_env_q15;
    int8_t delay[MENU_SYNTH_DELAY_SAMPLES];
} MenuSynthState;

static const uint32_t menu_synth_event_pos[5] = {
    20000u, 72000u, 126400u, 184000u, 244000u
};

/* MIDI 74, 69, 77, 72, 67 at 8 kHz, Q0.32 phase increments. */
static const uint32_t menu_synth_event_step[5] = {
    315320144u, 236223201u, 374980958u, 280918312u, 210450947u
};

static inline int32_t menu_synth_sin8(uint32_t phase) {
    return (int32_t)menu_synth_sine[phase >> 24];
}

static inline int32_t menu_synth_tri8(uint32_t phase) {
    uint32_t x = phase >> 24;
    if (x < 64u) return (int32_t)(x << 1);
    if (x < 192u) return 256 - (int32_t)(x << 1);
    return (int32_t)(x << 1) - 512;
}

static inline int32_t menu_synth_clip_i8(int32_t v) {
    if (v > 127) return 127;
    if (v < -128) return -128;
    return v;
}

static inline void menu_synth_reset(MenuSynthState* s) {
    if (!s) return;
    bool was_active = s->active;
    memset(s, 0, sizeof(*s));
    s->active = was_active;
}

static inline void menu_synth_start(MenuSynthState* s) {
    if (!s || s->active) return;
    memset(s, 0, sizeof(*s));
    s->active = true;
}

static inline void menu_synth_stop(MenuSynthState* s) {
    if (!s || !s->active) return;
    memset(s, 0, sizeof(*s));
}

static inline void menu_synth_trigger_bell(MenuSynthState* s, uint32_t step) {
    s->bell_step = step;
    s->bell_phase1 = 0u;
    s->bell_phase2 = 0u;
    s->bell_phase3 = 0u;
    s->bell_left = MENU_SYNTH_BELL_SAMPLES;
    s->bell_env_q15 = 32767u;
    s->bell_attack = 0u;
    s->decay_tick = 0u;
}

static inline int16_t menu_synth_next_sample(MenuSynthState* s) {
    if (!s || !s->active) return 0;

    if (s->event_index < 5u &&
        s->loop_pos == menu_synth_event_pos[s->event_index]) {
        menu_synth_trigger_bell(s, menu_synth_event_step[s->event_index]);
        s->event_index++;
    }

    /* Slow two-oscillator drone. Phases begin at zero, so menu entry is click-free. */
    const uint32_t drone_step1 = 39415018u;  /* D2 73.416 Hz */
    const uint32_t drone_step2 = 59055800u;  /* A2 110 Hz */
    const uint32_t lfo_step = 29826u;        /* 2 slow breaths per 36 s */

    int32_t lfo = menu_synth_sin8(s->lfo_phase);       /* -127..127 */
    int32_t breath_q8 = 198 + (lfo * 57) / 127;        /* ~55%..100% */
    int32_t drone = menu_synth_sin8(s->drone_phase1) * 18
                  + menu_synth_tri8(s->drone_phase2) * 8;
    drone = (drone * breath_q8) >> 8;

    s->drone_phase1 += drone_step1;
    s->drone_phase2 += drone_step2;
    s->lfo_phase += lfo_step;

    int32_t bell = 0;
    if (s->bell_left > 0u) {
        int32_t osc = menu_synth_sin8(s->bell_phase1) * 100
                    + menu_synth_sin8(s->bell_phase2) * 20
                    + menu_synth_tri8(s->bell_phase3) * 4;

        uint32_t env = s->bell_env_q15;
        uint32_t attack_q15 = 32767u;
        if (s->bell_attack < 160u) {
            attack_q15 = (uint32_t)s->bell_attack * 205u;
            s->bell_attack++;
        }
        bell = (int32_t)(((int64_t)osc * (int64_t)env * (int64_t)attack_q15) >> 30);

        s->bell_phase1 += s->bell_step;
        s->bell_phase2 += (s->bell_step << 1) + (s->bell_step / 100u); /* 2.01x */
        s->bell_phase3 += s->bell_step * 3u;

        s->bell_left--;
        s->decay_tick++;
        if (s->decay_tick >= 10u) {
            s->decay_tick = 0u;
            s->bell_env_q15 -= (s->bell_env_q15 >> 11); /* ~2.56 s decay */
        }
    }

    int32_t dry = drone + bell;

    /* One 2.32 KiB ring gives two echo taps at 145 ms and 290 ms. */
    uint16_t pos = s->delay_pos;
    uint16_t tap1 = (pos >= MENU_SYNTH_DELAY_TAP1)
        ? (uint16_t)(pos - MENU_SYNTH_DELAY_TAP1)
        : (uint16_t)(pos + MENU_SYNTH_DELAY_SAMPLES - MENU_SYNTH_DELAY_TAP1);
    int32_t d1 = ((int32_t)s->delay[tap1]) << 8;
    int32_t d2 = ((int32_t)s->delay[pos]) << 8;
    int32_t with_delay = dry + (d1 * 13) / 100 + (d2 * 45) / 1000;

    s->delay[pos] = (int8_t)menu_synth_clip_i8(dry >> 8);
    pos++;
    if (pos >= MENU_SYNTH_DELAY_SAMPLES) pos = 0u;
    s->delay_pos = pos;

    s->loop_pos++;
    if (s->loop_pos >= MENU_SYNTH_LOOP_SAMPLES) {
        s->loop_pos = 0u;
        s->event_index = 0u;
    }

    /* User requested menu music at 30% of voice level and below SFX. */
    with_delay = (with_delay * (int32_t)MENU_SYNTH_MUSIC_GAIN) / 100;
    if (with_delay > 32767) with_delay = 32767;
    if (with_delay < -32768) with_delay = -32768;
    return (int16_t)with_delay;
}

#endif
