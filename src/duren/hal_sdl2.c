#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "hal.h"
#include "rng.h"
#include "duren_pak.h"


struct HalTexture {
    SDL_Texture* sdl_tex;
    SDL_Surface* source_rgba; /* kept for exact fan-corner occlusion sampling */
    int w, h;
};

static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* checker_texture = NULL;
static SDL_Texture* grayscale_texture = NULL;
static Uint32* grayscale_pixels = NULL;
static unsigned int checker_last_a = 0xFFFFFFFFu;
static unsigned int checker_last_b = 0xFFFFFFFFu;
static bool btn_state[BTN_COUNT] = {false};
static bool btn_old_state[BTN_COUNT] = {false};
static bool any_key_pressed_this_frame = false;
static uint8_t fade_level = 0;
static bool table_sprite_lighting_active = false;
static uint8_t current_table_style = TABLE_STYLE_MOSS;
/* Shadow direction only: 0=down-right, 1=down-left, 2=straight down. */
static uint8_t gameplay_shadow_mode = 0;

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

static Uint8 table_light_keep_for_rect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    return 255u;
}

static unsigned int table_light_mod_hex(unsigned int hex_color, int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    return hex_color;
}

// =========================================================
// External DUREN audio pack.
// Every record is unsigned PCM8 / 8 kHz mono. Desktop output is S16 at the
// obtained device frequency. One source format eliminates byte/sample ambiguity.
// =========================================================
#define AUDIO_FMT_U8       1u
#define AUDIO_RETIRED_MAX  24u
#define AUDIO_FADE_MS      30u

typedef struct {
    uint8_t* data;
    uint32_t bytes;
    uint32_t frames;
    uint32_t frame_pos;
    uint32_t phase_q16;
    uint8_t format;
    uint8_t gain_percent;
} AudioChannel;

static SDL_AudioDeviceID sfx_device = 0;
static int audio_output_freq = 48000;
static uint32_t audio_source_step_q16 = (8000u << 16) / 48000u;
static uint32_t audio_fade_step_q15 = 1u;

static AudioChannel music_ch = {0};
static AudioChannel music_next_ch = {0};
static AudioChannel sfx_ch = {0};
static AudioChannel sfx_queued_ch = {0};
static AudioChannel voice_ch = {0};
static AudioChannel voice_queued_ch = {0};

static bool music_paused = false;
static bool music_gameplay = false;
static bool music_loop_current = false;
static char music_name[32] = "";
static int gameplay_track = 0;
static int music_next_track = 0;
static uint32_t music_fade_q15 = 0u;
static uint8_t music_volume_step = 5u;
static bool music_menu_intro_full = false;
static uint8_t effects_volume_step = 5u;
static bool graphics_shadows_enabled = true;

static uint8_t* audio_retired[AUDIO_RETIRED_MAX];
static uint8_t audio_retired_count = 0u;

static void audio_retire_locked(uint8_t* ptr) {
    if (!ptr) return;
    if (audio_retired_count < AUDIO_RETIRED_MAX)
        audio_retired[audio_retired_count++] = ptr;
    /* Never call free() from the realtime callback.  An impossible overflow
     * is leaked rather than interrupting audio. */
}

static void audio_channel_reset(AudioChannel* ch) {
    if (!ch) return;
    ch->data = NULL;
    ch->bytes = 0u;
    ch->frames = 0u;
    ch->frame_pos = 0u;
    ch->phase_q16 = 0u;
    ch->format = 0u;
    ch->gain_percent = 0u;
}

static bool audio_channel_active(const AudioChannel* ch) {
    return ch && ch->data && ch->frames && ch->frame_pos < ch->frames;
}

static uint16_t audio_u16le(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t audio_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static FILE* open_audio_pack(uint32_t* out_base) {
    FILE* f = NULL;
    uint32_t base = 0;
    if (!duren_pak_open_member(DUREN_PAK_MEMBER_AUDIO, &f, &base))
        return NULL;
    if (out_base) *out_base = base;
    return f;
}

static bool load_audio_record(const char* wanted, AudioChannel* out,
                              uint8_t gain_percent) {
    if (!wanted || !out) return false;
    uint32_t pack_base = 0;
    FILE* f = open_audio_pack(&pack_base);
    if (!f) return false;
    uint8_t header[16];
    bool ok = false;
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) goto done;
    if (memcmp(header, "DAUD", 4) != 0 || audio_u16le(header + 4) != 2u ||
        audio_u32le(header + 12) != 56u) goto done;
    const uint16_t count = audio_u16le(header + 6);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t raw[56];
        if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) goto done;
        char name[32]; memcpy(name, raw, 31); name[31] = '\0';
        if (strcmp(name, wanted) != 0) continue;
        const uint32_t offset = pack_base + audio_u32le(raw + 32);
        const uint32_t length = audio_u32le(raw + 36);
        const uint16_t rate = audio_u16le(raw + 40);
        const uint8_t channels = raw[42];
        const uint8_t format = (uint8_t)audio_u32le(raw + 48);
        if (!length || rate != 8000u || channels != 1u ||
            format != AUDIO_FMT_U8) goto done;
        const uint32_t bytes_per_frame = 1u;
        uint8_t* data = (uint8_t*)malloc(length);
        if (!data) goto done;
        if (fseek(f, (long)offset, SEEK_SET) != 0 ||
            fread(data, 1, length, f) != length) {
            free(data); goto done;
        }
        audio_channel_reset(out);
        out->data = data;
        out->bytes = length;
        out->frames = length / bytes_per_frame;
        out->format = format;
        out->gain_percent = gain_percent;
        ok = true;
        goto done;
    }
done:
    fclose(f);
    return ok;
}

static int32_t audio_source_sample(const AudioChannel* ch, uint32_t frame) {
    if (!ch || !ch->data || frame >= ch->frames) return 0;
    return ((int32_t)ch->data[frame] - 128) << 8;
}

static int32_t audio_channel_linear(const AudioChannel* ch) {
    if (!audio_channel_active(ch)) return 0;
    const int32_t a = audio_source_sample(ch, ch->frame_pos);
    const int32_t b = (ch->frame_pos + 1u < ch->frames)
                    ? audio_source_sample(ch, ch->frame_pos + 1u) : a;
    const uint32_t frac = ch->phase_q16;
    return (int32_t)(((int64_t)a * (int64_t)(65536u - frac) +
                      (int64_t)b * (int64_t)frac) >> 16);
}

static void audio_channel_advance(AudioChannel* ch) {
    if (!ch || !ch->data) return;
    ch->phase_q16 += audio_source_step_q16;
    while (ch->phase_q16 >= 65536u) {
        ch->phase_q16 -= 65536u;
        if (ch->frame_pos < ch->frames) ++ch->frame_pos;
    }
}

static int32_t apply_channel_gain(int32_t sample, uint8_t local_gain,
                                  uint8_t master_step) {
    const int32_t master_percent = (int32_t)master_step * 20;
    return (int32_t)(((int64_t)sample * local_gain * master_percent) / 10000);
}

static int32_t soft_limit_s16(int32_t sample) {
    int32_t sign = sample < 0 ? -1 : 1;
    int32_t a = sample < 0 ? -sample : sample;
    if (a > 24576) a = 24576 + (a - 24576) / 3;
    if (a > 32767) a = 32767;
    return sign * a;
}

static void promote_queued_locked(AudioChannel* active, AudioChannel* queued) {
    if (audio_channel_active(active) || !queued->data) return;
    audio_retire_locked(active->data);
    *active = *queued;
    audio_channel_reset(queued);
}

static void music_finish_or_switch_locked(void) {
    if (audio_channel_active(&music_ch)) return;
    if (music_gameplay && music_next_ch.data) {
        audio_retire_locked(music_ch.data);
        music_ch = music_next_ch;
        audio_channel_reset(&music_next_ch);
        gameplay_track = music_next_track;
        music_next_track = 0;
        music_fade_q15 = 0u;
    } else if (music_loop_current && music_ch.data) {
        music_ch.frame_pos = 0u;
        music_ch.phase_q16 = 0u;
        music_fade_q15 = 0u;
    }
}

static void sfx_audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    Sint16* buffer = (Sint16*)stream;
    const int count = len / (int)sizeof(Sint16);
    for (int i = 0; i < count; ++i) {
        music_finish_or_switch_locked();
        promote_queued_locked(&sfx_ch, &sfx_queued_ch);
        promote_queued_locked(&voice_ch, &voice_queued_ch);

        int32_t mixed = 0;
        const bool voice_now = audio_channel_active(&voice_ch);
        if (!music_paused && audio_channel_active(&music_ch)) {
            int32_t sample = audio_channel_linear(&music_ch);
            uint8_t gain = voice_now ? (uint8_t)((music_ch.gain_percent * 3u) / 4u)
                                     : music_ch.gain_percent;
            if (music_fade_q15 < 32768u) {
                sample = (int32_t)(((int64_t)sample * music_fade_q15) >> 15);
                uint32_t next = music_fade_q15 + audio_fade_step_q15;
                music_fade_q15 = next > 32768u ? 32768u : next;
            }
            mixed += apply_channel_gain(sample, gain, music_volume_step);
            audio_channel_advance(&music_ch);
        }
        if (audio_channel_active(&sfx_ch)) {
            mixed += apply_channel_gain(audio_channel_linear(&sfx_ch),
                                        sfx_ch.gain_percent, effects_volume_step);
            audio_channel_advance(&sfx_ch);
        }
        if (audio_channel_active(&voice_ch)) {
            mixed += apply_channel_gain(audio_channel_linear(&voice_ch),
                                        voice_ch.gain_percent, effects_volume_step);
            audio_channel_advance(&voice_ch);
        }
        buffer[i] = (Sint16)soft_limit_s16(mixed);
    }
}

static void retire_channel_locked(AudioChannel* ch) {
    if (!ch) return;
    audio_retire_locked(ch->data);
    audio_channel_reset(ch);
}

static int music_pick_random_game_track(int avoid_track) {
    if (avoid_track < 1 || avoid_track > 3)
        return (int)(duren_rand() % 3u) + 1;
    int pick = (int)(duren_rand() % 2u) + 1;
    if (pick >= avoid_track) ++pick;
    return pick;
}

static void set_music_record(const char* name, uint8_t gain, bool loop,
                             bool gameplay, int track) {
    if (!sfx_device || !name) return;
    AudioChannel loaded = {0};
    if (!load_audio_record(name, &loaded, gain)) return;
    SDL_LockAudioDevice(sfx_device);
    retire_channel_locked(&music_ch);
    retire_channel_locked(&music_next_ch);
    music_ch = loaded;
    music_paused = false;
    music_gameplay = gameplay;
    music_loop_current = loop;
    gameplay_track = track;
    music_next_track = 0;
    music_fade_q15 = 0u;
    snprintf(music_name, sizeof(music_name), "%s", name);
    SDL_UnlockAudioDevice(sfx_device);
}

static void prepare_next_gameplay_track(void) {
    if (!sfx_device) return;
    int next_track = 0;
    int current = 0;
    bool should_load = false;
    SDL_LockAudioDevice(sfx_device);
    if (music_gameplay && music_ch.data && !music_next_ch.data) {
        const uint32_t remain = music_ch.frames > music_ch.frame_pos
                              ? music_ch.frames - music_ch.frame_pos : 0u;
        if (remain <= 8000u) {
            current = gameplay_track;
            next_track = music_pick_random_game_track(current);
            should_load = true;
        }
    }
    SDL_UnlockAudioDevice(sfx_device);
    if (!should_load) return;

    char name[20];
    snprintf(name, sizeof(name), "music/game%d", next_track);
    AudioChannel loaded = {0};
    if (!load_audio_record(name, &loaded, 30u)) return;

    SDL_LockAudioDevice(sfx_device);
    if (music_gameplay && !music_next_ch.data && gameplay_track == current) {
        music_next_ch = loaded;
        music_next_track = next_track;
        audio_channel_reset(&loaded);
    }
    SDL_UnlockAudioDevice(sfx_device);
    free(loaded.data);
}

void hal_music_set_tone(unsigned int frequency_hz) { (void)frequency_hz; }
void hal_music_set_menu_intro(bool full_volume) {
    music_menu_intro_full = full_volume;
    if (sfx_device) {
        SDL_LockAudioDevice(sfx_device);
        if (music_ch.data && !music_gameplay) music_ch.gain_percent = full_volume ? 100u : 60u;
        SDL_UnlockAudioDevice(sfx_device);
    }
}
void hal_music_start(void) {
    if (music_ch.data && !music_gameplay && strcmp(music_name, "music/menu") == 0) return;
    set_music_record("music/menu", music_menu_intro_full ? 100u : 60u, true, false, 0);
}
void hal_music_start_gameplay(void) {
    if (music_ch.data && music_gameplay) return;
    hal_music_start_gameplay_new_match();
}
void hal_music_start_gameplay_new_match(void) {
    const int track = music_pick_random_game_track(gameplay_track);
    char name[20];
    snprintf(name, sizeof(name), "music/game%d", track);
    set_music_record(name, 30u, false, true, track);
}
void hal_music_pause(bool paused) {
    if (!sfx_device) return;
    SDL_LockAudioDevice(sfx_device);
    music_paused = paused;
    SDL_UnlockAudioDevice(sfx_device);
}
void hal_music_stop(void) {
    if (!sfx_device) return;
    SDL_LockAudioDevice(sfx_device);
    retire_channel_locked(&music_ch);
    retire_channel_locked(&music_next_ch);
    music_name[0] = '\0';
    music_paused = false;
    music_gameplay = false;
    music_loop_current = false;
    music_next_track = 0;
    music_fade_q15 = 0u;
    SDL_UnlockAudioDevice(sfx_device);
}

static void play_record_channel(const char* name, unsigned int gain, bool voice) {
    if (!sfx_device || !name) return;
    if (gain > 200u) gain = 200u;
    AudioChannel loaded = {0};
    if (!load_audio_record(name, &loaded, (uint8_t)gain)) return;
    SDL_LockAudioDevice(sfx_device);
    AudioChannel* active = voice ? &voice_ch : &sfx_ch;
    AudioChannel* queued = voice ? &voice_queued_ch : &sfx_queued_ch;
    retire_channel_locked(active);
    retire_channel_locked(queued);
    *active = loaded;
    SDL_UnlockAudioDevice(sfx_device);
}
void hal_play_sound_gain(SoundFx snd, unsigned int gain_percent) {
    static const char* const names[SND_COUNT] = {
        "sfx/card", "sfx/deal", "sfx/error", "sfx/flip", "sfx/select",
        "sfx/shuffle", "sfx/take", "sfx/win", "sfx/lose", "sfx/draw",
        "sfx/next_opponent", "sfx/jeff"
    };
    if ((unsigned int)snd < SND_COUNT)
        play_record_channel(names[(unsigned int)snd], gain_percent, false);
}
void hal_play_sound(SoundFx snd) {
    hal_play_sound_gain(snd, snd == SND_ERROR ? 27u : 50u);
}
void hal_play_face_voice(int face_index) {
    if (face_index < 0) face_index = 0;
    if (face_index > 19) face_index = 19;
    char name[16]; snprintf(name, sizeof(name), "voice/%02d", face_index);
    play_record_channel(name, 60u, true);
}
void hal_queue_face_voice(int face_index) {
    if (!sfx_device) return;
    if (face_index < 0) face_index = 0;
    if (face_index > 19) face_index = 19;
    char name[16]; snprintf(name, sizeof(name), "voice/%02d", face_index);
    AudioChannel loaded = {0};
    if (!load_audio_record(name, &loaded, 60u)) return;
    SDL_LockAudioDevice(sfx_device);
    retire_channel_locked(&voice_queued_ch);
    voice_queued_ch = loaded;
    SDL_UnlockAudioDevice(sfx_device);
}
bool hal_audio_busy(void) {
    if (!sfx_device) return false;
    SDL_LockAudioDevice(sfx_device);
    const bool busy = audio_channel_active(&sfx_ch) || sfx_queued_ch.data ||
                      audio_channel_active(&voice_ch) || voice_queued_ch.data;
    SDL_UnlockAudioDevice(sfx_device);
    return busy;
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
// =========================================================


static int map_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:     return BTN_UP;    case SDLK_DOWN:   return BTN_DOWN;
        case SDLK_LEFT:   return BTN_LEFT;  case SDLK_RIGHT:  return BTN_RIGHT;
        case SDLK_z:      case SDLK_a: return BTN_A;
        case SDLK_x:      case SDLK_b: return BTN_B;
        case SDLK_RETURN: return BTN_START; case SDLK_p:      return BTN_SELECT; case SDLK_g: return BTN_SELECT; 
        case SDLK_ESCAPE: return BTN_QUIT;  default:          return -1;
    }
}

bool hal_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) return false;
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) return false;

    window = SDL_CreateWindow("Duren", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                              SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) return false;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) return false;

    SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    SDL_AudioSpec wanted_spec, obtained_spec;
    SDL_zero(wanted_spec);
    SDL_zero(obtained_spec);
    wanted_spec.freq = 48000;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = 1;
    wanted_spec.samples = 1024; /* about 21 ms at 48 kHz */
    wanted_spec.callback = sfx_audio_callback;

    sfx_device = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &obtained_spec,
                                    SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (sfx_device > 0 && obtained_spec.format == AUDIO_S16SYS &&
        obtained_spec.channels == 1 && obtained_spec.freq >= 8000) {
        audio_output_freq = obtained_spec.freq;
        audio_source_step_q16 = (uint32_t)(((uint64_t)8000u << 16) /
                                           (uint32_t)audio_output_freq);
        if (audio_source_step_q16 == 0u) audio_source_step_q16 = 1u;
        audio_fade_step_q15 = (uint32_t)(32768u /
            (((uint32_t)audio_output_freq * AUDIO_FADE_MS) / 1000u));
        if (audio_fade_step_q15 == 0u) audio_fade_step_q15 = 1u;
        SDL_PauseAudioDevice(sfx_device, 0);
    } else if (sfx_device > 0) {
        SDL_CloseAudioDevice(sfx_device);
        sfx_device = 0;
    }

    return true;
}

/* Retro-Go lifecycle hooks are part of the shared HAL interface.  Windows
 * never unmounts or power-cycles storage on suspend, so the SDL backend must
 * provide harmless no-op implementations to satisfy main.c at link time. */
void hal_prepare_retro_go_sleep(void) {
}

void hal_on_retro_go_wakeup(void) {
}

void hal_shutdown(void) {
    if (sfx_device > 0) {
        SDL_LockAudioDevice(sfx_device);
        retire_channel_locked(&music_ch);
        retire_channel_locked(&music_next_ch);
        retire_channel_locked(&sfx_ch);
        retire_channel_locked(&sfx_queued_ch);
        retire_channel_locked(&voice_ch);
        retire_channel_locked(&voice_queued_ch);
        SDL_UnlockAudioDevice(sfx_device);
        SDL_CloseAudioDevice(sfx_device);
        sfx_device = 0;
        for (uint8_t i = 0; i < audio_retired_count; ++i) free(audio_retired[i]);
        audio_retired_count = 0u;
    }
    if (grayscale_texture) SDL_DestroyTexture(grayscale_texture);
    if (grayscale_pixels) free(grayscale_pixels);
    if (checker_texture) SDL_DestroyTexture(checker_texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    IMG_Quit(); 
    SDL_Quit();
}

void hal_update(void) {
    SDL_Event event;
    any_key_pressed_this_frame = false;
    memcpy(btn_old_state, btn_state, sizeof(btn_state));
    if (sfx_device) {
        uint8_t* retired[AUDIO_RETIRED_MAX];
        uint8_t retired_count = 0u;
        SDL_LockAudioDevice(sfx_device);
        retired_count = audio_retired_count;
        for (uint8_t i = 0; i < retired_count; ++i) retired[i] = audio_retired[i];
        audio_retired_count = 0u;
        SDL_UnlockAudioDevice(sfx_device);
        for (uint8_t i = 0; i < retired_count; ++i) free(retired[i]);
    }
    prepare_next_gameplay_track();
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) btn_state[BTN_QUIT] = true;
        else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                any_key_pressed_this_frame = true;
                /* PC equivalent of G&W TIME: L cycles shadow direction only. */
                if (event.key.keysym.sym == SDLK_l) {
                    gameplay_shadow_mode = (uint8_t)((gameplay_shadow_mode + 1u) % 3u);
                }
            }
            int btn = map_key(event.key.keysym.sym);
            if (btn >= 0) btn_state[btn] = (event.type == SDL_KEYDOWN);
        }
    }
}

bool hal_is_button_pressed(Button btn) { return btn_state[btn] && !btn_old_state[btn]; }
bool hal_is_button_held(Button btn) { return btn_state[btn]; }
bool hal_is_any_button_pressed(void) { return any_key_pressed_this_frame; }

static uint32_t felt_hash(uint32_t x, uint32_t y) {
    uint32_t v = x * 0x1f123bb5u + y * 0x9e3779b9u + 0x6d2b79f5u;
    v ^= v >> 15;
    v *= 0x85ebca6bu;
    v ^= v >> 13;
    return v;
}

static Uint32 felt_pixel_argb(int x, int y) {
    /* One seamless 320x240 cloth: no tiled source, stripes, pitch markings or
     * visible lighting contour.  The broad centre lift is baked into the
     * generated pixels and never recomputed during gameplay. */
    const int dx = x - 160;
    const int dy = y - 118;
    int d = (dx * dx * 96) / (175 * 175) +
            (dy * dy * 96) / (165 * 165);
    if (d > 96) d = 96;
    const int lift = 4 + (59 - (d * 59) / 96);
    const uint32_t h = felt_hash((uint32_t)x, (uint32_t)y);

    static const int base[TABLE_STYLE_COUNT][3] = {
        {2, 16, 6}, {8, 28, 13}, {18, 42, 24}
    };
    static const int span[TABLE_STYLE_COUNT][3] = {
        {60, 100, 55}, {69, 103, 65}, {75, 105, 74}
    };
    const int style = current_table_style < TABLE_STYLE_COUNT
                    ? current_table_style : TABLE_STYLE_MOSS;
    int r = base[style][0] + (lift * span[style][0]) / 63;
    int g = base[style][1] + (lift * span[style][1]) / 63;
    int b = base[style][2] + (lift * span[style][2]) / 63;

    /* Sparse, short irregular fibres rather than television noise. */
    const uint32_t sel = h & 255u;
    if (sel < 34u) { r -= 3; g -= 8; b -= 5; }
    else if (sel < 54u) { r += 6; g += 8; b += 7; }
    else if (sel < 61u && y + 1 < SCREEN_HEIGHT) { r += 8; g += 11; b += 9; }
    if (((h >> 8) & 1023u) < 5u) { r += 7; g += 10; b += 8; }

    if (r < 2) r = 2;
    if (r > 128) r = 128;
    if (g < 10) g = 10;
    if (g > 160) g = 160;
    if (b < 7) b = 7;
    if (b > 128) b = 128;
    return 0xFF000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
}

static void ensure_felt_texture(void) {
    if (checker_texture || !renderer) return;
    checker_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STATIC,
                                        SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!checker_texture) return;
    Uint32* pixels = (Uint32*)malloc((size_t)SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Uint32));
    if (!pixels) return;
    for (int y = 0; y < SCREEN_HEIGHT; ++y)
        for (int x = 0; x < SCREEN_WIDTH; ++x)
            pixels[y * SCREEN_WIDTH + x] = felt_pixel_argb(x, y);
    SDL_UpdateTexture(checker_texture, NULL, pixels, SCREEN_WIDTH * (int)sizeof(Uint32));
    SDL_SetTextureBlendMode(checker_texture, SDL_BLENDMODE_NONE);
    free(pixels);
}

static bool is_gameplay_felt_color(unsigned int hex_color) {
    return hex_color == 0x082A1Du || hex_color == 0x103820u;
}

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
    if (table_sprite_lighting_active && is_gameplay_felt_color(hex_color))
        hex_color = table_style_flat_rgb(current_table_style);
    Uint8 r = (hex_color >> 16) & 0xFF;
    Uint8 g = (hex_color >> 8) & 0xFF;
    Uint8 b = hex_color & 0xFF;
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderClear(renderer);
}

void hal_set_table_style(uint8_t style) {
    if (style >= TABLE_STYLE_COUNT) style = TABLE_STYLE_MOSS;
    if (current_table_style == style) return;
    current_table_style = style;
    if (checker_texture) {
        SDL_DestroyTexture(checker_texture);
        checker_texture = NULL;
    }
    checker_last_a = checker_last_b = 0xFFFFFFFFu;
}
uint8_t hal_get_table_style(void) { return current_table_style; }

void hal_randomize_table(void) {
    gameplay_shadow_mode = (uint8_t)(duren_rand() % 3u);
}

void hal_clear_checkerboard(unsigned int hex_color_a, unsigned int hex_color_b) {
    (void)hex_color_a; (void)hex_color_b;
    ensure_felt_texture();
    if (checker_texture) {
        SDL_RenderCopy(renderer, checker_texture, NULL, NULL);
    } else {
        const unsigned int c = table_style_flat_rgb(current_table_style);
        SDL_SetRenderDrawColor(renderer, (c >> 16) & 0xFFu,
                               (c >> 8) & 0xFFu, c & 0xFFu, 255);
        SDL_RenderClear(renderer);
    }
}

/* Gameplay lighting is baked into the static procedural felt.  These API
 * hooks remain as no-ops so game code does not pay a full-screen per-frame
 * lighting pass. */
void hal_apply_table_lighting_rect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void hal_set_table_sprite_lighting(bool enabled) {
    table_sprite_lighting_active = enabled;
}

/* One fixed lightweight ambient table mask for every gameplay mode.
 * The three selectable variants affect cast shadows only. */
void hal_apply_table_lighting(void) { }

/* Dynamic additive candle layer over the static menu artwork. The broad
 * ellipse lights the table, knight and nearby hands; the compact hot core
 * keeps the flame visibly bright. */
void hal_apply_menu_lighting(void) {
    if (!renderer) return;
    const uint32_t now = SDL_GetTicks();
    const uint32_t period = 1750u;
    const uint32_t p = now % period;
    const int tri = p < period / 2u
                  ? (int)((p * 255u) / (period / 2u))
                  : (int)(((period - p) * 255u) / (period / 2u));
    const int flicker = (int)((now / 97u) % 7u) - 3;
    const int cx = 170 + (int)((now / 223u) % 3u) - 1;
    const int cy = 184 + (int)((now / 307u) % 3u) - 1;
    const int rx = 124 + (tri * 10) / 255 + flicker;
    const int ry = 91 + (tri * 8) / 255 + flicker / 2;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
    for (int dy = -ry; dy <= ry; ++dy) {
        const int ny = (dy * 256) / ry;
        for (int dx = -rx; dx <= rx; ++dx) {
            const int nx = (dx * 256) / rx;
            const int d2 = nx * nx + ny * ny;
            if (d2 >= 65536) continue;
            const int remain = 65536 - d2;
            const int curved = (int)(((int64_t)remain * remain) >> 16);
            const int core = d2 < 15000 ? (15000 - d2) / 240 : 0;
            int alpha = (curved * (108 + tri / 5)) / 65536 + core;
            if (alpha > 176) alpha = 176;
            if (alpha == 0) continue;
            SDL_SetRenderDrawColor(renderer, 255, 140, 28, (Uint8)alpha);
            SDL_RenderDrawPoint(renderer, cx + dx, cy + dy);
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void hal_apply_darkened_rect(int x, int y, int w, int h, unsigned int percent) {
    if (!renderer || w <= 0 || h <= 0) return;
    if (percent > 100u) percent = 100u;
    SDL_Rect r = { x, y, w, h };
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)((percent * 255u) / 100u));
    SDL_RenderFillRect(renderer, &r);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void hal_apply_grayscale_darkened(void) {
    if (!renderer) return;

    if (!grayscale_pixels) {
        grayscale_pixels = (Uint32*)malloc((size_t)SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Uint32));
        if (!grayscale_pixels) return;
    }

    if (!grayscale_texture) {
        grayscale_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING,
                                              SCREEN_WIDTH, SCREEN_HEIGHT);
        if (!grayscale_texture) return;
        SDL_SetTextureBlendMode(grayscale_texture, SDL_BLENDMODE_NONE);
    }

    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                             grayscale_pixels, SCREEN_WIDTH * (int)sizeof(Uint32)) != 0) {
        return;
    }

    const int count = SCREEN_WIDTH * SCREEN_HEIGHT;
    for (int i = 0; i < count; i++) {
        Uint32 px = grayscale_pixels[i];
        Uint8 r = (Uint8)((px >> 16) & 0xFFu);
        Uint8 g = (Uint8)((px >> 8) & 0xFFu);
        Uint8 b = (Uint8)(px & 0xFFu);
        int y = (77 * r + 150 * g + 29 * b) >> 8;
        y = (y * 3) >> 2; // 25% додаткового затемнення
        if (y > 255) y = 255;
        grayscale_pixels[i] = 0xFF000000u | ((Uint32)y << 16) | ((Uint32)y << 8) | (Uint32)y;
    }

    SDL_UpdateTexture(grayscale_texture, NULL, grayscale_pixels,
                      SCREEN_WIDTH * (int)sizeof(Uint32));
    SDL_RenderCopy(renderer, grayscale_texture, NULL, NULL);
}

void hal_fill_rect(int x, int y, int w, int h, unsigned int hex_color) {
    if (!renderer || w <= 0 || h <= 0) return;
    hex_color = table_light_mod_hex(hex_color, x, y, w, h);
    Uint8 r = (Uint8)((hex_color >> 16) & 0xFFu);
    Uint8 g = (Uint8)((hex_color >> 8) & 0xFFu);
    Uint8 b = (Uint8)(hex_color & 0xFFu);
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(renderer, &rect);
}

void hal_set_fade_level(uint8_t level) { fade_level = level; }

void hal_present(void) {
    if (fade_level > 0) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, fade_level);
        SDL_Rect fade_rect = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
        SDL_RenderFillRect(renderer, &fade_rect);
    }
    SDL_RenderPresent(renderer);
}
void hal_delay(unsigned int ms) {
    (void)ms;
    static Uint32 deadline = 0;
    static Uint32 remainder = 0;
    Uint32 now = SDL_GetTicks();
    if (deadline == 0) deadline = now;
    Uint32 step_ms = 16u;
    remainder += 40u; /* 1000 % 60 */
    if (remainder >= 60u) { remainder -= 60u; ++step_ms; }
    deadline += step_ms;
    if ((Sint32)(deadline - now) <= 0) {
        deadline = now + step_ms;
        remainder = 0;
    }
    now = SDL_GetTicks();
    if ((Sint32)(deadline - now) > 0) SDL_Delay(deadline - now);
}
uint32_t hal_get_ticks_ms(void) { return SDL_GetTicks(); }

HalTexture* hal_load_texture(const char* filename) {
    SDL_Surface* loaded = IMG_Load(filename);
    if (!loaded) return NULL;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    if (!rgba) return NULL;

    SDL_Texture* sdl_tex = SDL_CreateTextureFromSurface(renderer, rgba);
    if (!sdl_tex) {
        SDL_FreeSurface(rgba);
        return NULL;
    }
    /* Some SDL_image builds leave the blend mode at NONE even for RGBA PNGs.
     * That exposed each bitmap-font cell as a dark 8x8/6x10 rectangle. */
    SDL_SetTextureBlendMode(sdl_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(sdl_tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(sdl_tex, 255);

    HalTexture* texture = malloc(sizeof(HalTexture));
    if (!texture) {
        SDL_DestroyTexture(sdl_tex);
        SDL_FreeSurface(rgba);
        return NULL;
    }
    texture->sdl_tex = sdl_tex;
    texture->source_rgba = rgba;
    texture->w = rgba->w;
    texture->h = rgba->h;
    return texture;
}

void hal_destroy_texture(HalTexture* texture) {
    if (texture) {
        if (texture->sdl_tex) SDL_DestroyTexture(texture->sdl_tex);
        if (texture->source_rgba) SDL_FreeSurface(texture->source_rgba);
        free(texture);
    }
}

void hal_evict_texture(HalTexture* texture) {
    /* SDL owns the decoded GPU texture for the wrapper lifetime.  Scene
     * eviction is needed only by Retro-Go's shared SRAM/SD cache. */
    (void)texture;
}

static void set_table_texture_mod(SDL_Texture* tex, int x, int y, int w, int h) {
    if (!table_sprite_lighting_active) return;
    Uint8 keep = table_light_keep_for_rect(x, y, w, h);
    SDL_SetTextureColorMod(tex, keep, keep, keep);
}

static void reset_table_texture_mod(SDL_Texture* tex) {
    if (!table_sprite_lighting_active) return;
    SDL_SetTextureColorMod(tex, 255, 255, 255);
}

void hal_draw_texture(HalTexture* texture, int x, int y) {
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect dst = { x, y, texture->w, texture->h };
    /* Card backs and other whole textures must use the same sprite-light mask.
     * FIX15 skipped this path, leaving opponent card backs fully bright. */
    set_table_texture_mod(texture->sdl_tex, x, y, texture->w, texture->h);
    SDL_RenderCopy(renderer, texture->sdl_tex, NULL, &dst);
    reset_table_texture_mod(texture->sdl_tex);
}

static Uint8 dim_keep_from_percent(unsigned int percent) {
    if (percent > 100u) percent = 100u;
    return (Uint8)(((100u - percent) * 255u + 50u) / 100u);
}

void hal_draw_texture_dimmed(HalTexture* texture, int x, int y, unsigned int percent) {
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect dst = { x, y, texture->w, texture->h };
    Uint8 keep = dim_keep_from_percent(percent);
    SDL_SetTextureColorMod(texture->sdl_tex, keep, keep, keep);
    SDL_RenderCopy(renderer, texture->sdl_tex, NULL, &dst);
    SDL_SetTextureColorMod(texture->sdl_tex, 255, 255, 255);
}

void hal_draw_sprite(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy) {
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect src = { sx, sy, sw, sh }; 
    SDL_Rect dst = { dx, dy, sw, sh };
    set_table_texture_mod(texture->sdl_tex, dx, dy, sw, sh);
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
    reset_table_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_dimmed(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, unsigned int percent) {
    if (!texture || !texture->sdl_tex || sw <= 0 || sh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, sw, sh };
    Uint8 keep = dim_keep_from_percent(percent);
    SDL_SetTextureColorMod(texture->sdl_tex, keep, keep, keep);
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
    SDL_SetTextureColorMod(texture->sdl_tex, 255, 255, 255);
}

void hal_draw_sprite_tinted(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, unsigned int hex_color,
                            unsigned int alpha_percent) {
    if (!texture || !texture->sdl_tex || sw <= 0 || sh <= 0 || alpha_percent == 0u) return;
    if (alpha_percent > 100u) alpha_percent = 100u;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, sw, sh };
    const Uint8 r = (Uint8)((hex_color >> 16) & 0xFFu);
    const Uint8 g = (Uint8)((hex_color >> 8) & 0xFFu);
    const Uint8 b = (Uint8)(hex_color & 0xFFu);
    const Uint8 a = (Uint8)((alpha_percent * 255u + 50u) / 100u);
    SDL_BlendMode old_mode = SDL_BLENDMODE_NONE;
    SDL_GetTextureBlendMode(texture->sdl_tex, &old_mode);
    SDL_SetTextureBlendMode(texture->sdl_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(texture->sdl_tex, r, g, b);
    SDL_SetTextureAlphaMod(texture->sdl_tex, a);
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
    SDL_SetTextureColorMod(texture->sdl_tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(texture->sdl_tex, 255);
    SDL_SetTextureBlendMode(texture->sdl_tex, old_mode);
}

static bool surface_rgba_at(const SDL_Surface* surface, int x, int y,
                            Uint8* r, Uint8* g, Uint8* b, Uint8* a) {
    if (!surface || !r || !g || !b || !a ||
        x < 0 || y < 0 || x >= surface->w || y >= surface->h)
        return false;
    const Uint8* row = (const Uint8*)surface->pixels + y * surface->pitch;
    const Uint32 pixel = ((const Uint32*)row)[x];
    SDL_GetRGBA(pixel, surface->format, r, g, b, a);
    return true;
}

void hal_draw_sprite_occluded_left(HalTexture* texture,
                                    int sx, int sy, int visible_w, int sh,
                                    int dx, int dy, int full_sprite_width,
                                    unsigned int dim_percent) {
    if (!texture || !texture->sdl_tex || !texture->source_rgba ||
        visible_w <= 0 || sh <= 0 || full_sprite_width <= 0)
        return;
    if (sx < 0 || sy < 0 || sx >= texture->w || sy >= texture->h) return;
    if (sx + full_sprite_width > texture->w)
        full_sprite_width = texture->w - sx;
    if (visible_w > full_sprite_width) visible_w = full_sprite_width;
    if (sy + sh > texture->h) sh = texture->h - sy;
    if (visible_w <= 0 || sh <= 0) return;

    Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 255;
    SDL_GetRenderDrawColor(renderer, &old_r, &old_g, &old_b, &old_a);
    SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &old_blend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    Uint8 keep = dim_percent > 0u ? dim_keep_from_percent(dim_percent) : 255u;
    if (dim_percent == 0u && table_sprite_lighting_active) {
        const Uint8 table_keep = table_light_keep_for_rect(dx, dy, visible_w, sh);
        keep = (Uint8)(((unsigned int)keep * table_keep + 127u) / 255u);
    }

    for (int y = 0; y < sh; ++y) {
        int first_opaque = 0;
        Uint8 er = 0, eg = 0, eb = 0, ea = 0;
        while (first_opaque < full_sprite_width) {
            if (!surface_rgba_at(texture->source_rgba,
                                 sx + first_opaque, sy + y,
                                 &er, &eg, &eb, &ea))
                break;
            if (ea >= 128u) break;
            ++first_opaque;
        }
        if (first_opaque <= 0 || first_opaque >= full_sprite_width) continue;
        const int fill_w = first_opaque < visible_w ? first_opaque : visible_w;
        if (fill_w <= 0) continue;

        er = (Uint8)(((unsigned int)er * keep + 127u) / 255u);
        eg = (Uint8)(((unsigned int)eg * keep + 127u) / 255u);
        eb = (Uint8)(((unsigned int)eb * keep + 127u) / 255u);
        SDL_SetRenderDrawColor(renderer, er, eg, eb, 255u);
        SDL_Rect fill = { dx, dy + y, fill_w, 1 };
        SDL_RenderFillRect(renderer, &fill);
    }

    SDL_SetRenderDrawBlendMode(renderer, old_blend);
    SDL_SetRenderDrawColor(renderer, old_r, old_g, old_b, old_a);

    if (dim_percent > 0u)
        hal_draw_sprite_dimmed(texture, sx, sy, visible_w, sh, dx, dy, dim_percent);
    else
        hal_draw_sprite(texture, sx, sy, visible_w, sh, dx, dy);
}

void hal_draw_sprite_flipped_x(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy) {
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, sw, sh };
    set_table_texture_mod(texture->sdl_tex, dx, dy, sw, sh);
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst, 0.0, NULL, SDL_FLIP_HORIZONTAL);
    reset_table_texture_mod(texture->sdl_tex);
}

static void set_shadow_mod(SDL_Texture* tex, unsigned int hex_color);
static void reset_texture_mod(SDL_Texture* tex);

void hal_draw_sprite_scaled(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, int dw, int dh) {
    if (!texture || !texture->sdl_tex || dw <= 0 || dh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, dw, dh };
    set_table_texture_mod(texture->sdl_tex, dx, dy, dw, dh);
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
    reset_table_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_scaled_tinted(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int hex_color) {
    if (!texture || !texture->sdl_tex || dw <= 0 || dh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, dw, dh };
    set_shadow_mod(texture->sdl_tex, hex_color);
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
    reset_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_scaled_flipped_x(HalTexture* texture, int sx, int sy, int sw, int sh,
                                      int dx, int dy, int dw, int dh) {
    if (!texture || !texture->sdl_tex || dw <= 0 || dh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, dw, dh };
    set_table_texture_mod(texture->sdl_tex, dx, dy, dw, dh);
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst,
                     0.0, NULL, SDL_FLIP_HORIZONTAL);
    reset_table_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_scaled_dimmed(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int percent) {
    if (!texture || !texture->sdl_tex || dw <= 0 || dh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, dw, dh };
    Uint8 keep = dim_keep_from_percent(percent);
    SDL_SetTextureColorMod(texture->sdl_tex, keep, keep, keep);
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
    SDL_SetTextureColorMod(texture->sdl_tex, 255, 255, 255);
}

void hal_draw_sprite_scaled_rot90(HalTexture* texture, int sx, int sy, int sw, int sh,
                                  int dx, int dy, int dw, int dh) {
    if (!texture || !texture->sdl_tex || dw <= 0 || dh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx + (dh - dw) / 2, dy + (dw - dh) / 2, dw, dh };
    SDL_Point center = { dw / 2, dh / 2 };
    set_table_texture_mod(texture->sdl_tex, dx, dy, dh, dw);
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst, 90.0, &center, SDL_FLIP_NONE);
    reset_table_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_scaled_rot90_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                         int dx, int dy, int dw, int dh,
                                         unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!texture || !texture->sdl_tex || dw <= 0 || dh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx + (dh - dw) / 2, dy + (dw - dh) / 2, dw, dh };
    SDL_Point center = { dw / 2, dh / 2 };
    hex_color = table_light_mod_hex(hex_color, dx, dy, dh, dw);
    set_shadow_mod(texture->sdl_tex, hex_color);
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst, 90.0, &center, SDL_FLIP_NONE);
    reset_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_scaled_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!texture || !texture->sdl_tex || dw <= 0 || dh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, dw, dh };
    hex_color = table_light_mod_hex(hex_color, dx, dy, dw, dh);
    set_shadow_mod(texture->sdl_tex, hex_color);
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
    reset_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_rot90_ccw(HalTexture* texture, int sx, int sy, int sw, int sh,
                                int out_x, int out_y) {
    if (!texture || !texture->sdl_tex || sw <= 0 || sh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    /* SDL rotates around the destination centre. Offset the unrotated 40x60
     * rectangle so the resulting 60x40 bounding box starts exactly at out_x/y. */
    SDL_Rect dst = { out_x + (sh - sw) / 2, out_y - (sh - sw) / 2, sw, sh };
    set_table_texture_mod(texture->sdl_tex, out_x, out_y, sh, sw);
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst, -90.0, NULL, SDL_FLIP_NONE);
    reset_table_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_rot90_ccw_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                       int out_x, int out_y, unsigned int hex_color) {
    if (!graphics_shadows_enabled || !texture || !texture->sdl_tex || sw <= 0 || sh <= 0) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { out_x + (sh - sw) / 2, out_y - (sh - sw) / 2, sw, sh };
    hex_color = table_light_mod_hex(hex_color, out_x, out_y, sh, sw);
    set_shadow_mod(texture->sdl_tex, hex_color);
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst, -90.0, NULL, SDL_FLIP_NONE);
    reset_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_rotated(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy, double angle) {
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect src = { sx, sy, sw, sh }; 
    SDL_Rect dst = { dx, dy, sw, sh };
    set_table_texture_mod(texture->sdl_tex, dx, dy, sw, sh);
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst, angle, NULL, SDL_FLIP_NONE);
    reset_table_texture_mod(texture->sdl_tex);
}

void hal_draw_texture_shadow(HalTexture* texture, int x, int y, unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!texture) return;
    hal_draw_sprite_shadow(texture, 0, 0, texture->w, texture->h, x, y, hex_color);
}

static void set_shadow_mod(SDL_Texture* tex, unsigned int hex_color) {
    Uint8 r = (hex_color >> 16) & 0xFF;
    Uint8 g = (hex_color >> 8) & 0xFF;
    Uint8 b = hex_color & 0xFF;
    SDL_SetTextureColorMod(tex, r, g, b);
    SDL_SetTextureAlphaMod(tex, 255);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
}

static void reset_texture_mod(SDL_Texture* tex) {
    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(tex, 255);
}

void hal_draw_sprite_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, sw, sh };
    hex_color = table_light_mod_hex(hex_color, dx, dy, sw, sh);
    set_shadow_mod(texture->sdl_tex, hex_color);
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
    reset_texture_mod(texture->sdl_tex);
}

void hal_draw_sprite_rotated_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                    int dx, int dy, double angle, unsigned int hex_color) {
    if (!graphics_shadows_enabled) return;
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect src = { sx, sy, sw, sh };
    SDL_Rect dst = { dx, dy, sw, sh };
    hex_color = table_light_mod_hex(hex_color, dx, dy, sw, sh);
    set_shadow_mod(texture->sdl_tex, hex_color);
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst, angle, NULL, SDL_FLIP_NONE);
    reset_texture_mod(texture->sdl_tex);
}
