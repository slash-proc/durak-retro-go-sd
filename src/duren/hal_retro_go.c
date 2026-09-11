#include "hal.h"
#include "main.h"
#include "gw_lcd.h"
#include "gw_buttons.h"
#include "gw_audio.h"
#include "gw_malloc.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "rng.h"
#include "duren_pak.h"

#include "gw_core_bridge.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DURAK_DIAGNOSTIC_NO_AUDIO 0

// ---------------------------------------------------------------------------
// External indexed graphics pack. Large art is loaded lazily into scene caches;
// only wrappers and the currently used resources live in overlay RAM.
// ---------------------------------------------------------------------------
#define GFX_PACK_MAX_ENTRIES 24
#define GFX_PACK_ENTRY_SIZE 56u
#define GFX_FLAG_PACKED_4BPP 2u
#define GFX_CACHE_LARGE_SIZE (2080u * 60u)
#define GFX_CACHE_FACES_SIZE (1100u * 55u)
#define GFX_CACHE_PLAYER_FACES_SIZE (110u * 55u)
#define GFX_CACHE_FONT8_SIZE (128u * 128u)
#define GFX_CACHE_FONT6_SIZE (96u * 160u)
#define GFX_CACHE_TITLE_SIZE (40u * 60u)

typedef struct {
    char name[32];
    uint32_t offset;
    uint32_t size;
    uint32_t checksum;
    uint16_t width;
    uint16_t height;
    uint16_t palette_count;
    uint16_t flags;
} GfxPackEntry;

typedef struct {
    uint8_t* pixels;
    uint32_t capacity;
    uint16_t* palette;
    int entry_index;
} GfxCache;

enum {
    GFX_GROUP_LARGE = 0,
    GFX_GROUP_FACES,
    GFX_GROUP_PLAYER_FACES,
    GFX_GROUP_FONT8,
    GFX_GROUP_FONT6,
    GFX_GROUP_TITLE,
    GFX_GROUP_COUNT
};

static GfxPackEntry gfx_entries[GFX_PACK_MAX_ENTRIES];
static int gfx_entry_count = 0;
static uint8_t gfx_large_pixels[GFX_CACHE_LARGE_SIZE];
static uint8_t gfx_faces_pixels[GFX_CACHE_FACES_SIZE];
static uint8_t gfx_player_faces_pixels[GFX_CACHE_PLAYER_FACES_SIZE];
static uint8_t gfx_font8_pixels[GFX_CACHE_FONT8_SIZE];
static uint8_t gfx_font6_pixels[GFX_CACHE_FONT6_SIZE];
static uint8_t gfx_title_pixels[GFX_CACHE_TITLE_SIZE];
static uint16_t gfx_palettes[GFX_GROUP_COUNT][256];
static GfxCache gfx_caches[GFX_GROUP_COUNT] = {
    { gfx_large_pixels, GFX_CACHE_LARGE_SIZE, gfx_palettes[GFX_GROUP_LARGE], -1 },
    { gfx_faces_pixels, GFX_CACHE_FACES_SIZE, gfx_palettes[GFX_GROUP_FACES], -1 },
    { gfx_player_faces_pixels, GFX_CACHE_PLAYER_FACES_SIZE,
      gfx_palettes[GFX_GROUP_PLAYER_FACES], -1 },
    { gfx_font8_pixels, GFX_CACHE_FONT8_SIZE, gfx_palettes[GFX_GROUP_FONT8], -1 },
    { gfx_font6_pixels, GFX_CACHE_FONT6_SIZE, gfx_palettes[GFX_GROUP_FONT6], -1 },
    { gfx_title_pixels, GFX_CACHE_TITLE_SIZE, gfx_palettes[GFX_GROUP_TITLE], -1 },
};

struct HalTexture {
    const uint8_t* indices;
    const uint16_t* palette;
    uint16_t* dim_palette;
    uint8_t dim_percent;
    int width;
    int height;
    int entry_index;
    int cache_group;
};

static uint16_t read_u16le(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t read_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t fnv1a_update(uint32_t h, const uint8_t* data, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) { h ^= data[i]; h *= 0x01000193u; }
    return h;
}
static FILE* gfx_open_pack(uint32_t* out_base) {
    FILE* f = NULL;
    uint32_t base = 0;
    if (!duren_pak_open_member(DUREN_PAK_MEMBER_GFX, &f, &base))
        return NULL;
    if (out_base) *out_base = base;
    return f;
}
static int gfx_group_for_name(const char* name) {
    if (strcmp(name, "faces.png") == 0 || strcmp(name, "faces2.png") == 0)
        return GFX_GROUP_FACES;
    /* Player and NPC portraits are visible in the same gameplay frame. They
     * must not share one lazy cache: alternating the two entries would read
     * and checksum both assets from SD every frame (about 5 FPS on hardware). */
    if (strcmp(name, "player_faces.png") == 0 ||
        strcmp(name, "player_faces2.png") == 0)
        return GFX_GROUP_PLAYER_FACES;
    if (strcmp(name, "font.png") == 0 || strcmp(name, "font_uk_cp1251.png") == 0 ||
        strcmp(name, "menu_font_regular.png") == 0 ||
        strcmp(name, "menu_font_regular_uk.png") == 0 ||
        strcmp(name, "menu_font_regular_white.png") == 0)
        return GFX_GROUP_FONT8;
    if (strcmp(name, "6x10_font.png") == 0 || strcmp(name, "6x10_font_uk_cp1251.png") == 0)
        return GFX_GROUP_FONT6;
    if (strcmp(name, "title.png") == 0 || strcmp(name, "title_poker.png") == 0 ||
        strcmp(name, "title_cat.png") == 0)
        return GFX_GROUP_TITLE;
    return GFX_GROUP_LARGE;
}
static bool gfx_pack_init(void) {
    uint32_t pack_base = 0;
    FILE* f = gfx_open_pack(&pack_base);
    if (!f) return false;
    uint8_t header[16];
    bool ok = false;
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) goto done;
    if (memcmp(header, "DGFX", 4) != 0 || read_u16le(header + 4) != 1u) goto done;
    const uint16_t count = read_u16le(header + 6);
    if (count == 0 || count > GFX_PACK_MAX_ENTRIES ||
        read_u32le(header + 12) != GFX_PACK_ENTRY_SIZE) goto done;
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t raw[GFX_PACK_ENTRY_SIZE];
        if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) goto done;
        GfxPackEntry* e = &gfx_entries[i];
        memcpy(e->name, raw, 31); e->name[31] = '\0';
        /* Absolute offset inside Duren.pak (member base + inner offset). */
        e->offset = pack_base + read_u32le(raw + 32);
        e->size = read_u32le(raw + 36);
        e->width = read_u16le(raw + 40);
        e->height = read_u16le(raw + 42);
        e->palette_count = read_u16le(raw + 44);
        e->flags = read_u16le(raw + 46);
        e->checksum = read_u32le(raw + 48);
        if (!e->width || !e->height || !e->palette_count || e->palette_count > 256u)
            goto done;
        const uint32_t pixel_count = (uint32_t)e->width * e->height;
        const uint32_t stored_pixel_bytes = (e->flags & GFX_FLAG_PACKED_4BPP)
                                          ? (pixel_count + 1u) / 2u
                                          : pixel_count;
        if ((e->flags & GFX_FLAG_PACKED_4BPP) && e->palette_count > 16u)
            goto done;
        if (e->size != stored_pixel_bytes + (uint32_t)e->palette_count * 2u)
            goto done;
    }
    gfx_entry_count = count;
    ok = true;
done:
    fclose(f);
    return ok;
}
static int gfx_find_entry(const char* filename) {
    for (int i = 0; i < gfx_entry_count; ++i)
        if (strcmp(gfx_entries[i].name, filename) == 0) return i;
    return -1;
}
static bool hal_texture_ensure(HalTexture* texture) {
    if (!texture || texture->entry_index < 0 || texture->entry_index >= gfx_entry_count)
        return false;
    GfxCache* cache = &gfx_caches[texture->cache_group];
    if (cache->entry_index == texture->entry_index) {
        texture->indices = cache->pixels;
        texture->palette = cache->palette;
        return true;
    }
    const GfxPackEntry* e = &gfx_entries[texture->entry_index];
    const uint32_t pixel_count = (uint32_t)e->width * e->height;
    const uint32_t stored_pixel_bytes = (e->flags & GFX_FLAG_PACKED_4BPP)
                                      ? (pixel_count + 1u) / 2u
                                      : pixel_count;
    if (pixel_count > cache->capacity) return false;
    FILE* f = gfx_open_pack(NULL);
    if (!f) return false;
    bool ok = false;
    if (fseek(f, (long)e->offset, SEEK_SET) != 0) goto done;
    if (fread(cache->pixels, 1, stored_pixel_bytes, f) != stored_pixel_bytes) goto done;
    memset(cache->palette, 0, 256u * sizeof(uint16_t));
    if (fread(cache->palette, 2, e->palette_count, f) != e->palette_count) goto done;
    uint32_t hash = fnv1a_update(0x811C9DC5u, cache->pixels, stored_pixel_bytes);
    hash = fnv1a_update(hash, (const uint8_t*)cache->palette,
                       (uint32_t)e->palette_count * 2u);
    if (hash != e->checksum) goto done;
    if (e->flags & GFX_FLAG_PACKED_4BPP) {
        /* Expand in place from the end. Reverse order prevents the output
         * bytes from overwriting packed nibbles that have not been read yet. */
        for (uint32_t i = pixel_count; i-- > 0u; ) {
            const uint8_t packed = cache->pixels[i >> 1];
            cache->pixels[i] = (i & 1u) ? (uint8_t)(packed >> 4)
                                        : (uint8_t)(packed & 0x0Fu);
        }
    }
    cache->entry_index = texture->entry_index;
    texture->indices = cache->pixels;
    texture->palette = cache->palette;
    texture->dim_percent = 0xFFu;
    ok = true;
done:
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Кнопки
// ---------------------------------------------------------------------------
static uint32_t btn_state = 0;
static uint32_t btn_old_state = 0;

/* Shadow direction only: 0=down-right, 1=down-left, 2=straight down.
 * Every gameplay mode uses one fixed, lightweight ambient table mask:
 * a very subtle warm/bright center and almost neutral edges. TIME changes
 * cast-shadow direction only; it never changes or rebuilds the light mask. */
static uint8_t gameplay_shadow_mode = 0;
static bool time_button_was_down = false;
#define GAME_LIGHT_MAP_PIXELS (SCREEN_WIDTH * SCREEN_HEIGHT)

/* Large mutable lighting tables live in AHB SRAM, not in the Durak overlay
 * BSS. AHB SRAM is also DMA2D-accessible. */
static uint8_t* gameplay_light_gain_map = NULL;
static uint32_t* gameplay_light_bg_clut = NULL;


void hal_get_shadow_offset(int object_center_x, int lift, int* dx, int* dy) {
    int sx = 0;
    int sy = 2;
    if (lift < 0) lift = 0;
    if (lift > 8) lift = 8;

    switch (gameplay_shadow_mode) {
        case 1: /* alternate cast: down-left */
            sx = -2;
            break;
        case 2: /* alternate cast: straight down */
            (void)object_center_x;
            sx = 0;
            break;
        case 0:
        default: /* alternate cast: down-right */
            sx = 2;
            break;
    }

    /* Resting cards stay within the requested 1..2 px contact shadow. A card
     * in flight gets only a modest extra cast instead of the old 6..18 px
     * offset that could cover neighbouring cards and the exposed trump. */
    if (lift > 0) {
        int extra = (lift + 3) / 4;
        if (sx > 0) sx += extra;
        else if (sx < 0) sx -= extra;
        sy += (lift + 2) / 3;
    }

    if (dx) *dx = sx;
    if (dy) *dy = sy;
}

static void durak_repaint_for_retro_go_menu(void) {
    // hal_update() runs only after the previous vblank completed.  Preserve
    // the last displayed Durak frame as the background for Retro-Go menus.
    lcd_clone();
}

static uint32_t map_button_bit(Button btn) {
    switch (btn) {
        case BTN_UP:     return B_Up;
        case BTN_DOWN:   return B_Down;
        case BTN_LEFT:   return B_Left;
        case BTN_RIGHT:  return B_Right;
        case BTN_A:      return B_A;
        case BTN_B:      return B_B;
        case BTN_START:  return B_START;
        case BTN_SELECT: return B_SELECT;
        /* POWER is owned by Retro-Go sleep/resume.  Never expose it as
         * Durak quit: common_emu_input_loop() keeps the pre-sleep joystick
         * snapshot when it returns after wake, which otherwise makes the
         * game exit immediately and tears down the overlay during resume. */
        case BTN_QUIT:   return 0;
        default:         return 0;
    }
}

// ---------------------------------------------------------------------------
// Audio: unsigned PCM8, 8 kHz mono, mixed directly into Retro-Go's
// signed PCM16 8 kHz DMA output.  This deliberately matches the last
// hardware build whose sound was confirmed working on Game & Watch.
// ---------------------------------------------------------------------------
/* A 512-sample DMA half lasts 64 ms at 8 kHz, so the 60 Hz game loop has
 * ample time to refill the half that just completed.  Do not resample this
 * console path to 48 kHz: the Retro-Go audio driver used by this port is
 * already configured for 8 kHz, and stretching samples caused the low hum
 * and apparent slow-motion audio seen in the broken build. */
#ifndef DUREN_AUDIO_DMA_HALF_SAMPLES
#define DUREN_AUDIO_DMA_HALF_SAMPLES 512u
#endif
#define AUDIO_DMA_HALF_SAMPLES DUREN_AUDIO_DMA_HALF_SAMPLES
#define AUDIO_PACK_MAX_ENTRIES 40
#define AUDIO_PACK_ENTRY_SIZE 56u
#define MUSIC_READ_AHEAD (24u * 1024u)
#define MUSIC_REFILL_LOW_WATER (16u * 1024u)
#define MUSIC_START_PREFILL (4u * 1024u)
#define MUSIC_PREFETCH_PER_FRAME (2u * 1024u)
#define SFX_READ_AHEAD 1024u
#define VOICE_READ_AHEAD 1024u
#define AUDIO_FMT_U8 1u
/* The known-good Retro-Go path is fixed at 8 kHz.  Do not inherit a generic
 * AUDIO_SAMPLE_RATE macro from another core: that was the source of the
 * accidental 8 -> 48 kHz stretch on hardware. */
#define AUDIO_OUTPUT_RATE 8000u
#define AUDIO_SOURCE_RATE 8000u
#define AUDIO_SOURCE_STEP_Q16 ((AUDIO_SOURCE_RATE << 16) / AUDIO_OUTPUT_RATE)
#define AUDIO_FADE_STEP_Q15 23u

_Static_assert(MUSIC_READ_AHEAD <= UINT16_MAX, "music ring indices are 16-bit");
_Static_assert(AUDIO_OUTPUT_RATE >= AUDIO_SOURCE_RATE, "audio output must not be below source rate");
_Static_assert(AUDIO_DMA_HALF_SAMPLES >= 512u, "DMA half too short for the 8 kHz refill loop");

typedef struct {
    char name[32];
    uint32_t offset;
    uint32_t length;
    uint32_t checksum;
    uint16_t sample_rate;
    uint8_t channels;
    uint8_t kind;
    uint8_t format;
} AudioPackEntry;

typedef struct {
    FILE* file;
    const AudioPackEntry* entry;
    uint32_t source_pos;
    uint16_t read_pos;
    uint16_t write_pos;
    uint16_t buffered;
    uint16_t capacity;
    uint8_t* buffer;
    const uint8_t* cached_data;
    uint8_t gain_percent;
    uint32_t phase_q16;
    int32_t sample_a;
    int32_t sample_b;
    uint16_t fade_q15;
    bool resample_primed;
    bool active;
    bool loop;
    bool playlist;
} AudioStream;

typedef struct {
    int entry_index;
    const uint8_t* data;
} CachedSfx;

/* The five card-table effects are the latency-sensitive sounds used during
 * play. FLIP is byte-identical to DEAL in the pack, so both entries share one
 * RAM copy. Keeping these effects out of FatFs removes gameplay-time seeks. */
#define CARD_SFX_CACHE_COUNT 5
static uint8_t cached_sfx_card[5391];
static uint8_t cached_sfx_deal[3480];
static uint8_t cached_sfx_shuffle[5903];
static uint8_t cached_sfx_take[4096];
static CachedSfx cached_card_sfx[CARD_SFX_CACHE_COUNT];

static AudioPackEntry audio_entries[AUDIO_PACK_MAX_ENTRIES];
static int audio_entry_count = 0;
static FILE* audio_music_file = NULL;
static FILE* audio_sfx_file = NULL;
static FILE* audio_voice_file = NULL;
static bool audio_storage_suspended = false;
static uint8_t* music_read_ahead = NULL; /* 24 KiB, allocated in AHB SRAM */
static uint8_t sfx_read_ahead[SFX_READ_AHEAD] __attribute__((aligned(32)));
static uint8_t voice_read_ahead[VOICE_READ_AHEAD] __attribute__((aligned(32)));
static AudioStream music_stream = { .buffer = NULL, .capacity = 0u };
static AudioStream sfx_stream = { .buffer = sfx_read_ahead, .capacity = SFX_READ_AHEAD };
static AudioStream voice_stream = { .buffer = voice_read_ahead, .capacity = VOICE_READ_AHEAD };
static int sfx_queued_index = -1;
static uint8_t sfx_queued_gain = 100u;
static int voice_queued_index = -1;
static uint8_t voice_queued_gain = 100u;
static uint32_t sfx_dma_counter_seen = 0;
static bool audio_resume_pending = false;
static uint8_t audio_resume_delay_frames = 0u;
static uint8_t audio_watchdog_retries = 0u;
static uint32_t audio_watchdog_counter = 0u;
static uint32_t audio_watchdog_tick = 0u;
static int music_current_index = -1;
static int music_game_track = 0;
static bool music_paused = false;
static uint8_t music_volume_step = 5u;
static bool music_menu_intro_full = false;
static uint32_t audio_test_tone_hz = 0u;
static uint32_t audio_test_tone_phase_q32 = 0u;
static uint8_t effects_volume_step = 5u;
static bool graphics_shadows_enabled = true;
static uint32_t music_underruns = 0u;
static uint32_t sfx_underruns = 0u;
static uint32_t voice_underruns = 0u;
static uint32_t dma_missed_halves = 0u;
static uint16_t music_min_buffered = MUSIC_READ_AHEAD;
static void music_filter_reset(void) { }

static FILE* audio_open_pack(void) {
    FILE* f = NULL;
    uint32_t base = 0;
    if (!duren_pak_open_member(DUREN_PAK_MEMBER_AUDIO, &f, &base))
        return NULL;
    (void)base;
    return f;
}
static int audio_find_entry(const char* name) {
    for (int i = 0; i < audio_entry_count; ++i)
        if (strcmp(audio_entries[i].name, name) == 0) return i;
    return -1;
}
static bool audio_cache_record(FILE* f, const char* name,
                               uint8_t* dst, uint32_t capacity,
                               CachedSfx* out) {
    const int index = audio_find_entry(name);
    if (!f || !dst || !out || index < 0) return false;
    const AudioPackEntry* e = &audio_entries[index];
    if (e->length > capacity || fseek(f, (long)e->offset, SEEK_SET) != 0) return false;
    if (fread(dst, 1, e->length, f) != e->length) return false;
    if (fnv1a_update(0x811C9DC5u, dst, e->length) != e->checksum) return false;
    out->entry_index = index;
    out->data = dst;
    return true;
}
static bool audio_cache_card_effects(FILE* f) {
    if (!audio_cache_record(f, "sfx/card", cached_sfx_card,
                            sizeof(cached_sfx_card), &cached_card_sfx[0])) return false;
    if (!audio_cache_record(f, "sfx/deal", cached_sfx_deal,
                            sizeof(cached_sfx_deal), &cached_card_sfx[1])) return false;

    const int flip_index = audio_find_entry("sfx/flip");
    const AudioPackEntry* deal = &audio_entries[cached_card_sfx[1].entry_index];
    if (flip_index < 0 || audio_entries[flip_index].length != deal->length ||
        audio_entries[flip_index].checksum != deal->checksum) return false;
    cached_card_sfx[2].entry_index = flip_index;
    cached_card_sfx[2].data = cached_sfx_deal;

    if (!audio_cache_record(f, "sfx/shuffle", cached_sfx_shuffle,
                            sizeof(cached_sfx_shuffle), &cached_card_sfx[3])) return false;
    if (!audio_cache_record(f, "sfx/take", cached_sfx_take,
                            sizeof(cached_sfx_take), &cached_card_sfx[4])) return false;
    return true;
}
static const uint8_t* audio_cached_sfx_data(int entry_index) {
    for (int i = 0; i < CARD_SFX_CACHE_COUNT; ++i)
        if (cached_card_sfx[i].entry_index == entry_index) return cached_card_sfx[i].data;
    return NULL;
}
static bool audio_pack_init(void) {
    uint32_t pack_base = 0;
    FILE* f = NULL;
    if (!duren_pak_open_member(DUREN_PAK_MEMBER_AUDIO, &f, &pack_base))
        return false;
    uint8_t header[16];
    bool ok = false;
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) goto done;
    if (memcmp(header, "DAUD", 4) != 0 || read_u16le(header + 4) != 2u) goto done;
    const uint16_t count = read_u16le(header + 6);
    if (!count || count > AUDIO_PACK_MAX_ENTRIES ||
        read_u32le(header + 12) != AUDIO_PACK_ENTRY_SIZE) goto done;
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t raw[AUDIO_PACK_ENTRY_SIZE];
        if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) goto done;
        AudioPackEntry* e = &audio_entries[i];
        memcpy(e->name, raw, 31); e->name[31] = '\0';
        e->offset = pack_base + read_u32le(raw + 32);
        e->length = read_u32le(raw + 36);
        e->sample_rate = read_u16le(raw + 40);
        e->channels = raw[42];
        e->kind = raw[43];
        e->checksum = read_u32le(raw + 44);
        e->format = (uint8_t)read_u32le(raw + 48);
        if (!e->length || e->sample_rate != AUDIO_SOURCE_RATE || e->channels != 1u ||
            e->format != AUDIO_FMT_U8) goto done;
    }
    audio_entry_count = count;
    if (!audio_cache_card_effects(f)) goto done;
    audio_music_file = audio_open_pack();
    audio_sfx_file = audio_open_pack();
    audio_voice_file = audio_open_pack();
    ok = audio_music_file && audio_sfx_file && audio_voice_file;
    if (ok) audio_storage_suspended = false;
done:
    fclose(f);
    return ok;
}

static bool audio_seek_reopened_stream(AudioStream* stream, FILE* file) {
    if (!stream || !stream->active || stream->cached_data) return true;
    if (!stream->entry || !file) return false;
    clearerr(file);
    return fseek(file, (long)(stream->entry->offset + stream->source_pos),
                 SEEK_SET) == 0;
}

static bool audio_resume_storage_after_wake(void) {
    if (!audio_storage_suspended) return true;

    FILE* music = audio_open_pack();
    FILE* sfx = audio_open_pack();
    FILE* voice = audio_open_pack();
    bool ok = music && sfx && voice &&
              audio_seek_reopened_stream(&music_stream, music) &&
              audio_seek_reopened_stream(&sfx_stream, sfx) &&
              audio_seek_reopened_stream(&voice_stream, voice);
    if (!ok) {
        if (music) fclose(music);
        if (sfx) fclose(sfx);
        if (voice) fclose(voice);
        return false;
    }

    audio_music_file = music;
    audio_sfx_file = sfx;
    audio_voice_file = voice;
    if (music_stream.active && !music_stream.cached_data) music_stream.file = music;
    if (sfx_stream.active && !sfx_stream.cached_data) sfx_stream.file = sfx;
    if (voice_stream.active && !voice_stream.cached_data) voice_stream.file = voice;
    audio_storage_suspended = false;
    return true;
}

void hal_prepare_retro_go_sleep(void) {
#if DURAK_DIAGNOSTIC_NO_AUDIO
    return;
#else
    /* GW_EnterDeepSleep() unmounts FatFs and removes SD power. Any FIL kept
     * open across that boundary becomes invalid even though its FILE* still
     * looks non-NULL. Preserve stream/ring positions, but release every pack
     * descriptor before the unmount. */
    audio_stop_playing();
    music_stream.file = NULL;
    sfx_stream.file = NULL;
    voice_stream.file = NULL;
    if (audio_music_file) { fclose(audio_music_file); audio_music_file = NULL; }
    if (audio_sfx_file) { fclose(audio_sfx_file); audio_sfx_file = NULL; }
    if (audio_voice_file) { fclose(audio_voice_file); audio_voice_file = NULL; }
    audio_storage_suspended = true;
#endif
}
static void audio_stream_stop(AudioStream* stream) {
    if (!stream) return;
    stream->entry = NULL;
    stream->source_pos = 0;
    stream->read_pos = stream->write_pos = stream->buffered = 0;
    stream->cached_data = NULL;
    stream->phase_q16 = 0u;
    stream->sample_a = 0;
    stream->sample_b = 0;
    stream->fade_q15 = 0u;
    stream->resample_primed = false;
    stream->active = false;
    stream->loop = false;
    stream->playlist = false;
}
static bool audio_stream_start(AudioStream* stream, FILE* file, int entry_index,
                               uint8_t gain, bool loop, bool playlist) {
    if (!stream || !stream->buffer || stream->capacity == 0 || !file ||
        entry_index < 0 || entry_index >= audio_entry_count)
        return false;
    const AudioPackEntry* e = &audio_entries[entry_index];
    if (fseek(file, (long)e->offset, SEEK_SET) != 0) return false;
    stream->file = file;
    stream->entry = e;
    stream->source_pos = 0;
    stream->read_pos = stream->write_pos = stream->buffered = 0;
    stream->cached_data = NULL;
    stream->gain_percent = gain;
    stream->phase_q16 = 0u;
    stream->sample_a = 0;
    stream->sample_b = 0;
    stream->fade_q15 = 0u;
    stream->resample_primed = false;
    stream->active = true;
    stream->loop = loop;
    stream->playlist = playlist;
    if (stream == &music_stream) music_filter_reset();
    return true;
}
static bool audio_stream_start_cached(AudioStream* stream, int entry_index,
                                      const uint8_t* data, uint8_t gain) {
    if (!stream || !data || entry_index < 0 || entry_index >= audio_entry_count) return false;
    stream->file = NULL;
    stream->entry = &audio_entries[entry_index];
    stream->source_pos = 0;
    stream->read_pos = stream->write_pos = stream->buffered = 0;
    stream->cached_data = data;
    stream->gain_percent = gain;
    stream->phase_q16 = 0u;
    stream->sample_a = 0;
    stream->sample_b = 0;
    stream->fade_q15 = 32768u;
    stream->resample_primed = false;
    stream->active = true;
    stream->loop = false;
    stream->playlist = false;
    return true;
}
static int music_pick_random_game_track(int avoid_track) {
    if (avoid_track < 1 || avoid_track > 3)
        return (int)(duren_rand() % 3u) + 1;
    int pick = (int)(duren_rand() % 2u) + 1;
    if (pick >= avoid_track) ++pick;
    return pick;
}

static bool music_select_next_source(void) {
    if (!music_stream.active || !music_stream.file) return false;
    if (music_stream.playlist) {
        /* Every automatic track change is shuffled, but never repeats the
         * track that just finished. */
        music_game_track = music_pick_random_game_track(music_game_track);
        char name[20];
        snprintf(name, sizeof(name), "music/game%d", music_game_track);
        int idx = audio_find_entry(name);
        if (idx < 0) return false;
        music_current_index = idx;
        music_stream.entry = &audio_entries[idx];
        music_stream.source_pos = 0;
        clearerr(music_stream.file);
        return fseek(music_stream.file, (long)music_stream.entry->offset, SEEK_SET) == 0;
    }
    if (music_stream.loop && music_stream.entry) {
        music_stream.source_pos = 0;
        return fseek(music_stream.file, (long)music_stream.entry->offset, SEEK_SET) == 0;
    }
    return false;
}
static void audio_stream_prefetch(AudioStream* stream, uint32_t budget) {
    if (!stream || !stream->active || !stream->entry || stream->cached_data ||
        !stream->buffer || !stream->capacity) return;
    while (budget > 0 && stream->buffered < stream->capacity) {
        if (stream->source_pos >= stream->entry->length) {
            if (stream == &music_stream) {
                if (!music_select_next_source()) break;
            } else {
                break;
            }
        }
        uint32_t free_space = (uint32_t)stream->capacity - stream->buffered;
        uint32_t contiguous = (uint32_t)stream->capacity - stream->write_pos;
        uint32_t remain = stream->entry->length - stream->source_pos;
        uint32_t want = free_space;
        if (want > contiguous) want = contiguous;
        if (want > remain) want = remain;
        if (want > budget) want = budget;
        if (want == 0) break;
        size_t got = fread(stream->buffer + stream->write_pos, 1, want, stream->file);
        if (got == 0) {
            /* SD/FAT reads can leave FILE in a persistent error state. Clear it,
             * seek back to the exact payload byte and retry once. Without this,
             * an isolated read fault leaves a channel permanently silent until
             * the whole game is exited and entered again. */
            clearerr(stream->file);
            const long retry_pos = (long)(stream->entry->offset + stream->source_pos);
            if (fseek(stream->file, retry_pos, SEEK_SET) == 0) {
                got = fread(stream->buffer + stream->write_pos, 1, want, stream->file);
            }
            if (got == 0) {
                clearerr(stream->file);
                if (stream == &music_stream && music_select_next_source()) continue;
                audio_stream_stop(stream);
                break;
            }
        }
        stream->write_pos = (uint16_t)((stream->write_pos + got) % stream->capacity);
        stream->buffered = (uint16_t)(stream->buffered + got);
        stream->source_pos += (uint32_t)got;
        budget -= (uint32_t)got;
        if (got < want) break;
    }
}
static bool audio_stream_pop_source_sample(AudioStream* stream, int32_t* sample) {
    if (!stream || !sample || !stream->entry) return false;
    if (stream->cached_data) {
        if (stream->source_pos >= stream->entry->length) return false;
        const uint8_t value = stream->cached_data[stream->source_pos++];
        *sample = ((int32_t)value - 128) << 8;
        return true;
    }
    if (stream->buffered == 0u) return false;
    const uint8_t value = stream->buffer[stream->read_pos];
    stream->read_pos = (uint16_t)((stream->read_pos + 1u) % stream->capacity);
    stream->buffered--;
    *sample = ((int32_t)value - 128) << 8;
    return true;
}

static bool audio_stream_next(AudioStream* stream, int32_t* sample,
                              uint8_t master_step) {
    if (!stream || !sample || !stream->active || !stream->entry) return false;

#if AUDIO_SOURCE_RATE == AUDIO_OUTPUT_RATE
    /* Exact Game & Watch path: one packed PCM8 byte becomes one signed PCM16
     * output sample. No interpolation, phase accumulator or sample stretching. */
    int32_t value = 0;
    if (!audio_stream_pop_source_sample(stream, &value)) {
        if (stream == &music_stream) ++music_underruns;
        else if (stream == &sfx_stream) ++sfx_underruns;
        else if (stream == &voice_stream) ++voice_underruns;
        if (stream->source_pos >= stream->entry->length && stream->buffered == 0u &&
            !stream->loop && !stream->playlist) audio_stream_stop(stream);
        return false;
    }
#else
    if (!stream->resample_primed) {
        if (!audio_stream_pop_source_sample(stream, &stream->sample_a)) {
            if (stream == &music_stream) ++music_underruns;
            else if (stream == &sfx_stream) ++sfx_underruns;
            else if (stream == &voice_stream) ++voice_underruns;
            if (stream->source_pos >= stream->entry->length && stream->buffered == 0u &&
                !stream->loop && !stream->playlist) audio_stream_stop(stream);
            return false;
        }
        if (!audio_stream_pop_source_sample(stream, &stream->sample_b))
            stream->sample_b = stream->sample_a;
        stream->phase_q16 = 0u;
        stream->resample_primed = true;
    }

    const uint32_t frac = stream->phase_q16;
    int32_t value = (int32_t)(((int64_t)stream->sample_a * (65536u - frac) +
                               (int64_t)stream->sample_b * frac) >> 16);
    stream->phase_q16 += AUDIO_SOURCE_STEP_Q16;
    while (stream->phase_q16 >= 65536u) {
        stream->phase_q16 -= 65536u;
        stream->sample_a = stream->sample_b;
        if (!audio_stream_pop_source_sample(stream, &stream->sample_b)) {
            stream->sample_b = stream->sample_a;
            stream->resample_primed = false;
            break;
        }
    }
#endif

    if (stream == &music_stream && stream->buffered < music_min_buffered)
        music_min_buffered = stream->buffered;

    if (stream == &music_stream && stream->fade_q15 < 32768u) {
        value = (int32_t)(((int64_t)value * stream->fade_q15) >> 15);
        uint32_t next = stream->fade_q15 + AUDIO_FADE_STEP_Q15;
        stream->fade_q15 = (uint16_t)(next > 32768u ? 32768u : next);
    }

    const int32_t master_percent = (int32_t)master_step * 20;
    *sample = (int32_t)(((int64_t)value * stream->gain_percent * master_percent) / 10000);
    return true;
}

static void start_sfx_index(int index, uint8_t gain) {
    if (index < 0) return;
    const uint8_t* cached = audio_cached_sfx_data(index);
    if (cached) {
        audio_stream_start_cached(&sfx_stream, index, cached, gain);
    } else {
        audio_stream_start(&sfx_stream, audio_sfx_file, index, gain, false, false);
        audio_stream_prefetch(&sfx_stream, SFX_READ_AHEAD);
    }
    sfx_queued_index = -1;
}
static void start_voice_index(int index, uint8_t gain) {
    if (index < 0) return;
    audio_stream_start(&voice_stream, audio_voice_file, index, gain, false, false);
    audio_stream_prefetch(&voice_stream, VOICE_READ_AHEAD);
    voice_queued_index = -1;
}

void hal_music_set_tone(unsigned int frequency_hz) {
    if (frequency_hz > AUDIO_OUTPUT_RATE / 3u) frequency_hz = AUDIO_OUTPUT_RATE / 3u;
    audio_test_tone_hz = frequency_hz;
    audio_test_tone_phase_q32 = 0u;
}
void hal_music_set_menu_intro(bool full_volume) {
    music_menu_intro_full = full_volume;
    if (music_stream.active && !music_stream.playlist)
        music_stream.gain_percent = full_volume ? 100u : 60u;
}
void hal_music_start(void) {
    const int idx = audio_find_entry("music/menu");
    if (idx < 0) return;
    if (music_stream.active && !music_stream.playlist && music_current_index == idx) return;
    const uint8_t gain = music_menu_intro_full ? 100u : 60u;
    if (audio_stream_start(&music_stream, audio_music_file, idx, gain, true, false)) {
        music_current_index = idx;
        audio_stream_prefetch(&music_stream, MUSIC_START_PREFILL);
    }
}
void hal_music_start_gameplay(void) {
    if (music_stream.active && music_stream.playlist) return;
    hal_music_start_gameplay_new_match();
}
void hal_music_start_gameplay_new_match(void) {
    /* Every real match/round starts on a fresh shuffled track. */
    music_game_track = music_pick_random_game_track(music_game_track);
    char name[20];
    snprintf(name, sizeof(name), "music/game%d", music_game_track);
    const int idx = audio_find_entry(name);
    if (idx >= 0 && audio_stream_start(&music_stream, audio_music_file, idx, 30u, false, true)) {
        music_current_index = idx;
        music_min_buffered = MUSIC_READ_AHEAD;
        audio_stream_prefetch(&music_stream, MUSIC_START_PREFILL);
    }
}
void hal_music_pause(bool paused) { music_paused = paused; }
void hal_music_stop(void) {
    audio_stream_stop(&music_stream);
    music_filter_reset();
    music_current_index = -1;
    music_paused = false;
}

static void audio_prefetch_update(void) {
    /* File seeks/reads are kept out of the DMA refill path. Queued channels
     * are opened and prefetched here from the ordinary frame update. */
    if (!sfx_stream.active && sfx_queued_index >= 0) {
        const int queued = sfx_queued_index;
        const uint8_t gain = sfx_queued_gain;
        sfx_queued_index = -1;
        start_sfx_index(queued, gain);
    }
    if (!voice_stream.active && voice_queued_index >= 0) {
        const int queued = voice_queued_index;
        const uint8_t gain = voice_queued_gain;
        voice_queued_index = -1;
        start_voice_index(queued, gain);
    }
    if (music_stream.active && music_stream.buffered < MUSIC_REFILL_LOW_WATER) {
        uint32_t budget = (uint32_t)music_stream.capacity - music_stream.buffered;
        if (budget > MUSIC_PREFETCH_PER_FRAME) budget = MUSIC_PREFETCH_PER_FRAME;
        audio_stream_prefetch(&music_stream, budget);
    }
    audio_stream_prefetch(&sfx_stream, 512u);
    audio_stream_prefetch(&voice_stream, 512u);
}
static int32_t audio_soft_limit(int32_t sample) {
    const int32_t sign = sample < 0 ? -1 : 1;
    int32_t a = sample < 0 ? -sample : sample;
    if (a > 24576) a = 24576 + (a - 24576) / 3;
    if (a > 32767) a = 32767;
    return sign * a;
}

static void sfx_fill_buffer(int16_t* dst, uint16_t count) {
    if (!dst || count == 0) return;
    const uint8_t volume = common_emu_sound_get_volume();
    for (uint16_t i = 0; i < count; ++i) {
        int32_t mixed = 0;
        if (audio_test_tone_hz != 0u) {
            audio_test_tone_phase_q32 += (uint32_t)(((uint64_t)audio_test_tone_hz << 32) / AUDIO_OUTPUT_RATE);
            mixed = (audio_test_tone_phase_q32 & 0x80000000u) ? 8192 : -8192;
            mixed = (mixed * volume) >> 8;
            dst[i] = (int16_t)mixed;
            continue;
        }
        int32_t sample = 0;
        const bool voice_now = voice_stream.active &&
                               (voice_stream.resample_primed || voice_stream.buffered > 0u);
        if (!music_paused && audio_stream_next(&music_stream, &sample, music_volume_step)) {
            if (voice_now) sample = (sample * 3) / 4;
            mixed += sample;
        }
        if (audio_stream_next(&sfx_stream, &sample, effects_volume_step))
            mixed += sample;
        if (audio_stream_next(&voice_stream, &sample, effects_volume_step))
            mixed += sample;
        mixed = (mixed * volume) >> 8;
        dst[i] = (int16_t)audio_soft_limit(mixed);
    }
}
static void audio_dma_start_known_good(void) {
    /* Match the known-good Game & Watch build exactly: Retro-Go owns the
     * peripheral clock and the double buffer; DUREN only starts a 512-sample
     * 8 kHz stream and refills audio_get_active_buffer() after callbacks. */
    audio_stop_playing();
    audio_prefetch_update();
    audio_start_playing(AUDIO_DMA_HALF_SAMPLES);
    /* Prime the first safe half immediately. After STOP2 the DMA buffers may
     * contain silence even though stream state and file handles remain valid. */
    sfx_fill_buffer(audio_get_active_buffer(), audio_get_buffer_length());
    sfx_dma_counter_seen = dma_counter;
    audio_watchdog_counter = dma_counter;
    audio_watchdog_tick = HAL_GetTick();
}

static void audio_schedule_resume(void) {
    audio_stop_playing();
    audio_resume_pending = true;
    audio_resume_delay_frames = 2u;
    audio_watchdog_retries = 0u;
    audio_watchdog_counter = dma_counter;
    audio_watchdog_tick = HAL_GetTick();
}

static void audio_resume_update(void) {
    if (!audio_resume_pending) return;
    /* The SD is mounted before Retro-Go's post-wakeup callback. If a card is
     * briefly slow to answer, keep retrying from ordinary frames and do not
     * start DMA with stale FatFs descriptors. */
    if (!audio_resume_storage_after_wake()) return;
    if (audio_resume_delay_frames > 0u) {
        --audio_resume_delay_frames;
        return;
    }
    /* Retro-Go finishes restoring clocks/LCD after common_emu_input_loop().
     * Waiting two game frames avoids racing its STOP2 return path. */
    audio_dma_start_known_good();
    audio_resume_pending = false;
}

static void audio_dma_watchdog_update(void) {
    if (audio_resume_pending) return;
    const uint32_t now_counter = dma_counter;
    const uint32_t now_ms = HAL_GetTick();
    if (now_counter != audio_watchdog_counter) {
        audio_watchdog_counter = now_counter;
        audio_watchdog_tick = now_ms;
        audio_watchdog_retries = 0u;
        return;
    }
    const uint32_t timeout = audio_watchdog_retries < 3u ? 350u : 1500u;
    if (now_ms - audio_watchdog_tick >= timeout) {
        audio_dma_start_known_good();
        if (audio_watchdog_retries < 255u) ++audio_watchdog_retries;
        audio_watchdog_tick = now_ms;
    }
}

static void sfx_audio_update(void) {
#if DURAK_DIAGNOSTIC_NO_AUDIO
    return;
#endif
    audio_prefetch_update();
    const uint32_t now = dma_counter;
    if (now == sfx_dma_counter_seen) return;

    const uint32_t delta = now - sfx_dma_counter_seen;
    if (delta > 1u) dma_missed_halves += delta - 1u;
    sfx_dma_counter_seen = now;

    /* In this Retro-Go tree audio_get_active_buffer() is the half that has
     * just completed playback and is safe for the CPU to refill.  Never write
     * the other half and never restart DMA merely because more than one game
     * frame elapsed. */
    sfx_fill_buffer(audio_get_active_buffer(), audio_get_buffer_length());
}
void hal_play_sound_gain(SoundFx snd, unsigned int gain_percent) {
#if DURAK_DIAGNOSTIC_NO_AUDIO
    (void)snd; (void)gain_percent; return;
#endif
    static const char* const names[SND_COUNT] = {
        "sfx/card", "sfx/deal", "sfx/error", "sfx/flip", "sfx/select",
        "sfx/shuffle", "sfx/take", "sfx/win", "sfx/lose", "sfx/draw",
        "sfx/next_opponent", "sfx/jeff"
    };
    if ((unsigned int)snd >= SND_COUNT) return;
    if (gain_percent > 200u) gain_percent = 200u;
    start_sfx_index(audio_find_entry(names[(unsigned int)snd]), (uint8_t)gain_percent);
}
void hal_play_sound(SoundFx snd) {
    hal_play_sound_gain(snd, (snd == SND_ERROR) ? 43u : 80u);
}
void hal_play_face_voice(int face_index) {
#if DURAK_DIAGNOSTIC_NO_AUDIO
    (void)face_index; return;
#endif
    if (face_index < 0) face_index = 0;
    if (face_index > 19) face_index = 19;
    char name[16];
    snprintf(name, sizeof(name), "voice/%02d", face_index);
    start_voice_index(audio_find_entry(name), 100u);
}
void hal_queue_face_voice(int face_index) {
#if DURAK_DIAGNOSTIC_NO_AUDIO
    (void)face_index; return;
#endif
    if (face_index < 0) face_index = 0;
    if (face_index > 19) face_index = 19;
    char name[16];
    snprintf(name, sizeof(name), "voice/%02d", face_index);
    voice_queued_index = audio_find_entry(name);
    voice_queued_gain = 100u;
}
bool hal_audio_busy(void) {
#if DURAK_DIAGNOSTIC_NO_AUDIO
    return false;
#else
    return sfx_stream.active || sfx_queued_index >= 0 ||
           voice_stream.active || voice_queued_index >= 0;
#endif
}
void hal_set_music_volume_step(uint8_t step) {
    if (step > 10u) step = 10u;
    music_volume_step = step;
}
void hal_set_effects_volume_step(uint8_t step) {
    if (step > 10u) step = 10u;
    effects_volume_step = step;
}
void hal_set_shadows_enabled(bool enabled) { graphics_shadows_enabled = enabled; }
bool hal_shadows_enabled(void) { return graphics_shadows_enabled; }

// ---------------------------------------------------------------------------
// Життєвий цикл / вхід
// ---------------------------------------------------------------------------
void hal_on_retro_go_wakeup(void) {
    /* Retro-Go invokes this after STOP2 restored clocks and peripherals.
     * Do not rely solely on the POWER bit: the platform may consume it before
     * the first resumed game frame. */
#if !DURAK_DIAGNOSTIC_NO_AUDIO
    (void)audio_resume_storage_after_wake();
    audio_schedule_resume();
#endif
}

static bool ensure_audio_read_ahead_memory(void) {
    if (!music_read_ahead) {
        /* Prefer AHB (non-cacheable DMA-friendly); fall back to RAM_EMU. */
        uint8_t* raw = (uint8_t*)ahb_malloc(MUSIC_READ_AHEAD + 31u);
        if (!raw)
            raw = (uint8_t*)ram_malloc(MUSIC_READ_AHEAD + 31u);
        if (raw) {
            uintptr_t aligned = ((uintptr_t)raw + 31u) & ~(uintptr_t)31u;
            music_read_ahead = (uint8_t*)aligned;
        }
    }
    if (!music_read_ahead) {
        printf("duren: music ring alloc failed (AHB+RAM)\n");
        return false;
    }
    music_stream.buffer = music_read_ahead;
    music_stream.capacity = MUSIC_READ_AHEAD;
    return true;
}

static bool ensure_gameplay_light_memory(void) __attribute__((unused));
static bool ensure_gameplay_light_memory(void) {
    /* The current MOSS tables are flat fills. Keep the legacy optional light
     * buffers lazy so they consume no AHB SRAM unless a future effect explicitly
     * requests them. This preserves headroom for audio and frame buffers. */
    if (!gameplay_light_gain_map) {
        uint8_t* raw = (uint8_t*)ahb_malloc(GAME_LIGHT_MAP_PIXELS + 31u);
        if (raw) {
            uintptr_t aligned = ((uintptr_t)raw + 31u) & ~(uintptr_t)31u;
            gameplay_light_gain_map = (uint8_t*)aligned;
        }
    }
    if (!gameplay_light_bg_clut) {
        uint8_t* raw = (uint8_t*)ahb_malloc(256u * sizeof(uint32_t) + 31u);
        if (raw) {
            uintptr_t aligned = ((uintptr_t)raw + 31u) & ~(uintptr_t)31u;
            gameplay_light_bg_clut = (uint32_t*)aligned;
        }
    }
    return gameplay_light_gain_map && gameplay_light_bg_clut;
}

bool hal_init(void) {
    wdog_refresh();
    if (!gfx_pack_init()) {
        printf("duren: gfx pack init failed: %s\n", duren_pak_last_error());
        return false;
    }
#if !DURAK_DIAGNOSTIC_NO_AUDIO
    /* Keep the 24 KiB music ring out of the constrained overlay BSS. */
    if (!ensure_audio_read_ahead_memory()) {
        printf("duren: continuing without music ring\n");
    } else if (!audio_pack_init()) {
        printf("duren: audio pack init failed: %s\n", duren_pak_last_error());
        /* Graphics can still run; keep audio silent. */
    }
#endif

    /* Match the LCD natively. The former 45 FPS 1/1/2-vblank cadence made
     * alternating startup frames visibly flash on the 60 Hz panel. */
    common_emu_state.frame_time_10us = (uint16_t)(100000u / 60u);
    common_emu_frame_loop_reset();
    cpumon_reset();
    cpumon_busy();

#if DURAK_DIAGNOSTIC_NO_AUDIO
    return true;
#endif
    if (audio_music_file || audio_sfx_file || audio_voice_file) {
        // odroid_system_init() has already configured SAI at this sample rate.
        // Reinitialising it here can race or invalidate the peripheral/DMA state
        // on hardware even though it tends to survive in desktop tests.
        audio_dma_start_known_good();
    }
    return true;
}

void hal_shutdown(void) {
#if DURAK_DIAGNOSTIC_NO_AUDIO
    return;
#endif
    audio_stop_playing();
    if (audio_music_file) { fclose(audio_music_file); audio_music_file = NULL; }
    if (audio_sfx_file) { fclose(audio_sfx_file); audio_sfx_file = NULL; }
    if (audio_voice_file) { fclose(audio_voice_file); audio_voice_file = NULL; }
}

void hal_update(void) {
    // Match the lifecycle used by Zelda 3, SMW and Celeste.  The previous
    // Durak loop bypassed both calls after its HAL was simplified, allowing
    // the hardware watchdog to reset the console and skipping Retro-Go's
    // per-frame bookkeeping entirely.
    wdog_refresh();
    (void)common_emu_frame_loop();

    odroid_gamepad_state_t joystick;
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_LAST
    };
    odroid_input_read_gamepad(&joystick);
    const bool raw_time_down = joystick.values[ODROID_INPUT_SELECT] != 0;
    const bool raw_pause_down = joystick.values[ODROID_INPUT_VOLUME] != 0;
    /* GAME is also a Durak pause key for Mario G&W units without rear Y. */
    const bool raw_game_down = joystick.values[ODROID_INPUT_START] != 0;

    /* common_emu_input_loop() handles POWER synchronously: it stops audio,
     * enters STOP2, restores clocks/LCD and returns only after wake.  Keep the
     * original POWER state because the input structure still contains the
     * pre-sleep snapshot after the blocking sleep path returns. */
    const bool resumed_from_power = joystick.values[ODROID_INPUT_POWER] != 0;
    common_emu_input_loop(&joystick, options, &durak_repaint_for_retro_go_menu);

#if !DURAK_DIAGNOSTIC_NO_AUDIO
    if (resumed_from_power) audio_schedule_resume();
#endif

    if (resumed_from_power) {
        /* The structure still contains the pre-sleep sample. Clear every key
         * so POWER or a held direction cannot leak into the first wake frame. */
        memset(&joystick, 0, sizeof(joystick));
        btn_old_state = 0;
        btn_state = 0;
    }

#if !DURAK_DIAGNOSTIC_NO_AUDIO
    audio_resume_update();
    if (!audio_resume_pending) {
        sfx_audio_update();
        audio_dma_watchdog_update();
    }
#endif

    /* TIME alone cycles three cast-shadow directions. The gameplay light
     * profile is fixed and identical in Career, Tournament, Random Battle
     * and Battle Royal. PAUSE/SET + TIME remains Retro-Go's speedup macro. */
    if (raw_time_down && !time_button_was_down && !raw_pause_down) {
        gameplay_shadow_mode = (uint8_t)((gameplay_shadow_mode + 1u) % 3u);
    }
    /* Track the raw key even when Retro-Go consumes PAUSE/SET+TIME, so
     * releasing PAUSE/SET first cannot accidentally cycle the light. */
    time_button_was_down = raw_time_down;

    // Convert the standard Retro-Go input state to Durak's small HAL enum.
    // common_emu_input_loop may consume pause/menu macros before this mapping.
    btn_old_state = btn_state;
    btn_state = 0;
    if (joystick.values[ODROID_INPUT_UP])     btn_state |= B_Up;
    if (joystick.values[ODROID_INPUT_DOWN])   btn_state |= B_Down;
    if (joystick.values[ODROID_INPUT_LEFT])   btn_state |= B_Left;
    if (joystick.values[ODROID_INPUT_RIGHT])  btn_state |= B_Right;
    if (joystick.values[ODROID_INPUT_A])      btn_state |= B_A;
    if (joystick.values[ODROID_INPUT_B])      btn_state |= B_B;

    /* Rear expansion buttons are exposed by this Retro-Go tree as X/Y.
     * GAME is intentionally duplicated as the Durak pause key for Mario
     * Game & Watch units that do not expose the rear Y button. TIME and
     * PAUSE/SET remain owned by Retro-Go system functions. Rear X remains
     * the dedicated TAKE key; rear Y and GAME both map to Durak pause. */
    if (joystick.values[ODROID_INPUT_X])       btn_state |= B_START;
    if (joystick.values[ODROID_INPUT_Y] || raw_game_down) btn_state |= B_SELECT;
    /* POWER is consumed by Retro-Go and is never a Durak gameplay button. */
}

bool hal_is_button_pressed(Button btn) {
    uint32_t bit = map_button_bit(btn);
    return (btn_state & bit) && !(btn_old_state & bit);
}

bool hal_is_button_held(Button btn) {
    uint32_t bit = map_button_bit(btn);
    return (btn_state & bit) != 0;
}

bool hal_is_any_button_pressed(void) {
    uint32_t new_presses = btn_state & ~btn_old_state;
    return (new_presses & ~B_POWER) != 0;
}

// ---------------------------------------------------------------------------
// Відео
// ---------------------------------------------------------------------------
static uint8_t fade_level = 0;
static bool table_sprite_lighting_active = false;
static uint8_t current_table_style = TABLE_STYLE_MOSS;

/*
 * One fixed ambient-table cache shared by Career, Tournament, Random Battle
 * and Battle Royal. The L8 map is built once and fed to DMA2D with a 256-entry
 * CLUT, so Chrom-ART paints the subtly warm/bright felt directly into the
 * active RGB565 framebuffer. Cards and portraits are not relit per pixel.
 * Shadow switching is independent and therefore never rebuilds this map.
 */
#define GAME_LIGHT_GAIN_OFFSET 96
static bool gameplay_light_map_ready __attribute__((unused)) = false;
static bool gameplay_light_clut_valid = false;
static uint16_t gameplay_light_clut_base = 0u;

/* Hardware L8+CLUT DMA2D is not exposed on the GWHB ABI (only RGB565 M2M/R2M).
 * Lit felt falls back to a flat fill; solid fills use dma2d_r2m_rgb565_*. */
static bool gameplay_light_dma_clut_loaded = false;

void hal_set_table_sprite_lighting(bool enabled) {
    table_sprite_lighting_active = enabled;
}

static bool durak_dma2d_fill_rgb565(uint16_t color565) {
    uint16_t* fb = (uint16_t*)lcd_get_active_buffer();
    if (!fb) return false;
    if (dma2d_r2m_rgb565_start((uint32_t)color565, (uint32_t)fb,
                               SCREEN_WIDTH, SCREEN_HEIGHT, 0u) != 0u)
        return false;
    return dma2d_poll(20u) == 0u;
}

static bool durak_dma2d_fill_rect_rgb565(int x, int y, int w, int h,
                                         uint16_t color565) {
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > SCREEN_WIDTH ||
        y + h > SCREEN_HEIGHT) return false;
    uint16_t* fb = (uint16_t*)lcd_get_active_buffer();
    if (!fb) return false;
    const uint16_t output_offset = (uint16_t)(SCREEN_WIDTH - w);
    uint16_t* dst = fb + y * SCREEN_WIDTH + x;
    if (dma2d_r2m_rgb565_start((uint32_t)color565, (uint32_t)dst,
                               (uint16_t)w, (uint16_t)h, output_offset) != 0u)
        return false;
    return dma2d_poll(20u) == 0u;
}

/* Kept for possible future L8+CLUT ABI; currently unused (flat fill fallback). */
static void gameplay_light_build_clut(uint16_t base565) __attribute__((unused));
static void gameplay_light_build_clut(uint16_t base565) {
    (void)base565;
    if (!gameplay_light_bg_clut || gameplay_light_clut_valid) return;
    static const uint8_t base_rgb[TABLE_STYLE_COUNT][3] = {
        {2u, 16u, 6u}, {8u, 28u, 13u}, {18u, 42u, 24u}
    };
    static const uint8_t span_rgb[TABLE_STYLE_COUNT][3] = {
        {60u, 100u, 55u}, {69u, 103u, 65u}, {75u, 105u, 74u}
    };
    const int style = current_table_style < TABLE_STYLE_COUNT
                    ? current_table_style : TABLE_STYLE_MOSS;
    for (int i = 0; i < 256; ++i) {
        const int type = (i >> 6) & 3;
        const int v = i & 63;
        int r = base_rgb[style][0] + (v * span_rgb[style][0]) / 63;
        int g = base_rgb[style][1] + (v * span_rgb[style][1]) / 63;
        int b = base_rgb[style][2] + (v * span_rgb[style][2]) / 63;
        if (type == 1) { r -= 2; g -= 7; b -= 4; }
        else if (type == 2) { r += 7; g += 5; b += 8; }
        else if (type == 3) { r += 9; g += 9; b += 10; }
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        gameplay_light_bg_clut[i] = 0xFF000000u |
                                    ((uint32_t)r << 16) |
                                    ((uint32_t)g << 8) |
                                    (uint32_t)b;
    }
    gameplay_light_clut_base = 0u;
    gameplay_light_clut_valid = true;
    SCB_CleanDCache_by_Addr((void*)gameplay_light_bg_clut,
                            (int32_t)(256u * sizeof(uint32_t)));
}

/* ABI has no L8+CLUT DMA2D path — fall through to flat fill callers. */
static bool durak_dma2d_render_lit_solid(uint16_t base565) {
    (void)base565;
    return false;
}

static inline uint16_t table_light_rgb565(uint16_t c, int pos) { (void)pos; return c; }

static unsigned int table_style_flat_rgb(uint8_t style) {
    static const unsigned int colors[TABLE_STYLE_COUNT] = {
        0x163F1Eu, /* MOSS: mean sampled from Android green felt */
        0x2C6132u, /* LIGHT MOSS */
        0x456F50u  /* SOFT MOSS */
    };
    if (style >= TABLE_STYLE_COUNT) style = TABLE_STYLE_MOSS;
    return colors[style];
}

void hal_clear_screen(unsigned int hex_color) {
    uint8_t r = (hex_color >> 16) & 0xFF;
    uint8_t g = (hex_color >> 8) & 0xFF;
    uint8_t b = hex_color & 0xFF;
    uint16_t color565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));

    if (table_sprite_lighting_active &&
        (hex_color == 0x082A1Du || hex_color == 0x103820u)) {
        const unsigned int rgb = table_style_flat_rgb(current_table_style);
        r = (rgb >> 16) & 0xFF; g = (rgb >> 8) & 0xFF; b = rgb & 0xFF;
        color565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    if (durak_dma2d_fill_rgb565(color565)) return;
    uint16_t* fb = (uint16_t*)lcd_get_active_buffer();
    for (int i = 0; i < GAME_LIGHT_MAP_PIXELS; i++) fb[i] = color565;
}

void hal_set_table_style(uint8_t style) {
    if (style >= TABLE_STYLE_COUNT) style = TABLE_STYLE_MOSS;
    current_table_style = style;
    gameplay_light_clut_valid = false;
    gameplay_light_dma_clut_loaded = false;
}
uint8_t hal_get_table_style(void) { return current_table_style; }

void hal_randomize_table(void) {
    /* New match/game gets one of three shadow directions.  The single fixed
     * ambient table mask stays identical in every mode and every match. */
    gameplay_shadow_mode = (uint8_t)(duren_rand() % 3);
}

void hal_clear_checkerboard(unsigned int hex_color_a, unsigned int hex_color_b) {
    (void)hex_color_a; (void)hex_color_b;
    const unsigned int rgb = table_style_flat_rgb(current_table_style);
    const uint16_t color565 = (uint16_t)((((rgb >> 16) & 0xF8u) << 8) |
                                         (((rgb >> 8) & 0xFCu) << 3) |
                                         ((rgb & 0xF8u) >> 3));
    if (!durak_dma2d_render_lit_solid(color565))
        hal_clear_screen(rgb);
}

void hal_apply_table_lighting(void) {
    /* Hardware gameplay is lit while the felt and sprites are rendered.
     * Keeping this compatibility call as a no-op removes the old extra
     * 76,800-pixel framebuffer pass from Battle Royal. */
}

void hal_apply_table_lighting_rect(int x, int y, int w, int h) {
    /* Sprites and their shadows are already masked per pixel by the blitters.
     * Keeping this as a no-op prevents cards from being darkened twice. */
    (void)x; (void)y; (void)w; (void)h;
}

/* Broad dynamic candle layer centred on the flame. It intentionally covers
 * much of the lower menu scene while retaining a brighter compact core. */
void hal_apply_menu_lighting(void) {
    const uint32_t now = HAL_GetTick();
    const uint32_t phase = now % 1750u;
    const int tri = phase < 875u ? (int)phase : (int)(1750u - phase);
    const int flicker = (int)((now / 97u) % 7u) - 3;
    const int cx = 170 + (int)((now / 223u) % 3u) - 1;
    const int cy = 184 + (int)((now / 307u) % 3u) - 1;
    const int rx = 124 + (tri * 10) / 875 + flicker;
    const int ry = 91 + (tri * 8) / 875 + flicker / 2;
    const int peak = 27 + (tri * 5) / 875;
    uint16_t* fb = (uint16_t*)lcd_get_active_buffer();
    if (!fb) return;

    for (int dy = -ry; dy <= ry; ++dy) {
        const int yy = cy + dy;
        if (yy < 0 || yy >= SCREEN_HEIGHT) continue;
        const int ny = (dy * 256) / ry;
        for (int dx = -rx; dx <= rx; ++dx) {
            const int xx = cx + dx;
            if (xx < 0 || xx >= SCREEN_WIDTH) continue;
            const int nx = (dx * 256) / rx;
            const int d2 = nx * nx + ny * ny;
            if (d2 >= 65536) continue;
            const int remain = 65536 - d2;
            const int curved = (int)(((int64_t)remain * remain) >> 16);
            const int core = d2 < 15000 ? (15000 - d2) / 1800 : 0;
            int gain = (curved * peak) / 65536 + core;
            if (gain <= 0) continue;
            uint16_t c = fb[yy * SCREEN_WIDTH + xx];
            int r = (c >> 11) & 31;
            int g = (c >> 5) & 63;
            int b = c & 31;
            /* RGB565 channel units differ. This ratio produces warm
             * orange/yellow candle light instead of a greenish halo. */
            r += gain;
            g += (gain * 5) / 4;
            b += gain / 4;
            if (r > 31) r = 31;
            if (g > 63) g = 63;
            if (b > 31) b = 31;
            fb[yy * SCREEN_WIDTH + xx] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

void hal_apply_darkened_rect(int x, int y, int w, int h, unsigned int percent) {
    if (w <= 0 || h <= 0) return;
    if (percent > 100u) percent = 100u;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH;
    int y1 = y + h; if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;

    /* Fixed-point keep factor: no integer divisions inside the pixel loop. */
    const uint32_t keep_q8 = ((100u - percent) * 256u + 50u) / 100u;
    uint16_t* fb = lcd_get_active_buffer();
    for (int yy = y0; yy < y1; ++yy) {
        int row = yy * SCREEN_WIDTH;
        for (int xx = x0; xx < x1; ++xx) {
            uint16_t c = fb[row + xx];
            uint32_t r = (((c >> 11) & 0x1Fu) * keep_q8) >> 8;
            uint32_t g = (((c >> 5) & 0x3Fu) * keep_q8) >> 8;
            uint32_t b = ((c & 0x1Fu) * keep_q8) >> 8;
            fb[row + xx] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

void hal_apply_grayscale_darkened(void) {
    uint16_t* fb = lcd_get_active_buffer();
    const int count = SCREEN_WIDTH * SCREEN_HEIGHT;

    for (int i = 0; i < count; i++) {
        uint16_t c = fb[i];
        uint8_t r5 = (uint8_t)((c >> 11) & 0x1Fu);
        uint8_t g6 = (uint8_t)((c >> 5) & 0x3Fu);
        uint8_t b5 = (uint8_t)(c & 0x1Fu);

        uint16_t r8 = (uint16_t)((r5 << 3) | (r5 >> 2));
        uint16_t g8 = (uint16_t)((g6 << 2) | (g6 >> 4));
        uint16_t b8 = (uint16_t)((b5 << 3) | (b5 >> 2));
        uint16_t y = (uint16_t)((77u * r8 + 150u * g8 + 29u * b8) >> 8);
        y = (uint16_t)((y * 3u) >> 2); // 25% темніше

        uint16_t gy = (uint16_t)(((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3));
        fb[i] = gy;
    }
}

void hal_fill_rect(int x, int y, int w, int h, unsigned int hex_color) {
    if (w <= 0 || h <= 0) return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH;
    if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;

    uint8_t r = (uint8_t)((hex_color >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((hex_color >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(hex_color & 0xFFu);
    uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    uint16_t* fb = lcd_get_active_buffer();

    /* Large flat UI panels are ideal DMA2D R2M jobs. Small borders stay on
     * the CPU because DMA setup would cost more than the fill itself. */
    const int clipped_w = x1 - x0;
    const int clipped_h = y1 - y0;
    if (!table_sprite_lighting_active && clipped_w * clipped_h >= 512 &&
        durak_dma2d_fill_rect_rgb565(x0, y0, clipped_w, clipped_h, c)) return;

    for (int yy = y0; yy < y1; yy++) {
        int row = yy * SCREEN_WIDTH;
        for (int xx = x0; xx < x1; xx++) {
            int pos = row + xx;
            fb[pos] = table_light_rgb565(c, pos);
        }
    }
}

void hal_set_fade_level(uint8_t level) { fade_level = level; }

void hal_present(void) {
    if (fade_level > 0) {
        uint16_t* fb = lcd_get_active_buffer();
        const uint32_t keep = 255u - (uint32_t)fade_level;
        const int count = SCREEN_WIDTH * SCREEN_HEIGHT;
        for (int i = 0; i < count; i++) {
            uint16_t c = fb[i];
            uint32_t r = ((c >> 11) & 0x1Fu) * keep / 255u;
            uint32_t g = ((c >> 5) & 0x3Fu) * keep / 255u;
            uint32_t b = (c & 0x1Fu) * keep / 255u;
            fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    // Draw the standard Retro-Go volume/brightness indicator after the game
    // frame, so PAUSE/SET shortcuts have visible feedback.
    common_ingame_overlay();

    /* Finish any previous swap before reusing its framebuffer, then queue this
     * frame without waiting for a second vblank. hal_delay() owns pacing. */
    while (lcd_is_swap_pending()) cpumon_sleep();
    lcd_swap();
}
void hal_delay(unsigned int ms) {
    (void)ms;
    /* Exactly one display synchronization per frame. hal_present() queues the
     * asynchronous swap; waiting for that swap here yields native 60 Hz with
     * no fractional software limiter and no second vblank wait. */
    while (lcd_is_swap_pending()) cpumon_sleep();
}
uint32_t hal_get_ticks_ms(void) { return HAL_GetTick(); }

HalTexture* hal_load_texture(const char* filename) {
    const int index = gfx_find_entry(filename);
    if (index < 0) return NULL;
    HalTexture* tex = (HalTexture*)malloc(sizeof(HalTexture));
    if (!tex) return NULL;
    memset(tex, 0, sizeof(*tex));
    tex->entry_index = index;
    tex->cache_group = gfx_group_for_name(filename);
    tex->width = gfx_entries[index].width;
    tex->height = gfx_entries[index].height;
    tex->dim_percent = 0xFFu;
    return tex;
}

void hal_destroy_texture(HalTexture* texture) {
    if (!texture) return;
    /* Pack pixels belong to shared scene caches. Only the wrapper and optional
     * dim palette are owned by this texture. */
    free(texture->dim_palette);
    free(texture);
}

void hal_evict_texture(HalTexture* texture) {
    if (!texture || texture->cache_group < 0 ||
        texture->cache_group >= GFX_GROUP_COUNT) return;
    GfxCache* cache = &gfx_caches[texture->cache_group];
    if (cache->entry_index == texture->entry_index)
        cache->entry_index = -1;
    texture->indices = NULL;
    texture->palette = NULL;
}

static const uint16_t* hal_texture_dim_palette(HalTexture* texture,
                                                unsigned int percent) {
    if (!hal_texture_ensure(texture)) return NULL;
    if (percent > 100u) percent = 100u;
    if (!texture->dim_palette) {
        texture->dim_palette = (uint16_t*)malloc(256u * sizeof(uint16_t));
        if (!texture->dim_palette) return texture->palette;
        texture->dim_percent = 0xFFu;
    }
    if (texture->dim_percent != (uint8_t)percent) {
        const uint32_t keep_q8 = ((100u - percent) * 256u + 50u) / 100u;
        for (int i = 0; i < 256; ++i) {
            const uint16_t c = texture->palette[i];
            if (c == 0x0000u) {
                texture->dim_palette[i] = 0x0000u;
                continue;
            }
            const uint32_t r = (((c >> 11) & 0x1Fu) * keep_q8) >> 8;
            const uint32_t g = (((c >> 5) & 0x3Fu) * keep_q8) >> 8;
            const uint32_t b = ((c & 0x1Fu) * keep_q8) >> 8;
            texture->dim_palette[i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
        texture->dim_percent = (uint8_t)percent;
    }
    return texture->dim_palette;
}

static void hal_draw_sprite_palette(HalTexture* texture,
                                    const uint16_t* palette,
                                    int sx, int sy, int sw, int sh,
                                    int dx, int dy) {
    if (!hal_texture_ensure(texture) || !palette) return;
    if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0) return;
    if (sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width) sw = texture->width - sx;
    if (sy + sh > texture->height) sh = texture->height - sy;
    if (sw <= 0 || sh <= 0) return;

    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    for (int y = 0; y < sh; y++) {
        const int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        const int src_row = (sy + y) * texture->width + sx;
        const int dst_row = fy * SCREEN_WIDTH;
        for (int x = 0; x < sw; x++) {
            const int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            const uint8_t palette_index = idx[src_row + x];
            /* Transparency belongs to the original asset palette, not to the
             * transformed/dimmed palette. Very dark opaque portrait pixels can
             * quantize to RGB565 black after dimming; they must still be drawn
             * as black instead of becoming accidental transparent holes. */
            if (texture->palette[palette_index] == 0x0000u) continue;
            const uint16_t color = palette[palette_index];
            fb[dst_row + fx] = table_light_rgb565(color, dst_row + fx);
        }
    }
}


/* Fan-card occlusion blitter.
 *
 * Rounded corners are correct for an isolated card, but in a tightly
 * overlapped hand those transparent left-corner pixels expose the previous
 * card and its shadow. Extend the first opaque pixel of each source row into
 * only that transparent left-edge run. The outside corners of the first card
 * remain untouched because callers use the ordinary blitter for index 0.
 */
static void hal_draw_sprite_palette_occluded_left(HalTexture* texture,
                                                   const uint16_t* palette,
                                                   int sx, int sy,
                                                   int visible_w, int sh,
                                                   int dx, int dy,
                                                   int full_sprite_width) {
    if (!hal_texture_ensure(texture) || !palette) return;
    if (visible_w <= 0 || sh <= 0 || full_sprite_width <= 0 || sx < 0 || sy < 0) return;
    if (sx >= texture->width || sy >= texture->height) return;
    if (sx + full_sprite_width > texture->width)
        full_sprite_width = texture->width - sx;
    if (visible_w > full_sprite_width) visible_w = full_sprite_width;
    if (sy + sh > texture->height) sh = texture->height - sy;
    if (visible_w <= 0 || sh <= 0) return;

    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;

    for (int y = 0; y < sh; ++y) {
        const int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        const int src_row = (sy + y) * texture->width + sx;
        const int dst_row = fy * SCREEN_WIDTH;

        int first_opaque = 0;
        while (first_opaque < full_sprite_width) {
            const uint8_t pi = idx[src_row + first_opaque];
            if (texture->palette[pi] != 0x0000u) break;
            ++first_opaque;
        }
        const bool can_extend = first_opaque > 0 && first_opaque < full_sprite_width;
        const uint16_t edge_color = can_extend
            ? palette[idx[src_row + first_opaque]]
            : 0u;

        for (int x = 0; x < visible_w; ++x) {
            const int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;

            const uint8_t pi = idx[src_row + x];
            uint16_t color;
            if (texture->palette[pi] == 0x0000u) {
                if (!can_extend || x >= first_opaque) continue;
                color = edge_color;
            } else {
                color = palette[pi];
            }
            fb[dst_row + fx] = table_light_rgb565(color, dst_row + fx);
        }
    }
}

void hal_draw_texture(HalTexture* texture, int x, int y) {
    if (!hal_texture_ensure(texture)) return;
    hal_draw_sprite_palette(texture, texture->palette,
                            0, 0, texture->width, texture->height, x, y);
}

void hal_draw_texture_dimmed(HalTexture* texture, int x, int y,
                             unsigned int percent) {
    if (!hal_texture_ensure(texture)) return;
    const uint16_t* pal = hal_texture_dim_palette(texture, percent);
    hal_draw_sprite_palette(texture, pal,
                            0, 0, texture->width, texture->height, x, y);
}

void hal_draw_sprite(HalTexture* texture, int sx, int sy, int sw, int sh,
                     int dx, int dy) {
    if (!hal_texture_ensure(texture)) return;
    hal_draw_sprite_palette(texture, texture->palette, sx, sy, sw, sh, dx, dy);
}

void hal_draw_sprite_dimmed(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, unsigned int percent) {
    if (!hal_texture_ensure(texture)) return;
    const uint16_t* pal = hal_texture_dim_palette(texture, percent);
    hal_draw_sprite_palette(texture, pal, sx, sy, sw, sh, dx, dy);
}

void hal_draw_sprite_tinted(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, unsigned int hex_color,
                            unsigned int alpha_percent) {
    if (!hal_texture_ensure(texture) || alpha_percent == 0u) return;
    if (alpha_percent > 100u) alpha_percent = 100u;
    if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0) return;
    if (sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width) sw = texture->width - sx;
    if (sy + sh > texture->height) sh = texture->height - sy;
    if (sw <= 0 || sh <= 0) return;

    const uint16_t tint565 = (uint16_t)((((hex_color >> 16) & 0xFFu) >> 3) << 11) |
                             (uint16_t)((((hex_color >> 8) & 0xFFu) >> 2) << 5) |
                             (uint16_t)((hex_color & 0xFFu) >> 3);
    const unsigned int inv = 100u - alpha_percent;
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;

    for (int y = 0; y < sh; ++y) {
        const int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        const int src_row = (sy + y) * texture->width + sx;
        const int dst_row = fy * SCREEN_WIDTH;
        for (int x = 0; x < sw; ++x) {
            const int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            const uint8_t pi = idx[src_row + x];
            if (texture->palette[pi] == 0x0000u) continue;

            const int pos = dst_row + fx;
            const uint16_t dst = fb[pos];
            const uint16_t over = table_light_rgb565(tint565, pos);
            const unsigned int dr = (dst >> 11) & 0x1Fu;
            const unsigned int dg = (dst >> 5) & 0x3Fu;
            const unsigned int db = dst & 0x1Fu;
            const unsigned int tr = (over >> 11) & 0x1Fu;
            const unsigned int tg = (over >> 5) & 0x3Fu;
            const unsigned int tb = over & 0x1Fu;
            const unsigned int r = (dr * inv + tr * alpha_percent + 50u) / 100u;
            const unsigned int g = (dg * inv + tg * alpha_percent + 50u) / 100u;
            const unsigned int b = (db * inv + tb * alpha_percent + 50u) / 100u;
            fb[pos] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

void hal_draw_sprite_occluded_left(HalTexture* texture,
                                    int sx, int sy, int visible_w, int sh,
                                    int dx, int dy, int full_sprite_width,
                                    unsigned int dim_percent) {
    if (!hal_texture_ensure(texture)) return;
    const uint16_t* pal = texture->palette;
    if (dim_percent > 0u)
        pal = hal_texture_dim_palette(texture, dim_percent);
    hal_draw_sprite_palette_occluded_left(texture, pal, sx, sy,
                                           visible_w, sh, dx, dy,
                                           full_sprite_width);
}

void hal_draw_sprite_flipped_x(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy) {
    if (!hal_texture_ensure(texture) || sw <= 0 || sh <= 0 || sx < 0 || sy < 0) return;
    if (sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width) sw = texture->width - sx;
    if (sy + sh > texture->height) sh = texture->height - sy;
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;
    for (int y = 0; y < sh; ++y) {
        int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_row = (sy + y) * texture->width;
        int dst_row = fy * SCREEN_WIDTH;
        for (int x = 0; x < sw; ++x) {
            int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            int src_x = sx + (sw - 1 - x);
            uint16_t color = pal[idx[src_row + src_x]];
            if (color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(color, dst_row + fx);
        }
    }
}

void hal_draw_sprite_scaled(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, int dw, int dh) {
    if (!hal_texture_ensure(texture) || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    for (int y = 0; y < dh; y++) {
        int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_y = sy + (y * sh) / dh;
        int src_row = src_y * texture->width;
        int dst_row = fy * SCREEN_WIDTH;

        for (int x = 0; x < dw; x++) {
            int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            int src_x = sx + (x * sw) / dw;
            uint16_t color = pal[idx[src_row + src_x]];
            if (color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(color, dst_row + fx);
        }
    }
}

void hal_draw_sprite_scaled_tinted(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int hex_color) {
    if (!hal_texture_ensure(texture) || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;

    const uint8_t r = (hex_color >> 16) & 0xFF;
    const uint8_t g = (hex_color >> 8) & 0xFF;
    const uint8_t b = hex_color & 0xFF;
    const uint16_t ink565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    for (int y = 0; y < dh; ++y) {
        const int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        const int src_y = sy + (y * sh) / dh;
        const int src_row = src_y * texture->width;
        const int dst_row = fy * SCREEN_WIDTH;
        for (int x = 0; x < dw; ++x) {
            const int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            const int src_x = sx + (x * sw) / dw;
            if (pal[idx[src_row + src_x]] == 0x0000) continue;
            fb[dst_row + fx] = ink565;
        }
    }
}

void hal_draw_sprite_scaled_flipped_x(HalTexture* texture, int sx, int sy, int sw, int sh,
                                      int dx, int dy, int dw, int dh) {
    if (!hal_texture_ensure(texture) || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    for (int y = 0; y < dh; ++y) {
        int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_y = sy + (y * sh) / dh;
        int src_row = src_y * texture->width;
        int dst_row = fy * SCREEN_WIDTH;
        for (int x = 0; x < dw; ++x) {
            int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            int src_rel_x = sw - 1 - (x * sw) / dw;
            if (src_rel_x < 0) src_rel_x = 0;
            int src_x = sx + src_rel_x;
            uint16_t color = pal[idx[src_row + src_x]];
            if (color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(color, dst_row + fx);
        }
    }
}

void hal_draw_sprite_scaled_dimmed(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int percent) {
    if (!hal_texture_ensure(texture) || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = hal_texture_dim_palette(texture, percent);

    for (int y = 0; y < dh; ++y) {
        int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_y = sy + (y * sh) / dh;
        int src_row = src_y * texture->width;
        int dst_row = fy * SCREEN_WIDTH;
        for (int x = 0; x < dw; ++x) {
            int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            int src_x = sx + (x * sw) / dw;
            uint16_t color = pal[idx[src_row + src_x]];
            if (color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(color, dst_row + fx);
        }
    }
}

void hal_draw_sprite_scaled_rot90(HalTexture* texture, int sx, int sy, int sw, int sh,
                                  int dx, int dy, int dw, int dh) {
    if (!hal_texture_ensure(texture) || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > texture->width || sy + sh > texture->height) return;
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    /* Output bounding box is dh x dw. */
    for (int oy = 0; oy < dw; ++oy) {
        int fy = dy + oy;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_x = sx + (oy * sw) / dw;
        if (src_x >= sx + sw) src_x = sx + sw - 1;
        int dst_row = fy * SCREEN_WIDTH;
        for (int ox = 0; ox < dh; ++ox) {
            int fx = dx + ox;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            int src_y_rel = sh - 1 - (ox * sh) / dh;
            if (src_y_rel < 0) src_y_rel = 0;
            uint16_t color = pal[idx[(sy + src_y_rel) * texture->width + src_x]];
            if (color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(color, dst_row + fx);
        }
    }
}

void hal_draw_sprite_scaled_rot90_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                         int dx, int dy, int dw, int dh,
                                         unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!hal_texture_ensure(texture) || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > texture->width || sy + sh > texture->height) return;

    uint8_t r = (hex_color >> 16) & 0xFF;
    uint8_t g = (hex_color >> 8) & 0xFF;
    uint8_t b = hex_color & 0xFF;
    uint16_t shadow565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    for (int oy = 0; oy < dw; ++oy) {
        int fy = dy + oy;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_x = sx + (oy * sw) / dw;
        if (src_x >= sx + sw) src_x = sx + sw - 1;
        int dst_row = fy * SCREEN_WIDTH;
        for (int ox = 0; ox < dh; ++ox) {
            int fx = dx + ox;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            int src_y_rel = sh - 1 - (ox * sh) / dh;
            if (src_y_rel < 0) src_y_rel = 0;
            uint16_t src_color = pal[idx[(sy + src_y_rel) * texture->width + src_x]];
            if (src_color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(shadow565, dst_row + fx);
        }
    }
}

void hal_draw_sprite_scaled_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!hal_texture_ensure(texture) || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;

    uint8_t r = (hex_color >> 16) & 0xFF;
    uint8_t g = (hex_color >> 8) & 0xFF;
    uint8_t b = hex_color & 0xFF;
    uint16_t shadow565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;
    for (int y = 0; y < dh; ++y) {
        int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_y = sy + (y * sh) / dh;
        int src_row = src_y * texture->width;
        int dst_row = fy * SCREEN_WIDTH;
        for (int x = 0; x < dw; ++x) {
            int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            int src_x = sx + (x * sw) / dw;
            uint16_t src_color = pal[idx[src_row + src_x]];
            if (src_color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(shadow565, dst_row + fx);
        }
    }
}


void hal_draw_texture_shadow(HalTexture* texture, int x, int y, unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!hal_texture_ensure(texture)) return;
    hal_draw_sprite_shadow(texture, 0, 0, texture->width, texture->height, x, y, hex_color);
}

void hal_draw_sprite_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!hal_texture_ensure(texture)) return;
    if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0) return;
    if (sx >= texture->width || sy >= texture->height) return;
    if (sx + sw > texture->width) sw = texture->width - sx;
    if (sy + sh > texture->height) sh = texture->height - sy;
    if (sw <= 0 || sh <= 0) return;

    uint8_t r = (hex_color >> 16) & 0xFF;
    uint8_t g = (hex_color >> 8) & 0xFF;
    uint8_t b = hex_color & 0xFF;
    uint16_t shadow565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    for (int y = 0; y < sh; y++) {
        int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_row = (sy + y) * texture->width;
        int dst_row = fy * SCREEN_WIDTH;

        for (int x = 0; x < sw; x++) {
            int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            uint16_t src_color = pal[idx[src_row + sx + x]];
            if (src_color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(shadow565, dst_row + fx);
        }
    }
}

static void draw_sprite_quarter_turn_core(HalTexture* texture,
                                           int sx, int sy, int sw, int sh,
                                           int dx, int dy, bool clockwise,
                                           bool shadow, uint16_t shadow565) {
    if (!hal_texture_ensure(texture)) return;
    if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    /* Preserve the center-based SDL placement. A 40x60 card becomes 60x40
     * with the same bounds for +90 and -90, while integer mapping avoids the
     * generic trigonometric path on Game & Watch. */
    const int out_x = dx + (sw / 2) - ((sh - 1) / 2);
    const int out_y = dy + (sh / 2) - (sw / 2);
    for (int oy = 0; oy < sw; ++oy) {
        const int fy = out_y + oy;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        const int dst_row = fy * SCREEN_WIDTH;
        for (int ox = 0; ox < sh; ++ox) {
            const int fx = out_x + ox;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            const int srcx = clockwise ? oy : (sw - 1 - oy);
            const int srcy = clockwise ? (sh - 1 - ox) : ox;
            const uint16_t color =
                pal[idx[(sy + srcy) * texture->width + (sx + srcx)]];
            if (color == 0x0000) continue;
            const uint16_t outc = shadow ? shadow565 : color;
            fb[dst_row + fx] = table_light_rgb565(outc, dst_row + fx);
        }
    }
}

void hal_draw_sprite_rot90_ccw(HalTexture* texture, int sx, int sy, int sw, int sh,
                                int out_x, int out_y) {
    if (!hal_texture_ensure(texture) || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > texture->width || sy + sh > texture->height) return;
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;
    for (int oy = 0; oy < sw; ++oy) {
        const int fy = out_y + oy;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        const int dst_row = fy * SCREEN_WIDTH;
        for (int ox = 0; ox < sh; ++ox) {
            const int fx = out_x + ox;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            const int srcx = sw - 1 - oy;
            const int srcy = ox;
            const uint16_t color = pal[idx[(sy + srcy) * texture->width + (sx + srcx)]];
            if (color == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(color, dst_row + fx);
        }
    }
}

void hal_draw_sprite_rot90_ccw_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                       int out_x, int out_y, unsigned int hex_color) {
    if (!graphics_shadows_enabled || !hal_texture_ensure(texture) || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > texture->width || sy + sh > texture->height) return;
    const uint8_t r = (uint8_t)((hex_color >> 16) & 0xFFu);
    const uint8_t g = (uint8_t)((hex_color >> 8) & 0xFFu);
    const uint8_t b = (uint8_t)(hex_color & 0xFFu);
    const uint16_t shadow565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;
    for (int oy = 0; oy < sw; ++oy) {
        const int fy = out_y + oy;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        const int dst_row = fy * SCREEN_WIDTH;
        for (int ox = 0; ox < sh; ++ox) {
            const int fx = out_x + ox;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;
            const int srcx = sw - 1 - oy;
            const int srcy = ox;
            const uint16_t source = pal[idx[(sy + srcy) * texture->width + (sx + srcx)]];
            if (source == 0x0000) continue;
            fb[dst_row + fx] = table_light_rgb565(shadow565, dst_row + fx);
        }
    }
}

void hal_draw_sprite_rotated_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                    int dx, int dy, double angle, unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!hal_texture_ensure(texture)) return;
    if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;

    uint8_t r = (hex_color >> 16) & 0xFF;
    uint8_t g = (hex_color >> 8) & 0xFF;
    uint8_t b = hex_color & 0xFF;
    uint16_t shadow565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    if (angle > 89.5 && angle < 90.5) {
        draw_sprite_quarter_turn_core(texture, sx, sy, sw, sh, dx, dy,
                                      true, true, shadow565);
        return;
    }
    if (angle < -89.5 && angle > -90.5) {
        draw_sprite_quarter_turn_core(texture, sx, sy, sw, sh, dx, dy,
                                      false, true, shadow565);
        return;
    }

    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    float rad = (float)(-angle * (M_PI / 180.0));
    float cs = cosf(rad), sn = sinf(rad);
    int cx = sw / 2, cy = sh / 2;
    int half = (int)(sqrtf((float)(sw * sw + sh * sh)) / 2.0f) + 1;

    for (int y = -half; y <= half; y++) {
        int fy = dy + cy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;

        for (int x = -half; x <= half; x++) {
            int fx = dx + cx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;

            int srcx = (int)roundf(x * cs - y * sn) + cx;
            int srcy = (int)roundf(x * sn + y * cs) + cy;
            if (srcx < 0 || srcx >= sw || srcy < 0 || srcy >= sh) continue;

            uint16_t src_color = pal[idx[(sy + srcy) * texture->width + (sx + srcx)]];
            if (src_color == 0x0000) continue;
            int pos = fy * SCREEN_WIDTH + fx;
            fb[pos] = table_light_rgb565(shadow565, pos);
        }
    }
}

// Обертання методом найближчого сусіда навколо центру спрайту.
void hal_draw_sprite_rotated(HalTexture* texture, int sx, int sy, int sw, int sh,
                              int dx, int dy, double angle) {
    if (!hal_texture_ensure(texture)) return;
    if (sw <= 0 || sh <= 0 || sx < 0 || sy < 0) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;
    if (angle > 89.5 && angle < 90.5) {
        draw_sprite_quarter_turn_core(texture, sx, sy, sw, sh, dx, dy,
                                      true, false, 0);
        return;
    }
    if (angle < -89.5 && angle > -90.5) {
        draw_sprite_quarter_turn_core(texture, sx, sy, sw, sh, dx, dy,
                                      false, false, 0);
        return;
    }
    uint16_t* fb = lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    float rad = (float)(-angle * (M_PI / 180.0));
    float cs = cosf(rad), sn = sinf(rad);
    int cx = sw / 2, cy = sh / 2;

    int half = (int)(sqrtf((float)(sw * sw + sh * sh)) / 2.0f) + 1;

    for (int y = -half; y <= half; y++) {
        int fy = dy + cy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;

        for (int x = -half; x <= half; x++) {
            int fx = dx + cx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;

            int srcx = (int)roundf(x * cs - y * sn) + cx;
            int srcy = (int)roundf(x * sn + y * cs) + cy;
            if (srcx < 0 || srcx >= sw || srcy < 0 || srcy >= sh) continue;

            uint16_t color = pal[idx[(sy + srcy) * texture->width + (sx + srcx)]];
            if (color == 0x0000) continue;

            int pos = fy * SCREEN_WIDTH + fx;
            fb[pos] = table_light_rgb565(color, pos);
        }
    }
}
