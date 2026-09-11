#ifndef DUREN_AUDIO_ASSETS_H
#define DUREN_AUDIO_ASSETS_H

#include <stdint.h>

#define DUREN_SFX_SAMPLE_RATE 8000u

typedef struct {
    const void *data;
    uint32_t length;
    uint8_t unsigned_pcm;
} DurenSfxSample;

typedef enum {
    DUREN_SFX_CARD = 0,
    DUREN_SFX_DEAL,
    DUREN_SFX_ERROR,
    DUREN_SFX_FLIP,
    DUREN_SFX_SELECT,
    DUREN_SFX_SHUFFLE,
    DUREN_SFX_TAKE,
    DUREN_SFX_WIN,
    DUREN_SFX_LOSE,
    DUREN_SFX_DRAW,
    DUREN_SFX_NEXT_OPPONENT,
    DUREN_SFX_JEFF,
    DUREN_SFX_SLIDE,
    DUREN_SFX_COUNT
} DurenSfxId;

extern const DurenSfxSample duren_sfx[DUREN_SFX_COUNT];
extern const DurenSfxSample duren_face_voices[20];

#endif
