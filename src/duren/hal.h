#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

typedef enum {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_START, BTN_SELECT, BTN_QUIT,
    BTN_COUNT
} Button;

typedef enum {
    SND_CARD,
    SND_DEAL,
    SND_ERROR,
    SND_FLIP,
    SND_SELECT,
    SND_SHUFFLE,
    SND_TAKE,
    SND_WIN,
    SND_LOSE,
    SND_DRAW,
    SND_NEXT_OPPONENT,
    SND_JEFF,
    SND_COUNT
} SoundFx;

typedef struct HalTexture HalTexture;

typedef enum {
    TABLE_STYLE_MOSS = 0,
    TABLE_STYLE_LIGHT_MOSS,
    TABLE_STYLE_SOFT_MOSS,
    TABLE_STYLE_COUNT
} TableStyle;

bool hal_init(void);
void hal_shutdown(void);
void hal_update(void);
void hal_on_retro_go_wakeup(void);
void hal_prepare_retro_go_sleep(void);

bool hal_is_button_pressed(Button btn);
bool hal_is_button_held(Button btn);
bool hal_is_any_button_pressed(void);

void hal_clear_screen(unsigned int hex_color);
void hal_clear_checkerboard(unsigned int hex_color_a, unsigned int hex_color_b);
void hal_randomize_table(void);
void hal_apply_table_lighting(void);
void hal_apply_table_lighting_rect(int x, int y, int w, int h);
void hal_set_table_sprite_lighting(bool enabled);
void hal_get_shadow_offset(int object_center_x, int lift, int* dx, int* dy);
void hal_apply_menu_lighting(void);
void hal_apply_grayscale_darkened(void);
void hal_apply_darkened_rect(int x, int y, int w, int h, unsigned int percent);
void hal_set_fade_level(uint8_t level);
void hal_fill_rect(int x, int y, int w, int h, unsigned int hex_color);
void hal_present(void);
void hal_delay(unsigned int ms);
uint32_t hal_get_ticks_ms(void);

HalTexture* hal_load_texture(const char* filename);
void hal_destroy_texture(HalTexture* texture);
void hal_evict_texture(HalTexture* texture);
void hal_draw_texture(HalTexture* texture, int x, int y);
void hal_draw_texture_dimmed(HalTexture* texture, int x, int y, unsigned int percent);
void hal_draw_sprite(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy);
void hal_draw_sprite_dimmed(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy, unsigned int percent);
/* Draw a lightweight semi-transparent color mask through the source sprite's
 * opaque pixels. alpha_percent is 0..100. Used for pulsing legal-card hints;
 * callers draw it immediately after the card so later cards naturally cover it. */
void hal_draw_sprite_tinted(HalTexture* texture, int sx, int sy, int sw, int sh,
                            int dx, int dy, unsigned int hex_color,
                            unsigned int alpha_percent);
/* Draw an overlapped fan card without letting the card underneath show
 * through this sprite's rounded left corners. Transparent pixels in the
 * left-edge run are extended from the first opaque pixel on the same row.
 * dim_percent uses the same semantics as hal_draw_sprite_dimmed (0 = normal). */
void hal_draw_sprite_occluded_left(HalTexture* texture,
                                    int sx, int sy, int visible_w, int sh,
                                    int dx, int dy, int full_sprite_width,
                                    unsigned int dim_percent);
void hal_draw_sprite_flipped_x(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy);
void hal_draw_sprite_scaled(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh);
void hal_draw_sprite_scaled_tinted(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int hex_color);
void hal_draw_sprite_scaled_flipped_x(HalTexture* texture, int sx, int sy, int sw, int sh,
                                      int dx, int dy, int dw, int dh);
void hal_draw_sprite_scaled_dimmed(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int percent);
void hal_draw_sprite_scaled_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh,
                                   unsigned int hex_color);
void hal_draw_sprite_scaled_rot90(HalTexture* texture, int sx, int sy, int sw, int sh,
                                  int dx, int dy, int dw, int dh);
void hal_draw_sprite_scaled_rot90_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                         int dx, int dy, int dw, int dh,
                                         unsigned int hex_color);
void hal_draw_sprite_rot90_ccw(HalTexture* texture, int sx, int sy, int sw, int sh,
                                int out_x, int out_y);
void hal_draw_sprite_rot90_ccw_shadow(HalTexture* texture, int sx, int sy, int sw, int sh,
                                       int out_x, int out_y, unsigned int hex_color);
void hal_draw_sprite_rotated(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy, double angle);
void hal_draw_texture_shadow(HalTexture* texture, int x, int y, unsigned int hex_color);
void hal_draw_sprite_shadow(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy, unsigned int hex_color);
void hal_draw_sprite_rotated_shadow(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy, double angle, unsigned int hex_color);

void hal_play_sound(SoundFx snd);
void hal_play_sound_gain(SoundFx snd, unsigned int gain_percent);
void hal_play_face_voice(int face_index);
void hal_queue_face_voice(int face_index);
bool hal_audio_busy(void);
void hal_music_set_tone(unsigned int frequency_hz);
void hal_music_start(void);
void hal_music_set_menu_intro(bool full_volume);
void hal_music_start_gameplay(void);
void hal_music_start_gameplay_new_match(void);
void hal_music_pause(bool paused);
void hal_music_stop(void);
void hal_set_music_volume_step(uint8_t step);
void hal_set_effects_volume_step(uint8_t step);
void hal_set_shadows_enabled(bool enabled);
void hal_set_table_style(uint8_t style);
uint8_t hal_get_table_style(void);
bool hal_shadows_enabled(void);

#endif // HAL_H
