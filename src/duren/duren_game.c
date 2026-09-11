#include "hal.h"
#include "game.h"
#include "modes.h"
#include "tournament.h"
#include "ui_modes.h"
#include "dialogue_data.h"
#include "dialogue_font_metrics.h"
#include "battle_royal.h"
#include "battle_royal_ui.h"
#include "savegame.h"
#include "career_map.h"
#include "career_progress.h"
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <string.h> 
#include "rng.h"
#ifdef PLATFORM_RETRO_GO
#include "rg_storage.h"
#endif

// PLATFORM_RETRO_GO має визначатись у Makefile/збірці для консолі
// (наприклад -DPLATFORM_RETRO_GO). На retro-go немає SDL, тому весь
// SDL-специфічний код фонової музики нижче для цієї платформи вимкнено.
#ifndef PLATFORM_RETRO_GO
#ifdef _WIN32
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#define HAS_SDL_BG_MUSIC 1
#endif

#undef main 

#define CARD_WIDTH  40
#define CARD_HEIGHT 60
#define BR_CARD_WIDTH CARD_WIDTH
#define BR_CARD_HEIGHT CARD_HEIGHT
/* Native 40x60 table cards. Up to seven attack/defence pairs share the
 * defender-side row and compress like a hand fan. The eighth pair starts a
 * second row, so the first seven do not jump when the extra pair appears. */
#define NORMAL_TABLE_CARD_W CARD_WIDTH
#define NORMAL_TABLE_CARD_H CARD_HEIGHT
#define NORMAL_TABLE_PAIR_W (CARD_WIDTH + 12)
#define NORMAL_TABLE_ROW_CAPACITY 7
#define NORMAL_TABLE_FIELD_LEFT 3
#define NORMAL_DECK_X 280
#define NORMAL_TRUMP_X (NORMAL_DECK_X - 11)
#define NORMAL_TRUMP_Y 92
#define NORMAL_TABLE_FIELD_RIGHT (NORMAL_TRUMP_X - 6)
#define NORMAL_TABLE_TOP_Y 74
#define NORMAL_TABLE_BOTTOM_Y 104
#define NORMAL_TABLE_ROW_OFFSET 30
#define NORMAL_TABLE_DEFENSE_DX 12
#define NORMAL_TABLE_DEFENSE_DY 8
#define ANIM_FRAMES       12
#define DEAL_ANIM_FRAMES   6
#define MAX_ANIMS          32
#define DURAK_DIAGNOSTIC_BOOT_CANARY 0

static int card_atlas_index(Card card) {
    return card_canonical_id(card);
}

#define TABLE_BG_A   0x082A1D
#define TABLE_BG_B   TABLE_BG_A
#define SHADOW_COLOR     0x0B100E
#define SHADOW_FAR_COLOR 0x101713
#define SHADOW_DX        5
#define SHADOW_DY        6

static void init_bg_music(void) {
    /* Audio device is owned by HAL; menu PCM starts from the main state loop. */
    hal_music_stop();
}

static int normal_layout_capacity_cache = 0;

/* PreSleep is called by Retro-Go outside the frame loop. These pointers are
 * valid only while main() owns the live session and let suspend flush Career
 * progression before the SD card is unmounted. */
static ModeSession* g_live_mode_session = NULL;
static GameState* g_live_game = NULL;

static int normal_layout_pair_count(const GameState* game, int requested_slot) {
    int count = game ? game->table_pair_count : 0;
    if (requested_slot + 1 > count) {
        count = requested_slot + 1;
        normal_layout_capacity_cache = count; /* pending attack animation */
    } else if (normal_layout_capacity_cache > count) {
        count = normal_layout_capacity_cache;
    }
    if (count < 1) count = 1;
    if (count > MAX_TABLE) count = MAX_TABLE;
    return count;
}

static int normal_row_pair_count(int total_pairs, int row) {
    int count = total_pairs - row * NORMAL_TABLE_ROW_CAPACITY;
    if (count < 1) count = 1;
    if (count > NORMAL_TABLE_ROW_CAPACITY) count = NORMAL_TABLE_ROW_CAPACITY;
    return count;
}

/* Dynamic overlap matches the hand fan: sparse bouts breathe and the first
 * row compresses just enough for seven complete pair layers inside the field. */
static int normal_pair_step(int row_pair_count) {
    if (row_pair_count <= 1) return 0;
    const int field_w = NORMAL_TABLE_FIELD_RIGHT - NORMAL_TABLE_FIELD_LEFT + 1;
    int step = (field_w - NORMAL_TABLE_PAIR_W) / (row_pair_count - 1);
    if (step > 70) step = 70;
    if (step < 1) step = 1;
    return step;
}

static int normal_table_x(const GameState* game, int slot, bool defense) {
    if (slot < 0) slot = 0;
    if (slot >= MAX_TABLE) slot = MAX_TABLE - 1;
    const int total = normal_layout_pair_count(game, slot);
    const int row = slot / NORMAL_TABLE_ROW_CAPACITY;
    const int row_slot = slot % NORMAL_TABLE_ROW_CAPACITY;
    const int row_count = normal_row_pair_count(total, row);
    const int step = normal_pair_step(row_count);
    int x = NORMAL_TABLE_FIELD_RIGHT - NORMAL_TABLE_PAIR_W + 1 - row_slot * step;
    if (defense) x += NORMAL_TABLE_DEFENSE_DX;
    return x;
}

static int normal_table_y(const GameState* game, int slot, bool defense) {
    if (slot < 0) slot = 0;
    if (slot >= MAX_TABLE) slot = MAX_TABLE - 1;
    const int row = slot / NORMAL_TABLE_ROW_CAPACITY;
    const bool defender_bottom = game && !game->is_player_turn;
    int y = defender_bottom
          ? NORMAL_TABLE_BOTTOM_Y - row * NORMAL_TABLE_ROW_OFFSET
          : NORMAL_TABLE_TOP_Y + row * NORMAL_TABLE_ROW_OFFSET;
    if (defense) y += NORMAL_TABLE_DEFENSE_DY;
    return y;
}

/* Draw the second row first, then the defender-side row. Within each row the
 * right-to-left fan is painted newest-to-oldest, keeping every attack/defence
 * pair an indivisible layer and its rank corner readable. */
static int normal_table_draw_pair_index(int pair_count, int draw_index) {
    if (pair_count < 0) pair_count = 0;
    if (pair_count > MAX_TABLE) pair_count = MAX_TABLE;
    if (draw_index < 0 || draw_index >= pair_count) return 0;

    const int first_count = pair_count < NORMAL_TABLE_ROW_CAPACITY
                          ? pair_count : NORMAL_TABLE_ROW_CAPACITY;
    const int second_count = pair_count - first_count;
    if (draw_index < second_count)
        return NORMAL_TABLE_ROW_CAPACITY + second_count - 1 - draw_index;

    const int local = draw_index - second_count;
    return first_count - 1 - local;
}


#define DURAK_SETTINGS_MAGIC 0x44535447u /* DSTG */
#define DURAK_SETTINGS_VERSION 8u
#define DURAK_SETTINGS_PATH "duren_settings.dat"
#define DURAK_SETTINGS_LEGACY_PATH "durak_settings.dat"

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t effects_volume;
    uint8_t music_volume;
    uint8_t shadows_enabled;
    uint8_t particle_count;
    uint8_t snow_enabled;
    uint8_t snowflake_count;
    uint32_t checksum;
} DurakSettingsFileV1;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t effects_volume;
    uint8_t music_volume;
    uint8_t shadows_enabled;
    uint8_t particle_count;
    uint8_t snow_enabled;
    uint8_t snowflake_count;
    uint8_t language;
    uint8_t table_style;       /* legacy: 0=dark, 1=normal, 2=light */
    uint8_t dialogues_enabled;
    uint8_t reserved;
    uint32_t checksum;
} DurakSettingsFileV2;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t effects_volume;
    uint8_t music_volume;
    uint8_t shadows_enabled;
    uint8_t particle_count;
    uint8_t snow_enabled;
    uint8_t snowflake_count;
    uint8_t language;
    uint8_t table_style;       /* legacy v3: six table styles */
    uint8_t dialogues_enabled;
    uint8_t auto_sort_enabled;
    uint32_t checksum;
} DurakSettingsFileV3;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t effects_volume;
    uint8_t music_volume;
    uint8_t shadows_enabled;
    uint8_t particle_count;
    uint8_t snow_enabled;
    uint8_t snowflake_count;
    uint8_t language;
    uint8_t table_style;       /* legacy v4: four bright table styles */
    uint8_t dialogues_enabled;
    uint8_t auto_sort_enabled;
    uint32_t checksum;
} DurakSettingsFileV4;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t effects_volume;
    uint8_t music_volume;
    uint8_t shadows_enabled;
    uint8_t particle_count;
    uint8_t snow_enabled;
    uint8_t snowflake_count;
    uint8_t language;
    uint8_t table_style;
    uint8_t dialogues_enabled;
    uint8_t auto_sort_enabled;
    uint32_t checksum;
} DurakSettingsFileV5;

typedef enum {
    DECK_STYLE_UA = 0,
    DECK_STYLE_EU,
    DECK_STYLE_ATLAS_UA,
    DECK_STYLE_ATLAS_EU,
    DECK_STYLE_CATS_UA,
    DECK_STYLE_CATS_EU,
    DECK_STYLE_COUNT
} DeckStyle;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t effects_volume;
    uint8_t music_volume;
    uint8_t shadows_enabled;
    uint8_t particle_count;
    uint8_t snow_enabled;
    uint8_t snowflake_count;
    uint8_t language;
    uint8_t table_style;
    uint8_t dialogues_enabled;
    uint8_t auto_sort_enabled;
    uint8_t deck_style;
    uint8_t reserved[3];
    uint32_t checksum;
} DurakSettingsFileV6;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t effects_volume;
    uint8_t music_volume;
    uint8_t shadows_enabled;
    uint8_t particle_count;
    uint8_t snow_enabled;
    uint8_t snowflake_count;
    uint8_t language;
    uint8_t table_style;       /* TableStyle stable ID */
    uint8_t dialogues_enabled;
    uint8_t auto_sort_enabled;
    uint8_t deck_style;        /* DECKUA / DECKEU */
    int8_t game_speed;         /* -5..+5; 0 is the new 30%-slower default */
    uint8_t reserved[2];
    uint32_t checksum;
} DurakSettingsFileV7;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t effects_volume;
    uint8_t music_volume;
    uint8_t shadows_enabled;
    uint8_t particle_count;
    uint8_t snow_enabled;
    uint8_t snowflake_count;
    uint8_t language;
    uint8_t table_style;       /* TableStyle stable ID */
    uint8_t dialogues_enabled;
    uint8_t auto_sort_enabled;
    uint8_t deck_style;        /* DECKUA / DECKEU */
    int8_t game_speed;         /* -5..+5 */
    uint8_t difficulty;        /* persisted one-time profile choice */
    uint8_t profile;           /* bit0 face, bit1 Cats, bit2 language set, bit7 profile set */
    uint32_t checksum;
} DurakSettingsFile;

typedef struct {
    uint8_t effects_volume;
    uint8_t music_volume;
    bool shadows_enabled;
    uint8_t particle_count;
    bool snow_enabled;
    uint8_t snowflake_count;
    uint8_t language;
    uint8_t table_style;
    bool dialogues_enabled;
    bool auto_sort_enabled;
    uint8_t deck_style;
    int8_t game_speed;
    uint8_t difficulty;
    uint8_t player_face;
    bool language_complete;
    bool profile_complete;
    bool cats_unlocked;
} DurakSettings;

static DurakSettings g_settings;
static bool g_settings_dirty = false;

/* Movement speed relative to the old build. The new neutral setting is 70%,
 * exactly 30% slower, while the full -5..+5 range remains useful. */
static const uint8_t g_game_speed_percent[11] = {
    40u, 46u, 52u, 58u, 64u, 70u, 78u, 86u, 95u, 105u, 115u
};

static int game_speed_percent(void) {
    int index = (int)g_settings.game_speed + 5;
    if (index < 0) index = 0;
    if (index > 10) index = 10;
    return g_game_speed_percent[index];
}

static uint32_t scaled_motion_ms(uint32_t old_ms) {
    const uint32_t speed = (uint32_t)game_speed_percent();
    uint32_t value = (old_ms * 100u + speed / 2u) / speed;
    return value < 1u ? 1u : value;
}

static uint8_t default_deck_style_for_language(int language) {
    return language == UI_LANG_UKRAINIAN ? DECK_STYLE_UA : DECK_STYLE_EU;
}

static bool deck_style_is_cats(int style) {
    return style == DECK_STYLE_CATS_UA || style == DECK_STYLE_CATS_EU;
}

static uint8_t deck_style_for_language(int style, int language) {
    const bool ua = language == UI_LANG_UKRAINIAN;
    if (style == DECK_STYLE_ATLAS_UA || style == DECK_STYLE_ATLAS_EU)
        return ua ? DECK_STYLE_ATLAS_UA : DECK_STYLE_ATLAS_EU;
    if (deck_style_is_cats(style))
        return ua ? DECK_STYLE_CATS_UA : DECK_STYLE_CATS_EU;
    return ua ? DECK_STYLE_UA : DECK_STYLE_EU;
}

static uint8_t next_available_deck_style(int style, int direction,
                                         bool cats_unlocked) {
    int next = style;
    do {
        next = (next + DECK_STYLE_COUNT + direction) % DECK_STYLE_COUNT;
    } while (!cats_unlocked && deck_style_is_cats(next));
    return (uint8_t)next;
}

static uint8_t random_online_deck_style(int language, bool cats_unlocked) {
    /* Online draws one visual family at match start. Cats joins the pool only
     * after the story reward has been earned. Rank letters follow UI language. */
    const int family_count = cats_unlocked ? 3 : 2;
    const int family = duren_rand() % family_count;
    if (family == 1)
        return deck_style_for_language(DECK_STYLE_ATLAS_EU, language);
    if (family == 2)
        return deck_style_for_language(DECK_STYLE_CATS_EU, language);
    return deck_style_for_language(DECK_STYLE_EU, language);
}

static const char* deck_style_label(int style) {
    static const char* const labels[DECK_STYLE_COUNT] = {
        "CLASSIC-UA", "CLASSIC-LAT", "ATLAS-UA", "ATLAS-LAT", "CATS-UA", "CATS-LAT"
    };
    return labels[(style >= 0 && style < DECK_STYLE_COUNT) ? style : DECK_STYLE_UA];
}

static uint32_t settings_checksum(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void settings_defaults(void) {
    g_settings.effects_volume = 5u;
    g_settings.music_volume = 5u;
    g_settings.shadows_enabled = true;
    g_settings.particle_count = 5u;
    g_settings.snow_enabled = false;
    g_settings.snowflake_count = 30u;
    g_settings.language = UI_LANG_UKRAINIAN;
    g_settings.table_style = TABLE_STYLE_MOSS;
    g_settings.dialogues_enabled = true;
    g_settings.auto_sort_enabled = true;
    g_settings.deck_style = default_deck_style_for_language(g_settings.language);
    g_settings.game_speed = 0;
    g_settings.difficulty = DIFFICULTY_NORMAL;
    g_settings.player_face = 0u;
    g_settings.language_complete = false;
    g_settings.profile_complete = false;
    g_settings.cats_unlocked = false;
    g_settings_dirty = false;
}

static void settings_apply(void) {
    hal_set_effects_volume_step(g_settings.effects_volume);
    hal_set_music_volume_step(g_settings.music_volume);
    hal_set_shadows_enabled(g_settings.shadows_enabled);
    hal_set_table_style(g_settings.table_style);
    br_ui_set_game_speed_step(g_settings.game_speed);
}

static bool settings_load(void) {
    settings_defaults();
    bool loaded_legacy_path = false;
    FILE* f = fopen(DURAK_SETTINGS_PATH, "rb");
    if (!f) {
        f = fopen(DURAK_SETTINGS_LEGACY_PATH, "rb");
        loaded_legacy_path = f != NULL;
    }
    if (!f) { settings_apply(); return false; }
    uint8_t raw[sizeof(DurakSettingsFile)] = {0};
    const size_t got = fread(raw, 1, sizeof(raw), f);
    bool exact_eof = false;
    if (!ferror(f)) {
        if (got < sizeof(raw)) {
            /* fread() reached EOF while reading an older, smaller settings
             * structure.  This is valid and is handled by the migration code. */
            exact_eof = feof(f) != 0;
        } else {
            /* Current-size files need one safe probe byte to reject trailing
             * garbage. Never probe after a failed fread(), because the stream
             * position may be indeterminate. */
            const int extra = fgetc(f);
            exact_eof = extra == EOF && feof(f) != 0 && !ferror(f);
        }
    }
    fclose(f);

    bool valid = false;
    uint16_t version = 0u;
    if (got >= sizeof(uint32_t) + sizeof(uint16_t))
        memcpy(&version, raw + sizeof(uint32_t), sizeof(version));

    /* v2 and v3 deliberately have the same byte size. Dispatch by the stored
     * version before validation; a size-only if/else chain would make v2
     * migration unreachable. */
    if (got == sizeof(DurakSettingsFile) && exact_eof && version == DURAK_SETTINGS_VERSION) {
        DurakSettingsFile disk;
        memcpy(&disk, raw, sizeof(disk));
        valid = disk.magic == DURAK_SETTINGS_MAGIC &&
                disk.checksum == settings_checksum(&disk, sizeof(disk) - sizeof(disk.checksum)) &&
                disk.effects_volume <= 10u && disk.music_volume <= 10u &&
                disk.shadows_enabled <= 1u && disk.particle_count >= 1u && disk.particle_count <= 20u &&
                disk.snow_enabled <= 1u && disk.snowflake_count >= 20u && disk.snowflake_count <= 40u &&
                disk.language < UI_LANG_COUNT && disk.table_style < TABLE_STYLE_COUNT &&
                disk.dialogues_enabled <= 1u && disk.auto_sort_enabled <= 1u &&
                disk.deck_style < DECK_STYLE_COUNT &&
                disk.game_speed >= -5 && disk.game_speed <= 5 &&
                disk.difficulty <= DIFFICULTY_HARD &&
                (disk.profile & 0x78u) == 0u;
        if (valid) {
            g_settings.effects_volume = disk.effects_volume;
            g_settings.music_volume = disk.music_volume;
            g_settings.shadows_enabled = disk.shadows_enabled != 0u;
            g_settings.particle_count = disk.particle_count;
            g_settings.snow_enabled = disk.snow_enabled != 0u;
            g_settings.snowflake_count = disk.snowflake_count;
            g_settings.language = disk.language;
            g_settings.table_style = disk.table_style;
            g_settings.dialogues_enabled = disk.dialogues_enabled != 0u;
            g_settings.auto_sort_enabled = disk.auto_sort_enabled != 0u;
            g_settings.deck_style = disk.deck_style;
            g_settings.game_speed = disk.game_speed;
            g_settings.difficulty = disk.difficulty;
            g_settings.player_face = disk.profile & 1u;
            g_settings.profile_complete = (disk.profile & 0x80u) != 0u;
            g_settings.language_complete = (disk.profile & 0x04u) != 0u ||
                                           g_settings.profile_complete;
            g_settings.cats_unlocked = (disk.profile & 0x02u) != 0u;
        }
    } else if (got == sizeof(DurakSettingsFileV7) && exact_eof && version == 7u) {
        DurakSettingsFileV7 old;
        memcpy(&old, raw, sizeof(old));
        valid = old.magic == DURAK_SETTINGS_MAGIC &&
                old.checksum == settings_checksum(&old, sizeof(old) - sizeof(old.checksum)) &&
                old.effects_volume <= 10u && old.music_volume <= 10u &&
                old.shadows_enabled <= 1u && old.particle_count >= 1u && old.particle_count <= 20u &&
                old.snow_enabled <= 1u && old.snowflake_count >= 20u && old.snowflake_count <= 40u &&
                old.language < UI_LANG_COUNT && old.table_style < TABLE_STYLE_COUNT &&
                old.dialogues_enabled <= 1u && old.auto_sort_enabled <= 1u &&
                old.deck_style < DECK_STYLE_COUNT &&
                old.game_speed >= -5 && old.game_speed <= 5;
        if (valid) {
            g_settings.effects_volume = old.effects_volume;
            g_settings.music_volume = old.music_volume;
            g_settings.shadows_enabled = old.shadows_enabled != 0u;
            g_settings.particle_count = old.particle_count;
            g_settings.snow_enabled = old.snow_enabled != 0u;
            g_settings.snowflake_count = old.snowflake_count;
            g_settings.language = old.language;
            g_settings.table_style = old.table_style;
            g_settings.dialogues_enabled = old.dialogues_enabled != 0u;
            g_settings.auto_sort_enabled = old.auto_sort_enabled != 0u;
            g_settings.deck_style = old.deck_style;
            g_settings.game_speed = old.game_speed;
            /* Existing installations keep their language, then choose the
             * new persistent difficulty and stock portrait exactly once. */
            g_settings.profile_complete = false;
            g_settings.language_complete = true;
            g_settings_dirty = true;
        }
    } else if (got == sizeof(DurakSettingsFileV6) && exact_eof && version == 6u) {
        DurakSettingsFileV6 old;
        memcpy(&old, raw, sizeof(old));
        valid = old.magic == DURAK_SETTINGS_MAGIC &&
                old.checksum == settings_checksum(&old, sizeof(old) - sizeof(old.checksum)) &&
                old.effects_volume <= 10u && old.music_volume <= 10u &&
                old.shadows_enabled <= 1u && old.particle_count >= 1u && old.particle_count <= 20u &&
                old.snow_enabled <= 1u && old.snowflake_count >= 20u && old.snowflake_count <= 40u &&
                old.language < UI_LANG_COUNT && old.table_style < TABLE_STYLE_COUNT &&
                old.dialogues_enabled <= 1u && old.auto_sort_enabled <= 1u &&
                old.deck_style < DECK_STYLE_COUNT;
        if (valid) {
            g_settings.effects_volume = old.effects_volume;
            g_settings.music_volume = old.music_volume;
            g_settings.shadows_enabled = old.shadows_enabled != 0u;
            g_settings.particle_count = old.particle_count;
            g_settings.snow_enabled = old.snow_enabled != 0u;
            g_settings.snowflake_count = old.snowflake_count;
            g_settings.language = old.language;
            g_settings.table_style = old.table_style;
            g_settings.dialogues_enabled = old.dialogues_enabled != 0u;
            g_settings.auto_sort_enabled = old.auto_sort_enabled != 0u;
            g_settings.deck_style = old.deck_style;
            g_settings.game_speed = 0;
            g_settings_dirty = true;
        }
    } else if (got == sizeof(DurakSettingsFileV5) && exact_eof && version == 5u) {
        DurakSettingsFileV5 old;
        memcpy(&old, raw, sizeof(old));
        valid = old.magic == DURAK_SETTINGS_MAGIC &&
                old.checksum == settings_checksum(&old, sizeof(old) - sizeof(old.checksum)) &&
                old.effects_volume <= 10u && old.music_volume <= 10u &&
                old.shadows_enabled <= 1u && old.particle_count >= 1u && old.particle_count <= 20u &&
                old.snow_enabled <= 1u && old.snowflake_count >= 20u && old.snowflake_count <= 40u &&
                old.language < UI_LANG_COUNT && old.table_style < TABLE_STYLE_COUNT &&
                old.dialogues_enabled <= 1u && old.auto_sort_enabled <= 1u;
        if (valid) {
            g_settings.effects_volume = old.effects_volume;
            g_settings.music_volume = old.music_volume;
            g_settings.shadows_enabled = old.shadows_enabled != 0u;
            g_settings.particle_count = old.particle_count;
            g_settings.snow_enabled = old.snow_enabled != 0u;
            g_settings.snowflake_count = old.snowflake_count;
            g_settings.language = old.language;
            g_settings.table_style = old.table_style;
            g_settings.dialogues_enabled = old.dialogues_enabled != 0u;
            g_settings.auto_sort_enabled = old.auto_sort_enabled != 0u;
            g_settings.deck_style = default_deck_style_for_language(old.language);
            g_settings_dirty = true;
        }
    } else if (got == sizeof(DurakSettingsFileV4) && exact_eof && version == 4u) {
        DurakSettingsFileV4 old;
        memcpy(&old, raw, sizeof(old));
        const bool valid = old.magic == DURAK_SETTINGS_MAGIC &&
                old.checksum == settings_checksum(&old, sizeof(old) - sizeof(old.checksum)) &&
                old.effects_volume <= 10u && old.music_volume <= 10u &&
                old.shadows_enabled <= 1u && old.particle_count >= 1u && old.particle_count <= 20u &&
                old.snow_enabled <= 1u && old.snowflake_count >= 20u && old.snowflake_count <= 40u &&
                old.language < UI_LANG_COUNT && old.table_style <= 3u &&
                old.dialogues_enabled <= 1u && old.auto_sort_enabled <= 1u;
        if (valid) {
            static const uint8_t v4_table_migration[4] = {
                TABLE_STYLE_LIGHT_MOSS, TABLE_STYLE_SOFT_MOSS,
                TABLE_STYLE_SOFT_MOSS, TABLE_STYLE_LIGHT_MOSS
            };
            g_settings.effects_volume = old.effects_volume;
            g_settings.music_volume = old.music_volume;
            g_settings.shadows_enabled = old.shadows_enabled != 0u;
            g_settings.particle_count = old.particle_count;
            g_settings.snow_enabled = old.snow_enabled != 0u;
            g_settings.snowflake_count = old.snowflake_count;
            g_settings.language = old.language;
            g_settings.deck_style = default_deck_style_for_language(old.language);
            g_settings.table_style = v4_table_migration[old.table_style];
            g_settings.dialogues_enabled = old.dialogues_enabled != 0u;
            g_settings.auto_sort_enabled = old.auto_sort_enabled != 0u;
            g_settings_dirty = true;
        }
    } else if (got == sizeof(DurakSettingsFileV3) && exact_eof && version == 3u) {
        DurakSettingsFileV3 old;
        memcpy(&old, raw, sizeof(old));
        valid = old.magic == DURAK_SETTINGS_MAGIC &&
                old.checksum == settings_checksum(&old, sizeof(old) - sizeof(old.checksum)) &&
                old.effects_volume <= 10u && old.music_volume <= 10u &&
                old.shadows_enabled <= 1u && old.particle_count >= 1u && old.particle_count <= 20u &&
                old.snow_enabled <= 1u && old.snowflake_count >= 20u && old.snowflake_count <= 40u &&
                old.language < UI_LANG_COUNT && old.table_style <= 5u &&
                old.dialogues_enabled <= 1u && old.auto_sort_enabled <= 1u;
        if (valid) {
            g_settings.effects_volume = old.effects_volume;
            g_settings.music_volume = old.music_volume;
            g_settings.shadows_enabled = old.shadows_enabled != 0u;
            g_settings.particle_count = old.particle_count;
            g_settings.snow_enabled = old.snow_enabled != 0u;
            g_settings.snowflake_count = old.snowflake_count;
            g_settings.language = old.language;
            g_settings.deck_style = default_deck_style_for_language(old.language);
            static const uint8_t table_migration[6] = {
                TABLE_STYLE_LIGHT_MOSS, /* old bright green */
                TABLE_STYLE_MOSS,       /* old dark */
                TABLE_STYLE_MOSS,       /* old normal */
                TABLE_STYLE_LIGHT_MOSS, /* old light */
                TABLE_STYLE_SOFT_MOSS,  /* old super light */
                TABLE_STYLE_LIGHT_MOSS  /* old blue */
            };
            g_settings.table_style = table_migration[old.table_style];
            g_settings.dialogues_enabled = old.dialogues_enabled != 0u;
            g_settings.auto_sort_enabled = old.auto_sort_enabled != 0u;
            g_settings_dirty = true;
        }
    } else if (got == sizeof(DurakSettingsFileV2) && exact_eof && version == 2u) {
        DurakSettingsFileV2 old;
        memcpy(&old, raw, sizeof(old));
        valid = old.magic == DURAK_SETTINGS_MAGIC &&
                old.checksum == settings_checksum(&old, sizeof(old) - sizeof(old.checksum)) &&
                old.effects_volume <= 10u && old.music_volume <= 10u &&
                old.shadows_enabled <= 1u && old.particle_count >= 1u && old.particle_count <= 20u &&
                old.snow_enabled <= 1u && old.snowflake_count >= 20u && old.snowflake_count <= 40u &&
                old.language < UI_LANG_COUNT && old.table_style <= 2u &&
                old.dialogues_enabled <= 1u;
        if (valid) {
            g_settings.effects_volume = old.effects_volume;
            g_settings.music_volume = old.music_volume;
            g_settings.shadows_enabled = old.shadows_enabled != 0u;
            g_settings.particle_count = old.particle_count;
            g_settings.snow_enabled = old.snow_enabled != 0u;
            g_settings.snowflake_count = old.snowflake_count;
            g_settings.language = old.language;
            g_settings.deck_style = default_deck_style_for_language(old.language);
            g_settings.table_style = old.table_style == 2u
                                     ? TABLE_STYLE_LIGHT_MOSS : TABLE_STYLE_MOSS;
            g_settings.dialogues_enabled = old.dialogues_enabled != 0u;
            g_settings.auto_sort_enabled = true;
            g_settings_dirty = true;
        }
    } else if (got == sizeof(DurakSettingsFileV1) && exact_eof && version == 1u) {
        DurakSettingsFileV1 old;
        memcpy(&old, raw, sizeof(old));
        valid = old.magic == DURAK_SETTINGS_MAGIC &&
                old.checksum == settings_checksum(&old, sizeof(old) - sizeof(old.checksum)) &&
                old.effects_volume <= 10u && old.music_volume <= 10u &&
                old.shadows_enabled <= 1u && old.particle_count >= 1u && old.particle_count <= 20u &&
                old.snow_enabled <= 1u && old.snowflake_count >= 20u && old.snowflake_count <= 40u;
        if (valid) {
            g_settings.effects_volume = old.effects_volume;
            g_settings.music_volume = old.music_volume;
            g_settings.shadows_enabled = old.shadows_enabled != 0u;
            g_settings.particle_count = old.particle_count;
            g_settings.snow_enabled = old.snow_enabled != 0u;
            g_settings.snowflake_count = old.snowflake_count;
            g_settings.table_style = TABLE_STYLE_MOSS;
            g_settings.dialogues_enabled = true;
            g_settings.auto_sort_enabled = true;
            g_settings_dirty = true;
        }
    }
    if (valid && version >= 2u && version <= 6u)
        g_settings.language_complete = true;
    if (!g_settings.cats_unlocked && deck_style_is_cats(g_settings.deck_style)) {
        g_settings.deck_style = default_deck_style_for_language(g_settings.language);
        g_settings_dirty = true;
    }
    if (valid && loaded_legacy_path) g_settings_dirty = true;
    settings_apply();
    return valid;
}

static bool settings_save(void) {
    DurakSettingsFile disk;
    memset(&disk, 0, sizeof(disk));
    disk.magic = DURAK_SETTINGS_MAGIC;
    disk.version = DURAK_SETTINGS_VERSION;
    disk.effects_volume = g_settings.effects_volume;
    disk.music_volume = g_settings.music_volume;
    disk.shadows_enabled = g_settings.shadows_enabled ? 1u : 0u;
    disk.particle_count = g_settings.particle_count;
    disk.snow_enabled = g_settings.snow_enabled ? 1u : 0u;
    disk.snowflake_count = g_settings.snowflake_count;
    disk.language = g_settings.language;
    disk.table_style = g_settings.table_style;
    disk.dialogues_enabled = g_settings.dialogues_enabled ? 1u : 0u;
    disk.auto_sort_enabled = g_settings.auto_sort_enabled ? 1u : 0u;
    disk.deck_style = g_settings.deck_style;
    disk.game_speed = g_settings.game_speed;
    disk.difficulty = g_settings.difficulty;
    disk.profile = (uint8_t)((g_settings.player_face & 1u) |
                             (g_settings.cats_unlocked ? 0x02u : 0u) |
                             (g_settings.language_complete ? 0x04u : 0u) |
                             (g_settings.profile_complete ? 0x80u : 0u));
    disk.checksum = settings_checksum(&disk, sizeof(disk) - sizeof(disk.checksum));

    /* Match working Retro-Go homebrew: write the final relative file directly.
     * fclose() flushes FatFs; no /saves mkdir, temp rename or core syscall patch. */
    FILE* f = fopen(DURAK_SETTINGS_PATH, "wb");
    if (!f) return false;
    bool ok = fwrite(&disk, 1, sizeof(disk), f) == sizeof(disk);
    /* Match the working Retro-Go cores: fwrite() + fclose(), no explicit
     * fflush(FILE*), because this firmware wraps fflush as an fd call. */
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(DURAK_SETTINGS_PATH);
        return false;
    }
    g_settings_dirty = false;
    return true;
}

void duren_system_PreSleep(void) {
    /* Persist menu/options changes even when the console is suspended before
     * the player backs out of the in-game options pages. */
    if (g_settings_dirty) (void)settings_save();
    if (g_live_mode_session && g_live_game &&
        g_live_mode_session->mode == MODE_CAREER)
        (void)career_progress_save(g_live_mode_session, g_live_game);
    /* STOP2 unmounts and power-cycles the microSD. Close persistent pack
     * handles while FatFs is still mounted; the HAL reopens and seeks them
     * after Retro-Go mounts the card again. */
    hal_prepare_retro_go_sleep();
}

typedef struct {
    bool active;
    Card card;
    float cx, cy;       
    float tx, ty;       
    float dx, dy;       
    int frames_left;
    int total_frames;
    bool hide_card;
    bool timing_started;
    uint32_t start_ms;
    uint32_t duration_ms;
    float start_x, start_y;
    float scale;
    uint32_t start_delay_ms;
    bool deal_card;
    bool destination_is_table;
    int8_t landing_player;
    bool landing_committed;
    uint8_t dim_percent;
} VisualAnim;

static uint32_t visual_anim_duration_ms(const VisualAnim* a) {
    const int frames = a && a->total_frames > 0 ? a->total_frames : ANIM_FRAMES;
    uint32_t duration = scaled_motion_ms((uint32_t)frames * 16u);
    if (duration < 64u) duration = 64u;
    return duration;
}

/* Android draws gameplay cards at 120x180 in its baseline layout while the
 * Game & Watch uses the native 40x60 atlas cell.  Its authored 44..66 px
 * surface travel is therefore exactly 14.67..22 px here, rounded to 15..22.
 * The proportional 16% rule remains unchanged. */
static float visual_anim_slide_distance(const VisualAnim* a) {
    if (!a || !a->destination_is_table) return 0.0f;
    const float dx = a->tx - a->start_x;
    const float dy = a->ty - a->start_y;
    const float distance = hypotf(dx, dy);
    if (distance <= 0.5f) return 0.0f;
    float slide = distance * 0.16f;
    if (slide < 15.0f) slide = 15.0f;
    if (slide > 22.0f) slide = 22.0f;
    if (slide > distance * 0.48f) slide = distance * 0.48f;
    return slide;
}

static uint32_t visual_anim_landing_ms(const VisualAnim* a) {
    if (a && a->destination_is_table) {
        const float slide = visual_anim_slide_distance(a);
        /* Preserve the Android 118..148 ms friction timing; only its spatial
         * distance is scaled for the smaller physical card. */
        float duration = 118.0f;
        if (slide > 15.0f) duration += (slide - 15.0f) * (30.0f / 7.0f);
        if (duration < 96.0f) duration = 96.0f;
        if (duration > 148.0f) duration = 148.0f;
        uint32_t scaled = scaled_motion_ms((uint32_t)(duration + 0.5f));
        return scaled < 72u ? 72u : scaled;
    }
    uint32_t duration = scaled_motion_ms(48u);
    return duration < 48u ? 48u : duration;
}

static uint32_t visual_anim_flight_ms(const VisualAnim* a) {
    if (!a || a->duration_ms == 0u || !a->destination_is_table)
        return a ? a->duration_ms : 0u;
    const float distance = hypotf(a->tx - a->start_x, a->ty - a->start_y);
    const float slide = visual_anim_slide_distance(a);
    if (distance <= 1.0f || slide <= 0.0f) return a->duration_ms;
    float fraction = (distance - slide) / distance;
    if (fraction < 0.52f) fraction = 0.52f;
    if (fraction > 0.96f) fraction = 0.96f;
    uint32_t duration = (uint32_t)((float)a->duration_ms * fraction + 0.5f);
    return duration < 64u ? 64u : duration;
}

static float visual_surface_friction(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * (2.0f - t);
}

static void visual_anim_contact_point(const VisualAnim* a,
                                      float* out_x, float* out_y) {
    float x = a ? a->tx : 0.0f;
    float y = a ? a->ty : 0.0f;
    if (a && a->destination_is_table) {
        const float dx = a->tx - a->start_x;
        const float dy = a->ty - a->start_y;
        const float distance = hypotf(dx, dy);
        if (distance > 0.5f) {
            const float slide = visual_anim_slide_distance(a);
            x -= dx / distance * slide;
            y -= dy / distance * slide;
        }
    }
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

/* The first Game & Watch port ended the airborne smoothstep at zero velocity
 * and then started the short surface slide from rest. On the 40x60 cards that
 * pause made the 15..22 px phase look like a static final placement. Match the
 * Android motion derivative at contact with a cubic Hermite terminal tangent,
 * so the card visibly keeps moving and decelerates only on the table. */
static void visual_anim_contact_tangent(const VisualAnim* a,
                                        float contact_x, float contact_y,
                                        float* out_x, float* out_y) {
    float mx = 0.0f;
    float my = 0.0f;
    if (a && a->destination_is_table && a->duration_ms > 0u) {
        const float slide_x = a->tx - contact_x;
        const float slide_y = a->ty - contact_y;
        const float slide_len = hypotf(slide_x, slide_y);
        const float flight_x = contact_x - a->start_x;
        const float flight_y = contact_y - a->start_y;
        const float flight_len = hypotf(flight_x, flight_y);
        const uint32_t landing_ms = visual_anim_landing_ms(a);
        const uint32_t flight_ms = visual_anim_flight_ms(a);
        if (slide_len > 0.5f && flight_len > 0.5f &&
            landing_ms > 0u && flight_ms > 0u) {
            float tangent_len = (2.0f * slide_len * (float)flight_ms) /
                                (float)landing_ms;
            const float safe_max = flight_len * 1.85f;
            if (tangent_len > safe_max) tangent_len = safe_max;
            mx = slide_x / slide_len * tangent_len;
            my = slide_y / slide_len * tangent_len;
        }
    }
    if (out_x) *out_x = mx;
    if (out_y) *out_y = my;
}

static void visual_anim_flight_position(const VisualAnim* a, float t,
                                        float* out_x, float* out_y) {
    if (!a) {
        if (out_x) *out_x = 0.0f;
        if (out_y) *out_y = 0.0f;
        return;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float contact_x = a->tx;
    float contact_y = a->ty;
    visual_anim_contact_point(a, &contact_x, &contact_y);
    if (!a->destination_is_table) {
        const float eased = t * t * (3.0f - 2.0f * t);
        if (out_x) *out_x = a->start_x + (contact_x - a->start_x) * eased;
        if (out_y) *out_y = a->start_y + (contact_y - a->start_y) * eased;
        return;
    }
    float end_mx = 0.0f;
    float end_my = 0.0f;
    visual_anim_contact_tangent(a, contact_x, contact_y, &end_mx, &end_my);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    if (out_x) *out_x = h00 * a->start_x + h01 * contact_x + h11 * end_mx;
    if (out_y) *out_y = h00 * a->start_y + h01 * contact_y + h11 * end_my;
}

static float visual_anim_ease(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float visual_anim_scale(float t) {
    if (t <= 0.0f || t >= 1.0f) return 1.0f;
    const float triangle = t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
    return 1.0f + 0.30f * visual_anim_ease(triangle);
}

static void visual_anim_draw_size(const VisualAnim* a, int* out_w, int* out_h) {
    float scale = a && a->scale >= 1.0f ? a->scale : 1.0f;
    int w = (int)(CARD_WIDTH * scale + 0.5f);
    int h = (int)(CARD_HEIGHT * scale + 0.5f);
    if (w < CARD_WIDTH) w = CARD_WIDTH;
    if (h < CARD_HEIGHT) h = CARD_HEIGHT;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

#define CONFETTI_COUNT 48

typedef struct {
    int16_t x_q4;
    int16_t y_q4;
    int8_t vx_q4;
    uint8_t vy_q4;
    uint8_t color_idx;
    uint8_t shape;
} Confetti;

static const unsigned int confetti_colors[] = {
    0xFFD84A, 0xFF4B4B, 0x48D9FF, 0xFF5CD9,
    0xFF9B42, 0x73F05A, 0x6B7CFF, 0xFFFFFF
};

static void confetti_reset_particle(Confetti* p, bool initial) {
    if (!p) return;
    p->x_q4 = (int16_t)((duren_rand() % SCREEN_WIDTH) << 4);
    if (initial) {
        p->y_q4 = (int16_t)(((duren_rand() % (SCREEN_HEIGHT + 80)) - 80) << 4);
    } else {
        p->y_q4 = (int16_t)(-((duren_rand() % 72) + 4) << 4);
    }
    p->vx_q4 = (int8_t)((duren_rand() % 9) - 4);
    p->vy_q4 = (uint8_t)(9 + (duren_rand() % 13));
    p->color_idx = (uint8_t)(duren_rand() % (sizeof(confetti_colors) / sizeof(confetti_colors[0])));
    p->shape = (uint8_t)(duren_rand() % 3);
}

static void confetti_init(Confetti* particles) {
    for (int i = 0; i < CONFETTI_COUNT; i++) {
        confetti_reset_particle(&particles[i], true);
    }
}

static void confetti_update(Confetti* particles) {
    for (int i = 0; i < CONFETTI_COUNT; i++) {
        Confetti* p = &particles[i];
        p->x_q4 += p->vx_q4;
        p->y_q4 += p->vy_q4;

        int x = p->x_q4 >> 4;
        int y = p->y_q4 >> 4;
        if (x < -3) p->x_q4 = (SCREEN_WIDTH + 2) << 4;
        else if (x > SCREEN_WIDTH + 2) p->x_q4 = -(2 << 4);

        if (y > SCREEN_HEIGHT + 3) {
            confetti_reset_particle(p, false);
        }
    }
}

static void confetti_draw(const Confetti* particles) {
    for (int i = 0; i < CONFETTI_COUNT; i++) {
        const Confetti* p = &particles[i];
        int x = p->x_q4 >> 4;
        int y = p->y_q4 >> 4;
        int w = 2;
        int h = 2;
        if (p->shape == 0) { w = 1; h = 2; }
        else if (p->shape == 1) { w = 2; h = 1; }
        hal_fill_rect(x, y, w, h, confetti_colors[p->color_idx]);
    }
}

void draw_card(HalTexture* cards_tex, Card card, int dx, int dy) {
    int card_index = card_atlas_index(card);
    int sx = card_index * CARD_WIDTH;
    int sy = 0;
    hal_draw_sprite(cards_tex, sx, sy, CARD_WIDTH, CARD_HEIGHT, dx, dy);
}

void draw_card_rotated(HalTexture* cards_tex, Card card, int dx, int dy, double angle) {
    int card_index = card_atlas_index(card);
    int sx = card_index * CARD_WIDTH;
    int sy = 0;
    hal_draw_sprite_rotated(cards_tex, sx, sy, CARD_WIDTH, CARD_HEIGHT, dx, dy, angle);
}


static void card_shadow_offset(int x, int lift, int* sdx, int* sdy) {
    hal_get_shadow_offset(x + CARD_WIDTH / 2, lift, sdx, sdy);
}

/* Card shadows use the sprite alpha mask only. Rectangular contact strips
 * produced black shelves and leaked through the newly transparent corners. */
/* Real-world table pair shadowing:
 * - every table card keeps a downward ground shadow;
 * - only the outer, exposed card on the light-opposite side gets a sideways
 *   cast shadow in left/right light modes;
 * - centered top light keeps straight-down shadows for all table cards.
 */
static void draw_table_card_shadow(HalTexture* cards_tex, Card card, int dx, int dy,
                                   bool allow_side_cast) {
    if (!hal_shadows_enabled()) return;
    int card_index = card_atlas_index(card);
    int sx = card_index * CARD_WIDTH;
    int sdx = 0, sdy = 0;
    hal_get_shadow_offset(dx + NORMAL_TABLE_CARD_W / 2, 0, &sdx, &sdy);
    if (!allow_side_cast) sdx = 0;
    /* Ordinary modes use the exact same native 40x60 face sprite as the hand.
     * No 30x45 battlefield downscale and no second card model. */
    hal_draw_sprite_shadow(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                           dx + sdx, dy + sdy, SHADOW_COLOR);
}

static void draw_table_card_sprite(HalTexture* cards_tex, Card card, int dx, int dy) {
    int card_index = card_atlas_index(card);
    int sx = card_index * CARD_WIDTH;
    /* Pixel-identical to the card in the player's hand. */
    hal_draw_sprite(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT, dx, dy);
    hal_apply_table_lighting_rect(dx, dy, CARD_WIDTH + 8, CARD_HEIGHT + 8);
}

void draw_card_with_shadow(HalTexture* cards_tex, Card card, int dx, int dy) {
    int card_index = card_atlas_index(card);
    int sx = card_index * CARD_WIDTH;
    int sdx, sdy;
    card_shadow_offset(dx, 0, &sdx, &sdy);

    hal_draw_sprite_shadow(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                           dx + sdx, dy + sdy, SHADOW_COLOR);
    hal_draw_sprite(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT, dx, dy);
    hal_apply_table_lighting_rect(dx, dy, CARD_WIDTH + 8, CARD_HEIGHT + 8);
}

static int visual_anim_shadow_lift(const VisualAnim* a) {
    if (!a || a->frames_left <= 0) return 0;
    int total = a->total_frames > 0 ? a->total_frames : ANIM_FRAMES;
    int elapsed = total - a->frames_left;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > total) elapsed = total;
    /* Parabolic 0..8 px arc without floating point in the render path. */
    return (4 * elapsed * (total - elapsed) * 8) / (total * total);
}

static void draw_card_in_flight(HalTexture* cards_tex, Card card, int dx, int dy,
                                int draw_w, int draw_h,
                                const VisualAnim* a) {
    int card_index = card_atlas_index(card);
    int sx = card_index * CARD_WIDTH;
    const int draw_x = dx - (draw_w - CARD_WIDTH) / 2;
    const int draw_y = dy - (draw_h - CARD_HEIGHT) / 2;
    int lift = visual_anim_shadow_lift(a);
    int sdx = 0, sdy = 0;
    hal_get_shadow_offset(draw_x + draw_w / 2, lift, &sdx, &sdy);
    int far_dx = sdx + (sdx > 0 ? 1 : (sdx < 0 ? -1 : 0));
    int far_dy = sdy + 1;

    if (!a || !a->deal_card) {
        hal_draw_sprite_scaled_shadow(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x + far_dx, draw_y + far_dy,
                                      draw_w, draw_h, SHADOW_FAR_COLOR);
        hal_draw_sprite_scaled_shadow(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x + sdx, draw_y + sdy,
                                      draw_w, draw_h, SHADOW_COLOR);
    }
    if (a && a->dim_percent > 0u)
        hal_draw_sprite_scaled_dimmed(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x, draw_y, draw_w, draw_h,
                                      a->dim_percent);
    else
        hal_draw_sprite_scaled(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                               draw_x, draw_y, draw_w, draw_h);
    hal_apply_table_lighting_rect(draw_x, draw_y, draw_w + 14, draw_h + 14);
}

static void draw_back_in_flight(HalTexture* card_back, int x, int y,
                                int draw_w, int draw_h,
                                const VisualAnim* a) {
    const int draw_x = x - (draw_w - CARD_WIDTH) / 2;
    const int draw_y = y - (draw_h - CARD_HEIGHT) / 2;
    int lift = visual_anim_shadow_lift(a);
    int sdx = 0, sdy = 0;
    hal_get_shadow_offset(draw_x + draw_w / 2, lift, &sdx, &sdy);
    int far_dx = sdx + (sdx > 0 ? 1 : (sdx < 0 ? -1 : 0));
    int far_dy = sdy + 1;

    if (!a || !a->deal_card) {
        hal_draw_sprite_scaled_shadow(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x + far_dx, draw_y + far_dy,
                                      draw_w, draw_h, SHADOW_FAR_COLOR);
        hal_draw_sprite_scaled_shadow(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x + sdx, draw_y + sdy,
                                      draw_w, draw_h, SHADOW_COLOR);
    }
    if (a && a->dim_percent > 0u)
        hal_draw_sprite_scaled_dimmed(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x, draw_y, draw_w, draw_h,
                                      a->dim_percent);
    else
        hal_draw_sprite_scaled(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT,
                               draw_x, draw_y, draw_w, draw_h);
    hal_apply_table_lighting_rect(draw_x, draw_y, draw_w + 14, draw_h + 14);
}

/* Adaptive animation quality keeps one shadow when many cards fly. Scaling is
 * still preserved so a large table transfer never changes visual language. */
static void draw_card_in_flight_fast(HalTexture* cards_tex, Card card, int dx, int dy,
                                     int draw_w, int draw_h, const VisualAnim* a) {
    int card_index = card_atlas_index(card);
    int sx = card_index * CARD_WIDTH;
    const int draw_x = dx - (draw_w - CARD_WIDTH) / 2;
    const int draw_y = dy - (draw_h - CARD_HEIGHT) / 2;
    int sdx = 0, sdy = 0;
    hal_get_shadow_offset(draw_x + draw_w / 2, 3, &sdx, &sdy);
    if (!a || !a->deal_card)
        hal_draw_sprite_scaled_shadow(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x + sdx, draw_y + sdy,
                                      draw_w, draw_h, SHADOW_COLOR);
    if (a && a->dim_percent > 0u)
        hal_draw_sprite_scaled_dimmed(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x, draw_y, draw_w, draw_h,
                                      a->dim_percent);
    else
        hal_draw_sprite_scaled(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                               draw_x, draw_y, draw_w, draw_h);
    hal_apply_table_lighting_rect(draw_x, draw_y, draw_w + 12, draw_h + 12);
}

static void draw_back_in_flight_fast(HalTexture* card_back, int x, int y,
                                     int draw_w, int draw_h, const VisualAnim* a) {
    const int draw_x = x - (draw_w - CARD_WIDTH) / 2;
    const int draw_y = y - (draw_h - CARD_HEIGHT) / 2;
    int sdx = 0, sdy = 0;
    hal_get_shadow_offset(draw_x + draw_w / 2, 3, &sdx, &sdy);
    if (!a || !a->deal_card)
        hal_draw_sprite_scaled_shadow(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x + sdx, draw_y + sdy,
                                      draw_w, draw_h, SHADOW_COLOR);
    if (a && a->dim_percent > 0u)
        hal_draw_sprite_scaled_dimmed(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT,
                                      draw_x, draw_y, draw_w, draw_h,
                                      a->dim_percent);
    else
        hal_draw_sprite_scaled(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT,
                               draw_x, draw_y, draw_w, draw_h);
    hal_apply_table_lighting_rect(draw_x, draw_y, draw_w + 12, draw_h + 12);
}

static void draw_normal_trump_android_style(HalTexture* cards_tex, Card card,
                                            int x, int y) {
    int card_index = card_atlas_index(card);
    int sx = card_index * CARD_WIDTH;
    int sdx = 0, sdy = 0;
    hal_get_shadow_offset(x + CARD_WIDTH / 2, 0, &sdx, &sdy);
    hal_draw_sprite_shadow(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                           x + sdx, y + sdy, SHADOW_COLOR);
    hal_draw_sprite(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT, x, y);
    hal_apply_table_lighting_rect(x, y, CARD_WIDTH + 8, CARD_HEIGHT + 8);
}

void draw_back_with_shadow(HalTexture* card_back, int x, int y) {
    int sdx, sdy;
    card_shadow_offset(x, 0, &sdx, &sdy);
    hal_draw_texture_shadow(card_back, x + sdx, y + sdy, SHADOW_COLOR);
    hal_draw_texture(card_back, x, y);
    hal_apply_table_lighting_rect(x, y, CARD_WIDTH + 8, CARD_HEIGHT + 8);
}

static void draw_draw_pile_without_shadow(HalTexture* card_back, int x, int y) {
    if (!card_back) return;
    hal_draw_texture(card_back, x, y);
    hal_apply_table_lighting_rect(x, y, CARD_WIDTH + 2, CARD_HEIGHT + 2);
}

static void draw_trump_suit_symbol(Suit suit, int x, int y) {
    /* Do not crop the suit out of a card face here. Black ink is the
     * transparency key for several indexed atlases, so a cropped black suit
     * can disappear while bits of the white card remain. A tiny procedural
     * mask is deterministic for all six deck atlases and all four suits. */
    static const uint16_t rows[SUIT_COUNT][11] = {
        { /* spades */
            0x020u, 0x070u, 0x0F8u, 0x1FCu, 0x3FEu, 0x7FFu,
            0x0F8u, 0x070u, 0x070u, 0x0F8u, 0x1FCu
        },
        { /* clubs */
            0x070u, 0x0F8u, 0x0F8u, 0x070u, 0x39Cu, 0x7FFu,
            0x7FFu, 0x1FCu, 0x070u, 0x0F8u, 0x1FCu
        },
        { /* diamonds */
            0x020u, 0x070u, 0x0F8u, 0x1FCu, 0x3FEu, 0x1FCu,
            0x0F8u, 0x070u, 0x020u, 0x000u, 0x000u
        },
        { /* hearts */
            0x1DCu, 0x3FEu, 0x7FFu, 0x7FFu, 0x3FEu, 0x1FCu,
            0x0F8u, 0x070u, 0x020u, 0x000u, 0x000u
        }
    };
    if (suit < SUIT_SPADES || suit >= SUIT_COUNT) return;
    const unsigned int colour =
        (suit == SUIT_DIAMONDS || suit == SUIT_HEARTS)
            ? 0xE73545u : 0x211F1Bu;
    for (int row = 0; row < 11; ++row) {
        const uint16_t mask = rows[suit][row];
        int column = 0;
        while (column < 11) {
            while (column < 11 && !(mask & (1u << (10 - column)))) ++column;
            const int start = column;
            while (column < 11 && (mask & (1u << (10 - column)))) ++column;
            if (column > start)
                hal_fill_rect(x + start, y + row, column - start, 1, colour);
        }
    }
}

/* Android R113.50 does not load a separate "trump patch" asset. It builds a
 * white suit badge procedurally 600 ms after the final stock card leaves. The
 * 320x240 port does the same with an atlas-independent 11x11 suit mask, which
 * also avoids black suit pixels colliding with indexed-atlas transparency. */
static void draw_empty_stock_trump_badge(HalTexture* cards_tex, Card trump,
                                         int cards_left, uint32_t now_ms) {
    static uint32_t empty_since_ms = 0u;
    if (cards_left > 0 || !cards_tex || trump.rank < RANK_6) {
        empty_since_ms = 0u;
        return;
    }
    if (empty_since_ms == 0u) {
        empty_since_ms = now_ms ? now_ms : 1u;
        return;
    }
    const uint32_t waited = now_ms - empty_since_ms;
    if (waited < 600u) return;
    uint32_t entry = waited - 600u;
    if (entry > 260u) entry = 260u;
    const uint32_t t = (entry * 1024u) / 260u;
    const uint32_t inv = 1024u - t;
    const uint32_t inv3 = (uint32_t)(((uint64_t)inv * inv * inv) >> 20);
    const uint32_t ease = 1024u - inv3;
    const int target_x = NORMAL_TRUMP_X + 11;
    const int start_x = SCREEN_WIDTH + 2;
    const int x = start_x - (int)(((uint32_t)(start_x - target_x) * ease) >> 10);
    const int y = NORMAL_TRUMP_Y + 21;
    const int badge = 17;
    hal_fill_rect(x, y, badge, badge, 0x3B3228u);
    hal_fill_rect(x + 1, y + 1, badge - 2, badge - 2, 0xF6F1E6u);
    draw_trump_suit_symbol((Suit)trump.suit, x + 3, y + 3);
    hal_apply_table_lighting_rect(x, y, badge, badge);
}

/* Hand cards are always drawn as complete canonical sprites. Overlap is
 * created only by painter order and X spacing. The transparent corner of the
 * upper card therefore reveals the real lower card (or the felt), never a
 * clipped rectangle. */
static void draw_card_sprite_full(HalTexture* cards_tex, Card card,
                                  int dx, int dy) {
    const int card_index = card_atlas_index(card);
    const int sx = card_index * CARD_WIDTH;
    hal_draw_sprite(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT, dx, dy);
}

#define CATS_REWARD_DURATION_MS 5000u

static void draw_cats_reward_flight(HalTexture* cats_tex, uint32_t elapsed_ms) {
    if (!cats_tex || elapsed_ms >= CATS_REWARD_DURATION_MS) return;
    /* All 52 cards cross the LCD in staggered lanes. The last card finishes
     * before the five-second reward screen ends, with no large animation RAM. */
    for (int id = 0; id < DECK_SIZE; ++id) {
        const uint32_t delay = (uint32_t)((id * 107) % 1800);
        const uint32_t duration = 2500u + (uint32_t)(id % 8) * 80u;
        if (elapsed_ms < delay || elapsed_ms >= delay + duration) continue;
        const uint32_t local = elapsed_ms - delay;
        const int x = -CARD_WIDTH +
            (int)((local * (SCREEN_WIDTH + CARD_WIDTH * 2u)) / duration);
        const uint32_t half = duration / 2u;
        const uint32_t arc_phase = local <= half ? local : duration - local;
        const int base_y = 38 + ((id * 47) % 164);
        const int y = base_y - (int)((arc_phase * 26u) / half);
        draw_card_sprite_full(cats_tex, card_from_canonical_id(id), x, y);
    }
}

static void draw_back_sprite_full(HalTexture* card_back, int x, int y) {
    hal_draw_sprite(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT, x, y);
}

/* Selection never changes horizontal fan geometry.  The selected card moves
 * only on Y; all neighbours keep their normal positions. */
static int player_hand_card_x(int count, int selected_idx, int card_idx, bool spread_selected) {
    const int start_x = 10;
    const int span_x = 195;
    (void)selected_idx;
    (void)spread_selected;

    if (count <= 1) return start_x;
    if (card_idx < 0) card_idx = 0;
    if (card_idx >= count) card_idx = count - 1;

    int normal_step = span_x / (count - 1);
    if (normal_step > CARD_WIDTH) normal_step = CARD_WIDTH;
    if (normal_step < 1) normal_step = 1;
    return start_x + card_idx * normal_step;
}

static void card_scroll_reset(void);

static bool card_same(Card a, Card b) {
    return a.rank == b.rank && a.suit == b.suit;
}

static bool auto_sort_player_hand(GameState* game, int* selected_idx, bool play_sound) {
    if (!game || !selected_idx || !g_settings.auto_sort_enabled) return false;
    Player* player = &game->players[0];
    Card selected = {0, 0};
    const bool had_selection = *selected_idx >= 0 && *selected_idx < player->card_count;
    if (had_selection) selected = player->hand[*selected_idx];
    const bool changed = game_sort_hand(player, game->trump_suit);
    if (!changed) return false;
    if (had_selection) {
        for (int i = 0; i < player->card_count; ++i) {
            if (card_same(player->hand[i], selected)) { *selected_idx = i; break; }
        }
    } else if (player->card_count > 0) {
        *selected_idx = 0;
    }
    card_scroll_reset();
    if (play_sound) hal_play_sound_gain(SND_SHUFFLE, 65u);
    return true;
}

static bool auto_sort_br_hand(BattleRoyalState* br, bool play_sound) {
    if (!br || !g_settings.auto_sort_enabled || !br->players[0].present) return false;
    Player* player = &br->players[0].hand;
    Card selected = {0, 0};
    const bool had_selection = br->selected_card >= 0 && br->selected_card < player->card_count;
    if (had_selection) selected = player->hand[br->selected_card];
    const bool changed = game_sort_hand(player, br->trump_suit);
    if (!changed) return false;
    if (had_selection) {
        for (int i = 0; i < player->card_count; ++i) {
            if (card_same(player->hand[i], selected)) { br->selected_card = i; break; }
        }
    } else if (player->card_count > 0) {
        br->selected_card = 0;
    }
    if (play_sound) hal_play_sound_gain(SND_SHUFFLE, 65u);
    return true;
}

void draw_text(HalTexture* font_tex, const char* text, int x, int y) {
    for (int i = 0; text[i] != '\0'; i++) {
        unsigned int ascii = (unsigned char)text[i];
        int sx = (int)(ascii % 16u) * 8;
        int sy = (ascii / 16) * 8;
        hal_draw_sprite(font_tex, sx, sy, 8, 8, x + (i * 8), y);
    }
}

void draw_text_shadowed(HalTexture* font_tex, const char* text, int x, int y) {
    if (!font_tex || !text) return;
    for (int i = 0; text[i] != '\0'; i++) {
        unsigned int ascii = (unsigned char)text[i];
        int sx = (int)(ascii % 16u) * 8;
        int sy = (ascii / 16) * 8;
        hal_draw_sprite_shadow(font_tex, sx, sy, 8, 8, x + (i * 8) + 1, y + 1, 0x000000);
    }
    draw_text(font_tex, text, x, y);
}


static void draw_pause_item(HalTexture* font, const char* label,
                            const char* value, int y, bool selected) {
    if (!font || !label) return;
    if (selected) draw_text(font, ">", 16, y);
    draw_text(font, label, 34, y);
    if (value && *value) {
        int w = (int)strlen(value) * 8;
        draw_text(font, value, SCREEN_WIDTH - 18 - w, y);
    }
}

static void make_slider_text(char out[20], int value, int max_value) {
    if (!out) return;
    if (max_value < 1) max_value = 1;
    if (value < 0) value = 0;
    if (value > max_value) value = max_value;
    int filled = (value * 10 + max_value / 2) / max_value;
    int pos = 0;
    out[pos++] = '[';
    for (int i = 0; i < 10; ++i) out[pos++] = i < filled ? '#' : '-';
    out[pos++] = ']';
    out[pos++] = ' ';
    if (value >= 10) out[pos++] = (char)('0' + (value / 10) % 10);
    out[pos++] = (char)('0' + value % 10);
    out[pos] = '\0';
}

static void make_slider_range_text(char out[20], int value, int min_value, int max_value) {
    if (!out) return;
    if (max_value <= min_value) max_value = min_value + 1;
    if (value < min_value) value = min_value;
    if (value > max_value) value = max_value;
    const int relative = value - min_value;
    const int span = max_value - min_value;
    const int filled = (relative * 10 + span / 2) / span;
    int pos = 0;
    out[pos++] = '[';
    for (int i = 0; i < 10; ++i) out[pos++] = i < filled ? '#' : '-';
    out[pos++] = ']';
    out[pos++] = ' ';
    if (value >= 10) out[pos++] = (char)('0' + (value / 10) % 10);
    out[pos++] = (char)('0' + value % 10);
    out[pos] = '\0';
}

#define FACE_WIDTH  55
#define FACE_HEIGHT 55
#define HUD_FACE_X   (SCREEN_WIDTH - FACE_WIDTH - 8)
#define OPP_FACE_Y   20
#define PLAYER_FACE_Y 174

/* =========================================================
 * NPC DIALOGUE
 * 6x10 font cells are 6x10 pixels in a 16x8 ASCII atlas.
 * A match shows at most six lines, with both time and turn cooldowns.
 * ========================================================= */
typedef struct {
    uint8_t face_index;
    uint8_t trigger;
    uint8_t priority;
    uint8_t turn_serial;
    uint32_t queued_ms;
} DialogueEvent;

#define DIALOGUE_QUEUE_CAPACITY 2
#define DIALOGUE_MAX_ORDINARY_PER_MATCH 12
#define DIALOGUE_MAX_ORDINARY_PER_ROUND 2
#define DIALOGUE_COOLDOWN_MS 7000u
#define DIALOGUE_FACE_COOLDOWN_MS 15000u
#define DIALOGUE_EVENT_STALE_MS 4000u
#define DIALOGUE_STALLING_MS 9500u
#define DIALOGUE_PUFF_MS 170u
#define DIALOGUE_BUBBLE_MIN_WIDTH 104
#define DIALOGUE_BUBBLE_MAX_WIDTH 160
#define DIALOGUE_BUBBLE_MAX_HEIGHT 64
#define DIALOGUE_TEXT_MAX_WIDTH 144
#define DIALOGUE_FONT_WIDTH 6
#define DIALOGUE_FONT_HEIGHT 10
#define DIALOGUE_COMPACT_W 6
#define DIALOGUE_COMPACT_H 10
#define DIALOGUE_LINE_HEIGHT 12
#define DIALOGUE_BUBBLE_PAD_X 7
#define DIALOGUE_BUBBLE_PAD_Y 7
#define DIALOGUE_MAX_LINE_CHARS 48
#define DIALOGUE_MAX_LINES 4

/* Trigger-driven dialogue runtime. Text is resolved only when an event becomes
 * visible, so queued events survive while another bubble is on screen without
 * retaining stale pointers across language changes. */
typedef struct {
    const char* text;
    uint32_t visible_until_ms;
    uint32_t visible_started_ms;
    uint32_t last_shown_ms;
    uint32_t decision_started_ms;
    uint32_t last_by_face_ms[DIALOGUE_CHARACTER_COUNT];
    uint8_t shown_count;
    uint8_t ordinary_shown_count;
    uint8_t round_shown_count;
    uint8_t stalling_match_count;
    uint8_t turn_serial;
    uint8_t queue_count;
    uint8_t active_face;
    uint8_t active_trigger;
    uint8_t used_mask[DIALOGUE_CHARACTER_COUNT][DIALOGUE_TRIGGER_COUNT];
    int8_t last_variant[DIALOGUE_CHARACTER_COUNT][DIALOGUE_TRIGGER_COUNT];
    DialogueEvent queue[DIALOGUE_QUEUE_CAPACITY];
    uint8_t stable_hand_count[2];
    uint8_t player_throw_count;
    uint8_t npc_throw_count;
    bool stalling_fired;
} DialogueState;

static int dialogue_wrap_text(const char* text,
                              char lines[DIALOGUE_MAX_LINES][DIALOGUE_MAX_LINE_CHARS + 1]);

static uint8_t dialogue_priority(DialogueTrigger trigger) {
    switch (trigger) {
        case DIALOGUE_NPC_WINS:
        case DIALOGUE_NPC_LOSES: return 100u;
        case DIALOGUE_PLAYER_LAST_CARD:
        case DIALOGUE_NPC_LAST_CARD: return 90u;
        case DIALOGUE_NPC_DEFENDED:
        case DIALOGUE_NPC_TAKES:
        case DIALOGUE_PLAYER_DEFENDED:
        case DIALOGUE_PLAYER_TAKES: return 80u;
        case DIALOGUE_PLAYER_THROWS_MANY: return 76u;
        case DIALOGUE_PLAYER_THROWS_SEVERAL:
        case DIALOGUE_NPC_THROWS_SEVERAL: return 70u;
        case DIALOGUE_PLAYER_THROWS_ONE:
        case DIALOGUE_NPC_THROWS_ONE: return 64u;
        case DIALOGUE_NPC_STARTS_ATTACK: return 55u;
        case DIALOGUE_GOOD_DRAW:
        case DIALOGUE_BAD_DRAW: return 45u;
        case DIALOGUE_PLAYER_STALLING: return 20u;
        default: return 10u;
    }
}

static bool dialogue_is_critical(DialogueTrigger trigger) {
    return trigger == DIALOGUE_NPC_WINS || trigger == DIALOGUE_NPC_LOSES ||
           trigger == DIALOGUE_PLAYER_LAST_CARD || trigger == DIALOGUE_NPC_LAST_CARD;
}

static bool dialogue_passes_frequency(DialogueTrigger trigger) {
    int chance = 100;
    switch (trigger) {
        case DIALOGUE_NPC_TAKES:
        case DIALOGUE_PLAYER_TAKES: chance = 75; break;
        case DIALOGUE_NPC_DEFENDED:
        case DIALOGUE_PLAYER_DEFENDED:
        case DIALOGUE_NPC_THROWS_SEVERAL:
        case DIALOGUE_PLAYER_THROWS_SEVERAL: chance = 60; break;
        case DIALOGUE_GOOD_DRAW:
        case DIALOGUE_BAD_DRAW: chance = 40; break;
        case DIALOGUE_NPC_STARTS_ATTACK: chance = 30; break;
        case DIALOGUE_PLAYER_THROWS_ONE:
        case DIALOGUE_NPC_THROWS_ONE: chance = 25; break;
        default: chance = 100; break;
    }
    return chance >= 100 || (duren_rand() % 100) < chance;
}

static void dialogue_clear_visible(DialogueState* d) {
    if (!d) return;
    d->text = NULL;
    d->visible_until_ms = 0u;
    d->visible_started_ms = 0u;
    d->active_face = 0u;
    d->active_trigger = 0u;
}

static void dialogue_reset(DialogueState* d) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    memset(d->last_variant, -1, sizeof(d->last_variant));
    d->stable_hand_count[0] = 6u;
    d->stable_hand_count[1] = 6u;
    uint32_t now = hal_get_ticks_ms();
    d->last_shown_ms = now >= DIALOGUE_COOLDOWN_MS
                     ? now - DIALOGUE_COOLDOWN_MS : 0u;
    for (int i = 0; i < DIALOGUE_CHARACTER_COUNT; ++i)
        d->last_by_face_ms[i] = now >= DIALOGUE_FACE_COOLDOWN_MS
                              ? now - DIALOGUE_FACE_COOLDOWN_MS : 0u;
}

static void dialogue_clear_queue(DialogueState* d) {
    if (!d) return;
    d->queue_count = 0u;
    d->player_throw_count = 0u;
    d->npc_throw_count = 0u;
    d->decision_started_ms = 0u;
    d->stalling_fired = false;
    dialogue_clear_visible(d);
}

static void dialogue_advance_turn(DialogueState* d) {
    if (!d) return;
    d->turn_serial++;
    d->round_shown_count = 0u;
    d->decision_started_ms = 0u;
    d->stalling_fired = false;
}

static uint32_t dialogue_duration_ms(const char* text) {
    if (!text) return 3000u;
    int chars = 0;
    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] != '\n' && text[i] != '\r') chars++;
    }
    if (chars <= 10) return 3000u;
    if (chars <= 20) return 4000u;
    if (chars <= 32) return 5000u;
    if (chars <= 46) return 6000u;
    return 7000u;
}

static bool dialogue_queue_event(DialogueState* d, int face_index,
                                 DialogueTrigger trigger) {
    if (!d || !g_settings.dialogues_enabled) return false;
    if (trigger < 0 || trigger >= DIALOGUE_TRIGGER_COUNT) return false;
    if (face_index < 0) face_index = 0;
    face_index %= DIALOGUE_CHARACTER_COUNT;

    if (trigger == DIALOGUE_PLAYER_STALLING) {
        if (d->stalling_fired || d->stalling_match_count >= 2u) return false;
    }
    if (!dialogue_is_critical(trigger) && !dialogue_passes_frequency(trigger))
        return false;

    const uint8_t priority = dialogue_priority(trigger);
    if (trigger == DIALOGUE_NPC_WINS || trigger == DIALOGUE_NPC_LOSES) {
        /* A match result supersedes all stale round comments. */
        dialogue_clear_queue(d);
    }

    for (uint8_t i = 0; i < d->queue_count; ++i) {
        DialogueEvent* e = &d->queue[i];
        if (e->face_index == (uint8_t)face_index &&
            e->trigger == (uint8_t)trigger &&
            e->turn_serial == d->turn_serial)
            return false;
    }

    if (d->queue_count >= DIALOGUE_QUEUE_CAPACITY) {
        uint8_t lowest = 0u;
        for (uint8_t i = 1u; i < d->queue_count; ++i) {
            if (d->queue[i].priority < d->queue[lowest].priority)
                lowest = i;
        }
        if (priority <= d->queue[lowest].priority) return false;
        d->queue[lowest] = d->queue[d->queue_count - 1u];
        d->queue_count--;
    }

    DialogueEvent* e = &d->queue[d->queue_count++];
    e->face_index = (uint8_t)face_index;
    e->trigger = (uint8_t)trigger;
    e->priority = priority;
    e->turn_serial = d->turn_serial;
    e->queued_ms = hal_get_ticks_ms();
    return true;
}

static const DialogueGlyphMetric* active_dialogue_metrics(void) {
    return dialogue_glyph_metrics(ui_language_uses_cp1251(ui_get_language()));
}

static int dialogue_char_advance(unsigned char ch) {
    return active_dialogue_metrics()[ch].advance;
}

static int dialogue_text_width(const char* text) {
    int width = 0;
    if (!text) return 0;
    for (int i = 0; text[i]; ++i) width += dialogue_char_advance((unsigned char)text[i]);
    return width;
}

static int dialogue_word_width(const char* p, int len) {
    int width = 0;
    for (int i = 0; i < len; ++i) width += dialogue_char_advance((unsigned char)p[i]);
    return width;
}

static bool dialogue_choose_variant(DialogueState* d, int face_index,
                                    DialogueTrigger trigger,
                                    const char** out_text) {
    if (!d || !out_text) return false;
    uint8_t mask = d->used_mask[face_index][trigger] & 0x07u;
    if (mask == 0x07u) mask = 0u;

    int choices[DIALOGUE_VARIANTS_PER_TRIGGER];
    int count = 0;
    const int last = d->last_variant[face_index][trigger];
    for (int i = 0; i < DIALOGUE_VARIANTS_PER_TRIGGER; ++i) {
        if ((mask & (1u << i)) == 0u && i != last) choices[count++] = i;
    }
    if (count == 0) {
        for (int i = 0; i < DIALOGUE_VARIANTS_PER_TRIGGER; ++i) {
            if ((mask & (1u << i)) == 0u) choices[count++] = i;
        }
    }
    if (count == 0) return false;

    const int first = duren_rand() % count;
    for (int attempt = 0; attempt < count; ++attempt) {
        const int variant = choices[(first + attempt) % count];
        const char* candidate = dialogue_get_line(ui_get_language(), face_index,
                                                  trigger, variant);
        char wrapped[DIALOGUE_MAX_LINES][DIALOGUE_MAX_LINE_CHARS + 1];
        const int line_count = dialogue_wrap_text(candidate, wrapped);
        if (!candidate || !candidate[0] || line_count < 1 || !wrapped[0][0])
            continue;
        d->used_mask[face_index][trigger] = (uint8_t)(mask | (1u << variant));
        d->last_variant[face_index][trigger] = (int8_t)variant;
        *out_text = candidate;
        return true;
    }
    return false;
}

static void dialogue_update(DialogueState* d, uint32_t now_ms) {
    if (!d) return;
    if (!g_settings.dialogues_enabled) {
        dialogue_clear_queue(d);
        return;
    }
    if (d->text && now_ms < d->visible_until_ms) return;
    if (d->text) dialogue_clear_visible(d);

    /* Expire comments whose situation is no longer fresh. Finals and last-card
     * reactions remain valid and are never dropped by age. */
    for (uint8_t i = 0; i < d->queue_count;) {
        if (d->queue[i].priority < 90u &&
            now_ms - d->queue[i].queued_ms > DIALOGUE_EVENT_STALE_MS) {
            d->queue[i] = d->queue[d->queue_count - 1u];
            d->queue_count--;
        } else {
            ++i;
        }
    }
    if (d->queue_count == 0u) return;

    uint8_t best = 0u;
    for (uint8_t i = 1u; i < d->queue_count; ++i) {
        if (d->queue[i].priority > d->queue[best].priority) best = i;
    }
    const DialogueEvent event = d->queue[best];
    const bool urgent = event.priority >= 90u;
    if (!urgent) {
        if (d->ordinary_shown_count >= DIALOGUE_MAX_ORDINARY_PER_MATCH ||
            d->round_shown_count >= DIALOGUE_MAX_ORDINARY_PER_ROUND) {
            d->queue[best] = d->queue[d->queue_count - 1u];
            d->queue_count--;
            return;
        }
        if (now_ms - d->last_shown_ms < DIALOGUE_COOLDOWN_MS) return;
        if (now_ms - d->last_by_face_ms[event.face_index] < DIALOGUE_FACE_COOLDOWN_MS) return;
    }

    d->queue[best] = d->queue[d->queue_count - 1u];
    d->queue_count--;

    const char* candidate = NULL;
    if (!dialogue_choose_variant(d, event.face_index,
                                 (DialogueTrigger)event.trigger, &candidate))
        return;

    d->text = candidate;
    d->active_face = event.face_index;
    d->active_trigger = event.trigger;
    d->visible_started_ms = now_ms;
    d->visible_until_ms = now_ms + DIALOGUE_PUFF_MS + dialogue_duration_ms(candidate);
    d->last_shown_ms = now_ms;
    d->last_by_face_ms[event.face_index] = now_ms;
    d->shown_count++;
    if (!urgent) {
        d->ordinary_shown_count++;
        d->round_shown_count++;
    }
    if (event.trigger == DIALOGUE_PLAYER_STALLING) d->stalling_match_count++;
}

static void dialogue_stalling_update(DialogueState* d, int face_index,
                                     bool actionable, bool input_happened,
                                     uint32_t now_ms) {
    if (!d || !g_settings.dialogues_enabled || !actionable) {
        if (d) {
            d->decision_started_ms = 0u;
            d->stalling_fired = false;
        }
        return;
    }
    if (input_happened) {
        d->decision_started_ms = now_ms;
        d->stalling_fired = false;
        return;
    }
    if (d->text || d->queue_count > 0u) {
        d->decision_started_ms = now_ms;
        return;
    }
    if (d->decision_started_ms == 0u) d->decision_started_ms = now_ms;
    if (!d->stalling_fired && d->stalling_match_count < 2u &&
        now_ms - d->decision_started_ms >= DIALOGUE_STALLING_MS) {
        if (dialogue_queue_event(d, face_index, DIALOGUE_PLAYER_STALLING))
            d->stalling_fired = true;
    }
}

static int dialogue_wrap_text(const char* text,
                              char lines[DIALOGUE_MAX_LINES][DIALOGUE_MAX_LINE_CHARS + 1]) {
    for (int i = 0; i < DIALOGUE_MAX_LINES; i++) lines[i][0] = '\0';
    if (!text || !text[0]) return 0;

    int line = 0;
    int line_len = 0;
    int line_width = 0;
    const char* p = text;
    while (*p && line < DIALOGUE_MAX_LINES) {
        while (*p == ' ') p++;
        if (!*p) break;

        const char* word = p;
        int word_len = 0;
        while (p[word_len] && p[word_len] != ' ') word_len++;
        const int word_width = dialogue_word_width(word, word_len);
        const int space_width = line_len > 0 ? dialogue_char_advance(' ') : 0;

        if (line_len > 0 && line_width + space_width + word_width > DIALOGUE_TEXT_MAX_WIDTH) {
            line++;
            line_len = 0;
            line_width = 0;
            if (line >= DIALOGUE_MAX_LINES) break;
        }

        if (word_width > DIALOGUE_TEXT_MAX_WIDTH) {
            for (int i = 0; i < word_len && line < DIALOGUE_MAX_LINES; ++i) {
                const int adv = dialogue_char_advance((unsigned char)word[i]);
                if (line_len > 0 && line_width + adv > DIALOGUE_TEXT_MAX_WIDTH) {
                    line++;
                    line_len = 0;
                    line_width = 0;
                    if (line >= DIALOGUE_MAX_LINES) break;
                }
                if (line_len < DIALOGUE_MAX_LINE_CHARS) {
                    lines[line][line_len++] = word[i];
                    lines[line][line_len] = '\0';
                    line_width += adv;
                }
            }
        } else {
            if (line_len > 0 && line_len < DIALOGUE_MAX_LINE_CHARS) {
                lines[line][line_len++] = ' ';
                line_width += space_width;
            }
            int copy = word_len;
            if (copy > DIALOGUE_MAX_LINE_CHARS - line_len)
                copy = DIALOGUE_MAX_LINE_CHARS - line_len;
            memcpy(&lines[line][line_len], word, (size_t)copy);
            line_len += copy;
            lines[line][line_len] = '\0';
            line_width += dialogue_word_width(word, copy);
        }
        p += word_len;
    }
    return line + 1;
}

static void draw_text_6x10(HalTexture* dialogue_font, const char* text, int x, int y) {
    if (!dialogue_font || !text) return;
    int dx = x;
    for (int i = 0; text[i] != '\0'; i++) {
        unsigned int ascii = (unsigned char)text[i];
        int sx = (int)(ascii % 16u) * DIALOGUE_FONT_WIDTH;
        int sy = (int)(ascii / 16u) * DIALOGUE_FONT_HEIGHT;
        /* The atlas already contains a real semibold 6x10 glyph. Drawing it
         * at native size is sharper than the old 7x11 scaled imitation. */
        const DialogueGlyphMetric metric = active_dialogue_metrics()[ascii];
        /* Dialogue glyphs are content, not a graphics shadow.  The old call
         * made all bubble text disappear whenever Shadows was disabled. */
        hal_draw_sprite_tinted(dialogue_font, sx, sy,
                               DIALOGUE_FONT_WIDTH, DIALOGUE_FONT_HEIGHT,
                               dx - metric.left, y, 0x121212u, 100u);
        dx += metric.advance;
    }
}

static void draw_text_6x10_white(HalTexture* font, const char* text, int x, int y) {
    if (!font || !text) return;
    int dx = x;
    const DialogueGlyphMetric* metrics = active_dialogue_metrics();
    for (int i = 0; text[i] != '\0'; ++i) {
        const unsigned int ch = (unsigned char)text[i];
        const int sx = (int)(ch % 16u) * DIALOGUE_FONT_WIDTH;
        const int sy = (int)(ch / 16u) * DIALOGUE_FONT_HEIGHT;
        const int glyph_x = dx - metrics[ch].left;
        hal_draw_sprite_shadow(font, sx, sy, DIALOGUE_FONT_WIDTH, DIALOGUE_FONT_HEIGHT,
                               glyph_x + 1, y + 1, 0x000000);
        hal_draw_sprite(font, sx, sy, DIALOGUE_FONT_WIDTH, DIALOGUE_FONT_HEIGHT,
                        glyph_x, y);
        dx += metrics[ch].advance;
    }
}

static void draw_text_6x10_centered(HalTexture* font, const char* text,
                                    int center_x, int y, int max_chars) {
    if (!font || !text) return;
    char clipped[24];
    if (max_chars < 1) return;
    if (max_chars > (int)sizeof(clipped) - 1) max_chars = (int)sizeof(clipped) - 1;
    snprintf(clipped, sizeof(clipped), "%.*s", max_chars, text);
    const int width = dialogue_text_width(clipped);
    draw_text_6x10_white(font, clipped, center_x - width / 2, y);
}

#define ACTION_TAKE_HOLD_MS 280u
#define BR_FAST_HOLD_MS 250u
#define BR_FAST_MULTIPLIER 4u
#define ACTION_HINT_VISIBLE_MS 450u
#define ACTION_HINT_HIDDEN_MS 250u

typedef struct {
    bool armed;
    bool fired;
    uint32_t started_ms;
} ActionHoldState;

static void action_hold_reset(ActionHoldState* state) {
    if (!state) return;
    state->armed = false;
    state->fired = false;
    state->started_ms = 0u;
}

static bool action_take_hold_update(ActionHoldState* state, bool available,
                                    uint32_t now_ms) {
    if (!state) return false;
    if (!available) {
        action_hold_reset(state);
        return false;
    }
    if (hal_is_button_pressed(BTN_B)) {
        state->armed = true;
        state->fired = false;
        state->started_ms = now_ms;
    }
    if (!hal_is_button_held(BTN_B)) {
        if (state->armed && !state->fired) action_hold_reset(state);
        return false;
    }
    if (state->armed && !state->fired &&
        now_ms - state->started_ms >= ACTION_TAKE_HOLD_MS) {
        state->fired = true;
        return true;
    }
    return false;
}

static bool action_fast_hold_update(ActionHoldState* state, bool available,
                                    uint32_t now_ms) {
    if (!state) return false;
    if (!available) {
        action_hold_reset(state);
        return false;
    }
    if (hal_is_button_pressed(BTN_A)) {
        state->armed = true;
        state->fired = false;
        state->started_ms = now_ms;
    }
    if (!hal_is_button_held(BTN_A)) {
        action_hold_reset(state);
        return false;
    }
    if (state->armed && !state->fired &&
        now_ms - state->started_ms >= BR_FAST_HOLD_MS)
        state->fired = true;
    return state->armed && state->fired;
}

static bool action_hint_visible(uint32_t now_ms, bool take_available,
                                const ActionHoldState* state) {
    if (take_available && state && state->armed && hal_is_button_held(BTN_B))
        return true;
    const uint32_t period = ACTION_HINT_VISIBLE_MS + ACTION_HINT_HIDDEN_MS;
    return (now_ms % period) < ACTION_HINT_VISIBLE_MS;
}

static void draw_action_hint_centered(HalTexture* font, const char* text,
                                      int center_x, int y) {
    if (!font || !text || !*text) return;
    const int w = (int)strlen(text) * DIALOGUE_FONT_WIDTH;
    const int x = center_x - w / 2;
    for (int i = 0; text[i] != '\0'; ++i) {
        const unsigned int ch = (unsigned char)text[i];
        const int sx = (int)(ch % 16u) * DIALOGUE_FONT_WIDTH;
        const int sy = (int)(ch / 16u) * DIALOGUE_FONT_HEIGHT;
        hal_draw_sprite_shadow(font, sx, sy, DIALOGUE_FONT_WIDTH,
                               DIALOGUE_FONT_HEIGHT,
                               x + i * DIALOGUE_FONT_WIDTH + 1, y + 1, 0x000000);
        hal_draw_sprite(font, sx, sy, DIALOGUE_FONT_WIDTH,
                        DIALOGUE_FONT_HEIGHT,
                        x + i * DIALOGUE_FONT_WIDTH, y);
    }
}

static void draw_action_hint(HalTexture* font, const char* text, int y) {
    draw_action_hint_centered(font, text, SCREEN_WIDTH / 2, y);
}

#define BR_HINT_CENTER_X 282
#define BR_HINT_MAX_CHARS 12
#define BR_HINT_LINE1_Y 151
#define BR_HINT_LINE2_Y 162
#define BR_HINT_SINGLE_Y 158

static void br_hint_copy_trimmed(char* dst, size_t dst_size,
                                 const char* begin, size_t length) {
    if (!dst || dst_size == 0u) return;
    while (length > 0u && (*begin == ' ' || *begin == '-')) {
        ++begin;
        --length;
    }
    while (length > 0u && (begin[length - 1u] == ' ' || begin[length - 1u] == '-'))
        --length;
    if (length >= dst_size) length = dst_size - 1u;
    memcpy(dst, begin, length);
    dst[length] = '\0';
}

/* Battle Royal reserves the narrow right HUD lane below the deck/trump and
 * above the human balance. Full localized fast-forward strings are split into
 * two compact lines; B actions remain one line. */
static void draw_br_action_hint(HalTexture* font, const char* text) {
    if (!font || !text || !*text) return;
    char line1[BR_HINT_MAX_CHARS + 1] = {0};
    char line2[BR_HINT_MAX_CHARS + 1] = {0};
    const size_t len = strlen(text);
    const char* dash = strstr(text, " - ");

    if (dash) {
        br_hint_copy_trimmed(line1, sizeof(line1), text, (size_t)(dash - text));
        br_hint_copy_trimmed(line2, sizeof(line2), dash + 3, strlen(dash + 3));
    } else if (len <= BR_HINT_MAX_CHARS) {
        br_hint_copy_trimmed(line1, sizeof(line1), text, len);
    } else {
        size_t split = BR_HINT_MAX_CHARS;
        while (split > 0u && text[split] != ' ') --split;
        if (split == 0u) split = BR_HINT_MAX_CHARS;
        br_hint_copy_trimmed(line1, sizeof(line1), text, split);
        br_hint_copy_trimmed(line2, sizeof(line2), text + split, len - split);
    }

    if (line2[0] == '\0') {
        draw_action_hint_centered(font, line1, BR_HINT_CENTER_X, BR_HINT_SINGLE_Y);
    } else {
        draw_action_hint_centered(font, line1, BR_HINT_CENTER_X, BR_HINT_LINE1_Y);
        draw_action_hint_centered(font, line2, BR_HINT_CENTER_X, BR_HINT_LINE2_Y);
    }
}

static int dialogue_round_inset(int edge) {
    if (edge <= 0) return 7;
    if (edge == 1) return 4;
    if (edge == 2) return 2;
    if (edge == 3) return 1;
    return 0;
}

static void dialogue_pill_fill(int x, int y, int w, int h,
                               unsigned int color) {
    if (w < 16 || h < 12) return;
    for (int row = 0; row < h; ++row) {
        int edge = row;
        if (h - 1 - row < edge) edge = h - 1 - row;
        const int inset = dialogue_round_inset(edge);
        const int span = w - inset * 2;
        if (span > 0) hal_fill_rect(x + inset, y + row, span, 1, color);
    }
}

/* One-pixel black outline with a clean white interior. The body is symmetric;
 * the tail is mirrored as a whole when the speaker is on the other side. */
static void dialogue_pill(int x, int y, int w, int h) {
    dialogue_pill_fill(x, y, w, h, 0x090909u);
    dialogue_pill_fill(x + 1, y + 1, w - 2, h - 2, 0xFFFFFFu);
}

static void dialogue_short_tail(int body_x, int body_w, int center_y,
                                bool points_right) {
    const unsigned int outline = 0x090909u;
    const unsigned int fill = 0xFFFFFFu;
    if (points_right) {
        const int x = body_x + body_w - 1;
        hal_fill_rect(x, center_y - 4, 3, 9, outline);
        hal_fill_rect(x + 2, center_y - 3, 3, 7, outline);
        hal_fill_rect(x + 4, center_y - 2, 5, 5, outline);
        hal_fill_rect(x - 1, center_y - 3, 4, 7, fill);
        hal_fill_rect(x + 3, center_y - 2, 2, 5, fill);
        hal_fill_rect(x + 5, center_y - 1, 3, 3, fill);
    } else {
        const int x = body_x;
        hal_fill_rect(x - 2, center_y - 4, 3, 9, outline);
        hal_fill_rect(x - 4, center_y - 3, 3, 7, outline);
        hal_fill_rect(x - 8, center_y - 2, 5, 5, outline);
        hal_fill_rect(x - 2, center_y - 3, 4, 7, fill);
        hal_fill_rect(x - 3, center_y - 2, 2, 5, fill);
        hal_fill_rect(x - 7, center_y - 1, 3, 3, fill);
    }
}

static int dialogue_puff_percent(uint32_t elapsed) {
    if (elapsed < 35u) return 30;
    if (elapsed < 75u) return 58;
    if (elapsed < 115u) return 82;
    if (elapsed < 145u) return 106;
    return 100;
}

static void dialogue_draw_text_bubble(const char* text, uint32_t started_ms,
                                      HalTexture* dialogue_font) {
    if (!text || !text[0] || !dialogue_font) return;
    const uint32_t now = hal_get_ticks_ms();

    char wrapped[DIALOGUE_MAX_LINES][DIALOGUE_MAX_LINE_CHARS + 1];
    int line_count = dialogue_wrap_text(text, wrapped);
    if (line_count < 1 || !wrapped[0][0]) return;
    if (line_count > DIALOGUE_MAX_LINES) line_count = DIALOGUE_MAX_LINES;

    int max_text_w = 0;
    for (int i = 0; i < line_count; ++i) {
        const int w = dialogue_text_width(wrapped[i]);
        if (w > max_text_w) max_text_w = w;
    }
    int final_w = max_text_w + DIALOGUE_BUBBLE_PAD_X * 2;
    if (final_w < DIALOGUE_BUBBLE_MIN_WIDTH) final_w = DIALOGUE_BUBBLE_MIN_WIDTH;
    if (final_w > DIALOGUE_BUBBLE_MAX_WIDTH) final_w = DIALOGUE_BUBBLE_MAX_WIDTH;
    const int text_block_h = line_count * DIALOGUE_COMPACT_H +
                             (line_count - 1) *
                             (DIALOGUE_LINE_HEIGHT - DIALOGUE_COMPACT_H);
    int final_h = text_block_h + DIALOGUE_BUBBLE_PAD_Y * 2;
    if (final_h > DIALOGUE_BUBBLE_MAX_HEIGHT) final_h = DIALOGUE_BUBBLE_MAX_HEIGHT;

    const int portrait_x = HUD_FACE_X;
    const int portrait_y = OPP_FACE_Y;
    const int center_y = portrait_y + FACE_HEIGHT / 2;
    const bool portrait_left = portrait_x + FACE_WIDTH / 2 < SCREEN_WIDTH / 2;
    const int gap = 7;
    const int target_x = portrait_left ? portrait_x + FACE_WIDTH : portrait_x;
    const uint32_t elapsed = now - started_ms;
    const int pct = dialogue_puff_percent(elapsed);
    int bubble_w = final_w * pct / 100;
    int bubble_h = final_h * pct / 100;
    if (bubble_w < 14) bubble_w = 14;
    if (bubble_h < 10) bubble_h = 10;
    int bubble_x = portrait_left ? target_x + gap : target_x - gap - bubble_w;
    int bubble_y = center_y - bubble_h / 2;
    if (bubble_x < 1) bubble_x = 1;
    if (bubble_x + bubble_w >= SCREEN_WIDTH) bubble_x = SCREEN_WIDTH - bubble_w - 1;
    if (bubble_y < 1) bubble_y = 1;
    if (bubble_y + bubble_h >= SCREEN_HEIGHT) bubble_y = SCREEN_HEIGHT - bubble_h - 1;

    int tail_y = center_y;
    if (tail_y < bubble_y + 8) tail_y = bubble_y + 8;
    if (tail_y > bubble_y + bubble_h - 9) tail_y = bubble_y + bubble_h - 9;
    dialogue_pill(bubble_x, bubble_y, bubble_w, bubble_h);
    dialogue_short_tail(bubble_x, bubble_w, tail_y, !portrait_left);
    if (elapsed < DIALOGUE_PUFF_MS) return;

    const int text_y = bubble_y + (bubble_h - text_block_h) / 2;
    for (int i = 0; i < line_count; ++i) {
        const int text_w = dialogue_text_width(wrapped[i]);
        draw_text_6x10(dialogue_font, wrapped[i],
                       bubble_x + (bubble_w - text_w) / 2,
                       text_y + i * DIALOGUE_LINE_HEIGHT);
    }
}

static void dialogue_draw(DialogueState* d, HalTexture* bubble_tex,
                          HalTexture* dialogue_font) {
    (void)bubble_tex;
    if (!d || !dialogue_font) return;
    const uint32_t now = hal_get_ticks_ms();
    dialogue_update(d, now);
    if (!d->text || !d->text[0] || now >= d->visible_until_ms) return;
    dialogue_draw_text_bubble(d->text, d->visible_started_ms, dialogue_font);
}

static int dialogue_table_card_count(const GameState* game) {
    int count = 0;
    if (!game) return 0;
    for (int i = 0; i < game->table_pair_count; i++) {
        if (game->table_attack[i].rank >= 6) count++;
        if (game->table_defense[i].rank >= 6) count++;
    }
    return count;
}

static void dialogue_note_throw(DialogueState* d, bool by_player) {
    if (!d) return;
    uint8_t* count = by_player ? &d->player_throw_count : &d->npc_throw_count;
    if (*count < 255u) (*count)++;
}

static void dialogue_queue_throw_summary(DialogueState* d, int face_index) {
    if (!d) return;
    if (d->player_throw_count == 1u)
        dialogue_queue_event(d, face_index, DIALOGUE_PLAYER_THROWS_ONE);
    else if (d->player_throw_count >= 4u)
        dialogue_queue_event(d, face_index, DIALOGUE_PLAYER_THROWS_MANY);
    else if (d->player_throw_count >= 2u)
        dialogue_queue_event(d, face_index, DIALOGUE_PLAYER_THROWS_SEVERAL);

    if (d->npc_throw_count == 1u)
        dialogue_queue_event(d, face_index, DIALOGUE_NPC_THROWS_ONE);
    else if (d->npc_throw_count >= 2u)
        dialogue_queue_event(d, face_index, DIALOGUE_NPC_THROWS_SEVERAL);

    d->player_throw_count = 0u;
    d->npc_throw_count = 0u;
}

static void dialogue_react_to_ai_draw(DialogueState* d, const GameState* game,
                                      int first_drawn_index) {
    if (!d || !game) return;
    const int end = game->players[1].card_count;
    if (first_drawn_index < 0) first_drawn_index = 0;
    if (first_drawn_index >= end) return;

    int score = 0;
    int count = 0;
    for (int i = first_drawn_index; i < end; i++) {
        Card c = game->players[1].hand[i];
        if (c.suit == game->trump_suit) score += 4;
        if (c.rank == RANK_A) score += 4;
        else if (c.rank == RANK_K) score += 3;
        else if (c.rank == RANK_Q) score += 2;
        else if (c.rank == RANK_J || c.rank == RANK_10) score += 1;
        else if (card_rank_strength_u8(c.rank) <= 7) score -= 2;
        count++;
    }

    const int face = game->current_opponent.face_index;
    if (count > 0 && score >= count * 2)
        dialogue_queue_event(d, face, DIALOGUE_GOOD_DRAW);
    else if (count > 0 && score <= 0)
        dialogue_queue_event(d, face, DIALOGUE_BAD_DRAW);
}

static void dialogue_check_last_cards(DialogueState* d, const GameState* game) {
    if (!d || !game) return;
    const bool deck_empty = game->deck.top_index >= DECK_SIZE;
    const uint8_t player_now = (uint8_t)game->players[0].card_count;
    const uint8_t npc_now = (uint8_t)game->players[1].card_count;
    const int face = game->current_opponent.face_index;

    if (deck_empty && d->stable_hand_count[0] >= 2u && player_now == 1u)
        dialogue_queue_event(d, face, DIALOGUE_PLAYER_LAST_CARD);
    if (deck_empty && d->stable_hand_count[1] >= 2u && npc_now == 1u)
        dialogue_queue_event(d, face, DIALOGUE_NPC_LAST_CARD);

    d->stable_hand_count[0] = player_now;
    d->stable_hand_count[1] = npc_now;
}

static void dialogue_sync_hand_counts(DialogueState* d, const GameState* game) {
    if (!d || !game) return;
    d->stable_hand_count[0] = (uint8_t)game->players[0].card_count;
    d->stable_hand_count[1] = (uint8_t)game->players[1].card_count;
}

static unsigned int card_action_alpha(uint32_t now_ms) {
    const uint32_t phase = now_ms % 900u;
    const uint32_t tri = phase < 450u ? phase : 900u - phase;
    return 24u + (unsigned int)((tri * 22u) / 450u);
}

static void draw_card_action_mask(HalTexture* cards_tex, Card card,
                                  int x, int y, uint32_t now_ms) {
    if (!cards_tex || card.rank < RANK_6) return;
    const int card_index = card_atlas_index(card);
    hal_draw_sprite_tinted(cards_tex, card_index * CARD_WIDTH, 0,
                           CARD_WIDTH, CARD_HEIGHT, x, y,
                           0xF07818u, card_action_alpha(now_ms));
}

static bool player_has_legal_throw_rank(const GameState* game, int rank) {
    if (!game) return false;
    for (int i = 0; i < game->players[0].card_count; ++i) {
        const Card c = game->players[0].hand[i];
        if (c.rank == rank && game_can_attack_with((GameState*)game, c)) return true;
    }
    return false;
}

static bool player_has_any_legal_throw(const GameState* game) {
    if (!game || game->table_pair_count <= 0) return false;
    for (int i = 0; i < game->players[0].card_count; ++i) {
        if (game_can_attack_with((GameState*)game, game->players[0].hand[i]))
            return true;
    }
    return false;
}

typedef enum {
    NORMAL_RESOLVE_NONE = 0,
    NORMAL_RESOLVE_DISCARD,
    NORMAL_RESOLVE_NPC_TAKE
} NormalResolveAction;

static NormalResolveAction normal_auto_resolve_action(const GameState* game,
                                                      bool npc_taking) {
    if (!game || game->table_pair_count <= 0) return NORMAL_RESOLVE_NONE;

    /* When the NPC defender has declared TAKE, the human attacker may still
     * throw ranks already present on the table.  Once that legal set is empty,
     * no confirmation is required and the transfer must start automatically. */
    if (npc_taking) {
        return player_has_any_legal_throw(game)
             ? NORMAL_RESOLVE_NONE : NORMAL_RESOLVE_NPC_TAKE;
    }

    /* A fully covered table belongs to the discard pile as soon as the current
     * attacker has no legal continuation.  This applies symmetrically when the
     * human attacks and when the NPC attacks; previously only the human-attacker
     * branch was checked directly, leaving some NPC-attacker bouts on [B] BEAT. */
    if (game_should_auto_discard(game))
        return NORMAL_RESOLVE_DISCARD;

    return NORMAL_RESOLVE_NONE;
}

#define NORMAL_RESOLVE_DELAY_MS 500u

static void begin_discard_animation(GameState* game, VisualAnim anims[MAX_ANIMS],
                                    int* anim_phase, bool* phase_took,
                                    bool* hide_table) {
    if (!game || !anims || !anim_phase || !phase_took || !hide_table) return;
    *anim_phase = 1;
    *phase_took = false;
    *hide_table = true;
    memset(anims, 0, sizeof(VisualAnim) * MAX_ANIMS);
    int anim_idx = 0;
    for (int k = 0; k < game->table_pair_count && anim_idx < MAX_ANIMS; ++k) {
        anims[anim_idx].active = true;
        anims[anim_idx].timing_started = false;
        anims[anim_idx].card = game->table_attack[k];
        anims[anim_idx].cx = (float)normal_table_x(game, k, false);
        anims[anim_idx].cy = (float)normal_table_y(game, k, false);
        anims[anim_idx].tx = -60.0f;
        anims[anim_idx].ty = 100.0f;
        anims[anim_idx].frames_left = ANIM_FRAMES;
        anims[anim_idx].total_frames = ANIM_FRAMES;
        anims[anim_idx].hide_card = false;
        ++anim_idx;
        if (game->table_defense[k].rank >= RANK_6 && anim_idx < MAX_ANIMS) {
            anims[anim_idx].active = true;
            anims[anim_idx].timing_started = false;
            anims[anim_idx].card = game->table_defense[k];
            anims[anim_idx].cx = (float)normal_table_x(game, k, true);
            anims[anim_idx].cy = (float)normal_table_y(game, k, true);
            anims[anim_idx].tx = -60.0f;
            anims[anim_idx].ty = 100.0f;
            anims[anim_idx].frames_left = ANIM_FRAMES;
            anims[anim_idx].total_frames = ANIM_FRAMES;
            anims[anim_idx].hide_card = false;
            ++anim_idx;
        }
    }
}

static void begin_npc_take_animation(GameState* game, VisualAnim anims[MAX_ANIMS],
                                     int* anim_phase, bool* phase_took,
                                     bool* hide_table) {
    if (!game || !anims || !anim_phase || !phase_took || !hide_table) return;
    *anim_phase = 1;
    *phase_took = true;
    *hide_table = true;
    memset(anims, 0, sizeof(VisualAnim) * MAX_ANIMS);

    int taken_count = 0;
    for (int k = 0; k < game->table_pair_count; ++k) {
        if (game->table_attack[k].rank >= RANK_6) ++taken_count;
        if (game->table_defense[k].rank >= RANK_6) ++taken_count;
    }
    int future_count = game->players[1].card_count + taken_count;
    if (future_count < 1) future_count = 1;
    int step = future_count > 1 ? 180 / (future_count - 1) : CARD_WIDTH;
    if (step > CARD_WIDTH) step = CARD_WIDTH;
    if (step < 1) step = 1;

    int anim_idx = 0;
    int target_slot = game->players[1].card_count;
    for (int k = 0; k < game->table_pair_count && anim_idx < MAX_ANIMS; ++k) {
        anims[anim_idx].active = true;
        anims[anim_idx].timing_started = false;
        anims[anim_idx].card = game->table_attack[k];
        anims[anim_idx].cx = (float)normal_table_x(game, k, false);
        anims[anim_idx].cy = (float)normal_table_y(game, k, false);
        anims[anim_idx].tx = (float)(10 + target_slot * step);
        anims[anim_idx].ty = 10.0f;
        anims[anim_idx].frames_left = ANIM_FRAMES;
        anims[anim_idx].total_frames = ANIM_FRAMES;
        anims[anim_idx].hide_card = false;
        ++target_slot;
        ++anim_idx;
        if (game->table_defense[k].rank >= RANK_6 && anim_idx < MAX_ANIMS) {
            anims[anim_idx].active = true;
            anims[anim_idx].timing_started = false;
            anims[anim_idx].card = game->table_defense[k];
            anims[anim_idx].cx = (float)normal_table_x(game, k, true);
            anims[anim_idx].cy = (float)normal_table_y(game, k, true);
            anims[anim_idx].tx = (float)(10 + target_slot * step);
            anims[anim_idx].ty = 10.0f;
            anims[anim_idx].frames_left = ANIM_FRAMES;
            anims[anim_idx].total_frames = ANIM_FRAMES;
            anims[anim_idx].hide_card = false;
            ++target_slot;
            ++anim_idx;
        }
    }
}

static int begin_game_turn_resolution(GameState* game, bool took,
                                      DialogueState* dialogue,
                                      int* first_draw_player,
                                      int* second_draw_player) {
    if (!game || !dialogue) return 0;
    const bool human_attacked = game->is_player_turn;
    const int face = game->current_opponent.face_index;
    int ai_draw_start = game->players[1].card_count;
    const int defender_idx = human_attacked ? 1 : 0;
    const int attacker_idx = human_attacked ? 0 : 1;
    if (took && defender_idx == 1)
        ai_draw_start += dialogue_table_card_count(game);

    dialogue_queue_throw_summary(dialogue, face);
    if (human_attacked) {
        dialogue_queue_event(dialogue, face,
            took ? DIALOGUE_NPC_TAKES : DIALOGUE_NPC_DEFENDED);
    } else {
        dialogue_queue_event(dialogue, face,
            took ? DIALOGUE_PLAYER_TAKES : DIALOGUE_PLAYER_DEFENDED);
    }

    if (first_draw_player) *first_draw_player = attacker_idx;
    if (second_draw_player) *second_draw_player = defender_idx;
    game_end_turn_no_deal(game, took);
    return ai_draw_start;
}

static void finish_game_turn_after_deal(GameState* game,
                                        DialogueState* dialogue,
                                        int ai_draw_start) {
    if (!game || !dialogue) return;
    const int face = game->current_opponent.face_index;
    game_check_status(game);
    normal_layout_capacity_cache = 0;
    dialogue_react_to_ai_draw(dialogue, game, ai_draw_start);
    dialogue_check_last_cards(dialogue, game);

    if (game->result == RESULT_WIN)
        dialogue_queue_event(dialogue, face, DIALOGUE_NPC_LOSES);
    else if (game->result == RESULT_LOSE)
        dialogue_queue_event(dialogue, face, DIALOGUE_NPC_WINS);

    dialogue_advance_turn(dialogue);
}

/* Reserve cards without exposing them in the logical hand. Each card owns its
 * destination slot until landing; only then is it appended to the hand. This
 * is the small-screen equivalent of the HD presentation ownership model and
 * prevents a static duplicate from appearing underneath a flying card. */
static int begin_normal_deal_animation(GameState* game,
                                       VisualAnim anims[MAX_ANIMS],
                                       int first_player,
                                       int second_player) {
    if (!game || !anims) return 0;
    Card reserved[2][6] = {0};
    int counts[2] = {0, 0};
    const int total = game_reserve_draw_round_robin(
        game, first_player, second_player, reserved, counts);
    memset(anims, 0, sizeof(VisualAnim) * MAX_ANIMS);
    if (total <= 0) return 0;

    const int final_count[2] = {
        game->players[0].card_count + counts[0],
        game->players[1].card_count + counts[1]
    };
    const int order[2] = {first_player, second_player};
    int used[2] = {0, 0};
    int sequence = 0;
    bool queued = true;
    while (queued && sequence < MAX_ANIMS) {
        queued = false;
        for (int oi = 0; oi < 2 && sequence < MAX_ANIMS; ++oi) {
            const int pidx = order[oi];
            if (pidx < 0 || pidx > 1 || used[pidx] >= counts[pidx]) continue;
            const int slot = game->players[pidx].card_count + used[pidx];
            const int span = pidx == 0 ? 195 : 180;
            int step = final_count[pidx] > 1 ? span / (final_count[pidx] - 1)
                                             : CARD_WIDTH;
            if (step > CARD_WIDTH) step = CARD_WIDTH;
            if (step < 1) step = 1;

            VisualAnim* a = &anims[sequence];
            a->active = true;
            a->card = reserved[pidx][used[pidx]++];
            a->hide_card = true;
            a->cx = 250.0f;
            a->cy = 110.0f;
            a->tx = (float)(10 + slot * step);
            a->ty = pidx == 0 ? 160.0f : 10.0f;
            a->frames_left = DEAL_ANIM_FRAMES;
            a->total_frames = DEAL_ANIM_FRAMES;
            a->start_delay_ms = scaled_motion_ms((uint32_t)sequence * 120u);
            a->deal_card = true;
            a->landing_player = (int8_t)pidx;
            queued = true;
            ++sequence;
        }
    }
    return sequence;
}

/* game_deal_cards() must run first because it also selects the opening
 * attacker from the lowest trump.  Move those already-decided cards into the
 * visual transaction, then commit each one back to its hand only when its
 * flight lands.  The deck top and opening-turn decision remain untouched. */
static int begin_initial_normal_deal_animation(GameState* game,
                                               VisualAnim anims[MAX_ANIMS]) {
    if (!game || !anims) return 0;
    Card dealt[2][MAX_HAND];
    int counts[2] = {game->players[0].card_count, game->players[1].card_count};
    for (int p = 0; p < 2; ++p) {
        if (counts[p] < 0) counts[p] = 0;
        if (counts[p] > MAX_HAND) counts[p] = MAX_HAND;
        for (int i = 0; i < counts[p]; ++i) dealt[p][i] = game->players[p].hand[i];
        game->players[p].card_count = 0;
    }

    memset(anims, 0, sizeof(VisualAnim) * MAX_ANIMS);
    int used[2] = {0, 0};
    int sequence = 0;
    bool queued = true;
    while (queued && sequence < MAX_ANIMS) {
        queued = false;
        /* Alternate cards exactly like a physical deal. */
        for (int pidx = 0; pidx < 2 && sequence < MAX_ANIMS; ++pidx) {
            if (used[pidx] >= counts[pidx]) continue;
            const int slot = used[pidx];
            const int span = pidx == 0 ? 195 : 180;
            int step = counts[pidx] > 1 ? span / (counts[pidx] - 1) : CARD_WIDTH;
            if (step > CARD_WIDTH) step = CARD_WIDTH;
            if (step < 1) step = 1;

            VisualAnim* a = &anims[sequence];
            a->active = true;
            a->card = dealt[pidx][used[pidx]++];
            a->hide_card = true;
            a->cx = 250.0f;
            a->cy = 110.0f;
            a->tx = (float)(10 + slot * step);
            a->ty = pidx == 0 ? 160.0f : 10.0f;
            a->frames_left = DEAL_ANIM_FRAMES;
            a->total_frames = DEAL_ANIM_FRAMES;
            a->start_delay_ms = scaled_motion_ms((uint32_t)sequence * 120u);
            a->deal_card = true;
            a->landing_player = (int8_t)pidx;
            queued = true;
            ++sequence;
        }
    }
    return sequence;
}


#define MODE_STAR_MAX 5

// Falling mode-menu stars. Coordinates are 8.8 fixed-point so movement
// remains smooth even when the visible center only changes by whole pixels.
typedef struct {
    int32_t x_fp;
    int32_t y_fp;
    int16_t vx_fp;
    int16_t vy_fp;
    uint8_t radius;      // 1..4 => visible diameter about 1..8 px
    uint8_t phase;       // twinkle phase 0..255
    uint8_t phase_step;  // individual pulse speed
    uint8_t peak;        // max core brightness
    bool active;
} ModeStar;

static void mode_star_spawn(ModeStar* s, bool from_top) {
    int r = 1 + duren_rand() % 4;
    int x = 8 + r + duren_rand() % (SCREEN_WIDTH - 16 - r * 2);
    int y = from_top ? -(r + duren_rand() % 24)
                     : (8 + duren_rand() % (SCREEN_HEIGHT - 16));

    s->x_fp = x << 8;
    s->y_fp = y << 8;

    // Slow mostly-downward movement with tiny sideways drift.
    // vx: -0.25 .. +0.25 px/frame, vy: 0.30 .. 0.75 px/frame.
    s->vx_fp = (int16_t)(-64 + duren_rand() % 129);
    s->vy_fp = (int16_t)(77 + duren_rand() % 116);
    s->radius = (uint8_t)r;
    s->phase = (uint8_t)(duren_rand() & 255);
    s->phase_step = (uint8_t)(2 + duren_rand() % 5);
    s->peak = (uint8_t)(170 + duren_rand() % 86);
    s->active = true;
}

static void mode_stars_init(ModeStar* stars, int* active_count, int* retarget_timer) {
    for (int i = 0; i < MODE_STAR_MAX; i++) stars[i].active = false;
    *active_count = 3 + duren_rand() % 3;
    for (int i = 0; i < *active_count; i++) mode_star_spawn(&stars[i], false);
    *retarget_timer = 120 + duren_rand() % 181;
}

static void mode_stars_update(ModeStar* stars, int* active_count, int* retarget_timer) {
    (*retarget_timer)--;
    if (*retarget_timer <= 0) {
        int new_count = 3 + duren_rand() % 3;
        if (new_count > *active_count) {
            for (int i = *active_count; i < new_count; i++) mode_star_spawn(&stars[i], true);
        } else if (new_count < *active_count) {
            for (int i = new_count; i < *active_count; i++) stars[i].active = false;
        }
        *active_count = new_count;
        *retarget_timer = 120 + duren_rand() % 181;
    }

    for (int i = 0; i < *active_count; i++) {
        ModeStar* s = &stars[i];
        if (!s->active) {
            mode_star_spawn(s, true);
            continue;
        }

        s->x_fp += s->vx_fp;
        s->y_fp += s->vy_fp;
        s->phase = (uint8_t)(s->phase + s->phase_step);

        int x = s->x_fp >> 8;
        int y = s->y_fp >> 8;
        int r = s->radius;

        if (y - r > SCREEN_HEIGHT || x + r < 0 || x - r >= SCREEN_WIDTH) {
            mode_star_spawn(s, true);
        }
    }
}

static void mode_stars_draw(const ModeStar* stars, int active_count) {
    for (int i = 0; i < active_count; i++) {
        const ModeStar* s = &stars[i];
        if (!s->active) continue;

        int cx = s->x_fp >> 8;
        int cy = s->y_fp >> 8;
        int r = s->radius;

        // Triangle-wave twinkle: brightens and dims continuously while falling.
        int p = s->phase;
        int tri = (p < 128) ? (p * 2) : ((255 - p) * 2); // 0..254..0
        int core = 36 + ((int)(s->peak - 36) * tri) / 254;

        int rr = r * r;
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 > rr) continue;

                // Circular radial falloff, keeping a bright core and softer edge.
                int radial = rr - d2;
                int brightness = 18 + (core * radial) / (rr > 0 ? rr : 1);
                if (dx == 0 && dy == 0) brightness = core;
                if (brightness > 255) brightness = 255;

                unsigned int c = ((unsigned int)brightness << 16) |
                                 ((unsigned int)brightness << 8) |
                                 (unsigned int)brightness;
                hal_fill_rect(cx + dx, cy + dy, 1, 1, c);
            }
        }
    }
}


typedef enum {
    MENU_FOCUS_TOP = 0,
    MENU_FOCUS_MODES
} MenuFocus;

#define MENU_BIRD_COUNT 5

typedef struct {
    bool active;
    int32_t x_q8;
    int32_t y_q8;
    int16_t speed_q8;
    int8_t direction;
    uint8_t phase;
} MenuBirdGnw;

static MenuBirdGnw g_menu_birds_gnw[MENU_BIRD_COUNT];
static uint32_t g_menu_birds_last_ms;
static uint32_t g_menu_birds_next_ms;
static double g_menu_windmill_angle;
static uint32_t g_menu_windmill_last_ms;

static void menu_gnw_ambience_reset(uint32_t now_ms) {
    memset(g_menu_birds_gnw, 0, sizeof(g_menu_birds_gnw));
    g_menu_birds_last_ms = now_ms;
    g_menu_birds_next_ms = now_ms + 900u;
    g_menu_windmill_last_ms = now_ms;
}

static void menu_gnw_birds_update_draw(HalTexture* birds, uint32_t now_ms) {
    if (!birds) return;
    uint32_t dt = now_ms - g_menu_birds_last_ms;
    if (dt > 64u) dt = 64u;
    g_menu_birds_last_ms = now_ms;

    if ((int32_t)(now_ms - g_menu_birds_next_ms) >= 0) {
        const int group = 1 + duren_rand() % 3;
        const int direction = (duren_rand() & 1) ? 1 : -1;
        for (int n = 0; n < group; ++n) {
            int slot = -1;
            for (int i = 0; i < MENU_BIRD_COUNT; ++i) {
                if (!g_menu_birds_gnw[i].active) { slot = i; break; }
            }
            if (slot < 0) break;
            MenuBirdGnw* b = &g_menu_birds_gnw[slot];
            b->active = true;
            b->direction = (int8_t)direction;
            b->x_q8 = (direction > 0 ? -18 : SCREEN_WIDTH + 18) << 8;
            b->y_q8 = (28 + duren_rand() % 72 + n * 7) << 8;
            b->speed_q8 = (int16_t)((20 + duren_rand() % 13) << 8);
            b->phase = (uint8_t)(duren_rand() & 3u);
        }
        g_menu_birds_next_ms = now_ms + 2600u + (uint32_t)(duren_rand() % 4200);
    }

    static const int8_t wave[4] = {0, 1, 0, -1};
    for (int i = 0; i < MENU_BIRD_COUNT; ++i) {
        MenuBirdGnw* b = &g_menu_birds_gnw[i];
        if (!b->active) continue;
        const int32_t step = (int32_t)(((int64_t)b->speed_q8 * dt) / 1000);
        b->x_q8 += b->direction * step;
        const int frame = (int)((now_ms / 110u + b->phase) % 3u);
        const int x = b->x_q8 >> 8;
        const int y = (b->y_q8 >> 8) + wave[(now_ms / 180u + b->phase) & 3u];
        if (b->direction > 0)
            hal_draw_sprite(birds, 210 + frame * 14, 288, 14, 8, x, y);
        else
            hal_draw_sprite_flipped_x(birds, 210 + frame * 14, 288, 14, 8, x, y);
        if ((b->direction > 0 && x > SCREEN_WIDTH + 20) ||
            (b->direction < 0 && x < -20))
            b->active = false;
    }
}

static void menu_gnw_windmill_update_draw(HalTexture* windmill, uint32_t now_ms) {
    if (!windmill) return;
    uint32_t dt = now_ms - g_menu_windmill_last_ms;
    if (dt > 100u) dt = 100u;
    g_menu_windmill_last_ms = now_ms;
    /* Android uses 30 degrees/second.  The compact sprite is pivot-centred by
     * the asset builder, so the exact same angular speed has no visible orbit. */
    g_menu_windmill_angle += (double)dt * 0.030;
    while (g_menu_windmill_angle >= 360.0) g_menu_windmill_angle -= 360.0;
    hal_draw_sprite_rotated(windmill, 210, 240, 48, 48, 252, 134,
                            g_menu_windmill_angle);
}

static void menu_gnw_restore_animated_background_occluders(HalTexture* menu_atlas) {
    if (!menu_atlas) return;
    /* The compact cards and glass panels are baked into the visible part of
     * the atlas.  Birds and the windmill rotor are drawn after that base
     * image, so restore every foreground UI region here to make the animated
     * scenery pass behind the interface instead of cutting through it. */
    hal_draw_sprite(menu_atlas, 5, 4, 310, 60, 5, 4);
    for (int i = 0; i < 4; ++i) {
        const int x = 8 + i * 78;
        hal_draw_sprite(menu_atlas, x, 64, 70, 101, x, 64);
    }
    hal_draw_sprite(menu_atlas, 18, 184, 284, 42, 18, 184);
}

static int menu_gnw_deck_family(int style) {
    if (style == DECK_STYLE_ATLAS_UA || style == DECK_STYLE_ATLAS_EU) return 1;
    if (deck_style_is_cats(style)) return 2;
    return 0;
}

static uint8_t menu_gnw_style_for_family(int family, int language) {
    if (family == 1)
        return deck_style_for_language(DECK_STYLE_ATLAS_EU, language);
    if (family == 2)
        return deck_style_for_language(DECK_STYLE_CATS_EU, language);
    return deck_style_for_language(DECK_STYLE_EU, language);
}

static int menu_gnw_mode_from_slot(int slot) {
    static const int modes[4] = {
        MODE_CAREER, MODE_RANDOM_BATTLE, MODE_TOURNAMENT, MODE_BATTLE_ROYAL
    };
    return modes[(slot >= 0 && slot < 4) ? slot : 0];
}

static UiStringId menu_gnw_description_id(int slot, int line) {
    static const UiStringId ids[4][2] = {
        {UI_STR_MENU_CAREER_DESC_1, UI_STR_MENU_CAREER_DESC_2},
        {UI_STR_MENU_ONLINE_DESC_1, UI_STR_MENU_ONLINE_DESC_2},
        {UI_STR_MENU_TOURNAMENT_DESC_1, UI_STR_MENU_TOURNAMENT_DESC_2},
        {UI_STR_MENU_BR_DESC_1, UI_STR_MENU_BR_DESC_2},
    };
    if (slot < 0 || slot > 3) slot = 0;
    return ids[slot][line != 0];
}

static void menu_gnw_panel(int x, int y, int w, int h, bool selected) {
    if (!selected) return;
    const unsigned int border = 0xFFD15A;
    hal_fill_rect(x, y, w, 1, border);
    hal_fill_rect(x, y + h - 1, w, 1, border);
    hal_fill_rect(x, y, 1, h, border);
    hal_fill_rect(x + w - 1, y, 1, h, border);
}

static void menu_gnw_small_line(HalTexture* font, const char* text,
                                int length, int center_x, int y) {
    if (!font || !text || length <= 0) return;
    if (length > 13) length = 13;
    const int advance = 6;
    const int x = center_x - length * advance / 2;
    for (int i = 0; i < length; ++i) {
        const unsigned int ch = (unsigned char)text[i];
        hal_draw_sprite(font, (int)(ch % 16u) * 6,
                        (int)(ch / 16u) * 10, 6, 10,
                        x + i * advance, y);
    }
}

static void menu_gnw_footer_line(HalTexture* font, const char* text,
                                 int center_x, int y) {
    if (!font || !text) return;
    int length = (int)strlen(text);
    if (length > 48) length = 48;
    const int advance = 6;
    const int x = center_x - length * advance / 2;
    for (int i = 0; i < length; ++i) {
        const unsigned int ch = (unsigned char)text[i];
        hal_draw_sprite(font, (int)(ch % 16u) * 6,
                        (int)(ch / 16u) * 10, 6, 10,
                        x + i * advance, y);
    }
}

static void menu_gnw_scaled_text(HalTexture* font, const char* text,
                                 int length, int x, int y, int advance,
                                 int glyph_h, bool shadow) {
    if (!font || !text || length <= 0 || advance <= 0) return;
    (void)shadow;
    const int glyph_w = advance > 4 ? advance - 1 : advance;
    for (int i = 0; i < length; ++i) {
        const unsigned int ch = (unsigned char)text[i];
        hal_draw_sprite_scaled(font, (int)(ch % 16u) * 6,
                               (int)(ch / 16u) * 10, 6, 10,
                               x + i * advance, y, glyph_w, glyph_h);
    }
}

static void menu_gnw_text_fit(HalTexture* font, const char* text,
                              int x, int y, int max_width) {
    if (!font || !text) return;
    int length = (int)strlen(text);
    if (length <= 0) return;
    int advance = 9;
    if (length * advance > max_width) advance = max_width / length;
    if (advance < 4) advance = 4;
    menu_gnw_scaled_text(font, text, length, x, y, advance, 10, false);
}

static void menu_gnw_center_line(HalTexture* font, const char* text,
                                 int length, int center_x, int y,
                                 int max_width, int preferred_advance,
                                 int glyph_h) {
    if (!font || !text || length <= 0) return;
    int advance = preferred_advance;
    if (length * advance > max_width) advance = max_width / length;
    if (advance < 4) advance = 4;
    const int x = center_x - length * advance / 2;
    menu_gnw_scaled_text(font, text, length, x, y, advance, glyph_h, false);
}

static void menu_gnw_mode_label(HalTexture* font, const char* text,
                                int center_x, int y) {
    if (!font || !text) return;
    const int length = (int)strlen(text);
    int split = -1;
    if (length * 8 > 70) {
        for (int i = 1; i < length - 1; ++i) {
            if (text[i] == ' ') split = i;
        }
        if (split < 0 && length > 10) split = (length + 1) / 2;
    }
    if (split > 0) {
        menu_gnw_center_line(font, text, split, center_x, y,
                             70, 8, 10);
        menu_gnw_center_line(font, text + split + 1,
                             length - split - 1, center_x, y + 12,
                             70, 8, 10);
    } else {
        menu_gnw_center_line(font, text, length, center_x, y + 5,
                             70, 8, 10);
    }
}

static void menu_gnw_top_control(int x, int w, bool selected) {
    if (!selected) return;
    hal_fill_rect(x, 6, w, 1, 0xFFD15A);
    hal_fill_rect(x, 61, w, 1, 0xFFD15A);
    hal_fill_rect(x, 6, 1, 56, 0xFFD15A);
    hal_fill_rect(x + w - 1, 6, 1, 56, 0xFFD15A);
}

static void menu_gnw_option_button(int x, int y, int w, int h,
                                   bool active) {
    const unsigned int border = active ? 0xFFD15A : 0xE7DDC2;
    if (active) hal_fill_rect(x, y, w, h, border);
    hal_fill_rect(x, y, w, 1, border);
    hal_fill_rect(x, y + h - 1, w, 1, border);
    hal_fill_rect(x, y, 1, h, border);
    hal_fill_rect(x + w - 1, y, 1, h, border);
}

static void menu_gnw_draw(HalTexture* font, HalTexture* footer_font,
                          HalTexture* menu_atlas,
                          MenuFocus focus, int top_item, int mode_slot,
                          int deck_family, int deck_size, int difficulty,
                          bool language_open,
                          int language_selected, uint32_t language_opened_ms,
                          uint32_t now_ms) {
    if (!font) return;
    /* Exactly two glass-equivalent panels: settings above and selected-mode
     * information below. The four mode cards float directly over the map. */
    menu_gnw_panel(5, 4, 310, 60, focus == MENU_FOCUS_TOP);
    menu_gnw_top_control(7, 86, focus == MENU_FOCUS_TOP && top_item == 0);
    menu_gnw_top_control(95, 55, focus == MENU_FOCUS_TOP && top_item == 1);
    menu_gnw_top_control(152, 89, focus == MENU_FOCUS_TOP && top_item == 2);
    menu_gnw_top_control(243, 70, focus == MENU_FOCUS_TOP && top_item == 3);

    menu_gnw_small_line(font, ui_tr(UI_STR_DECK),
                        (int)strlen(ui_tr(UI_STR_DECK)), 50, 7);
    if (menu_atlas) {
        const int ua_row = ui_language_uses_cp1251(ui_get_language()) ? 1 : 0;
        hal_draw_sprite_scaled(menu_atlas, 210 + deck_family * 26,
                               296 + ua_row * 26, 26, 26,
                               34, 23, 32, 32);
    }

    menu_gnw_small_line(font, ui_tr(UI_STR_CARDS_LABEL),
                        (int)strlen(ui_tr(UI_STR_CARDS_LABEL)), 122, 7);
    menu_gnw_option_button(104, 21, 37, 16, deck_size == DECK_MODE_36);
    menu_gnw_option_button(104, 40, 37, 16, deck_size == DECK_MODE_52);
    menu_gnw_center_line(font, "36", 2, 122, 22, 32, 8, 10);
    menu_gnw_center_line(font, "52", 2, 122, 41, 32, 8, 10);

    menu_gnw_small_line(font, ui_tr(UI_STR_DIFFICULTY),
                        (int)strlen(ui_tr(UI_STR_DIFFICULTY)), 195, 7);
    for (int i = 0; i < 3; ++i) {
        const int y = 20 + i * 13;
        const char* value = ui_difficulty_name_localized(i);
        menu_gnw_option_button(157, y, 77, 11, difficulty == i);
        menu_gnw_small_line(font, value, (int)strlen(value), 195, y + 1);
    }

    menu_gnw_small_line(font, ui_tr(UI_STR_LANGUAGE),
                        (int)strlen(ui_tr(UI_STR_LANGUAGE)), 278, 7);
    if (menu_atlas)
        hal_draw_sprite_scaled(menu_atlas, 180 + ui_get_language() * 17,
                               352, 17, 10, 261, 27, 34, 20);

    for (int i = 0; i < 4; ++i) {
        const int x = 8 + i * 78;
        if (focus == MENU_FOCUS_MODES && mode_slot == i) {
            hal_fill_rect(x - 2, 62, 74, 2, 0xFFD15A);
            hal_fill_rect(x - 2, 165, 74, 2, 0xFFD15A);
            hal_fill_rect(x - 2, 62, 2, 105, 0xFFD15A);
            hal_fill_rect(x + 70, 62, 2, 105, 0xFFD15A);
        }
        menu_gnw_mode_label(font,
                            ui_mode_name_localized(menu_gnw_mode_from_slot(i)),
                            x + 35, i == 3 ? 70 : 65);
    }

    menu_gnw_panel(18, 180, 284, 46, focus == MENU_FOCUS_MODES);
    const char* mode_title = ui_mode_name_localized(
        menu_gnw_mode_from_slot(mode_slot));
    menu_gnw_text_fit(font, mode_title, 22, 185, 250);
    menu_gnw_text_fit(font, ui_tr(menu_gnw_description_id(mode_slot, 0)), 22, 198, 250);
    menu_gnw_text_fit(font, ui_tr(menu_gnw_description_id(mode_slot, 1)), 22, 210, 250);
    menu_gnw_text_fit(font, mode_slot == 3 ? "4" : "1V1", 274, 211, 24);
    menu_gnw_footer_line(footer_font ? footer_font : font,
                         "ALSO AVAILABLE ON GOOGLE PLAY", SCREEN_WIDTH / 2, 229);

    if (language_open && menu_atlas) {
        uint32_t elapsed = now_ms - language_opened_ms;
        int visible = 1 + (int)(elapsed / 42u);
        if (visible > UI_LANG_COUNT) visible = UI_LANG_COUNT;
        hal_apply_darkened_rect(284, 62, 30, 128, 58u);
        menu_gnw_panel(284, 62, 30, 128, true);
        for (int i = 0; i < visible; ++i) {
            const int y = 66 + i * 15;
            if (i == language_selected)
                hal_fill_rect(286, y - 2, 26, 14, 0xFFD15A);
            hal_draw_sprite_scaled(menu_atlas, 180 + i * 17, 352, 17, 10,
                                   290, y, 20, 12);
        }
    }
}

#define TABLE_DUST_COUNT 20

typedef struct {
    int16_t x_q4;
    int16_t y_q4;
    int8_t vx_q4;
    int8_t vy_q4;
    uint8_t active;
    uint8_t color_idx;
    uint8_t size_phase;
    uint8_t size_speed;
    uint8_t spawn_edge; /* 0=left, 1=right, 2=top, 3=bottom */
} TableDust;

static const unsigned int table_dust_colors[] = {
    0x9F987B, 0xC2B995
};

static void table_dust_spawn_from_edge(TableDust* p, int edge) {
    if (!p) return;
    edge &= 3;
    p->spawn_edge = (uint8_t)edge;

    /* Spawn just outside one edge and travel across the whole table toward the
     * opposite edge.  Tangential drift is deliberately tiny so a mote cannot
     * enter and immediately disappear through a neighbouring edge. */
    int tangent = (duren_rand() % 3) - 1; /* -1..+1 Q4 units/frame */
    int inward = 3 + (duren_rand() % 3);  /* 3..5 Q4 units/frame */
    const int safe_x = 20 + duren_rand() % (SCREEN_WIDTH - 40);
    const int safe_y = 20 + duren_rand() % (SCREEN_HEIGHT - 40);

    switch (edge) {
        case 0: /* left -> across to right */
            p->x_q4 = (int16_t)(-(2 << 4));
            p->y_q4 = (int16_t)(safe_y << 4);
            p->vx_q4 = (int8_t)inward;
            p->vy_q4 = (int8_t)tangent;
            break;
        case 1: /* right -> across to left */
            p->x_q4 = (int16_t)((SCREEN_WIDTH + 1) << 4);
            p->y_q4 = (int16_t)(safe_y << 4);
            p->vx_q4 = (int8_t)-inward;
            p->vy_q4 = (int8_t)tangent;
            break;
        case 2: /* top -> across to bottom */
            p->x_q4 = (int16_t)(safe_x << 4);
            p->y_q4 = (int16_t)(-(2 << 4));
            p->vx_q4 = (int8_t)tangent;
            p->vy_q4 = (int8_t)inward;
            break;
        case 3: /* bottom -> across to top */
        default:
            p->x_q4 = (int16_t)(safe_x << 4);
            p->y_q4 = (int16_t)((SCREEN_HEIGHT + 1) << 4);
            p->vx_q4 = (int8_t)tangent;
            p->vy_q4 = (int8_t)-inward;
            break;
    }

    p->color_idx = (uint8_t)(duren_rand() & 1);
    p->size_phase = (uint8_t)(duren_rand() & 0xFF);
    p->size_speed = (uint8_t)(2 + duren_rand() % 4);
}

static void table_dust_respawn(TableDust* p) {
    if (!p) return;
    int edge = duren_rand() & 3;
    if (edge == p->spawn_edge) edge = (edge + 1 + (duren_rand() % 3)) & 3;
    table_dust_spawn_from_edge(p, edge);
}

static void table_dust_init(TableDust* particles) {
    int active_count = g_settings.particle_count;
    if (active_count < 1) active_count = 1;
    if (active_count > TABLE_DUST_COUNT) active_count = TABLE_DUST_COUNT;
    int edges[4] = {0, 1, 2, 3};
    for (int i = 3; i > 0; --i) {
        int j = duren_rand() % (i + 1);
        int t = edges[i]; edges[i] = edges[j]; edges[j] = t;
    }

    for (int i = 0; i < TABLE_DUST_COUNT; i++) {
        particles[i].active = (uint8_t)(i < active_count);
        particles[i].spawn_edge = 255u;
        if (particles[i].active) {
            int edge = (i < 4) ? edges[i] : (duren_rand() & 3);
            table_dust_spawn_from_edge(&particles[i], edge);
            int travel_frames = 180 + (duren_rand() % 520);
            particles[i].x_q4 += (int16_t)(particles[i].vx_q4 * travel_frames);
            particles[i].y_q4 += (int16_t)(particles[i].vy_q4 * travel_frames);
        }
    }
}

static void table_dust_set_count(TableDust* particles, int active_count) {
    if (!particles) return;
    if (active_count < 1) active_count = 1;
    if (active_count > TABLE_DUST_COUNT) active_count = TABLE_DUST_COUNT;
    for (int i = 0; i < TABLE_DUST_COUNT; ++i) {
        bool should_be_active = i < active_count;
        if (should_be_active && !particles[i].active) {
            particles[i].active = 1u;
            table_dust_spawn_from_edge(&particles[i], duren_rand() & 3);
            int travel_frames = 60 + (duren_rand() % 360);
            particles[i].x_q4 += (int16_t)(particles[i].vx_q4 * travel_frames);
            particles[i].y_q4 += (int16_t)(particles[i].vy_q4 * travel_frames);
        } else if (!should_be_active) {
            particles[i].active = 0u;
        }
    }
}

static void table_dust_update(TableDust* particles, uint32_t dt_ms) {
    if (dt_ms < 1u) dt_ms = 1u;
    if (dt_ms > 50u) dt_ms = 50u;
    for (int i = 0; i < TABLE_DUST_COUNT; i++) {
        TableDust* p = &particles[i];
        if (!p->active) continue;

        p->x_q4 += (int16_t)((int32_t)p->vx_q4 * (int32_t)dt_ms / 25);
        p->y_q4 += (int16_t)((int32_t)p->vy_q4 * (int32_t)dt_ms / 25);
        p->size_phase = (uint8_t)(p->size_phase +
                                   ((uint32_t)p->size_speed * dt_ms + 12u) / 25u);

        int x = p->x_q4 >> 4;
        int y = p->y_q4 >> 4;

        /* Keep tangential drift on-screen.  A mote is recycled only after it
         * crosses the edge opposite to the one it entered from. */
        if ((p->spawn_edge == 0 || p->spawn_edge == 1) && (y < 4 || y > SCREEN_HEIGHT - 5)) {
            p->vy_q4 = (int8_t)-p->vy_q4;
            if (y < 4) p->y_q4 = (int16_t)(4 << 4);
            if (y > SCREEN_HEIGHT - 5) p->y_q4 = (int16_t)((SCREEN_HEIGHT - 5) << 4);
        }
        if ((p->spawn_edge == 2 || p->spawn_edge == 3) && (x < 4 || x > SCREEN_WIDTH - 5)) {
            p->vx_q4 = (int8_t)-p->vx_q4;
            if (x < 4) p->x_q4 = (int16_t)(4 << 4);
            if (x > SCREEN_WIDTH - 5) p->x_q4 = (int16_t)((SCREEN_WIDTH - 5) << 4);
        }

        bool crossed_opposite = false;
        switch (p->spawn_edge) {
            case 0: crossed_opposite = x > SCREEN_WIDTH + 3; break;
            case 1: crossed_opposite = x < -3; break;
            case 2: crossed_opposite = y > SCREEN_HEIGHT + 3; break;
            case 3: crossed_opposite = y < -3; break;
            default: crossed_opposite = true; break;
        }
        if (crossed_opposite) table_dust_respawn(p);
    }
}

static void table_dust_draw(const TableDust* particles) {
    for (int i = 0; i < TABLE_DUST_COUNT; i++) {
        const TableDust* p = &particles[i];
        if (!p->active) continue;
        int x = p->x_q4 >> 4;
        int y = p->y_q4 >> 4;
        uint8_t tri = (p->size_phase < 128u) ? p->size_phase : (uint8_t)(255u - p->size_phase);
        int size = (tri >= 72u) ? 3 : 2;
        hal_fill_rect(x, y, size, size, table_dust_colors[p->color_idx]);
        hal_apply_table_lighting_rect(x, y, size, size);
    }
}


#define SNOWFLAKE_MAX 40

typedef struct {
    int32_t x_q8;
    int32_t y_q8;
    int16_t fall_speed_q8; /* pixels/second in Q8 */
    int16_t drift_speed_q8;
    uint8_t size;          /* 1..5 X-shaped mask */
} Snowflake;

static void snowflake_respawn(Snowflake* s, bool initial) {
    if (!s) return;
    s->x_q8 = (int32_t)(duren_rand() % SCREEN_WIDTH) << 8;
    s->y_q8 = initial
              ? ((int32_t)(duren_rand() % SCREEN_HEIGHT) << 8)
              : -((int32_t)(2 + duren_rand() % 24) << 8);
    s->fall_speed_q8 = (int16_t)((18 + duren_rand() % 25) << 8);
    s->drift_speed_q8 = (int16_t)(((duren_rand() % 7) - 3) << 7);
    s->size = (uint8_t)(1 + duren_rand() % 5);
}

static void snow_init(Snowflake* snow) {
    for (int i = 0; i < SNOWFLAKE_MAX; ++i) snowflake_respawn(&snow[i], true);
}

static void snow_update(Snowflake* snow, uint32_t dt_ms) {
    if (!snow || !g_settings.snow_enabled) return;
    if (dt_ms > 50u) dt_ms = 50u;
    int count = g_settings.snowflake_count;
    if (count < 20) count = 20;
    if (count > SNOWFLAKE_MAX) count = SNOWFLAKE_MAX;
    for (int i = 0; i < count; ++i) {
        Snowflake* f = &snow[i];
        f->y_q8 += (int32_t)f->fall_speed_q8 * (int32_t)dt_ms / 1000;
        f->x_q8 += (int32_t)f->drift_speed_q8 * (int32_t)dt_ms / 1000;
        int x = f->x_q8 >> 8;
        int y = f->y_q8 >> 8;
        if (x < -5) f->x_q8 = (SCREEN_WIDTH + 4) << 8;
        else if (x > SCREEN_WIDTH + 4) f->x_q8 = -(4 << 8);
        if (y > SCREEN_HEIGHT + 5) snowflake_respawn(f, false);
    }
}

static void snow_draw(const Snowflake* snow) {
    if (!snow || !g_settings.snow_enabled) return;
    int count = g_settings.snowflake_count;
    if (count < 20) count = 20;
    if (count > SNOWFLAKE_MAX) count = SNOWFLAKE_MAX;
    for (int i = 0; i < count; ++i) {
        const Snowflake* f = &snow[i];
        int x = f->x_q8 >> 8;
        int y = f->y_q8 >> 8;
        int size = f->size;
        for (int d = 0; d < size; ++d) {
            hal_fill_rect(x + d, y + d, 1, 1, 0xEAF7FF);
            int mirror = size - 1 - d;
            if (mirror != d) hal_fill_rect(x + mirror, y + d, 1, 1, 0xEAF7FF);
        }
    }
}

static int portrait_corner_cut(int edge) {
    if (edge <= 0) return 3;
    if (edge == 1) return 2;
    if (edge == 2) return 1;
    return 0;
}

static void draw_rounded_outline(int x, int y, int w, int h,
                                 unsigned int color) {
    if (w < 8 || h < 8) return;
    for (int row = 0; row < h; ++row) {
        int edge = row;
        if (h - 1 - row < edge) edge = h - 1 - row;
        const int cut = portrait_corner_cut(edge);
        if (row == 0 || row == h - 1) {
            hal_fill_rect(x + cut, y + row, w - cut * 2, 1, color);
        } else {
            hal_fill_rect(x + cut, y + row, 1, 1, color);
            hal_fill_rect(x + w - 1 - cut, y + row, 1, 1, color);
        }
    }
}

static unsigned int portrait_turn_color(uint32_t now_ms) {
    const uint32_t phase = now_ms % 900u;
    const uint32_t tri = phase < 450u ? phase : 900u - phase;
    const unsigned int r = 224u + (31u * tri) / 450u;
    const unsigned int g = 112u + (104u * tri) / 450u;
    const unsigned int b = 16u + (34u * tri) / 450u;
    return (r << 16) | (g << 8) | b;
}

void draw_face(HalTexture* faces_tex, int face_idx, int x, int y) {
    if (!faces_tex) return;
    int sx = face_idx * FACE_WIDTH;
    hal_draw_sprite(faces_tex, sx, 0, FACE_WIDTH, FACE_HEIGHT, x, y);
    draw_rounded_outline(x - 1, y - 1, FACE_WIDTH + 2, FACE_HEIGHT + 2, 0x603517u);
}

static void draw_face_with_shadow(HalTexture* faces_tex, int face_idx, int x, int y,
                                  bool dimmed) {
    if (!faces_tex) return;
    int sx = face_idx * FACE_WIDTH;
    int sdx, sdy;
    hal_get_shadow_offset(x + FACE_WIDTH / 2, 0, &sdx, &sdy);
    int far_dx = sdx + (sdx > 0 ? 1 : (sdx < 0 ? -1 : 0));
    int far_dy = sdy + 1;

    /* The portrait PNGs carry the same 3-2-1 transparent corner mask as cards,
     * so both shadows and inactive dimming follow the rounded silhouette. */
    hal_draw_sprite_shadow(faces_tex, sx, 0, FACE_WIDTH, FACE_HEIGHT,
                           x + far_dx, y + far_dy, SHADOW_FAR_COLOR);
    hal_draw_sprite_shadow(faces_tex, sx, 0, FACE_WIDTH, FACE_HEIGHT,
                           x + sdx, y + sdy, SHADOW_COLOR);

    draw_rounded_outline(x - 2, y - 2, FACE_WIDTH + 4, FACE_HEIGHT + 4, 0x241108u);
    draw_rounded_outline(x - 1, y - 1, FACE_WIDTH + 2, FACE_HEIGHT + 2, 0x603517u);

    if (dimmed)
        hal_draw_sprite_dimmed(faces_tex, sx, 0, FACE_WIDTH, FACE_HEIGHT,
                               x, y, 42u);
    else
        hal_draw_sprite(faces_tex, sx, 0, FACE_WIDTH, FACE_HEIGHT, x, y);
}

static void draw_face_turn_frame(int x, int y, uint32_t now_ms) {
    draw_rounded_outline(x - 3, y - 3, FACE_WIDTH + 6, FACE_HEIGHT + 6,
                         portrait_turn_color(now_ms));
}



/* =========================================================
 * QUICK SCREEN FADE
 * A screen change starts dark and clears in a few frames.
 * This keeps transitions cheap on Game & Watch and avoids
 * a second full scene render pass.
 * ========================================================= */
typedef enum {
    UI_SCREEN_SPLASH = 0,
    UI_SCREEN_LANGUAGE,
    UI_SCREEN_MENU,
    UI_SCREEN_DIFFICULTY,
    UI_SCREEN_DECK_SIZE,
    UI_SCREEN_MODE,
    UI_SCREEN_BR_PLAYERS,
    UI_SCREEN_AVATAR,
    UI_SCREEN_CAREER_MAP,
    UI_SCREEN_BRACKET,
    UI_SCREEN_CHAMPION,
    UI_SCREEN_GAME,
    UI_SCREEN_PAUSE
} UiScreenId;

typedef enum {
    PAUSE_PAGE_MAIN = 0,
    PAUSE_PAGE_OPTIONS,
    PAUSE_PAGE_SOUND,
    PAUSE_PAGE_GRAPHICS,
    PAUSE_PAGE_GAMEPLAY,
    PAUSE_PAGE_SAVE_SLOTS,
    PAUSE_PAGE_LOAD_SLOTS,
    PAUSE_PAGE_CONFIRM_OVERWRITE,
    PAUSE_PAGE_CONFIRM_LOAD
} PausePage;

#define TRANSITION_FADE_START 224
#define TRANSITION_FADE_STEP   16
#define DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS 0

static void present_with_transition(UiScreenId screen,
                                    int* last_screen,
                                    int* fade_level) {
#if DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
    *last_screen = (int)screen;
    *fade_level = 0;
    hal_set_fade_level(0);
    hal_present();
    return;
#endif
    if (*last_screen != (int)screen) {
        *last_screen = (int)screen;
        *fade_level = TRANSITION_FADE_START;
    }

    hal_set_fade_level((uint8_t)(*fade_level));
    hal_present();

    if (*fade_level > 0) {
        *fade_level -= TRANSITION_FADE_STEP;
        if (*fade_level < 0) *fade_level = 0;
    }
}

/* =========================================================
 * HELD LEFT/RIGHT CARD SCROLL
 * Fixed-rate repeat with no acceleration and no inertia.
 * A tap always moves exactly one card. Holding waits briefly, then advances
 * one card at a steady interval. This prevents 2-3 card jumps while keeping
 * the visual hand glide smooth and responsive.
 * ========================================================= */
#define CARD_SCROLL_HOLD_DELAY_MS  260u
#define CARD_SCROLL_REPEAT_MS      135u

static int card_scroll_last_count = -1;
static uint32_t card_scroll_hold_started_ms = 0;
static uint32_t card_scroll_last_step_ms = 0;
static int card_scroll_hold_dir = 0;

/* Smooth visual hand layout. Selection changes remain logically immediate, but
 * every card glides toward its new fan position instead of teleporting between
 * discrete layouts. */
#define HAND_LAYOUT_TAU_MS 34u
static int32_t hand_visual_x_q16[DECK_SIZE];
static int32_t hand_visual_y_q16[DECK_SIZE];
static Card hand_visual_cards[DECK_SIZE];
static int hand_visual_count = -1;
static int32_t ai_hand_visual_x_q16[DECK_SIZE];
static int32_t ai_hand_visual_y_q16[DECK_SIZE];
static Card ai_hand_visual_cards[DECK_SIZE];
static int ai_hand_visual_count = -1;
static bool hand_visual_sorting = false;
static bool ai_hand_visual_sorting = false;

static void hand_visual_reset(void) {
    hand_visual_count = -1;
    ai_hand_visual_count = -1;
    hand_visual_sorting = false;
    ai_hand_visual_sorting = false;
}

static int32_t hand_visual_approach_q16(int32_t current, int32_t target, uint32_t dt_ms) {
    if (dt_ms < 1u) dt_ms = 1u;
    if (dt_ms > 50u) dt_ms = 50u;
    const uint32_t tau_ms = scaled_motion_ms(HAND_LAYOUT_TAU_MS);
    uint32_t alpha_q16 = (uint32_t)(((uint64_t)dt_ms << 16) / (tau_ms + dt_ms));
    int64_t delta = (int64_t)target - current;
    int32_t next = current + (int32_t)((delta * alpha_q16) >> 16);
    if (next == current && current != target) next += (target > current) ? 1 : -1;
    return next;
}

static void hand_visual_update(const Player* player, int selected_idx,
                               bool spread_selected, uint32_t dt_ms) {
    hand_visual_sorting = false;
    if (!player || player->card_count <= 0) {
        hand_visual_reset();
        return;
    }
    int count = player->card_count;
    if (count > DECK_SIZE) count = DECK_SIZE;

    Card old_cards[DECK_SIZE];
    int32_t old_x[DECK_SIZE];
    int32_t old_y[DECK_SIZE];
    const int old_count = hand_visual_count > 0 ? hand_visual_count : 0;
    for (int i = 0; i < old_count; ++i) {
        old_cards[i] = hand_visual_cards[i];
        old_x[i] = hand_visual_x_q16[i];
        old_y[i] = hand_visual_y_q16[i];
    }

    for (int i = 0; i < count; i++) {
        const int32_t tx = player_hand_card_x(count, selected_idx, i, spread_selected) << 16;
        /* Keep three pixels below every 40x60 hand card for its alpha-shaped
         * contact shadow. At y=180 the 240-line screen clipped that shadow. */
        const int32_t ty = ((spread_selected && i == selected_idx) ? 167 : 177) << 16;
        int old_idx = -1;
        for (int j = 0; j < old_count; ++j) {
            if (card_same(player->hand[i], old_cards[j])) {
                old_idx = j;
                break;
            }
        }
        if (old_idx >= 0) {
            hand_visual_x_q16[i] = hand_visual_approach_q16(old_x[old_idx], tx, dt_ms);
            hand_visual_y_q16[i] = hand_visual_approach_q16(old_y[old_idx], ty, dt_ms);
            if (hand_visual_x_q16[i] != tx) hand_visual_sorting = true;
        } else {
            /* A newly dealt card already completed its flight into the hand;
             * begin its fan interpolation from the landing slot. */
            hand_visual_x_q16[i] = tx;
            hand_visual_y_q16[i] = ty;
        }
        hand_visual_cards[i] = player->hand[i];
    }
    hand_visual_count = count;
}

static void ai_hand_visual_update(const Player* player, uint32_t dt_ms) {
    ai_hand_visual_sorting = false;
    if (!player || player->card_count <= 0) {
        ai_hand_visual_count = -1;
        return;
    }
    int count = player->card_count;
    if (count > DECK_SIZE) count = DECK_SIZE;

    Card old_cards[DECK_SIZE];
    int32_t old_x[DECK_SIZE];
    int32_t old_y[DECK_SIZE];
    const int old_count = ai_hand_visual_count > 0 ? ai_hand_visual_count : 0;
    for (int i = 0; i < old_count; ++i) {
        old_cards[i] = ai_hand_visual_cards[i];
        old_x[i] = ai_hand_visual_x_q16[i];
        old_y[i] = ai_hand_visual_y_q16[i];
    }

    int step = count > 1 ? 180 / (count - 1) : CARD_WIDTH;
    if (step > CARD_WIDTH) step = CARD_WIDTH;
    if (step < 1) step = 1;
    for (int i = 0; i < count; ++i) {
        const int32_t tx = (10 + i * step) << 16;
        const int32_t ty = 10 << 16;
        int old_idx = -1;
        for (int j = 0; j < old_count; ++j) {
            if (card_same(player->hand[i], old_cards[j])) {
                old_idx = j;
                break;
            }
        }
        if (old_idx >= 0) {
            ai_hand_visual_x_q16[i] = hand_visual_approach_q16(old_x[old_idx], tx, dt_ms);
            ai_hand_visual_y_q16[i] = hand_visual_approach_q16(old_y[old_idx], ty, dt_ms);
            if (ai_hand_visual_x_q16[i] != tx) ai_hand_visual_sorting = true;
        } else {
            ai_hand_visual_x_q16[i] = tx;
            ai_hand_visual_y_q16[i] = ty;
        }
        ai_hand_visual_cards[i] = player->hand[i];
    }
    ai_hand_visual_count = count;
}

static int ai_hand_visual_x(int card_idx) {
    return (int)((ai_hand_visual_x_q16[card_idx] + 32768) >> 16);
}

static int ai_hand_visual_y(int card_idx) {
    return (int)((ai_hand_visual_y_q16[card_idx] + 32768) >> 16);
}

static int hand_visual_x(int card_idx) {
    return (int)((hand_visual_x_q16[card_idx] + 32768) >> 16);
}

static int hand_visual_y(int card_idx) {
    return (int)((hand_visual_y_q16[card_idx] + 32768) >> 16);
}

static int wrap_card_index(int idx, int count) {
    if (count <= 0) return 0;
    while (idx < 0) idx += count;
    while (idx >= count) idx -= count;
    return idx;
}

static void card_scroll_reset(void) {
    card_scroll_last_count = -1;
    card_scroll_hold_started_ms = 0;
    card_scroll_last_step_ms = 0;
    card_scroll_hold_dir = 0;
}

static void card_scroll_update(int* selected_idx, int count) {
    if (!selected_idx || count <= 0) {
        card_scroll_reset();
        if (selected_idx) *selected_idx = 0;
        return;
    }

    *selected_idx = wrap_card_index(*selected_idx, count);
    const uint32_t now = hal_get_ticks_ms();

    if (card_scroll_last_count != count) {
        card_scroll_hold_started_ms = 0;
        card_scroll_last_step_ms = 0;
        card_scroll_hold_dir = 0;
        card_scroll_last_count = count;
    }

    const bool left_pressed = hal_is_button_pressed(BTN_LEFT);
    const bool right_pressed = hal_is_button_pressed(BTN_RIGHT);
    const bool left_held = hal_is_button_held(BTN_LEFT);
    const bool right_held = hal_is_button_held(BTN_RIGHT);

    int dir = 0;
    if (left_held && !right_held) dir = -1;
    else if (right_held && !left_held) dir = 1;

    if (left_pressed && !right_pressed) {
        *selected_idx = wrap_card_index(*selected_idx - 1, count);
        card_scroll_hold_dir = -1;
        card_scroll_hold_started_ms = now;
        card_scroll_last_step_ms = now;
        return;
    }
    if (right_pressed && !left_pressed) {
        *selected_idx = wrap_card_index(*selected_idx + 1, count);
        card_scroll_hold_dir = 1;
        card_scroll_hold_started_ms = now;
        card_scroll_last_step_ms = now;
        return;
    }

    if (dir == 0) {
        card_scroll_hold_dir = 0;
        card_scroll_hold_started_ms = 0;
        card_scroll_last_step_ms = 0;
        return;
    }

    if (card_scroll_hold_dir != dir) {
        card_scroll_hold_dir = dir;
        card_scroll_hold_started_ms = now;
        card_scroll_last_step_ms = now;
        return;
    }

    if ((uint32_t)(now - card_scroll_hold_started_ms) < CARD_SCROLL_HOLD_DELAY_MS)
        return;

    if ((uint32_t)(now - card_scroll_last_step_ms) >= CARD_SCROLL_REPEAT_MS) {
        *selected_idx = wrap_card_index(*selected_idx + dir, count);
        card_scroll_last_step_ms = now;
    }
}

/* FACE selector: time-independent state with a hard idle snap. Holding a
 * direction scrolls continuously; releasing brakes and lands on one exact
 * Q8 icon coordinate. Once idle, no interpolation runs, so the strip cannot
 * oscillate between neighbouring LCD pixels. The final portrait voice starts
 * 100 ms after the carousel becomes still. It may replace an older menu voice,
 * but holding a direction never spams intermediate portraits. */
#define FACE_SCROLL_ONE_Q8          256
#define FACE_HOLD_ACCEL_AFTER_FRAMES 12
#define FACE_SCROLL_ACCEL_Q8          4
#define FACE_SCROLL_MAX_SPEED_Q8     48
#define FACE_SCROLL_BRAKE_Q8          6
#define FACE_SCROLL_SNAP_MAX_Q8      64
#define FACE_SCROLL_SNAP_MIN_Q8       8
#define FACE_VOICE_DELAY_MS         100u
#define FACE_VOICE_MIN_GAP_MS       100u

typedef enum {
    FACE_SCROLL_IDLE = 0,
    FACE_SCROLL_SNAP,
    FACE_SCROLL_HELD,
    FACE_SCROLL_BRAKING
} FaceScrollMode;

static FaceScrollMode face_scroll_mode = FACE_SCROLL_IDLE;
static int face_scroll_velocity_q8 = 0;
static int face_scroll_snap_target_q8 = 0;
static int face_hold_direction = 0;
static int face_hold_frames = 0;
static int face_pending_voice = -1;
static uint32_t face_voice_due_ms = 0u;
static uint32_t face_last_voice_ms = 0u;

static int face_round_index_q8(int value) {
    if (value >= 0) return (value + FACE_SCROLL_ONE_Q8 / 2) / FACE_SCROLL_ONE_Q8;
    return -((-value + FACE_SCROLL_ONE_Q8 / 2) / FACE_SCROLL_ONE_Q8);
}

static int face_wrap_index(int index) {
    index %= 2;
    if (index < 0) index += 2;
    return index;
}

static void face_repeat_reset(void) {
    face_scroll_mode = FACE_SCROLL_IDLE;
    face_scroll_velocity_q8 = 0;
    face_scroll_snap_target_q8 = 0;
    face_hold_direction = 0;
    face_hold_frames = 0;
    face_pending_voice = -1;
    face_voice_due_ms = 0u;
    face_last_voice_ms = 0u;
}

static void face_schedule_voice(int face, uint32_t now) {
    /* The two neutral player silhouettes are not story characters and have
     * no character voice. NPC voices remain attached only to NPC portraits. */
    (void)face;
    (void)now;
    face_pending_voice = -1;
    face_voice_due_ms = 0u;
}

static void face_selector_begin(int* selected_face, int* scroll_q8) {
    if (!selected_face || !scroll_q8) return;
    face_repeat_reset();
    *selected_face = face_wrap_index(*selected_face);
    *scroll_q8 = *selected_face * FACE_SCROLL_ONE_Q8;
    face_scroll_snap_target_q8 = *scroll_q8;
    face_schedule_voice(*selected_face, hal_get_ticks_ms());
}

static void face_selector_enter_idle(int* selected_face, int* scroll_q8) {
    const int snapped = face_round_index_q8(*scroll_q8);
    *scroll_q8 = snapped * FACE_SCROLL_ONE_Q8;
    *selected_face = face_wrap_index(snapped);
    face_scroll_snap_target_q8 = *scroll_q8;
    face_scroll_velocity_q8 = 0;
    face_scroll_mode = FACE_SCROLL_IDLE;
    face_schedule_voice(*selected_face, hal_get_ticks_ms());
}

static void face_selector_force_stop(int* selected_face, int* scroll_q8) {
    if (!selected_face || !scroll_q8) return;
    face_selector_enter_idle(selected_face, scroll_q8);
    /* Confirming a face must not queue a menu voice behind the match intro. */
    face_pending_voice = -1;
}

static void face_scroll_snap_update(int* selected_face, int* scroll_q8) {
    int diff = face_scroll_snap_target_q8 - *scroll_q8;
    if (diff == 0 || (diff > -FACE_SCROLL_SNAP_MIN_Q8 &&
                      diff < FACE_SCROLL_SNAP_MIN_Q8)) {
        *scroll_q8 = face_scroll_snap_target_q8;
        face_selector_enter_idle(selected_face, scroll_q8);
        return;
    }

    int step = diff / 4;
    if (step > FACE_SCROLL_SNAP_MAX_Q8) step = FACE_SCROLL_SNAP_MAX_Q8;
    if (step < -FACE_SCROLL_SNAP_MAX_Q8) step = -FACE_SCROLL_SNAP_MAX_Q8;
    if (step > 0 && step < FACE_SCROLL_SNAP_MIN_Q8) step = FACE_SCROLL_SNAP_MIN_Q8;
    if (step < 0 && step > -FACE_SCROLL_SNAP_MIN_Q8) step = -FACE_SCROLL_SNAP_MIN_Q8;
    if ((diff > 0 && step >= diff) || (diff < 0 && step <= diff))
        *scroll_q8 = face_scroll_snap_target_q8;
    else
        *scroll_q8 += step;

    *selected_face = face_wrap_index(face_round_index_q8(*scroll_q8));
}

static void face_select_update(int* selected_face, int* scroll_q8) {
    if (!selected_face || !scroll_q8) return;

    const bool left_pressed = hal_is_button_pressed(BTN_LEFT);
    const bool right_pressed = hal_is_button_pressed(BTN_RIGHT);
    const bool left_held = hal_is_button_held(BTN_LEFT);
    const bool right_held = hal_is_button_held(BTN_RIGHT);
    int dir = 0;
    if (left_held && !right_held) dir = -1;
    else if (right_held && !left_held) dir = 1;

    if ((left_pressed && !right_pressed) || (right_pressed && !left_pressed)) {
        const int pressed_dir = left_pressed ? -1 : 1;
        const int nearest = face_round_index_q8(*scroll_q8);
        face_scroll_snap_target_q8 = (nearest + pressed_dir) * FACE_SCROLL_ONE_Q8;
        face_scroll_mode = FACE_SCROLL_SNAP;
        face_scroll_velocity_q8 = 0;
        face_hold_direction = pressed_dir;
        face_hold_frames = 0;
        face_pending_voice = -1;
        hal_play_sound_gain(SND_FLIP, 25);
    }

    if (dir != 0) {
        if (dir != face_hold_direction) {
            face_hold_direction = dir;
            face_hold_frames = 0;
        } else {
            ++face_hold_frames;
        }

        if (face_hold_frames >= FACE_HOLD_ACCEL_AFTER_FRAMES) {
            face_scroll_mode = FACE_SCROLL_HELD;
            face_pending_voice = -1;
            face_scroll_velocity_q8 += dir * FACE_SCROLL_ACCEL_Q8;
            if (face_scroll_velocity_q8 > FACE_SCROLL_MAX_SPEED_Q8)
                face_scroll_velocity_q8 = FACE_SCROLL_MAX_SPEED_Q8;
            if (face_scroll_velocity_q8 < -FACE_SCROLL_MAX_SPEED_Q8)
                face_scroll_velocity_q8 = -FACE_SCROLL_MAX_SPEED_Q8;
            *scroll_q8 += face_scroll_velocity_q8;
            *selected_face = face_wrap_index(face_round_index_q8(*scroll_q8));
            return;
        }
    } else {
        face_hold_direction = 0;
        face_hold_frames = 0;
        if (face_scroll_mode == FACE_SCROLL_HELD)
            face_scroll_mode = FACE_SCROLL_BRAKING;
    }

    if (face_scroll_mode == FACE_SCROLL_BRAKING) {
        *scroll_q8 += face_scroll_velocity_q8;
        if (face_scroll_velocity_q8 > 0) {
            face_scroll_velocity_q8 -= FACE_SCROLL_BRAKE_Q8;
            if (face_scroll_velocity_q8 < 0) face_scroll_velocity_q8 = 0;
        } else if (face_scroll_velocity_q8 < 0) {
            face_scroll_velocity_q8 += FACE_SCROLL_BRAKE_Q8;
            if (face_scroll_velocity_q8 > 0) face_scroll_velocity_q8 = 0;
        }
        *selected_face = face_wrap_index(face_round_index_q8(*scroll_q8));
        if (face_scroll_velocity_q8 == 0) {
            face_scroll_snap_target_q8 =
                face_round_index_q8(*scroll_q8) * FACE_SCROLL_ONE_Q8;
            face_scroll_mode = FACE_SCROLL_SNAP;
        }
    }

    if (face_scroll_mode == FACE_SCROLL_SNAP)
        face_scroll_snap_update(selected_face, scroll_q8);
}

static void face_selector_voice_update(void) {
    if (face_pending_voice < 0 || face_scroll_mode != FACE_SCROLL_IDLE) return;
    const uint32_t now = hal_get_ticks_ms();
    /* Signed subtraction keeps the deadline check correct across uint32 wrap. */
    if ((int32_t)(now - face_voice_due_ms) < 0) return;
    if (face_last_voice_ms != 0u &&
        (uint32_t)(now - face_last_voice_ms) < FACE_VOICE_MIN_GAP_MS) return;

    /* Do not wait for the short navigation SFX: voice and SFX use independent
     * channels. Direct play also replaces a stale portrait voice instead of
     * letting it finish and speaking the newly selected portrait much later. */
    const int voice = face_pending_voice;
    face_pending_voice = -1;
    face_last_voice_ms = now;
    hal_play_face_voice(voice);
}


static bool visual_anims_any(const VisualAnim* anims) {
    for (int i = 0; i < MAX_ANIMS; ++i) if (anims[i].active) return true;
    return false;
}

static bool visual_anims_update_no_commit(VisualAnim* anims, uint32_t now) {
    bool running = false;
    bool any_active = false;
    for (int i = 0; i < MAX_ANIMS; ++i) {
        VisualAnim* a = &anims[i];
        if (!a->active) continue;
        any_active = true;
        if (a->landing_committed) continue;
        if (!a->timing_started) {
            a->timing_started = true;
            a->start_ms = now + a->start_delay_ms;
            a->duration_ms = visual_anim_duration_ms(a);
            a->start_x = a->cx;
            a->start_y = a->cy;
            a->scale = 1.0f;
        }
        if ((int32_t)(now - a->start_ms) < 0) {
            running = true;
            continue;
        }
        uint32_t elapsed = now - a->start_ms;
        const uint32_t flight_ms = visual_anim_flight_ms(a);
        const uint32_t landing_ms = visual_anim_landing_ms(a);
        if (elapsed < flight_ms) {
            float t = (float)elapsed / (float)(flight_ms ? flight_ms : 1u);
            visual_anim_flight_position(a, t, &a->cx, &a->cy);
            a->scale = visual_anim_scale(t);
            a->frames_left = (int)(((flight_ms - elapsed) *
                                     (uint32_t)(a->total_frames > 0 ? a->total_frames : ANIM_FRAMES)) /
                                    (flight_ms ? flight_ms : 1u));
            if (a->frames_left < 1) a->frames_left = 1;
            running = true;
        } else if (elapsed < flight_ms + landing_ms) {
            const uint32_t land = elapsed - flight_ms;
            if (a->destination_is_table) {
                const float t = (float)land / (float)(landing_ms ? landing_ms : 1u);
                const float settle = visual_surface_friction(t);
                float contact_x = a->tx;
                float contact_y = a->ty;
                visual_anim_contact_point(a, &contact_x, &contact_y);
                a->cx = contact_x + (a->tx - contact_x) * settle;
                a->cy = contact_y + (a->ty - contact_y) * settle;
            } else {
                const uint32_t third = landing_ms / 3u;
                a->cx = a->tx;
                a->cy = a->ty +
                    (land < third ? -2.0f : (land < third * 2u ? 1.0f : 0.0f));
            }
            a->scale = 1.0f;
            a->frames_left = 0;
            running = true;
        } else {
            a->cx = a->tx;
            a->cy = a->ty;
            a->scale = 1.0f;
            a->frames_left = 0;
            /* Keep this landed visual at its destination while later deal
             * cards are still flying.  The event commits the logical group
             * atomically after the final landing, so no card disappears or
             * changes texture between flight and the static hand/table. */
            a->landing_committed = true;
        }
    }
    if (any_active) {
        bool all_landed = true;
        for (int i = 0; i < MAX_ANIMS; ++i) {
            if (anims[i].active && !anims[i].landing_committed) {
                all_landed = false;
                break;
            }
        }
        if (all_landed) {
            for (int i = 0; i < MAX_ANIMS; ++i) {
                anims[i].active = false;
                anims[i].timing_started = false;
            }
            running = false;
        }
    }
    return running;
}

static void br_start_event_animation(const BattleRoyalState* br, VisualAnim* anims,
                                     bool* hide_table) {
    for (int i = 0; i < MAX_ANIMS; ++i) {
        memset(&anims[i], 0, sizeof(anims[i]));
    }
    *hide_table = false;
    if (!br || !br->event_pending) return;
    const BREvent* e = &br->event;
    if (e->type == BR_EVENT_ATTACK || e->type == BR_EVENT_DEFEND) {
        int sx = 0, sy = 0;
        if (e->source_hand_index >= 0 && e->source_hand_count > 0) {
            br_ui_hand_card_anchor(br, e->actor,
                                   e->source_hand_count,
                                   e->source_hand_index,
                                   e->actor == 0,
                                   &sx, &sy);
        } else {
            int sw = 0;
            br_ui_hand_anchor(br, e->actor, &sx, &sy, &sw);
            sx += sw / 2;
            sy += 4;
        }
        int tx, ty;
        br_ui_table_anchor(br, e->table_index, e->type == BR_EVENT_DEFEND, &tx, &ty);
        VisualAnim* a = &anims[0];
        a->active = true;
        a->timing_started = false;
        a->card = e->card;
        /* A played attack/defense card is public information. NPC cards must
         * fly face-up exactly like the human card; the canonical card back is used only for hidden hands, the deck
         * and deal/draw animations. */
        a->hide_card = false;
        a->cx = (float)sx;
        a->cy = (float)sy;
        a->tx = (float)tx;
        a->ty = (float)ty;
        a->frames_left = ANIM_FRAMES;
        a->total_frames = ANIM_FRAMES;
        a->destination_is_table = true;
        return;
    }
    if (e->type == BR_EVENT_RESOLVE_BEAT || e->type == BR_EVENT_RESOLVE_TAKE) {
        *hide_table = true;
        int table_cards = 0;
        for (int i = 0; i < br->table_pair_count; ++i) {
            if (br->table_attack[i].rank >= RANK_6) ++table_cards;
            if (br->table_defense[i].rank >= RANK_6) ++table_cards;
        }
        const int future_count = br->players[br->defender].hand.card_count + table_cards;
        int target_slot = br->players[br->defender].hand.card_count;
        int n = 0;
        for (int i = 0; i < br->table_pair_count && n < MAX_ANIMS; ++i) {
            int x, y;
            int target_x = -50, target_y = 100;
            if (e->type == BR_EVENT_RESOLVE_TAKE) {
                br_ui_hand_card_anchor(br, br->defender, future_count,
                                       target_slot++, false, &target_x, &target_y);
            }
            br_ui_table_anchor(br, i, false, &x, &y);
            anims[n].active = true; anims[n].timing_started = false;
            anims[n].card = br->table_attack[i]; anims[n].hide_card = false;
            anims[n].cx = (float)x; anims[n].cy = (float)y;
            anims[n].tx = (float)target_x; anims[n].ty = (float)target_y;
            anims[n].total_frames = ANIM_FRAMES; anims[n].frames_left = ANIM_FRAMES; ++n;
            if (br->table_defense[i].rank >= RANK_6 && n < MAX_ANIMS) {
                if (e->type == BR_EVENT_RESOLVE_TAKE) {
                    br_ui_hand_card_anchor(br, br->defender, future_count,
                                           target_slot++, false, &target_x, &target_y);
                }
                br_ui_table_anchor(br, i, true, &x, &y);
                anims[n].active = true; anims[n].timing_started = false;
                anims[n].card = br->table_defense[i]; anims[n].hide_card = false;
                anims[n].cx = (float)x; anims[n].cy = (float)y;
                anims[n].tx = (float)target_x; anims[n].ty = (float)target_y;
                anims[n].total_frames = ANIM_FRAMES; anims[n].frames_left = ANIM_FRAMES; ++n;
            }
        }
        return;
    }
    if (e->type == BR_EVENT_DEAL) {
        int order[BR_MAX_PLAYERS];
        int order_count = 0;
        if (br->phase == BR_PHASE_ROUND_BANNER) {
            /* Match deal_initial(): one card clockwise from Player. */
            for (int player = 0; player < BR_MAX_PLAYERS; ++player)
                if (br->players[player].present && !br->players[player].eliminated)
                    order[order_count++] = player;
        } else {
            int player = br->primary_attacker;
            for (int step = 0; step < BR_MAX_PLAYERS; ++step) {
                if (br->players[player].present && !br->players[player].eliminated &&
                    player != br->defender)
                    order[order_count++] = player;
                player = (player + 1) % BR_MAX_PLAYERS;
            }
            if (br->defender >= 0 && br->defender < BR_MAX_PLAYERS &&
                br->players[br->defender].present &&
                !br->players[br->defender].eliminated)
                order[order_count++] = br->defender;
        }

        int final_count[BR_MAX_PLAYERS];
        for (int i = 0; i < BR_MAX_PLAYERS; ++i)
            final_count[i] = br->players[i].hand.card_count;

        int draw_player[MAX_ANIMS] = {0};
        int draw_slot[MAX_ANIMS] = {0};
        int draw_total = 0;
        int temp_top = br->deck.top_index;
        bool queued = true;
        while (queued && temp_top < DECK_SIZE && draw_total < MAX_ANIMS) {
            queued = false;
            for (int oi = 0; oi < order_count && temp_top < DECK_SIZE &&
                             draw_total < MAX_ANIMS; ++oi) {
                const int pidx = order[oi];
                if (final_count[pidx] >= 6) continue;
                draw_player[draw_total] = pidx;
                draw_slot[draw_total] = final_count[pidx]++;
                ++temp_top;
                ++draw_total;
                queued = true;
            }
        }

        for (int n = 0; n < draw_total; ++n) {
            const int pidx = draw_player[n];
            int tx = 0, ty = 0;
            br_ui_hand_card_anchor(br, pidx, final_count[pidx],
                                   draw_slot[n], pidx == 0, &tx, &ty);
            VisualAnim* a = &anims[n];
            a->active = true;
            a->card = br->deck.cards[br->deck.top_index + n];
            a->hide_card = true;
            a->cx = 280.0f;
            a->cy = 89.0f;
            a->tx = (float)tx;
            a->ty = (float)ty;
            a->start_delay_ms = scaled_motion_ms((uint32_t)n * 120u);
            a->frames_left = DEAL_ANIM_FRAMES;
            a->total_frames = DEAL_ANIM_FRAMES;
            a->deal_card = true;
            a->landing_player = (int8_t)pidx;
            /* Capture the destination hand brightness once for the complete
             * flight. Initial deal is neutral; ordinary BR draw batches match
             * the hand that is already dimmed by the current action phase. */
            a->dim_percent = br->phase == BR_PHASE_ROUND_BANNER
                           ? 0u
                           : (uint8_t)br_ui_hand_dim_percent(br, pidx);
        }
    }
}

static void br_draw_visual_anims(const BattleRoyalState* br,
                                 const VisualAnim* anims,
                                 HalTexture* cards_tex,
                                 HalTexture* card_back) {
    (void)br;
    for (int i = 0; i < MAX_ANIMS; ++i) {
        if (!anims[i].active) continue;
        int draw_w = BR_CARD_WIDTH, draw_h = BR_CARD_HEIGHT;
        visual_anim_draw_size(&anims[i], &draw_w, &draw_h);
        const int x = (int)anims[i].cx - (draw_w - BR_CARD_WIDTH) / 2;
        const int y = (int)anims[i].cy - (draw_h - BR_CARD_HEIGHT) / 2;
        const int lift = visual_anim_shadow_lift(&anims[i]);
        int sdx = 0, sdy = 0;
        hal_get_shadow_offset(x + draw_w / 2, lift, &sdx, &sdy);
        const int far_dx = sdx + (sdx > 0 ? 1 : (sdx < 0 ? -1 : 0));
        const int far_dy = sdy + 1;

        if (anims[i].hide_card) {
            if (!anims[i].deal_card) {
                hal_draw_sprite_scaled_shadow(card_back, 0, 0, BR_CARD_WIDTH, BR_CARD_HEIGHT,
                                              x + far_dx, y + far_dy,
                                              draw_w, draw_h, SHADOW_FAR_COLOR);
                hal_draw_sprite_scaled_shadow(card_back, 0, 0, BR_CARD_WIDTH, BR_CARD_HEIGHT,
                                              x + sdx, y + sdy,
                                              draw_w, draw_h, SHADOW_COLOR);
            }
            if (anims[i].dim_percent > 0u)
                hal_draw_sprite_scaled_dimmed(card_back, 0, 0,
                                              BR_CARD_WIDTH, BR_CARD_HEIGHT,
                                              x, y, draw_w, draw_h,
                                              anims[i].dim_percent);
            else
                hal_draw_sprite_scaled(card_back, 0, 0,
                                       BR_CARD_WIDTH, BR_CARD_HEIGHT,
                                       x, y, draw_w, draw_h);
        } else {
            const int card_index = card_atlas_index(anims[i].card);
            const int sx = card_index * BR_CARD_WIDTH;
            if (!anims[i].deal_card) {
                hal_draw_sprite_scaled_shadow(cards_tex, sx, 0, BR_CARD_WIDTH, BR_CARD_HEIGHT,
                                              x + far_dx, y + far_dy,
                                              draw_w, draw_h, SHADOW_FAR_COLOR);
                hal_draw_sprite_scaled_shadow(cards_tex, sx, 0, BR_CARD_WIDTH, BR_CARD_HEIGHT,
                                              x + sdx, y + sdy,
                                              draw_w, draw_h, SHADOW_COLOR);
            }
            if (anims[i].dim_percent > 0u)
                hal_draw_sprite_scaled_dimmed(cards_tex, sx, 0,
                                              BR_CARD_WIDTH, BR_CARD_HEIGHT,
                                              x, y, draw_w, draw_h,
                                              anims[i].dim_percent);
            else
                hal_draw_sprite_scaled(cards_tex, sx, 0,
                                       BR_CARD_WIDTH, BR_CARD_HEIGHT,
                                       x, y, draw_w, draw_h);
        }
    }
}

static void reset_frontend_run(GameState* game,
                               ModeSession* mode_session,
                               TournamentState* tournament,
                               VisualAnim* anims,
                               int* selected_difficulty,
                               int* selected_mode,
                               int* selected_face,
                               ModeAction* pending_action,
                               bool* result_processed,
                               GameResult* last_result_sfx,
                               bool* confetti_running,
                               int* selected_card_idx,
                               int* ai_delay,
                               int* anim_phase,
                               bool* hide_table,
                               int* single_target_pair_idx,
                               int* pause_selected) {
    game_init_global(game);
    modes_init(mode_session);
    memset(tournament, 0, sizeof(*tournament));
    *selected_difficulty = g_settings.difficulty;
    *selected_mode = MODE_CAREER;
    *selected_face = g_settings.player_face;
    *pending_action = MODE_ACTION_NONE;
    *result_processed = false;
    *last_result_sfx = RESULT_NONE;
    *confetti_running = false;
    *selected_card_idx = 0;
    card_scroll_reset();
    hand_visual_reset();
    face_repeat_reset();
    *ai_delay = 0;
    *anim_phase = 0;
    *hide_table = false;
    *single_target_pair_idx = -1;
    *pause_selected = 0;
    memset(anims, 0, sizeof(VisualAnim) * MAX_ANIMS);
}

/* One shared staging buffer is enough: save snapshots and pending loads are
 * never consumed at the same time. This saves sizeof(SaveGamePayload) BSS. */
static SaveGamePayload g_system_save_buffer;
static bool g_system_snapshot_valid = false;
static bool g_system_load_pending = false;

static void capture_save_payload(SaveGamePayload* out,
                                 const GameState* game,
                                 const ModeSession* mode_session,
                                 const TournamentState* tournament,
                                 const BattleRoyalState* br,
                                 int selected_difficulty,
                                 int selected_mode,
                                 int selected_face,
                                 int selected_br_players,
                                 int selected_card_idx,
                                 ModeAction pending_action,
                                 bool result_processed,
                                 GameResult last_result_sfx,
                                 uint32_t play_time_ms) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->game = *game;
    out->mode_session = *mode_session;
    out->tournament = *tournament;
    out->battle_royal = *br;
    out->selected_difficulty = selected_difficulty;
    out->selected_mode = selected_mode;
    out->selected_face = selected_face;
    out->selected_br_players = selected_br_players;
    out->selected_card_idx = selected_card_idx;
    out->pending_action = (int)pending_action;
    out->result_processed = result_processed;
    out->last_result_sfx = (int)last_result_sfx;
    out->play_time_ms = play_time_ms;
    out->rng_state = duren_rng_get_state();
}

static void apply_save_payload_runtime(const SaveGamePayload* in,
                                       GameState* game,
                                       ModeSession* mode_session,
                                       TournamentState* tournament,
                                       BattleRoyalState* br,
                                       int* selected_difficulty,
                                       int* selected_mode,
                                       int* selected_face,
                                       int* selected_br_players,
                                       int* selected_card_idx,
                                       ModeAction* pending_action,
                                       bool* result_processed,
                                       GameResult* last_result_sfx,
                                       uint32_t* play_time_ms) {
    if (!in) return;
    *game = in->game;
    normal_layout_capacity_cache = 0;
    *mode_session = in->mode_session;
    *tournament = in->tournament;
    *br = in->battle_royal;
    *selected_difficulty = in->selected_difficulty;
    *selected_mode = in->selected_mode;
    *selected_face = in->selected_face & 1;
    mode_session->player_face = *selected_face;
    br->players[0].face_index = *selected_face;
    for (int i = 0; i < 8; ++i)
        if (tournament->players[i].is_player)
            tournament->players[i].face_idx = *selected_face;
    *selected_br_players = in->selected_br_players;
    *selected_card_idx = in->selected_card_idx;
    *pending_action = (ModeAction)in->pending_action;
    *result_processed = in->result_processed;
    *last_result_sfx = (GameResult)in->last_result_sfx;
    *play_time_ms = in->play_time_ms;
    duren_rng_set_state(in->rng_state);
    br->last_tick_ms = hal_get_ticks_ms();
    if (br->phase == BR_PHASE_ROUND_BANNER) {
        br->phase_until_ms = br->last_tick_ms + 1000u;
    } else if (br->auto_resolve_pending ||
               (br->phase == BR_PHASE_THROW && br->decision_player < 0 &&
                br->phase_until_ms != 0u)) {
        br->auto_resolve_pending = true;
        br->auto_resolve_at_ms = br->last_tick_ms + 500u;
        br->phase_until_ms = br->auto_resolve_at_ms;
    }
    if (br->phase >= BR_PHASE_ATTACK && br->phase <= BR_PHASE_THROW)
        br->ai_ready_at_ms = br->last_tick_ms;
    br->event_pending = false;
    br->event.type = BR_EVENT_NONE;
}

bool durak_system_SaveState(const char* savePathName) {
    if (!savePathName) {
        printf("DUREN SAVE: missing Retro-Go path\n");
        return false;
    }
    if (!g_system_snapshot_valid) {
        printf("DUREN SAVE: snapshot not ready\n");
        return false;
    }
    const bool ok = savegame_write_path(savePathName, &g_system_save_buffer);
    printf("DUREN SAVE: %s path=%s err=%d\n",
           ok ? "OK" : "FAIL", savePathName, savegame_last_error());
    return ok;
}

bool durak_system_LoadState(const char* savePathName) {
    if (!savePathName) return false;
    const bool ok = savegame_read_path(savePathName, &g_system_save_buffer);
    printf("DUREN LOAD: %s path=%s err=%d\n",
           ok ? "OK" : "FAIL", savePathName, savegame_last_error());
    if (!ok) return false;
    g_system_snapshot_valid = true;
    g_system_load_pending = true;
    return true;
}

#ifndef DUREN_EXCLUDE_APPLICATION_MAIN
#ifdef PLATFORM_RETRO_GO
int durak_run(void) {
#elif defined(DUREN_APPLICATION_MAIN)
int duren_application_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
#else
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
#endif
#if DURAK_DIAGNOSTIC_BOOT_CANARY
    if (!hal_init()) return 1;

    int marker_x = 0;
    while (true) {
        hal_update();
        if (hal_is_button_pressed(BTN_QUIT)) break;

        // Minimal live LCD test: no textures, game state, effects or audio.
        hal_clear_screen(0x103820);
        hal_fill_rect(marker_x, 116, 24, 8, 0xFFFFFF);
        hal_present();
        hal_delay(16);
        marker_x = (marker_x + 2) % SCREEN_WIDTH;
    }

    hal_shutdown();
    return 0;
#else
    duren_srand((unsigned int)time(NULL));
    if (!hal_init()) return 1;

    const bool settings_loaded = settings_load();
    init_bg_music();

    HalTexture* menu_bg = hal_load_texture("mainmenu.png");
    HalTexture* splash_tex = hal_load_texture("splash.png");
    HalTexture* card_back_classic_tex = hal_load_texture("title.png");
    HalTexture* card_back_poker_tex = hal_load_texture("title_poker.png");
    HalTexture* card_back_cats_tex = hal_load_texture("title_cat.png");
    HalTexture* cards_uk_tex = hal_load_texture("cards.png");
    HalTexture* cards_lat_tex = hal_load_texture("cards_lat.png");
    HalTexture* cards_atlas_uk_tex = hal_load_texture("cards_atlas.png");
    HalTexture* cards_atlas_lat_tex = hal_load_texture("cards_atlas_lat.png");
    HalTexture* cards_cats_uk_tex = hal_load_texture("cards_cats.png");
    HalTexture* cards_cats_lat_tex = hal_load_texture("cards_cats_lat.png");
    HalTexture* card_style_tex[DECK_STYLE_COUNT] = {
        cards_uk_tex, cards_lat_tex,
        cards_atlas_uk_tex, cards_atlas_lat_tex,
        cards_cats_uk_tex, cards_cats_lat_tex
    };
    HalTexture* card_style_back[DECK_STYLE_COUNT] = {
        card_back_poker_tex, card_back_poker_tex,
        card_back_classic_tex, card_back_classic_tex,
        card_back_cats_tex, card_back_cats_tex
    };
    HalTexture* cards_tex = cards_uk_tex;
    HalTexture* card_back = card_back_classic_tex;
    /* Every mode uses native 40x60 art; Cats has its official Android back. */
    HalTexture* br_card_back = card_back;
    HalTexture* br_cards_tex = cards_uk_tex;
    HalTexture* font_ascii_tex = hal_load_texture("font.png");
    HalTexture* font_uk_tex = hal_load_texture("font_uk_cp1251.png");
    HalTexture* font_tex = font_ascii_tex;
    /* The new Android-derived front end uses the original regular face.
     * Keep separate byte maps for Latin, Ukrainian and Polish so every
     * localized label is thin and readable rather than falling back to the
     * semibold gameplay atlas. */
    HalTexture* menu_font_lat_tex = hal_load_texture("menu_font_regular.png");
    HalTexture* menu_font_uk_tex = hal_load_texture("menu_font_regular_uk.png");
    HalTexture* menu_font_white_tex = hal_load_texture("menu_font_regular_white.png");
    HalTexture* faces_tex = hal_load_texture("faces.png");
    HalTexture* br_faces_tex = hal_load_texture("faces2.png");
    HalTexture* player_faces_tex = hal_load_texture("player_faces.png");
    HalTexture* br_player_faces_tex = hal_load_texture("player_faces2.png");
    HalTexture* career_map_tex = hal_load_texture("career_map.png");
    HalTexture* dialogue_font_ascii_tex = hal_load_texture("6x10_font.png");
    HalTexture* dialogue_font_uk_tex = hal_load_texture("6x10_font_uk_cp1251.png");
    HalTexture* dialogue_font_tex = dialogue_font_ascii_tex;
    HalTexture* dialog_tex = NULL; /* procedural mirrored white dialogue bubble */
    // Завантаження mainscreen.png повністю видалено для економії пам'яті!

    ui_set_language(g_settings.language);
    if (!dialogue_load_language(g_settings.language)) {
        g_settings.language = UI_LANG_ENGLISH;
        ui_set_language(UI_LANG_ENGLISH);
        (void)dialogue_load_language(UI_LANG_ENGLISH);
    }
    if (ui_language_uses_cp1251(g_settings.language) && font_uk_tex && dialogue_font_uk_tex) {
        font_tex = font_uk_tex;
        dialogue_font_tex = dialogue_font_uk_tex;
    } else {
        font_tex = font_ascii_tex;
        dialogue_font_tex = dialogue_font_ascii_tex;
    }
    cards_tex = card_style_tex[g_settings.deck_style]
              ? card_style_tex[g_settings.deck_style] : cards_uk_tex;
    card_back = card_style_back[g_settings.deck_style]
              ? card_style_back[g_settings.deck_style] : card_back_classic_tex;
    br_cards_tex = cards_tex;
    br_card_back = card_back;

    GameState game;
    game_init_global(&game);
    ModeSession mode_session;
    TournamentState tournament;
    BattleRoyalState battle_royal;
    BRUiState br_ui;
    memset(&battle_royal, 0, sizeof(battle_royal));
    br_ui_reset(&br_ui, hal_get_ticks_ms());
    br_ui_sync_hands(&br_ui, &battle_royal);
    br_ui_set_dialogues_enabled(g_settings.dialogues_enabled);
    modes_init(&mode_session);
    memset(&tournament, 0, sizeof(tournament));
    g_live_mode_session = &mode_session;
    g_live_game = &game;

    int selected_card_idx = 0;
    unsigned int bg_color = TABLE_BG_A; 
    
    VisualAnim anims[MAX_ANIMS] = {0};
    int anim_phase = 0;           
    bool phase_took = false;
    bool normal_npc_taking = false;
    NormalResolveAction normal_resolve_action = NORMAL_RESOLVE_NONE;
    uint32_t normal_resolve_at_ms = 0u;
    bool normal_throw_hint_was_active = false;
    uint32_t normal_throw_hint_started_ms = 0u;
    bool hide_table = false;      
    int single_target_pair_idx = -1;
    int normal_pending_ai_draw_start = 0;
    uint32_t normal_sort_until_ms = 0u;

    int ai_delay = 0;
    const bool language_setup_needed = !settings_loaded || !g_settings.language_complete;
    const bool profile_setup_needed = !settings_loaded || !g_settings.profile_complete;
    bool launch_selected_game = false;
    bool in_language_select = language_setup_needed;
    bool in_splash = !language_setup_needed && !profile_setup_needed;
    bool in_menu = false;
    bool in_difficulty_select = false;
    bool in_deck_size_select = false;
    bool in_mode_select = false;
    bool in_br_player_select = false;
    bool in_avatar_select = !language_setup_needed && profile_setup_needed;
    bool in_career_map = false;
    bool in_tournament_bracket = false;
    bool in_tournament_champion = false;
    bool in_pause = false;
    int pause_selected = 0;
    ActionHoldState b_action_hold = {0};
    ActionHoldState a_fast_hold = {0};
    uint32_t br_time_bonus_ms = 0u;
    PausePage pause_page = PAUSE_PAGE_MAIN;
    int pause_options_selected = 0;
    int pause_sound_selected = 0;
    int pause_graphics_selected = 0;
    int pause_gameplay_selected = 0;
    int pause_slot_selected = 0;
    bool pause_confirm_yes = false;
    char pause_message[64] = {0};
    uint32_t pause_message_until = 0;
    int selected_difficulty = g_settings.difficulty;
    int selected_language = g_settings.language;
    int selected_mode = MODE_CAREER;
    int selected_deck_size = DECK_MODE_36;
    int selected_br_players = 3;
    int selected_face = g_settings.player_face;
    int face_scroll_q8 = selected_face * FACE_SCROLL_ONE_Q8;
    ModeAction pending_action = MODE_ACTION_NONE;
    bool result_processed = false;
    int champion_frame = 0;
    ModeStar mode_stars[MODE_STAR_MAX] = {0};
    int mode_star_count = 0;
    int mode_star_retarget_timer = 0;
    MenuFocus menu_focus = MENU_FOCUS_MODES;
    int menu_top_selected = 0;
    int menu_mode_slot = 0;
    int menu_back_selected = menu_gnw_deck_family(g_settings.deck_style);
    bool menu_language_open = false;
    int menu_language_selected = g_settings.language;
    uint32_t menu_language_opened_ms = 0u;
    uint32_t splash_started_ms = hal_get_ticks_ms();
    GameResult last_result_sfx = RESULT_NONE;
    Confetti confetti[CONFETTI_COUNT] = {0};
    bool confetti_running = false;
    bool cats_reward_active = false;
    uint32_t cats_reward_started_ms = 0u;
    TableDust table_dust[TABLE_DUST_COUNT] = {0};
    Snowflake snow[SNOWFLAKE_MAX] = {0};
    DialogueState dialogue = {0};
    static CareerMapUi career_map_ui;
    career_map_init(&career_map_ui, &mode_session, hal_get_ticks_ms());
    bool br_event_started = false;
    bool br_hide_table = false;
    bool br_champion_voice_played = false;
    bool br_intro_voices_active = false;
    int br_intro_voice_cursor = 1;
    dialogue_reset(&dialogue);
    dialogue_sync_hand_counts(&dialogue, &game);
    menu_gnw_ambience_reset(hal_get_ticks_ms());
    table_dust_init(table_dust);
    snow_init(snow);
    mode_stars_init(mode_stars, &mode_star_count, &mode_star_retarget_timer);

    int transition_last_screen = -1;
    int transition_fade_level = 0;

    uint32_t last_loop_ms = hal_get_ticks_ms();
    uint32_t session_play_time_ms = 0u;
    bool quit_armed = false;
    while (true) {
        hal_update();
        if (!hal_is_button_held(BTN_QUIT)) {
            quit_armed = true;
        } else if (quit_armed && hal_is_button_pressed(BTN_QUIT)) {
            break;
        }

        uint32_t now_loop_ms = hal_get_ticks_ms();
        uint32_t frame_dt_ms = now_loop_ms - last_loop_ms;
        if (frame_dt_ms > 50u) frame_dt_ms = 50u;
        last_loop_ms = now_loop_ms;
        if (cats_reward_active &&
            now_loop_ms - cats_reward_started_ms >= CATS_REWARD_DURATION_MS)
            cats_reward_active = false;

        if (g_system_load_pending) {
            apply_save_payload_runtime(&g_system_save_buffer, &game, &mode_session, &tournament,
                                       &battle_royal, &selected_difficulty, &selected_mode,
                                       &selected_face, &selected_br_players, &selected_card_idx,
                                       &pending_action, &result_processed, &last_result_sfx,
                                       &session_play_time_ms);
            br_time_bonus_ms = 0u;
            action_hold_reset(&a_fast_hold);
            hand_visual_reset();
            normal_npc_taking = false;
            normal_resolve_action = NORMAL_RESOLVE_NONE;
            normal_resolve_at_ms = 0u;
            g_system_load_pending = false;
            in_language_select = false;
            in_splash = false;
            in_menu = false;
            in_difficulty_select = false;
            in_deck_size_select = false;
            in_mode_select = false;
            in_br_player_select = false;
            in_avatar_select = false;
            in_career_map = false;
            in_tournament_bracket = false;
            in_tournament_champion = false;
            in_pause = false;
            pause_page = PAUSE_PAGE_MAIN;
            anim_phase = 0;
            hide_table = false;
            br_hide_table = false;
            br_event_started = false;
            br_intro_voices_active = false;
            memset(anims, 0, sizeof(anims));
            dialogue_reset(&dialogue);
            dialogue_sync_hand_counts(&dialogue, &game);
            br_ui_reset(&br_ui, now_loop_ms);
            br_ui_sync_hands(&br_ui, &battle_royal);
            ai_delay = 0;
        }

        bool frontend_screen = in_language_select || in_splash || in_menu || in_difficulty_select ||
                               in_deck_size_select || in_mode_select ||
                               in_br_player_select || in_avatar_select ||
                               in_career_map ||
                               in_tournament_bracket || in_tournament_champion;
        if (!frontend_screen && !in_pause) {
            session_play_time_ms += frame_dt_ms;
            snow_update(snow, frame_dt_ms);
        }

        /* Keep one loadable gameplay snapshot available for both the in-game
         * slots and Retro-Go. It may be an AI or human turn; it only needs to
         * be between transactions, with no animation/event half-applied. */
        bool system_snapshot_ready = !frontend_screen && !in_pause &&
                                     !visual_anims_any(anims) && anim_phase == 0 &&
                                     !battle_royal.event_pending && !br_event_started &&
                                     !br_intro_voices_active && !normal_npc_taking;
        if (system_snapshot_ready) {
            capture_save_payload(&g_system_save_buffer, &game, &mode_session, &tournament,
                                 &battle_royal, selected_difficulty, selected_mode,
                                 selected_face, selected_br_players, selected_card_idx,
                                 pending_action, result_processed, last_result_sfx,
                                 session_play_time_ms);
            g_system_snapshot_valid = true;
        }

        if (frontend_screen) {
            hal_music_set_menu_intro(in_splash || in_menu);
            hal_music_start();
            hal_music_pause(false);
            action_hold_reset(&b_action_hold);
            action_hold_reset(&a_fast_hold);
        } else {
            hal_music_start_gameplay();
            hal_music_pause(in_pause);
            if (in_pause) { action_hold_reset(&b_action_hold); action_hold_reset(&a_fast_hold); }
        }

        if (in_menu || in_splash || in_language_select || in_difficulty_select ||
            in_deck_size_select ||
            in_mode_select || in_br_player_select || in_avatar_select ||
            in_career_map ||
            in_tournament_bracket || in_tournament_champion ||
            mode_session.mode == MODE_BATTLE_ROYAL) {
            normal_resolve_action = NORMAL_RESOLVE_NONE;
            normal_resolve_at_ms = 0u;
        }

        if (in_language_select) {
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_update(mode_stars, &mode_star_count, &mode_star_retarget_timer);
#endif
            if (hal_is_button_pressed(BTN_UP)) {
                selected_language = (selected_language + UI_LANG_COUNT - 1) % UI_LANG_COUNT;
                hal_play_sound_gain(SND_FLIP, 35);
            }
            if (hal_is_button_pressed(BTN_DOWN)) {
                selected_language = (selected_language + 1) % UI_LANG_COUNT;
                hal_play_sound_gain(SND_FLIP, 35);
            }
            if (hal_is_button_pressed(BTN_A)) {
                int language_to_apply = selected_language;
                if (!dialogue_load_language(language_to_apply)) {
                    language_to_apply = UI_LANG_ENGLISH;
                    (void)dialogue_load_language(language_to_apply);
                }
                selected_language = language_to_apply;
                ui_set_language(selected_language);
                if (ui_language_uses_cp1251(selected_language) &&
                    font_uk_tex && dialogue_font_uk_tex) {
                    font_tex = font_uk_tex;
                    dialogue_font_tex = dialogue_font_uk_tex;
                } else {
                    if (ui_language_uses_cp1251(selected_language) &&
                        (!font_uk_tex || !dialogue_font_uk_tex)) {
                        selected_language = UI_LANG_ENGLISH;
                        ui_set_language(UI_LANG_ENGLISH);
                        (void)dialogue_load_language(UI_LANG_ENGLISH);
                    }
                    /* font.png and 6x10_font.png now contain ASCII plus the
                     * shared Latin-extended glyphs for DE/PL/ES/FR/IT/PT-BR. */
                    font_tex = font_ascii_tex;
                    dialogue_font_tex = dialogue_font_ascii_tex;
                }
                g_settings.language = (uint8_t)ui_get_language();
                g_settings.deck_style = default_deck_style_for_language(g_settings.language);
                g_settings.language_complete = true;
                g_settings_dirty = true;
                cards_tex = card_style_tex[g_settings.deck_style]
                          ? card_style_tex[g_settings.deck_style] : cards_uk_tex;
                card_back = card_style_back[g_settings.deck_style]
                          ? card_style_back[g_settings.deck_style] : card_back_classic_tex;
                br_cards_tex = cards_tex;
                br_card_back = card_back;
                (void)settings_save();
                in_language_select = false;
                in_avatar_select = true;
                launch_selected_game = false;
                selected_face = g_settings.player_face;
                face_scroll_q8 = selected_face * FACE_SCROLL_ONE_Q8;
                selected_difficulty = g_settings.difficulty;
                hal_play_sound_gain(SND_FLIP, 35);
            }

            hal_clear_screen(0x000000);
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_draw(mode_stars, mode_star_count);
#endif
            ui_draw_language_select(font_tex, selected_language);
            present_with_transition(UI_SCREEN_LANGUAGE, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }

        if (in_splash) {
            const uint32_t elapsed = now_loop_ms - splash_started_ms;
            if (elapsed >= 1800u ||
                (elapsed >= 250u && hal_is_button_pressed(BTN_A))) {
                in_splash = false;
                in_menu = true;
                menu_gnw_ambience_reset(now_loop_ms);
                hal_play_sound_gain(SND_FLIP, 30u);
            }
            hal_clear_screen(0x000000);
            if (splash_tex)
                hal_draw_sprite(splash_tex, 0, 0, 320, 240, 0, 0);
            present_with_transition(UI_SCREEN_SPLASH,
                                    &transition_last_screen,
                                    &transition_fade_level);
            hal_delay(16);
            continue;
        }

        if (in_menu) {
            if (menu_language_open) {
                if (hal_is_button_pressed(BTN_B)) {
                    menu_language_open = false;
                    hal_play_sound_gain(SND_FLIP, 30u);
                } else if (hal_is_button_pressed(BTN_UP)) {
                    menu_language_selected =
                        (menu_language_selected + UI_LANG_COUNT - 1) % UI_LANG_COUNT;
                    hal_play_sound_gain(SND_FLIP, 30u);
                } else if (hal_is_button_pressed(BTN_DOWN)) {
                    menu_language_selected =
                        (menu_language_selected + 1) % UI_LANG_COUNT;
                    hal_play_sound_gain(SND_FLIP, 30u);
                } else if (hal_is_button_pressed(BTN_A)) {
                    int language_to_apply = menu_language_selected;
                    if (!dialogue_load_language(language_to_apply)) {
                        language_to_apply = UI_LANG_ENGLISH;
                        (void)dialogue_load_language(language_to_apply);
                    }
                    const int deck_family = menu_gnw_deck_family(g_settings.deck_style);
                    selected_language = language_to_apply;
                    ui_set_language(selected_language);
                    if (ui_language_uses_cp1251(selected_language) &&
                        font_uk_tex && dialogue_font_uk_tex) {
                        font_tex = font_uk_tex;
                        dialogue_font_tex = dialogue_font_uk_tex;
                    } else {
                        font_tex = font_ascii_tex;
                        dialogue_font_tex = dialogue_font_ascii_tex;
                    }
                    g_settings.language = (uint8_t)selected_language;
                    g_settings.deck_style = menu_gnw_style_for_family(
                        deck_family, selected_language);
                    menu_back_selected = deck_family;
                    cards_tex = card_style_tex[g_settings.deck_style]
                              ? card_style_tex[g_settings.deck_style] : cards_uk_tex;
                    card_back = card_style_back[g_settings.deck_style]
                              ? card_style_back[g_settings.deck_style]
                              : card_back_classic_tex;
                    br_cards_tex = cards_tex;
                    br_card_back = card_back;
                    g_settings_dirty = true;
                    (void)settings_save();
                    menu_language_selected = selected_language;
                    menu_language_open = false;
                    hal_play_sound_gain(SND_SELECT, 40u);
                }
            } else {
                if (hal_is_button_pressed(BTN_SELECT)) {
                    menu_focus = MENU_FOCUS_TOP;
                    menu_top_selected = 3;
                    menu_language_selected = g_settings.language;
                    menu_language_opened_ms = now_loop_ms;
                    menu_language_open = true;
                    hal_play_sound_gain(SND_FLIP, 30u);
                } else if (hal_is_button_pressed(BTN_UP)) {
                    menu_focus = menu_focus == MENU_FOCUS_TOP
                               ? MENU_FOCUS_MODES : MENU_FOCUS_TOP;
                    hal_play_sound_gain(SND_FLIP, 30u);
                } else if (hal_is_button_pressed(BTN_DOWN)) {
                    menu_focus = menu_focus == MENU_FOCUS_TOP
                               ? MENU_FOCUS_MODES : MENU_FOCUS_TOP;
                    hal_play_sound_gain(SND_FLIP, 30u);
                } else if (hal_is_button_pressed(BTN_LEFT)) {
                    if (menu_focus == MENU_FOCUS_TOP)
                        menu_top_selected = (menu_top_selected + 3) % 4;
                    else
                        menu_mode_slot = (menu_mode_slot + 3) % 4;
                    hal_play_sound_gain(SND_FLIP, 30u);
                } else if (hal_is_button_pressed(BTN_RIGHT)) {
                    if (menu_focus == MENU_FOCUS_TOP)
                        menu_top_selected = (menu_top_selected + 1) % 4;
                    else
                        menu_mode_slot = (menu_mode_slot + 1) % 4;
                    hal_play_sound_gain(SND_FLIP, 30u);
                } else if (hal_is_button_pressed(BTN_B)) {
                    menu_focus = MENU_FOCUS_MODES;
                    hal_play_sound_gain(SND_FLIP, 25u);
                } else if (hal_is_button_pressed(BTN_A)) {
                    if (menu_focus == MENU_FOCUS_TOP) {
                        if (menu_top_selected == 0) {
                            int next_family = (menu_back_selected + 1) % 3;
                            if (next_family == 2 && !g_settings.cats_unlocked)
                                next_family = 0;
                            menu_back_selected = next_family;
                            g_settings.deck_style = menu_gnw_style_for_family(
                                menu_back_selected, g_settings.language);
                            cards_tex = card_style_tex[g_settings.deck_style]
                                      ? card_style_tex[g_settings.deck_style] : cards_uk_tex;
                            card_back = card_style_back[g_settings.deck_style]
                                      ? card_style_back[g_settings.deck_style]
                                      : card_back_classic_tex;
                            br_cards_tex = cards_tex;
                            br_card_back = card_back;
                            g_settings_dirty = true;
                            (void)settings_save();
                            hal_play_sound_gain(SND_SELECT, 40u);
                        } else if (menu_top_selected == 1) {
                            selected_deck_size = selected_deck_size == DECK_MODE_36
                                               ? DECK_MODE_52 : DECK_MODE_36;
                            game_set_default_deck_mode(selected_deck_size);
                            hal_play_sound_gain(SND_SELECT, 40u);
                        } else if (menu_top_selected == 2) {
                            selected_difficulty = (selected_difficulty + 1) % 3;
                            g_settings.difficulty = (uint8_t)selected_difficulty;
                            g_settings_dirty = true;
                            (void)settings_save();
                            hal_play_sound_gain(SND_SELECT, 40u);
                        } else {
                            menu_language_selected = g_settings.language;
                            menu_language_opened_ms = now_loop_ms;
                            menu_language_open = true;
                            hal_play_sound_gain(SND_FLIP, 30u);
                        }
                    } else {
                        selected_mode = menu_gnw_mode_from_slot(menu_mode_slot);
                        game_set_default_deck_mode(selected_deck_size);
                        g_settings.difficulty = (uint8_t)selected_difficulty;
                        hal_music_set_menu_intro(false);
                        in_menu = false;
                        if (selected_mode == MODE_BATTLE_ROYAL) {
                            in_br_player_select = true;
                            selected_br_players = 3;
                        } else {
                            in_avatar_select = true;
                            launch_selected_game = true;
                            selected_face = g_settings.player_face;
                        }
                        hal_play_sound_gain(SND_SELECT, 45u);
                    }
                }
            }

            hal_clear_screen(0x000000);
            if (menu_bg) hal_draw_sprite(menu_bg, 0, 0, 320, 240, 0, 0);
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            menu_gnw_birds_update_draw(menu_bg, now_loop_ms);
            menu_gnw_windmill_update_draw(menu_bg, now_loop_ms);
            menu_gnw_restore_animated_background_occluders(menu_bg);
#endif
            HalTexture* menu_font_tex = menu_font_lat_tex;
            if (ui_get_language() == UI_LANG_UKRAINIAN)
                menu_font_tex = menu_font_uk_tex;
            menu_gnw_draw(menu_font_tex, menu_font_white_tex, menu_bg,
                          menu_focus, menu_top_selected, menu_mode_slot,
                          menu_back_selected, selected_deck_size,
                          selected_difficulty,
                          menu_language_open, menu_language_selected,
                          menu_language_opened_ms, now_loop_ms);

            present_with_transition(UI_SCREEN_MENU, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }


        if (in_deck_size_select) {
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_update(mode_stars, &mode_star_count, &mode_star_retarget_timer);
#endif
            if (hal_is_button_pressed(BTN_UP) || hal_is_button_pressed(BTN_DOWN)) {
                selected_deck_size = selected_deck_size == DECK_MODE_52
                                   ? DECK_MODE_36 : DECK_MODE_52;
                hal_play_sound_gain(SND_FLIP, 35);
            }
            if (hal_is_button_pressed(BTN_B)) {
                in_deck_size_select = false;
                in_menu = true;
                hal_play_sound_gain(SND_FLIP, 35);
            } else if (hal_is_button_pressed(BTN_A)) {
                game_set_default_deck_mode(selected_deck_size);
                in_deck_size_select = false;
                in_mode_select = true;
                hal_play_sound_gain(SND_FLIP, 35);
            }
            hal_clear_screen(0x000000);
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_draw(mode_stars, mode_star_count);
#endif
            ui_draw_deck_size_select(font_tex, selected_deck_size);
            present_with_transition(UI_SCREEN_DECK_SIZE, &transition_last_screen,
                                    &transition_fade_level);
            hal_delay(16);
            continue;
        }


        if (in_difficulty_select) {
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_update(mode_stars, &mode_star_count, &mode_star_retarget_timer);
#endif

            if (hal_is_button_pressed(BTN_UP)) {
                selected_difficulty = (selected_difficulty + 2) % 3;
                hal_play_sound_gain(SND_FLIP, 35);
            }
            if (hal_is_button_pressed(BTN_DOWN)) {
                selected_difficulty = (selected_difficulty + 1) % 3;
                hal_play_sound_gain(SND_FLIP, 35);
            }

            if (hal_is_button_pressed(BTN_B)) {
                in_difficulty_select = false;
                in_language_select = true;
                hal_play_sound_gain(SND_FLIP, 35);
            } else if (hal_is_button_pressed(BTN_A)) {
                g_settings.difficulty = (uint8_t)selected_difficulty;
                in_difficulty_select = false;
                in_avatar_select = true;
                launch_selected_game = false;
                selected_face = g_settings.player_face;
                face_selector_begin(&selected_face, &face_scroll_q8);
                hal_play_sound_gain(SND_FLIP, 35);
            }

            hal_clear_screen(0x000000);
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_draw(mode_stars, mode_star_count);
#endif
            ui_draw_difficulty_select(font_tex, selected_difficulty);
            present_with_transition(UI_SCREEN_DIFFICULTY, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }

        if (in_mode_select) {
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_update(mode_stars, &mode_star_count, &mode_star_retarget_timer);
#endif

            if (hal_is_button_pressed(BTN_UP)) {
                selected_mode = (selected_mode + 3) % 4;
                hal_play_sound_gain(SND_FLIP, 35);
            }
            if (hal_is_button_pressed(BTN_DOWN)) {
                selected_mode = (selected_mode + 1) % 4;
                hal_play_sound_gain(SND_FLIP, 35);
            }

            if (hal_is_button_pressed(BTN_B)) {
                in_mode_select = false;
                in_deck_size_select = true;
                hal_play_sound_gain(SND_FLIP, 35);
            } else if (hal_is_button_pressed(BTN_A)) {
                in_mode_select = false;
                if (selected_mode == MODE_BATTLE_ROYAL) {
                    in_br_player_select = true;
                    selected_br_players = 3;
                } else {
                    in_avatar_select = true;
                    launch_selected_game = true;
                    selected_face = g_settings.player_face;
                }
                hal_play_sound_gain(SND_FLIP, 35);
            }

            hal_clear_screen(0x000000);
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_draw(mode_stars, mode_star_count);
#endif
            ui_draw_mode_select(font_tex, selected_mode);

            present_with_transition(UI_SCREEN_MODE, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }

        if (in_br_player_select) {
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_update(mode_stars, &mode_star_count, &mode_star_retarget_timer);
#endif
            if (hal_is_button_pressed(BTN_UP) || hal_is_button_pressed(BTN_DOWN)) {
                selected_br_players = selected_br_players == 3 ? 4 : 3;
                hal_play_sound_gain(SND_FLIP, 35);
            }
            if (hal_is_button_pressed(BTN_B)) {
                in_br_player_select = false;
                in_mode_select = true;
                hal_play_sound_gain(SND_FLIP, 35);
            } else if (hal_is_button_pressed(BTN_A)) {
                in_br_player_select = false;
                in_avatar_select = true;
                launch_selected_game = true;
                selected_face = g_settings.player_face;
                hal_play_sound_gain(SND_FLIP, 35);
            }
            hal_clear_screen(0x000000);
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_draw(mode_stars, mode_star_count);
#endif
            ui_draw_battle_royal_players(font_tex, selected_br_players);
            present_with_transition(UI_SCREEN_BR_PLAYERS, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }

        if (in_avatar_select) {
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_update(mode_stars, &mode_star_count, &mode_star_retarget_timer);
#endif

            const bool launching = launch_selected_game;
            if (!launching) {
                face_select_update(&selected_face, &face_scroll_q8);
                face_selector_voice_update();
            }

            if (!launching && hal_is_button_pressed(BTN_B)) {
                in_avatar_select = false;
                if (!g_settings.profile_complete)
                    in_language_select = true;
                else
                    in_menu = true;
                face_selector_force_stop(&selected_face, &face_scroll_q8);
                hal_play_sound_gain(SND_FLIP, 35);
            } else if (launching || hal_is_button_pressed(BTN_A)) {
                if (launching) {
                    selected_face = g_settings.player_face;
                    face_scroll_q8 = selected_face * FACE_SCROLL_ONE_Q8;
                } else {
                    face_selector_force_stop(&selected_face, &face_scroll_q8);
                }
                if (!launching) {
                    g_settings.difficulty = (uint8_t)selected_difficulty;
                    g_settings.player_face = (uint8_t)face_wrap_index(selected_face);
                    g_settings.profile_complete = true;
                    g_settings_dirty = true;
                    (void)settings_save();
                    selected_face = g_settings.player_face;
                    in_avatar_select = false;
                    in_splash = true;
                    splash_started_ms = now_loop_ms;
                    hal_play_sound_gain(SND_FLIP, 35);
                } else {
                    launch_selected_game = false;
                    const uint8_t runtime_deck_style =
                        selected_mode == MODE_RANDOM_BATTLE
                        ? random_online_deck_style(g_settings.language,
                                                   g_settings.cats_unlocked)
                        : g_settings.deck_style;
                    cards_tex = card_style_tex[runtime_deck_style]
                              ? card_style_tex[runtime_deck_style] : cards_uk_tex;
                    card_back = card_style_back[runtime_deck_style]
                              ? card_style_back[runtime_deck_style] : card_back_classic_tex;
                    br_cards_tex = cards_tex;
                    br_card_back = card_back;
                    modes_begin(&mode_session, (GameMode)selected_mode, selected_face,
                                (Difficulty)selected_difficulty, &game, &tournament);
                    if (selected_mode == MODE_BATTLE_ROYAL) {
                        br_time_bonus_ms = 0u;
                        action_hold_reset(&a_fast_hold);
                        br_init(&battle_royal, selected_br_players, selected_face,
                                (Difficulty)selected_difficulty);
                        br_start_first_round(&battle_royal, hal_get_ticks_ms());
                        auto_sort_br_hand(&battle_royal, false);
                        hal_music_start_gameplay_new_match();
                        br_ui_new_round(&br_ui, hal_get_ticks_ms());
                        br_ui_sync_hands(&br_ui, &battle_royal);
                        br_event_started = false;
                        br_hide_table = false;
                        hal_randomize_table();
                        hal_play_sound(SND_SHUFFLE);
                        br_intro_voices_active = true;
                        br_intro_voice_cursor = 1;
                    }
                    pending_action = MODE_ACTION_NONE;
                    result_processed = false;
                    last_result_sfx = RESULT_NONE;
                    confetti_running = false;
                    selected_card_idx = 0;
                    ai_delay = 0;
                    anim_phase = 0;
                    hide_table = false;
                    memset(anims, 0, sizeof(anims));

                    in_avatar_select = false;
                    in_difficulty_select = false;
                    if (mode_session.mode == MODE_CAREER) {
                        /* Career always enters through the persistent 320x240
                         * world map. A missing journal means a fresh point 1. */
                        (void)career_progress_load(&mode_session, &game);
                        if (mode_session.career_complete &&
                            !g_settings.cats_unlocked) {
                            /* Migrate an already completed career from an
                             * older build without asking the player to replay. */
                            g_settings.cats_unlocked = true;
                            g_settings.deck_style = deck_style_for_language(
                                DECK_STYLE_CATS_EU, g_settings.language);
                            cards_tex = card_style_tex[g_settings.deck_style]
                                      ? card_style_tex[g_settings.deck_style]
                                      : cards_uk_tex;
                            card_back = card_style_back[g_settings.deck_style]
                                      ? card_style_back[g_settings.deck_style]
                                      : card_back_classic_tex;
                            br_cards_tex = cards_tex;
                            br_card_back = card_back;
                            g_settings_dirty = true;
                            (void)settings_save();
                        }
                        career_map_init(&career_map_ui, &mode_session,
                                        hal_get_ticks_ms());
                        (void)career_progress_save(&mode_session, &game);
                        in_career_map = true;
                    } else if (mode_session.mode == MODE_TOURNAMENT) {
                        in_tournament_bracket = true;
                    } else if (mode_session.mode == MODE_BATTLE_ROYAL) {
                        /* Battle Royal was prepared above; enter the table directly. */
                    } else {
                        normal_layout_capacity_cache = 0;
                        if (modes_prepare_match(&mode_session, &game, &tournament)) {
                            if (mode_session.mode == MODE_RANDOM_BATTLE) {
                                const char* bot_name = ui_online_bot_name(
                                    duren_rand() % 600);
                                snprintf(game.current_opponent.name,
                                         sizeof(game.current_opponent.name), "%s", bot_name);
                                /* Pseudo-online may use only the same two
                                 * neutral profile portraits available to the player. */
                                game.current_opponent.face_index =
                                    1 - (mode_session.player_face & 1);
                            }
                            hal_music_start_gameplay_new_match();
                            dialogue_reset(&dialogue);
                            dialogue_sync_hand_counts(&dialogue, &game);
                            hal_randomize_table();
                            hal_play_sound(SND_SHUFFLE);
                            if (mode_session.mode != MODE_RANDOM_BATTLE)
                                hal_queue_face_voice(game.current_opponent.face_index);
                            anim_phase = begin_initial_normal_deal_animation(&game, anims) > 0 ? 4 : 0;
                        } else {
                            in_menu = true;
                            reset_frontend_run(&game, &mode_session, &tournament, anims,
                                               &selected_difficulty, &selected_mode, &selected_face,
                                               &pending_action, &result_processed, &last_result_sfx,
                                               &confetti_running, &selected_card_idx, &ai_delay,
                                               &anim_phase, &hide_table, &single_target_pair_idx,
                                               &pause_selected);
                        }
                    }
                }
            }

            if (!launching && in_avatar_select) {
                hal_clear_screen(0x000000);
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
                mode_stars_draw(mode_stars, mode_star_count);
#endif
                ui_draw_avatar_select(font_tex, player_faces_tex,
                                      selected_face, face_scroll_q8);
                present_with_transition(UI_SCREEN_AVATAR, &transition_last_screen,
                                        &transition_fade_level);
            }
            hal_delay(16);
            continue;
        }

        if (in_career_map) {
            const uint32_t map_now = hal_get_ticks_ms();
            if (career_map_update(&career_map_ui, &mode_session, &game, map_now)) {
                /* The saved location changes only after the firefly reaches
                 * the destination, never when travel merely starts. */
                (void)career_progress_save(&mode_session, &game);
                hal_play_sound_gain(SND_FLIP, 35);
            }

            if (!career_map_ui.traveling) {
                int cursor_dx = 0;
                int cursor_dy = 0;
                if (hal_is_button_pressed(BTN_LEFT))  cursor_dx -= 1;
                if (hal_is_button_pressed(BTN_RIGHT)) cursor_dx += 1;
                if (hal_is_button_pressed(BTN_UP))    cursor_dy -= 1;
                if (hal_is_button_pressed(BTN_DOWN))  cursor_dy += 1;
                if ((cursor_dx || cursor_dy) &&
                    career_map_move_cursor(&career_map_ui, &mode_session,
                                           cursor_dx, cursor_dy))
                    hal_play_sound_gain(SND_FLIP, 25);

                if (hal_is_button_pressed(BTN_B)) {
                    (void)career_progress_save(&mode_session, &game);
                    in_career_map = false;
                    in_menu = true;
                    reset_frontend_run(&game, &mode_session, &tournament, anims,
                                       &selected_difficulty, &selected_mode, &selected_face,
                                       &pending_action, &result_processed, &last_result_sfx,
                                       &confetti_running, &selected_card_idx, &ai_delay,
                                       &anim_phase, &hide_table, &single_target_pair_idx,
                                       &pause_selected);
                    hal_play_sound_gain(SND_FLIP, 35);
                } else if (hal_is_button_pressed(BTN_SELECT)) {
                    /* Profile settings are still edited only through the one
                     * common menu flow; Career is journaled before leaving. */
                    (void)career_progress_save(&mode_session, &game);
                    selected_language = g_settings.language;
                    selected_difficulty = g_settings.difficulty;
                    selected_face = g_settings.player_face;
                    face_scroll_q8 = selected_face * FACE_SCROLL_ONE_Q8;
                    in_career_map = false;
                    in_language_select = true;
                    mode_stars_init(mode_stars, &mode_star_count,
                                    &mode_star_retarget_timer);
                    hal_play_sound_gain(SND_FLIP, 35);
                } else if (hal_is_button_pressed(BTN_A)) {
                    const CareerMapAction map_action = career_map_activate(
                        &career_map_ui, &mode_session, map_now);
                    if (map_action == CAREER_MAP_ACTION_START_BATTLE) {
                        normal_layout_capacity_cache = 0;
                        if (modes_prepare_match(&mode_session, &game, &tournament)) {
                            /* Career map and the active card atlas share the
                             * large scene cache. Release the map immediately
                             * when sitting at the table; the first card draw
                             * reuses that same SRAM instead of retaining a
                             * second 320x240 image. */
                            hal_evict_texture(career_map_tex);
                            in_career_map = false;
                            pending_action = MODE_ACTION_NONE;
                            result_processed = false;
                            last_result_sfx = RESULT_NONE;
                            confetti_running = false;
                            selected_card_idx = 0;
                            ai_delay = 0;
                            anim_phase = 0;
                            hide_table = false;
                            memset(anims, 0, sizeof(anims));
                            hal_music_start_gameplay_new_match();
                            dialogue_reset(&dialogue);
                            dialogue_sync_hand_counts(&dialogue, &game);
                            card_scroll_reset();
                            hand_visual_reset();
                            hal_randomize_table();
                            hal_play_sound(SND_SHUFFLE);
                            hal_queue_face_voice(game.current_opponent.face_index);
                            anim_phase = begin_initial_normal_deal_animation(&game, anims) > 0 ? 4 : 0;
                        } else {
                            hal_play_sound(SND_ERROR);
                        }
                    } else if (map_action == CAREER_MAP_ACTION_TRAVEL_STARTED) {
                        hal_play_sound_gain(SND_FLIP, 35);
                    } else {
                        hal_play_sound(SND_ERROR);
                    }
                }
            }

            if (in_career_map) {
                hal_clear_screen(0x000000);
                career_map_draw(career_map_tex, font_tex, &career_map_ui,
                                &mode_session, map_now);
                present_with_transition(UI_SCREEN_CAREER_MAP,
                                        &transition_last_screen,
                                        &transition_fade_level);
            }
            hal_delay(16);
            continue;
        }

        if (in_tournament_bracket) {
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_update(mode_stars, &mode_star_count, &mode_star_retarget_timer);
#endif
            if (hal_is_button_pressed(BTN_A)) {
                normal_layout_capacity_cache = 0;
                if (modes_prepare_match(&mode_session, &game, &tournament)) {
                    hal_music_start_gameplay_new_match();
                    dialogue_reset(&dialogue);
                    dialogue_sync_hand_counts(&dialogue, &game);
                    hal_randomize_table();
                    in_tournament_bracket = false;
                    pending_action = MODE_ACTION_NONE;
                    result_processed = false;
                    last_result_sfx = RESULT_NONE;
                    confetti_running = false;
                    selected_card_idx = 0;
                    ai_delay = 0;
                    anim_phase = 0;
                    hide_table = false;
                    memset(anims, 0, sizeof(anims));
                    hal_play_sound(SND_SHUFFLE);
                    hal_queue_face_voice(game.current_opponent.face_index);
                    anim_phase = begin_initial_normal_deal_animation(&game, anims) > 0 ? 4 : 0;
                }
            }

            hal_clear_screen(0x000000);
#if !DURAK_DIAGNOSTIC_SIMPLE_GRAPHICS
            mode_stars_draw(mode_stars, mode_star_count);
#endif
            ui_draw_tournament_bracket(font_tex, faces_tex, player_faces_tex, &tournament);
            present_with_transition(UI_SCREEN_BRACKET, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }

        if (in_tournament_champion) {
            champion_frame++;
            if (!confetti_running) {
                confetti_init(confetti);
                confetti_running = true;
            }
            confetti_update(confetti);

            hal_clear_screen(0x000000);
            confetti_draw(confetti);
            ui_draw_tournament_champion(font_tex, faces_tex, player_faces_tex,
                                        &tournament, champion_frame);

            if (hal_is_any_button_pressed() && champion_frame > 20) {
                in_tournament_champion = false;
                in_difficulty_select = false;
                in_mode_select = false;
                in_avatar_select = false;
                in_tournament_bracket = false;
                in_menu = true;
                reset_frontend_run(&game, &mode_session, &tournament, anims,
                                   &selected_difficulty, &selected_mode, &selected_face,
                                   &pending_action, &result_processed, &last_result_sfx,
                                   &confetti_running, &selected_card_idx, &ai_delay,
                                   &anim_phase, &hide_table, &single_target_pair_idx,
                                   &pause_selected);
                table_dust_init(table_dust);
            }

            present_with_transition(UI_SCREEN_CHAMPION, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }

        // SELECT opens the four-item pause menu during an active run.
        bool pause_opened_now = false;
        if (!in_pause && (mode_session.mode == MODE_BATTLE_ROYAL || game.result == RESULT_NONE) &&
            hal_is_button_pressed(BTN_SELECT)) {
            in_pause = true;
            pause_selected = 0;
            pause_page = PAUSE_PAGE_MAIN;
            pause_slot_selected = 0;
            pause_confirm_yes = false;
            pause_message[0] = '\0';
            pause_opened_now = true;
            hal_play_sound_gain(SND_FLIP, 35);
        }

        if (in_pause) {
            uint32_t pause_now = hal_get_ticks_ms();
            bool nav_up = !pause_opened_now && hal_is_button_pressed(BTN_UP);
            bool nav_down = !pause_opened_now && hal_is_button_pressed(BTN_DOWN);
            bool nav_left = !pause_opened_now && hal_is_button_pressed(BTN_LEFT);
            bool nav_right = !pause_opened_now && hal_is_button_pressed(BTN_RIGHT);
            bool vertical_up = nav_up || nav_left;
            bool vertical_down = nav_down || nav_right;
            bool back_pressed = !pause_opened_now && (hal_is_button_pressed(BTN_SELECT) || hal_is_button_pressed(BTN_B));
            bool accept = !pause_opened_now && hal_is_button_pressed(BTN_A);
            /* Saving uses the last complete human-decision snapshot. Opening
             * pause during an animation therefore remains safe and does not
             * freeze the animation while waiting for a saveable frame. */
            bool can_save_snapshot = g_system_snapshot_valid;

            if (pause_page == PAUSE_PAGE_MAIN) {
                if (nav_up) { pause_selected = (pause_selected + 4) % 5; hal_play_sound_gain(SND_FLIP, 35); }
                if (nav_down) { pause_selected = (pause_selected + 1) % 5; hal_play_sound_gain(SND_FLIP, 35); }
                if (back_pressed) {
                    if (g_settings_dirty) settings_save();
                    in_pause = false;
                    hal_play_sound_gain(SND_FLIP, 35);
                } else if (accept) {
                    if (pause_selected == 0) {
                        if (g_settings_dirty) settings_save();
                        in_pause = false;
                    } else if (pause_selected == 1 || pause_selected == 2) {
                        if (pause_selected == 1 && !can_save_snapshot) {
                            snprintf(pause_message, sizeof(pause_message), "%s S0", ui_tr(UI_STR_WAIT_ACTION));
                            pause_message_until = pause_now + 1800u;
                            hal_play_sound(SND_ERROR);
                        } else {
                            pause_page = pause_selected == 1
                                       ? PAUSE_PAGE_SAVE_SLOTS : PAUSE_PAGE_LOAD_SLOTS;
                            pause_slot_selected = 0;
                        }
                    } else if (pause_selected == 3) {
                        pause_page = PAUSE_PAGE_OPTIONS;
                        pause_options_selected = 0;
                    } else {
                        if (g_settings_dirty) settings_save();
                        if (mode_session.mode == MODE_CAREER)
                            (void)career_progress_save(&mode_session, &game);
                        in_pause = false;
                        in_menu = true;
                        in_difficulty_select = false;
                        in_mode_select = false;
                        in_br_player_select = false;
                        in_avatar_select = false;
                        in_tournament_bracket = false;
                        in_tournament_champion = false;
                        reset_frontend_run(&game, &mode_session, &tournament, anims,
                                           &selected_difficulty, &selected_mode, &selected_face,
                                           &pending_action, &result_processed, &last_result_sfx,
                                           &confetti_running, &selected_card_idx, &ai_delay,
                                           &anim_phase, &hide_table, &single_target_pair_idx,
                                           &pause_selected);
                        memset(&battle_royal, 0, sizeof(battle_royal));
                        br_time_bonus_ms = 0u;
                        action_hold_reset(&a_fast_hold);
                        br_ui_reset(&br_ui, pause_now);
                        br_ui_sync_hands(&br_ui, &battle_royal);
                        session_play_time_ms = 0;
                        table_dust_init(table_dust);
                    }
                    hal_play_sound_gain(SND_FLIP, 35);
                }
            } else if (pause_page == PAUSE_PAGE_OPTIONS) {
                if (nav_up) { pause_options_selected = (pause_options_selected + 4) % 5; hal_play_sound_gain(SND_FLIP, 35); }
                if (nav_down) { pause_options_selected = (pause_options_selected + 1) % 5; hal_play_sound_gain(SND_FLIP, 35); }
                bool language_change = pause_options_selected == 3 && (nav_left || nav_right || accept);
                if (language_change) {
                    const int old_deck_style = g_settings.deck_style;
                    int next = (int)g_settings.language + ((nav_left && !accept) ? -1 : 1);
                    if (next < 0) next = UI_LANG_COUNT - 1;
                    if (next >= UI_LANG_COUNT) next = 0;
                    if (dialogue_load_language(next)) {
                        g_settings.language = (uint8_t)next;
                        selected_language = next;
                        ui_set_language(next);
                        if (ui_language_uses_cp1251(next) && font_uk_tex && dialogue_font_uk_tex) {
                            font_tex = font_uk_tex;
                            dialogue_font_tex = dialogue_font_uk_tex;
                        } else {
                            font_tex = font_ascii_tex;
                            dialogue_font_tex = dialogue_font_ascii_tex;
                        }
                        g_settings.deck_style = deck_style_for_language(old_deck_style, next);
                        cards_tex = card_style_tex[g_settings.deck_style]
                                  ? card_style_tex[g_settings.deck_style] : cards_uk_tex;
                        card_back = card_style_back[g_settings.deck_style]
                                  ? card_style_back[g_settings.deck_style]
                                  : card_back_classic_tex;
                        br_cards_tex = cards_tex;
                        br_card_back = card_back;
                        dialogue_reset(&dialogue);
                        dialogue_sync_hand_counts(&dialogue, &game);
                        br_ui_reset(&br_ui, pause_now);
                        br_ui_sync_hands(&br_ui, &battle_royal);
                        g_settings_dirty = true;
                        hal_play_sound_gain(SND_FLIP, 35);
                    } else {
                        hal_play_sound(SND_ERROR);
                    }
                }
                if (back_pressed) {
                    if (g_settings_dirty) settings_save();
                    pause_page = PAUSE_PAGE_MAIN;
                    pause_selected = 3;
                    hal_play_sound_gain(SND_FLIP, 35);
                } else if (accept && pause_options_selected != 3) {
                    if (pause_options_selected == 0) {
                        pause_page = PAUSE_PAGE_SOUND;
                        pause_sound_selected = 0;
                    } else if (pause_options_selected == 1) {
                        pause_page = PAUSE_PAGE_GRAPHICS;
                        pause_graphics_selected = 0;
                    } else if (pause_options_selected == 2) {
                        pause_page = PAUSE_PAGE_GAMEPLAY;
                        pause_gameplay_selected = 0;
                    } else {
                        if (g_settings_dirty) settings_save();
                        pause_page = PAUSE_PAGE_MAIN;
                        pause_selected = 3;
                    }
                    hal_play_sound_gain(SND_FLIP, 35);
                }
            } else if (pause_page == PAUSE_PAGE_SOUND) {
                if (nav_up) { pause_sound_selected = (pause_sound_selected + 3) % 4; hal_play_sound_gain(SND_FLIP, 35); }
                if (nav_down) { pause_sound_selected = (pause_sound_selected + 1) % 4; hal_play_sound_gain(SND_FLIP, 35); }
                bool changed = false;
                if (pause_sound_selected == 0 && (nav_left || nav_right)) {
                    int value = (int)g_settings.effects_volume + (nav_right ? 1 : -1);
                    if (value < 0) value = 0;
                    if (value > 10) value = 10;
                    if (value != g_settings.effects_volume) {
                        g_settings.effects_volume = (uint8_t)value;
                        hal_set_effects_volume_step(g_settings.effects_volume);
                        changed = true;
                    }
                } else if (pause_sound_selected == 1 && (nav_left || nav_right)) {
                    int value = (int)g_settings.music_volume + (nav_right ? 1 : -1);
                    if (value < 0) value = 0;
                    if (value > 10) value = 10;
                    if (value != g_settings.music_volume) {
                        g_settings.music_volume = (uint8_t)value;
                        hal_set_music_volume_step(g_settings.music_volume);
                        changed = true;
                    }
                } else if (pause_sound_selected == 2 && (nav_left || nav_right || accept)) {
                    g_settings.dialogues_enabled = !g_settings.dialogues_enabled;
                    br_ui_set_dialogues_enabled(g_settings.dialogues_enabled);
                    if (!g_settings.dialogues_enabled) {
                        dialogue_reset(&dialogue);
                        dialogue_sync_hand_counts(&dialogue, &game);
                        br_ui_reset(&br_ui, pause_now);
                        br_ui_sync_hands(&br_ui, &battle_royal);
                    }
                    changed = true;
                }
                if (changed) {
                    g_settings_dirty = true;
                    hal_play_sound_gain(SND_FLIP, 35);
                }
                if (back_pressed || (accept && pause_sound_selected == 3)) {
                    pause_page = PAUSE_PAGE_OPTIONS;
                    pause_options_selected = 0;
                    hal_play_sound_gain(SND_FLIP, 35);
                }
            } else if (pause_page == PAUSE_PAGE_GRAPHICS) {
                if (nav_up || nav_down) {
                    const int delta = nav_down ? 1 : 5;
                    do {
                        pause_graphics_selected = (pause_graphics_selected + delta) % 6;
                    } while (!g_settings.snow_enabled && pause_graphics_selected == 4);
                    hal_play_sound_gain(SND_FLIP, 35);
                }
                bool changed = false;
                if (pause_graphics_selected == 0 && (nav_left || nav_right || accept)) {
                    g_settings.shadows_enabled = !g_settings.shadows_enabled;
                    hal_set_shadows_enabled(g_settings.shadows_enabled);
                    changed = true;
                } else if (pause_graphics_selected == 1 && (nav_left || nav_right)) {
                    int value = (int)g_settings.table_style + (nav_right ? 1 : -1);
                    if (value < 0) value = TABLE_STYLE_COUNT - 1;
                    if (value >= TABLE_STYLE_COUNT) value = 0;
                    g_settings.table_style = (uint8_t)value;
                    hal_set_table_style(g_settings.table_style);
                    changed = true;
                } else if (pause_graphics_selected == 2 && (nav_left || nav_right)) {
                    int value = (int)g_settings.particle_count + (nav_right ? 1 : -1);
                    if (value < 1) value = 1;
                    if (value > 20) value = 20;
                    if (value != g_settings.particle_count) {
                        g_settings.particle_count = (uint8_t)value;
                        table_dust_set_count(table_dust, value);
                        changed = true;
                    }
                } else if (pause_graphics_selected == 3 && (nav_left || nav_right || accept)) {
                    g_settings.snow_enabled = !g_settings.snow_enabled;
                    changed = true;
                } else if (pause_graphics_selected == 4 && g_settings.snow_enabled &&
                           (nav_left || nav_right)) {
                    int value = (int)g_settings.snowflake_count + (nav_right ? 1 : -1);
                    if (value < 20) value = 20;
                    if (value > 40) value = 40;
                    if (value != g_settings.snowflake_count) {
                        g_settings.snowflake_count = (uint8_t)value;
                        changed = true;
                    }
                }
                if (changed) {
                    g_settings_dirty = true;
                    hal_play_sound_gain(SND_FLIP, 35);
                }
                if (back_pressed || (accept && pause_graphics_selected == 5)) {
                    pause_page = PAUSE_PAGE_OPTIONS;
                    pause_options_selected = 1;
                    hal_play_sound_gain(SND_FLIP, 35);
                }
            } else if (pause_page == PAUSE_PAGE_GAMEPLAY) {
                if (nav_up) {
                    pause_gameplay_selected = (pause_gameplay_selected + 3) % 4;
                    hal_play_sound_gain(SND_FLIP, 35);
                }
                if (nav_down) {
                    pause_gameplay_selected = (pause_gameplay_selected + 1) % 4;
                    hal_play_sound_gain(SND_FLIP, 35);
                }
                if (pause_gameplay_selected == 0 && (nav_left || nav_right || accept)) {
                    g_settings.auto_sort_enabled = !g_settings.auto_sort_enabled;
                    g_settings_dirty = true;
                    hal_play_sound_gain(SND_FLIP, 35);
                } else if (pause_gameplay_selected == 1 && (nav_left || nav_right || accept)) {
                    int direction = nav_left ? -1 : 1;
                    g_settings.deck_style = next_available_deck_style(
                        g_settings.deck_style, direction, g_settings.cats_unlocked);
                    cards_tex = card_style_tex[g_settings.deck_style]
                              ? card_style_tex[g_settings.deck_style] : cards_uk_tex;
                    card_back = card_style_back[g_settings.deck_style]
                              ? card_style_back[g_settings.deck_style]
                              : card_back_classic_tex;
                    br_cards_tex = cards_tex;
                    br_card_back = card_back;
                    g_settings_dirty = true;
                    hal_play_sound_gain(SND_FLIP, 35);
                } else if (pause_gameplay_selected == 2 && (nav_left || nav_right)) {
                    int value = (int)g_settings.game_speed + (nav_right ? 1 : -1);
                    if (value < -5) value = -5;
                    if (value > 5) value = 5;
                    if (value != g_settings.game_speed) {
                        g_settings.game_speed = (int8_t)value;
                        br_ui_set_game_speed_step(value);
                        g_settings_dirty = true;
                        hal_play_sound_gain(SND_FLIP, 35);
                    }
                }
                if (back_pressed || (accept && pause_gameplay_selected == 3)) {
                    pause_page = PAUSE_PAGE_OPTIONS;
                    pause_options_selected = 2;
                    hal_play_sound_gain(SND_FLIP, 35);
                }
            } else if (pause_page == PAUSE_PAGE_SAVE_SLOTS || pause_page == PAUSE_PAGE_LOAD_SLOTS) {
                if (vertical_up) { pause_slot_selected = (pause_slot_selected + 3) % 4; hal_play_sound_gain(SND_FLIP, 35); }
                if (vertical_down) { pause_slot_selected = (pause_slot_selected + 1) % 4; hal_play_sound_gain(SND_FLIP, 35); }
                if (back_pressed) {
                    bool from_save_slots = pause_page == PAUSE_PAGE_SAVE_SLOTS;
                    pause_page = PAUSE_PAGE_MAIN;
                    pause_selected = from_save_slots ? 1 : 2;
                    hal_play_sound_gain(SND_FLIP, 35);
                } else if (accept) {
                    if (pause_slot_selected == 3) {
                        bool from_save_slots = pause_page == PAUSE_PAGE_SAVE_SLOTS;
                        pause_page = PAUSE_PAGE_MAIN;
                        pause_selected = from_save_slots ? 1 : 2;
                    } else if (pause_page == PAUSE_PAGE_SAVE_SLOTS) {
                        if (!g_system_snapshot_valid) {
                            snprintf(pause_message, sizeof(pause_message), "%s S0", ui_tr(UI_STR_WAIT_ACTION));
                            pause_message_until = pause_now + 1800u;
                            hal_play_sound(SND_ERROR);
                        } else {
                            SaveGameMeta meta = savegame_read_meta(pause_slot_selected);
                            if (meta.exists) {
                                pause_page = PAUSE_PAGE_CONFIRM_OVERWRITE;
                                pause_confirm_yes = false;
                            } else {
                                bool ok = savegame_write_slot(pause_slot_selected,
                                                              &g_system_save_buffer);
                                if (ok) {
                                    snprintf(pause_message, sizeof(pause_message), "%s", ui_tr(UI_STR_GAME_SAVED));
                                } else {
                                    snprintf(pause_message, sizeof(pause_message), "%s E%d", ui_tr(UI_STR_SAVE_FAILED), savegame_last_error());
                                }
                                pause_message_until = pause_now + 1800u;
                                hal_play_sound(ok ? SND_SELECT : SND_ERROR);
                            }
                        }
                    } else {
                        SaveGameMeta meta = savegame_read_meta(pause_slot_selected);
                        if (!meta.exists || !meta.compatible || meta.corrupted) {
                            snprintf(pause_message, sizeof(pause_message), "%s", meta.exists ? ui_tr(UI_STR_BAD_SAVE) : ui_tr(UI_STR_EMPTY_SLOT));
                            pause_message_until = pause_now + 1800u;
                            hal_play_sound(SND_ERROR);
                        } else {
                            pause_page = PAUSE_PAGE_CONFIRM_LOAD;
                            pause_confirm_yes = false;
                        }
                    }
                }
            } else {
                if (vertical_up || vertical_down) { pause_confirm_yes = !pause_confirm_yes; hal_play_sound_gain(SND_FLIP, 35); }
                if (back_pressed) {
                    pause_page = pause_page == PAUSE_PAGE_CONFIRM_OVERWRITE
                               ? PAUSE_PAGE_SAVE_SLOTS : PAUSE_PAGE_LOAD_SLOTS;
                    hal_play_sound_gain(SND_FLIP, 35);
                } else if (accept) {
                    if (!pause_confirm_yes) {
                        pause_page = pause_page == PAUSE_PAGE_CONFIRM_OVERWRITE
                                   ? PAUSE_PAGE_SAVE_SLOTS : PAUSE_PAGE_LOAD_SLOTS;
                    } else if (pause_page == PAUSE_PAGE_CONFIRM_OVERWRITE) {
                        bool ok = g_system_snapshot_valid &&
                                  savegame_write_slot(pause_slot_selected,
                                                      &g_system_save_buffer);
                        pause_page = PAUSE_PAGE_SAVE_SLOTS;
                        if (ok) {
                            snprintf(pause_message, sizeof(pause_message), "%s", ui_tr(UI_STR_GAME_SAVED));
                        } else {
                            snprintf(pause_message, sizeof(pause_message), "%s E%d", ui_tr(UI_STR_SAVE_FAILED), savegame_last_error());
                        }
                        pause_message_until = pause_now + 1800u;
                        hal_play_sound(ok ? SND_SELECT : SND_ERROR);
                    } else {
                        SaveGamePayload payload;
                        if (savegame_read_slot(pause_slot_selected, &payload)) {
                            apply_save_payload_runtime(&payload, &game, &mode_session, &tournament,
                                                       &battle_royal, &selected_difficulty, &selected_mode,
                                                       &selected_face, &selected_br_players, &selected_card_idx,
                                                       &pending_action, &result_processed, &last_result_sfx,
                                                       &session_play_time_ms);
                            br_time_bonus_ms = 0u;
                            action_hold_reset(&a_fast_hold);
                            hand_visual_reset();
                            normal_npc_taking = false;
                            normal_resolve_action = NORMAL_RESOLVE_NONE;
                            normal_resolve_at_ms = 0u;
                            in_pause = false;
                            pause_page = PAUSE_PAGE_MAIN;
                            anim_phase = 0;
                            hide_table = false;
                            br_hide_table = false;
                            br_event_started = false;
                            br_intro_voices_active = false;
                            memset(anims, 0, sizeof(anims));
                            dialogue_reset(&dialogue);
                            dialogue_sync_hand_counts(&dialogue, &game);
                            br_ui_reset(&br_ui, pause_now);
                            br_ui_sync_hands(&br_ui, &battle_royal);
                            ai_delay = 0;
                            hal_play_sound(SND_SELECT);
                        } else {
                            pause_page = PAUSE_PAGE_LOAD_SLOTS;
                            snprintf(pause_message, sizeof(pause_message), "%s", ui_tr(UI_STR_LOAD_FAILED));
                            pause_message_until = pause_now + 1800u;
                            hal_play_sound(SND_ERROR);
                        }
                    }
                }
            }

            hal_clear_screen(0x000000);
            if (font_tex) {
                draw_text_shadowed(font_tex, ui_tr(UI_STR_PAUSED), (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_PAUSED)) * 8) / 2, 38);
                if (pause_page == PAUSE_PAGE_MAIN) {
                    const char* items[5] = {
                        ui_tr(UI_STR_CONTINUE), ui_tr(UI_STR_SAVE_GAME),
                        ui_tr(UI_STR_LOAD_GAME), ui_tr(UI_STR_OPTIONS),
                        ui_tr(UI_STR_GO_BACK_MENU)
                    };
                    for (int i = 0; i < 5; ++i)
                        draw_pause_item(font_tex, items[i], NULL, 70 + i * 25,
                                        i == pause_selected);
                } else if (pause_page == PAUSE_PAGE_OPTIONS) {
                    const char* items[5] = {
                        ui_tr(UI_STR_SOUND), ui_tr(UI_STR_GRAPHICS),
                        ui_tr(UI_STR_GAMEPLAY), ui_tr(UI_STR_LANGUAGE),
                        ui_tr(UI_STR_BACK)
                    };
                    draw_text(font_tex, ui_tr(UI_STR_OPTIONS),
                              (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_OPTIONS)) * 8) / 2, 38);
                    for (int i = 0; i < 5; ++i) {
                        const char* value = i == 3 ? ui_language_label(g_settings.language) : NULL;
                        draw_pause_item(font_tex, items[i], value, 66 + i * 28,
                                        i == pause_options_selected);
                    }
                } else if (pause_page == PAUSE_PAGE_SOUND) {
                    char effects_value[20], music_value[20];
                    make_slider_text(effects_value, g_settings.effects_volume, 10);
                    make_slider_text(music_value, g_settings.music_volume, 10);
                    draw_text(font_tex, ui_tr(UI_STR_SOUND),
                              (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_SOUND)) * 8) / 2, 48);
                    draw_pause_item(font_tex, ui_tr(UI_STR_EFFECTS), effects_value, 78,
                                    pause_sound_selected == 0);
                    draw_pause_item(font_tex, ui_tr(UI_STR_MUSIC), music_value, 106,
                                    pause_sound_selected == 1);
                    draw_pause_item(font_tex, ui_tr(UI_STR_DIALOGUES),
                                    ui_tr(g_settings.dialogues_enabled ? UI_STR_ON : UI_STR_OFF), 134,
                                    pause_sound_selected == 2);
                    draw_pause_item(font_tex, ui_tr(UI_STR_BACK), NULL, 174,
                                    pause_sound_selected == 3);
                } else if (pause_page == PAUSE_PAGE_GRAPHICS) {
                    char particles_value[20], snowflakes_value[20];
                    char snowflakes_disabled[20];
                    make_slider_text(particles_value, g_settings.particle_count, 20);
                    make_slider_range_text(snowflakes_value, g_settings.snowflake_count, 20, 40);
                    if (!g_settings.snow_enabled) snprintf(snowflakes_disabled, sizeof(snowflakes_disabled), "--");
                    UiStringId table_id = UI_STR_DARK;
                    switch (g_settings.table_style) {
                        case TABLE_STYLE_LIGHT_MOSS: table_id = UI_STR_LIGHT; break;
                        case TABLE_STYLE_SOFT_MOSS: table_id = UI_STR_SOFT_MOSS; break;
                        default: table_id = UI_STR_DARK; break;
                    }
                    const char* table_value = ui_tr(table_id);
                    draw_text(font_tex, ui_tr(UI_STR_GRAPHICS),
                              (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_GRAPHICS)) * 8) / 2, 38);
                    draw_pause_item(font_tex, ui_tr(UI_STR_SHADOWS),
                                    ui_tr(g_settings.shadows_enabled ? UI_STR_ON : UI_STR_OFF), 62,
                                    pause_graphics_selected == 0);
                    draw_pause_item(font_tex, ui_tr(UI_STR_TABLE_COLOUR), table_value, 86,
                                    pause_graphics_selected == 1);
                    draw_pause_item(font_tex, ui_tr(UI_STR_PARTICLES), particles_value, 110,
                                    pause_graphics_selected == 2);
                    draw_pause_item(font_tex, ui_tr(UI_STR_SNOW),
                                    ui_tr(g_settings.snow_enabled ? UI_STR_ON : UI_STR_OFF), 134,
                                    pause_graphics_selected == 3);
                    draw_pause_item(font_tex, ui_tr(UI_STR_SNOWFLAKES),
                                    g_settings.snow_enabled ? snowflakes_value : snowflakes_disabled, 158,
                                    pause_graphics_selected == 4);
                    draw_pause_item(font_tex, ui_tr(UI_STR_BACK), NULL, 194,
                                    pause_graphics_selected == 5);
                } else if (pause_page == PAUSE_PAGE_GAMEPLAY) {
                    draw_text(font_tex, ui_tr(UI_STR_GAMEPLAY),
                              (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_GAMEPLAY)) * 8) / 2, 42);
                    char speed_value[8];
                    snprintf(speed_value, sizeof(speed_value), "%+d", (int)g_settings.game_speed);
                    draw_pause_item(font_tex, ui_tr(UI_STR_AUTO_SORT),
                                    ui_tr(g_settings.auto_sort_enabled ? UI_STR_ON : UI_STR_OFF), 72,
                                    pause_gameplay_selected == 0);
                    draw_pause_item(font_tex, "DECK",
                                    deck_style_label(g_settings.deck_style), 102,
                                    pause_gameplay_selected == 1);
                    draw_pause_item(font_tex, ui_tr(UI_STR_GAME_SPEED), speed_value, 132,
                                    pause_gameplay_selected == 2);
                    draw_pause_item(font_tex, ui_tr(UI_STR_BACK), NULL, 174,
                                    pause_gameplay_selected == 3);
                } else if (pause_page == PAUSE_PAGE_SAVE_SLOTS || pause_page == PAUSE_PAGE_LOAD_SLOTS) {
                    {
                        const char* page_title = pause_page == PAUSE_PAGE_SAVE_SLOTS
                                               ? ui_tr(UI_STR_SAVE_GAME) : ui_tr(UI_STR_LOAD_GAME);
                        draw_text(font_tex, page_title,
                                  (SCREEN_WIDTH - (int)strlen(page_title) * 8) / 2, 58);
                    }
                    for (int i = 0; i < 3; ++i) {
                        SaveGameMeta meta = savegame_read_meta(i);
                        char line[38];
                        if (!meta.exists) {
                            snprintf(line, sizeof(line), "%s %d  %s", ui_tr(UI_STR_SLOT), i + 1, ui_tr(UI_STR_EMPTY));
                        } else if (!meta.compatible || meta.corrupted) {
                            snprintf(line, sizeof(line), "%s %d  %s", ui_tr(UI_STR_SLOT), i + 1, ui_tr(UI_STR_INCOMPATIBLE));
                        } else {
                            uint32_t sec = meta.play_time_ms / 1000u;
                            uint32_t min = sec / 60u;
                            sec %= 60u;
                            snprintf(line, sizeof(line), "%s %d %s %02lu:%02lu", ui_tr(UI_STR_SLOT), i + 1,
                                     ui_mode_name_localized(meta.mode), (unsigned long)min, (unsigned long)sec);
                        }
                        int y = 86 + i * 28;
                        if (i == pause_slot_selected) draw_text(font_tex, ">", 8, y);
                        draw_text(font_tex, line, 24, y);
                    }
                    if (pause_slot_selected == 3) draw_text(font_tex, ">", 104, 178);
                    draw_text(font_tex, ui_tr(UI_STR_BACK), (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_BACK)) * 8) / 2, 178);
                } else {
                    const char* title = pause_page == PAUSE_PAGE_CONFIRM_OVERWRITE
                                      ? ui_tr(UI_STR_OVERWRITE_SLOT) : ui_tr(UI_STR_LOAD_SLOT);
                    draw_text(font_tex, title, (SCREEN_WIDTH - (int)strlen(title) * 8) / 2, 76);
                    if (pause_page == PAUSE_PAGE_CONFIRM_LOAD)
                        draw_text(font_tex, ui_tr(UI_STR_CURRENT_PROGRESS_LOST), (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_CURRENT_PROGRESS_LOST)) * 8) / 2, 100);
                    {
                        char yes_line[48];
                        snprintf(yes_line, sizeof(yes_line), "%s %s", pause_confirm_yes ? ">" : " ", ui_tr(UI_STR_YES));
                        draw_text(font_tex, yes_line, 112, 132);
                    }
                    {
                        char no_line[48];
                        snprintf(no_line, sizeof(no_line), "%s %s", pause_confirm_yes ? " " : ">", ui_tr(UI_STR_NO));
                        draw_text(font_tex, no_line, 112, 158);
                    }
                }
                if (pause_message[0] && pause_now < pause_message_until)
                    draw_text(font_tex, pause_message, (SCREEN_WIDTH - (int)strlen(pause_message) * 8) / 2, 214);
            }
            present_with_transition(UI_SCREEN_PAUSE, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }

        if (mode_session.mode == MODE_BATTLE_ROYAL) {
            const uint32_t br_real_now = hal_get_ticks_ms();
            const bool br_fast_available = battle_royal.players[0].present &&
                                           battle_royal.players[0].round_out &&
                                           battle_royal.spectator &&
                                           battle_royal.phase != BR_PHASE_ROUND_RESULT &&
                                           battle_royal.phase != BR_PHASE_NEXT_PROMPT &&
                                           battle_royal.phase != BR_PHASE_CHAMPION;
            const bool br_fast_active = action_fast_hold_update(
                &a_fast_hold, br_fast_available, br_real_now);
            if (br_fast_active)
                br_time_bonus_ms += frame_dt_ms * (BR_FAST_MULTIPLIER - 1u);
            const uint32_t br_now = br_real_now + br_time_bonus_ms;
            /* A hold is valid only during the current human defense decision.
             * Reset it on every other phase so an old press cannot carry over
             * through an animation or an AI turn. */
            if (!(br_human_can_act(&battle_royal) &&
                  battle_royal.phase == BR_PHASE_DEFEND)) {
                action_hold_reset(&b_action_hold);
            }
            if (br_intro_voices_active) {
                if (!hal_audio_busy()) {
                    while (br_intro_voice_cursor < BR_MAX_PLAYERS &&
                           !battle_royal.players[br_intro_voice_cursor].present)
                        br_intro_voice_cursor++;
                    if (br_intro_voice_cursor < BR_MAX_PLAYERS) {
                        hal_play_face_voice(battle_royal.players[br_intro_voice_cursor].face_index);
                        br_intro_voice_cursor++;
                    } else {
                        br_intro_voices_active = false;
                        battle_royal.phase_until_ms = br_now + 1500u;
                    }
                }
            } else {
                br_update_clock(&battle_royal, br_now);
                if (br_fast_active) {
                    const uint32_t accelerated_extra = frame_dt_ms * (BR_FAST_MULTIPLIER - 1u);
                    if (battle_royal.play_time_ms >= accelerated_extra)
                        battle_royal.play_time_ms -= accelerated_extra;
                }
            }

            bool br_anim_running = visual_anims_update_no_commit(anims, br_now);
            if (br_event_started && !br_anim_running && !visual_anims_any(anims)) {
                const BREventType completed_event = battle_royal.event.type;
                br_ack_event(&battle_royal, br_now);
                br_ui_after_event_ack(&br_ui, &battle_royal, completed_event, br_real_now);
                br_event_started = false;
                br_hide_table = false;
            }

            if (battle_royal.event_pending && !br_event_started) {
                BREvent event_copy = battle_royal.event;
                br_ui_react_event(&br_ui, &battle_royal, &event_copy, br_real_now);
                if (event_copy.type == BR_EVENT_ATTACK || event_copy.type == BR_EVENT_DEFEND)
                    hal_play_sound(SND_CARD);
                else if (event_copy.type == BR_EVENT_TAKE_DECLARED)
                    hal_play_sound(SND_TAKE);
                else if (event_copy.type == BR_EVENT_RESOLVE_BEAT || event_copy.type == BR_EVENT_RESOLVE_TAKE)
                    hal_play_sound(SND_SHUFFLE);
                else if (event_copy.type == BR_EVENT_DEAL)
                    hal_play_sound(SND_CARD);
                else if (event_copy.type == BR_EVENT_ROUND_FINISHED)
                    hal_play_sound(SND_LOSE);

                br_start_event_animation(&battle_royal, anims, &br_hide_table);
                br_event_started = true;
                if (!visual_anims_any(anims)) {
                    const BREventType completed_event = battle_royal.event.type;
                    br_ack_event(&battle_royal, br_now);
                    br_ui_after_event_ack(&br_ui, &battle_royal, completed_event, br_real_now);
                    br_event_started = false;
                    br_hide_table = false;
                }
            }

            bool br_busy = br_event_started || battle_royal.event_pending || visual_anims_any(anims);
            /* Sort before any cursor movement or A-button action. Previously the
             * BR hand was sorted after input, so the selected index could refer
             * to the old order and auto-sort appeared broken. */
            if (!br_busy) auto_sort_br_hand(&battle_royal, false);
            const bool br_human_actionable = !br_busy && br_human_can_act(&battle_royal);
            br_ui_update_stalling(&br_ui, &battle_royal, br_human_actionable,
                                  hal_is_any_button_pressed(), br_real_now);
            if (!br_busy) {
                if (battle_royal.phase == BR_PHASE_ROUND_RESULT) {
                    if (hal_is_button_pressed(BTN_A)) {
                        br_confirm_round_result(&battle_royal);
                        br_confirm_next_prompt(&battle_royal, br_now);
                        if (battle_royal.phase == BR_PHASE_ROUND_BANNER)
                            hal_music_start_gameplay_new_match();
                        br_ui_new_round(&br_ui, br_now);
                        br_ui_sync_hands(&br_ui, &battle_royal);
                        br_champion_voice_played = false;
                        if (battle_royal.phase == BR_PHASE_ROUND_BANNER) hal_play_sound(SND_SHUFFLE);
                        else hal_play_sound_gain(SND_FLIP, 35);
                    }
                } else if (battle_royal.phase == BR_PHASE_NEXT_PROMPT) {
                    /* Compatibility with saves made while this transient phase existed. */
                    if (hal_is_button_pressed(BTN_A)) {
                        br_confirm_next_prompt(&battle_royal, br_now);
                        if (battle_royal.phase == BR_PHASE_ROUND_BANNER)
                            hal_music_start_gameplay_new_match();
                        br_ui_new_round(&br_ui, br_now);
                        br_ui_sync_hands(&br_ui, &battle_royal);
                        br_champion_voice_played = false;
                        hal_play_sound(SND_SHUFFLE);
                    }
                } else if (battle_royal.phase == BR_PHASE_CHAMPION) {
                    if (!br_champion_voice_played && battle_royal.champion >= 0) {
                        hal_play_face_voice(battle_royal.players[battle_royal.champion].face_index);
                        br_champion_voice_played = true;
                    }
                    if (hal_is_button_pressed(BTN_A)) {
                        in_menu = true;
                        in_difficulty_select = false;
                        in_mode_select = false;
                        in_br_player_select = false;
                        in_avatar_select = false;
                        mode_session.mode = MODE_CAREER;
                        hal_play_sound_gain(SND_FLIP, 35);
                    }
                } else if (br_human_can_act(&battle_royal)) {
                    int count = battle_royal.players[0].hand.card_count;
                    if (count > 0) {
                        if (battle_royal.selected_card >= count) battle_royal.selected_card = count - 1;
                        if (battle_royal.selected_card < 0) battle_royal.selected_card = 0;
                        card_scroll_update(&battle_royal.selected_card, count);
                    } else {
                        battle_royal.selected_card = 0;
                        card_scroll_reset();
                    }
                    if (hal_is_button_pressed(BTN_A)) {
                        if (!br_human_play(&battle_royal, battle_royal.selected_card, br_now))
                            hal_play_sound(SND_ERROR);
                    }
                    const bool br_take_available = battle_royal.phase == BR_PHASE_DEFEND;
                    const bool br_take_now = action_take_hold_update(
                        &b_action_hold, br_take_available, br_now);
                    if (br_take_now) {
                        if (!br_human_take(&battle_royal, br_now)) hal_play_sound(SND_ERROR);
                    } else if (!br_take_available &&
                               battle_royal.phase == BR_PHASE_THROW &&
                               hal_is_button_pressed(BTN_B)) {
                        if (!br_human_pass(&battle_royal, br_now)) hal_play_sound(SND_ERROR);
                    }
                } else {
                    card_scroll_reset();
                    br_update_ai(&battle_royal, br_now);
                }
            }

            br_ui_draw(&battle_royal, &br_ui, br_cards_tex, br_card_back, br_faces_tex,
                       br_player_faces_tex,
                       dialogue_font_tex, dialog_tex, br_hide_table);
            const bool br_take_hint = br_human_can_act(&battle_royal) &&
                                      battle_royal.phase == BR_PHASE_DEFEND;
            const bool br_pass_hint = br_human_can_act(&battle_royal) &&
                                      battle_royal.phase == BR_PHASE_THROW;
            if ((br_take_hint || br_pass_hint) &&
                action_hint_visible(br_now, br_take_hint, &b_action_hold)) {
                const UiStringId br_hint_id = br_take_hint
                    ? UI_STR_ACTION_TAKE
                    : (battle_royal.defender_taking
                        ? UI_STR_THROW_IN_DONE : UI_STR_ACTION_BEAT);
                draw_br_action_hint(dialogue_font_tex, ui_tr(br_hint_id));
            }
            if (br_fast_available) {
                draw_br_action_hint(dialogue_font_tex,
                                    ui_tr(br_fast_active ? UI_STR_FAST_FORWARD_X4
                                                         : UI_STR_HOLD_A_FAST));
            }
            /* BR UI disables felt lighting before portraits/HUD. Re-enable it
             * only for flying cards so their brightness and shadow preset match
             * the table without touching the UI. */
            hal_set_table_sprite_lighting(true);
            br_draw_visual_anims(&battle_royal, anims, br_cards_tex, br_card_back);
            hal_set_table_sprite_lighting(false);
            snow_draw(snow);
            present_with_transition(UI_SCREEN_GAME, &transition_last_screen, &transition_fade_level);
            hal_delay(16);
            continue;
        }

        bool is_animating = false;
        if (anim_phase > 0) {
            is_animating = true;
        } else {
            for (int i = 0; i < MAX_ANIMS; i++) {
                if (anims[i].active) { is_animating = true; break; }
            }
        }

        int p_count = game.players[0].card_count;
        bool need_defense = (game.table_pair_count > 0 && game.table_defense[game.table_pair_count - 1].rank < 6);
        const bool human_actionable = !is_animating &&
            normal_resolve_action == NORMAL_RESOLVE_NONE &&
            game.result == RESULT_NONE && p_count > 0 &&
            (normal_npc_taking || (game.is_player_turn && !need_defense) ||
             (!game.is_player_turn && need_defense));
        dialogue_stalling_update(&dialogue, game.current_opponent.face_index,
                                 human_actionable, hal_is_any_button_pressed(),
                                 now_loop_ms);

        if (is_animating || game.result != RESULT_NONE || p_count <= 0) {
            card_scroll_reset();
        }

        bool any_anim_running = false;
        uint32_t anim_now_ms = hal_get_ticks_ms();
        for (int i = 0; i < MAX_ANIMS; i++) {
            VisualAnim* a = &anims[i];
            if (!a->active) continue;

            if (!a->timing_started) {
                a->timing_started = true;
                a->start_ms = anim_now_ms + a->start_delay_ms;
                a->duration_ms = visual_anim_duration_ms(a);
                a->start_x = a->cx;
                a->start_y = a->cy;
                a->scale = 1.0f;
            }

            if ((int32_t)(anim_now_ms - a->start_ms) < 0) {
                any_anim_running = true;
                continue;
            }

            uint32_t elapsed = anim_now_ms - a->start_ms;
            const uint32_t flight_ms = visual_anim_flight_ms(a);
            const uint32_t landing_ms = visual_anim_landing_ms(a);
            if (elapsed < flight_ms) {
                float t = (float)elapsed / (float)(flight_ms ? flight_ms : 1u);
                visual_anim_flight_position(a, t, &a->cx, &a->cy);
                a->scale = visual_anim_scale(t);
                a->frames_left = (int)(((flight_ms - elapsed) *
                                         (uint32_t)(a->total_frames > 0 ? a->total_frames : ANIM_FRAMES)) /
                                        (flight_ms ? flight_ms : 1u));
                if (a->frames_left < 1) a->frames_left = 1;
                any_anim_running = true;
            } else if (elapsed < flight_ms + landing_ms) {
                uint32_t land = elapsed - flight_ms;
                if (a->destination_is_table) {
                    const float t = (float)land / (float)(landing_ms ? landing_ms : 1u);
                    const float settle = visual_surface_friction(t);
                    float contact_x = a->tx;
                    float contact_y = a->ty;
                    visual_anim_contact_point(a, &contact_x, &contact_y);
                    a->cx = contact_x + (a->tx - contact_x) * settle;
                    a->cy = contact_y + (a->ty - contact_y) * settle;
                } else {
                    const uint32_t third = landing_ms / 3u;
                    a->cx = a->tx;
                    if (land < third) a->cy = a->ty - 2.0f;
                    else if (land < third * 2u) a->cy = a->ty + 1.0f;
                    else a->cy = a->ty;
                }
                a->scale = 1.0f;
                a->frames_left = 0;
                any_anim_running = true;
            } else {
                a->cx = a->tx;
                a->cy = a->ty;
                a->scale = 1.0f;
                a->frames_left = 0;
                if (a->deal_card && !a->landing_committed &&
                    a->landing_player >= 0 && a->landing_player <= 1) {
                    Player* landing = &game.players[a->landing_player];
                    if (landing->card_count < MAX_HAND)
                        landing->hand[landing->card_count++] = a->card;
                    a->landing_committed = true;
                }
                a->active = false;
                a->timing_started = false;
                if (anim_phase == 0 && i == 0) {
                    if (single_target_pair_idx == -1) {
                        game.table_attack[game.table_pair_count] = anims[0].card;
                        game.table_pair_count++;
                    } else {
                        game.table_defense[single_target_pair_idx] = anims[0].card;
                    }
                    /* Result is checked only after the bout is resolved and
                     * both players draw. Emptying a hand mid-bout is not a win. */
                }
                a->deal_card = false;
                a->start_delay_ms = 0u;
            }
        }

        if (!any_anim_running && anim_phase > 0) {
            if (anim_phase == 1) {
                int first_draw_player = 0;
                int second_draw_player = 1;
                normal_pending_ai_draw_start = begin_game_turn_resolution(
                    &game, phase_took, &dialogue,
                    &first_draw_player, &second_draw_player);
                const int deal_count = begin_normal_deal_animation(
                    &game, anims, first_draw_player, second_draw_player);
                hide_table = false;
                normal_npc_taking = false;
                selected_card_idx = 0;
                ai_delay = 0;
                if (deal_count > 0) {
                    anim_phase = 2;
                    hal_play_sound(SND_SHUFFLE);
                } else {
                    if (g_settings.auto_sort_enabled) {
                        (void)game_sort_hand(&game.players[0], game.trump_suit);
                        (void)game_sort_hand(&game.players[1], game.trump_suit);
                    }
                    finish_game_turn_after_deal(
                        &game, &dialogue, normal_pending_ai_draw_start);
                    normal_sort_until_ms = anim_now_ms + scaled_motion_ms(320u);
                    anim_phase = 3;
                }
            } else if (anim_phase == 2) {
                if (g_settings.auto_sort_enabled) {
                    (void)game_sort_hand(&game.players[0], game.trump_suit);
                    (void)game_sort_hand(&game.players[1], game.trump_suit);
                }
                finish_game_turn_after_deal(
                    &game, &dialogue, normal_pending_ai_draw_start);
                normal_sort_until_ms = anim_now_ms + scaled_motion_ms(320u);
                anim_phase = 3;
            } else if (anim_phase == 3 &&
                       (int32_t)(anim_now_ms - normal_sort_until_ms) >= 0) {
                anim_phase = 0;
                normal_sort_until_ms = 0u;
                normal_npc_taking = false;
                selected_card_idx = 0;
                ai_delay = 0;
            } else if (anim_phase == 4) {
                if (g_settings.auto_sort_enabled) {
                    (void)game_sort_hand(&game.players[0], game.trump_suit);
                    (void)game_sort_hand(&game.players[1], game.trump_suit);
                }
                dialogue_sync_hand_counts(&dialogue, &game);
                normal_sort_until_ms = anim_now_ms + scaled_motion_ms(320u);
                anim_phase = 3;
            }
        }

        /* The last flying defense card can become inactive in this same frame.
         * Refresh both derived flags after committing its table slot; otherwise
         * one stale frame can still expose [B] BEAT instead of arming the 500 ms
         * automatic resolve immediately. */
        is_animating = anim_phase > 0 || visual_anims_any(anims);
        need_defense = game.table_pair_count > 0 &&
                       game.table_defense[game.table_pair_count - 1].rank < RANK_6;

        if (!is_animating && anim_phase == 0 && game.result == RESULT_NONE)
            auto_sort_player_hand(&game, &selected_card_idx, true);

        /* Resolve a completed bout only after a visible 500 ms table hold.
         * If the human has no legal throw-in, confirmation is skipped and the
         * corresponding take/discard flight starts automatically. */
        if (!is_animating && anim_phase == 0 && game.result == RESULT_NONE) {
            if (normal_resolve_action == NORMAL_RESOLVE_NONE) {
                const NormalResolveAction automatic =
                    normal_auto_resolve_action(&game, normal_npc_taking);
                if (automatic != NORMAL_RESOLVE_NONE) {
                    normal_resolve_action = automatic;
                    normal_resolve_at_ms = now_loop_ms + NORMAL_RESOLVE_DELAY_MS;
                }
            }
            if (normal_resolve_action != NORMAL_RESOLVE_NONE &&
                (int32_t)(now_loop_ms - normal_resolve_at_ms) >= 0) {
                if (normal_resolve_action == NORMAL_RESOLVE_NPC_TAKE)
                    begin_npc_take_animation(&game, anims, &anim_phase,
                                             &phase_took, &hide_table);
                else
                    begin_discard_animation(&game, anims, &anim_phase,
                                            &phase_took, &hide_table);
                normal_resolve_action = NORMAL_RESOLVE_NONE;
                normal_resolve_at_ms = 0u;
                normal_npc_taking = false;
                selected_card_idx = 0;
                is_animating = true;
                hal_play_sound(SND_SHUFFLE);
            }
        }

        if (!is_animating && game.result == RESULT_NONE && !normal_npc_taking &&
            normal_resolve_action == NORMAL_RESOLVE_NONE) {
            if (ai_delay > 0) {
                ai_delay--;
            } else {
                if (!game.is_player_turn && !need_defense) {
                    int ai_idx = game_ai_choose_attack(&game);
                    if (ai_idx != -1) {
                        const bool ai_first_attack = game.table_pair_count == 0;
                        hal_play_sound(SND_CARD);
                        for(int k=0; k<MAX_ANIMS; k++) anims[k].active = false;
                        anims[0].active = true; anims[0].timing_started = false; anims[0].card = game.players[1].hand[ai_idx];
                        single_target_pair_idx = -1;
                        anims[0].cx = 140.0f; anims[0].cy = -20.0f;
                        anims[0].tx = (float)normal_table_x(&game, game.table_pair_count, false); anims[0].ty = (float)normal_table_y(&game, game.table_pair_count, false);
                        anims[0].destination_is_table = true;
                        anims[0].frames_left = ANIM_FRAMES;
                        anims[0].total_frames = ANIM_FRAMES;
                        anims[0].dx = (anims[0].tx - anims[0].cx) / ANIM_FRAMES; anims[0].dy = (anims[0].ty - anims[0].cy) / ANIM_FRAMES;
                        anims[0].hide_card = false;
                        
                        for (int i = ai_idx; i < game.players[1].card_count - 1; i++) game.players[1].hand[i] = game.players[1].hand[i + 1];
                        game.players[1].card_count--;
                        if (ai_first_attack)
                            dialogue_queue_event(&dialogue, game.current_opponent.face_index,
                                                 DIALOGUE_NPC_STARTS_ATTACK);
                        else
                            dialogue_note_throw(&dialogue, false);
                        ai_delay = 0;
                    } else if (game.table_pair_count > 0) {
                        normal_resolve_action = NORMAL_RESOLVE_DISCARD;
                        normal_resolve_at_ms = now_loop_ms + NORMAL_RESOLVE_DELAY_MS;
                    }
                }
                else if (game.is_player_turn && need_defense) {
                    int ai_idx = game_ai_choose_defend(&game, game.table_attack[game.table_pair_count - 1]);
                    if (ai_idx != -1) {
                        hal_play_sound(SND_CARD);
                        for(int k=0; k<MAX_ANIMS; k++) anims[k].active = false;
                        anims[0].active = true; anims[0].timing_started = false; anims[0].card = game.players[1].hand[ai_idx];
                        single_target_pair_idx = game.table_pair_count - 1;
                        anims[0].cx = 140.0f; anims[0].cy = -20.0f;
                        anims[0].tx = (float)normal_table_x(&game, game.table_pair_count - 1, true); anims[0].ty = (float)normal_table_y(&game, game.table_pair_count - 1, true);
                        anims[0].destination_is_table = true;
                        anims[0].frames_left = ANIM_FRAMES;
                        anims[0].total_frames = ANIM_FRAMES;
                        anims[0].dx = (anims[0].tx - anims[0].cx) / ANIM_FRAMES; anims[0].dy = (anims[0].ty - anims[0].cy) / ANIM_FRAMES;
                        anims[0].hide_card = false;
                        
                        for (int i = ai_idx; i < game.players[1].card_count - 1; i++) game.players[1].hand[i] = game.players[1].hand[i + 1];
                        game.players[1].card_count--;
                        ai_delay = 0;
                    } else {
                        /* Defender declared TAKE. Keep the table in place for
                         * legal human throw-ins. If none exist, the fixed hold
                         * and transfer animation are scheduled automatically. */
                        normal_npc_taking = true;
                        ai_delay = 0;
                        card_scroll_reset();
                        hal_play_sound(SND_TAKE);
                    }
                }
            }
        }

        if (!is_animating) {
            if (game.result != RESULT_NONE) {
                if (!result_processed) {
                    const bool career_was_complete = mode_session.career_complete;
                    pending_action = modes_process_result(&mode_session, &game, &tournament);
                    result_processed = true;
                    if (mode_session.mode == MODE_CAREER &&
                        !career_was_complete && mode_session.career_complete &&
                        !g_settings.cats_unlocked) {
                        g_settings.cats_unlocked = true;
                        g_settings.deck_style = deck_style_for_language(
                            DECK_STYLE_CATS_EU, g_settings.language);
                        cards_tex = card_style_tex[g_settings.deck_style]
                                  ? card_style_tex[g_settings.deck_style]
                                  : cards_uk_tex;
                        card_back = card_style_back[g_settings.deck_style]
                                  ? card_style_back[g_settings.deck_style]
                                  : card_back_classic_tex;
                        br_cards_tex = cards_tex;
                        br_card_back = card_back;
                        g_settings_dirty = true;
                        (void)settings_save();
                        cats_reward_active = true;
                        cats_reward_started_ms = now_loop_ms;
                    }
                    /* Win/unlock and loss/draw are durable before the result
                     * acknowledgement or a possible suspend/power loss. */
                    if (mode_session.mode == MODE_CAREER)
                        (void)career_progress_save(&mode_session, &game);
                }

                if (!cats_reward_active && hal_is_button_pressed(BTN_A)) {
                    bool start_next = false;
                    GameResult finished_result = game.result;

                    if (pending_action == MODE_ACTION_NEXT_MATCH ||
                        pending_action == MODE_ACTION_REPLAY_MATCH) {
                        normal_layout_capacity_cache = 0;
                        start_next = modes_prepare_match(&mode_session, &game, &tournament);
                        if (start_next) hal_music_start_gameplay_new_match();
                    } else if (pending_action == MODE_ACTION_SHOW_BRACKET) {
                        in_tournament_bracket = true;
                    } else if (pending_action == MODE_ACTION_SHOW_CAREER_MAP) {
                        in_career_map = true;
                        career_map_init(&career_map_ui, &mode_session,
                                        hal_get_ticks_ms());
                        (void)career_progress_save(&mode_session, &game);
                    } else if (pending_action == MODE_ACTION_TOURNAMENT_CHAMPION) {
                        in_tournament_champion = true;
                        champion_frame = 0;
                        confetti_running = false;
                    } else {
                        /* Career complete/game over, tournament loss, and Random Battle end here. */
                        if (mode_session.mode == MODE_CAREER)
                            (void)career_progress_save(&mode_session, &game);
                        cards_tex = card_style_tex[g_settings.deck_style]
                                  ? card_style_tex[g_settings.deck_style]
                                  : cards_uk_tex;
                        card_back = card_style_back[g_settings.deck_style]
                                  ? card_style_back[g_settings.deck_style]
                                  : card_back_classic_tex;
                        br_cards_tex = cards_tex;
                        br_card_back = card_back;
                        in_menu = true;
                        in_difficulty_select = false;
                        in_mode_select = false;
                        in_avatar_select = false;
                        in_tournament_bracket = false;
                        in_tournament_champion = false;
                        reset_frontend_run(&game, &mode_session, &tournament, anims,
                                           &selected_difficulty, &selected_mode, &selected_face,
                                           &pending_action, &result_processed, &last_result_sfx,
                                           &confetti_running, &selected_card_idx, &ai_delay,
                                           &anim_phase, &hide_table, &single_target_pair_idx,
                                           &pause_selected);
                        table_dust_init(table_dust);
                    }

                    if (start_next) {
                        /* One gameplay track per match. Force the next numbered
                         * track for consecutive Career matches as well. */
                        hal_music_stop();
                        dialogue_reset(&dialogue);
                        dialogue_sync_hand_counts(&dialogue, &game);
                        card_scroll_reset();
                        hand_visual_reset();
                        transition_last_screen = -1; /* fade the fresh table even though screen kind is still GAME */
                        hal_randomize_table();
                        hal_play_sound(finished_result == RESULT_WIN ? SND_NEXT_OPPONENT : SND_SHUFFLE);
                        hal_queue_face_voice(game.current_opponent.face_index);
                    }

                    if (start_next || in_career_map || in_tournament_bracket ||
                        in_tournament_champion || in_menu) {
                        pending_action = MODE_ACTION_NONE;
                        result_processed = false;
                        last_result_sfx = RESULT_NONE;
                        selected_card_idx = 0;
                        ai_delay = 0;
                        anim_phase = 0;
                        hide_table = false;
                        memset(anims, 0, sizeof(anims));
                        if (start_next)
                            anim_phase = begin_initial_normal_deal_animation(&game, anims) > 0 ? 4 : 0;
                    }
                }
            }
            else {
                if (p_count > 0 && normal_resolve_action == NORMAL_RESOLVE_NONE) {
                    card_scroll_update(&selected_card_idx, p_count);
                    
                    if (hal_is_button_pressed(BTN_A)) {
                        Card selected_card = game.players[0].hand[selected_card_idx];
                        if (game.is_player_turn && (!need_defense || normal_npc_taking)) {
                            if (game_can_attack_with(&game, selected_card)) {
                                const bool player_is_throwing = game.table_pair_count > 0;
                                hal_play_sound(SND_CARD); 
                                for(int k=0; k<MAX_ANIMS; k++) anims[k].active = false;
                                anims[0].active = true; anims[0].timing_started = false; anims[0].card = selected_card;
                                single_target_pair_idx = -1;
                                anims[0].cx = (float)player_hand_card_x(p_count, selected_card_idx,
                                                                         selected_card_idx, true);
                                anims[0].cy = 145.0f;
                                anims[0].tx = (float)normal_table_x(&game, game.table_pair_count, false); anims[0].ty = (float)normal_table_y(&game, game.table_pair_count, false);
                                anims[0].destination_is_table = true;
                                anims[0].frames_left = ANIM_FRAMES;
                                anims[0].total_frames = ANIM_FRAMES;
                                anims[0].dx = (anims[0].tx - anims[0].cx) / ANIM_FRAMES; anims[0].dy = (anims[0].ty - anims[0].cy) / ANIM_FRAMES;
                                anims[0].hide_card = false;

                                Player* p = &game.players[0];
                                for (int i = selected_card_idx; i < p->card_count - 1; i++) p->hand[i] = p->hand[i + 1];
                                p->card_count--;
                                if (player_is_throwing) dialogue_note_throw(&dialogue, true);
                                card_scroll_reset();
                                if (selected_card_idx >= p->card_count && p->card_count > 0) selected_card_idx = p->card_count - 1;
                            } else hal_play_sound(SND_ERROR);
                        } else if (!game.is_player_turn && need_defense) {
                            if (game_can_defend_with(&game, game.table_attack[game.table_pair_count - 1], selected_card)) {
                                hal_play_sound(SND_CARD); 
                                for(int k=0; k<MAX_ANIMS; k++) anims[k].active = false;
                                anims[0].active = true; anims[0].timing_started = false; anims[0].card = selected_card;
                                single_target_pair_idx = game.table_pair_count - 1;
                                anims[0].cx = (float)player_hand_card_x(p_count, selected_card_idx,
                                                                         selected_card_idx, true);
                                anims[0].cy = 145.0f;
                                anims[0].tx = (float)normal_table_x(&game, game.table_pair_count - 1, true); anims[0].ty = (float)normal_table_y(&game, game.table_pair_count - 1, true);
                                anims[0].destination_is_table = true;
                                anims[0].frames_left = ANIM_FRAMES;
                                anims[0].total_frames = ANIM_FRAMES;
                                anims[0].dx = (anims[0].tx - anims[0].cx) / ANIM_FRAMES; anims[0].dy = (anims[0].ty - anims[0].cy) / ANIM_FRAMES;
                                anims[0].hide_card = false;

                                Player* p = &game.players[0];
                                for (int i = selected_card_idx; i < p->card_count - 1; i++) p->hand[i] = p->hand[i + 1];
                                p->card_count--;
                                card_scroll_reset();
                                if (selected_card_idx >= p->card_count && p->card_count > 0) selected_card_idx = p->card_count - 1;
                            } else hal_play_sound(SND_ERROR);
                        } else hal_play_sound(SND_ERROR);
                    }
                }
                /* Game & Watch gameplay controls:
                 * A      = play selected card / defend
                 * B tap  = beat-off after every table card is defended
                 * B hold = take cards after 280 ms while defending
                 * SELECT = pause menu (CONTINUE / GO BACK TO MENU).
                 */
                if (normal_npc_taking &&
                    normal_resolve_action == NORMAL_RESOLVE_NONE &&
                    hal_is_button_pressed(BTN_B)) {
                    normal_resolve_action = NORMAL_RESOLVE_NPC_TAKE;
                    normal_resolve_at_ms = now_loop_ms + NORMAL_RESOLVE_DELAY_MS;
                }

                const bool normal_take_available =
                    normal_resolve_action == NORMAL_RESOLVE_NONE &&
                    game.table_pair_count > 0 && !game.is_player_turn && need_defense;
                const bool normal_take_now = action_take_hold_update(
                    &b_action_hold, normal_take_available, now_loop_ms);
                const bool normal_beat_now = normal_resolve_action == NORMAL_RESOLVE_NONE &&
                                             !normal_take_available &&
                                             game.table_pair_count > 0 &&
                                             game.is_player_turn && !need_defense &&
                                             hal_is_button_pressed(BTN_B);
                if (normal_beat_now) {
                    if (game.table_pair_count > 0 && game.is_player_turn && !need_defense &&
                        normal_resolve_action == NORMAL_RESOLVE_NONE) {
                        normal_resolve_action = NORMAL_RESOLVE_DISCARD;
                        normal_resolve_at_ms = now_loop_ms + NORMAL_RESOLVE_DELAY_MS;
                    } else {
                        hal_play_sound(SND_ERROR);
                    }
                }

                if (normal_take_now) {
                    if (game.table_pair_count > 0 && !game.is_player_turn && need_defense) {
                        anim_phase = 1; phase_took = true; hide_table = true;

                        // Гравець бере карти: анімуємо їх прямо в нижню "руку",
                        // а не за межі екрана. Рахуємо майбутню кількість карт,
                        // щоб цільові X збігалися з тим самим fan-layout, який
                        // використовується при звичайному малюванні руки.
                        int taken_count = 0;
                        for (int k = 0; k < game.table_pair_count; k++) {
                            if (game.table_attack[k].rank >= 6) taken_count++;
                            if (game.table_defense[k].rank >= 6) taken_count++;
                        }

                        int future_hand_count = game.players[0].card_count + taken_count;
                        int take_offset_x = (future_hand_count > 1) ? (195 / (future_hand_count - 1)) : CARD_WIDTH;
                        if (take_offset_x > CARD_WIDTH) take_offset_x = CARD_WIDTH;

                        int anim_idx = 0;
                        int taken_slot = game.players[0].card_count;
                        for (int k = 0; k < game.table_pair_count; k++) {
                            anims[anim_idx].active = true; anims[anim_idx].timing_started = false; anims[anim_idx].card = game.table_attack[k];
                            anims[anim_idx].cx = (float)normal_table_x(&game, k, false); anims[anim_idx].cy = (float)normal_table_y(&game, k, false);
                            anims[anim_idx].tx = (float)(10 + (taken_slot * take_offset_x));
                            anims[anim_idx].ty = 160.0f;
                            anims[anim_idx].frames_left = ANIM_FRAMES;
                            anims[anim_idx].total_frames = ANIM_FRAMES;
                            anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES;
                            anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                            anims[anim_idx].hide_card = false;
                            anim_idx++; taken_slot++;

                            if (game.table_defense[k].rank >= 6) {
                                anims[anim_idx].active = true; anims[anim_idx].timing_started = false; anims[anim_idx].card = game.table_defense[k];
                                anims[anim_idx].cx = (float)normal_table_x(&game, k, true); anims[anim_idx].cy = (float)normal_table_y(&game, k, true);
                                anims[anim_idx].tx = (float)(10 + (taken_slot * take_offset_x));
                                anims[anim_idx].ty = 160.0f;
                                anims[anim_idx].frames_left = ANIM_FRAMES;
                                anims[anim_idx].total_frames = ANIM_FRAMES;
                                anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES;
                                anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                                anims[anim_idx].hide_card = false;
                                anim_idx++; taken_slot++;
                            }
                        }
                        hal_play_sound(SND_TAKE);
                        selected_card_idx = 0;
                    } else {
                        hal_play_sound(SND_ERROR);
                    }
                }
            }
        }

        // Одноразові звуки результату раунду.
        if (game.result != RESULT_NONE && game.result != last_result_sfx) {
            if (game.result == RESULT_WIN) hal_play_sound(SND_WIN);
            else if (game.result == RESULT_LOSE) hal_play_sound(SND_LOSE);
            else if (game.result == RESULT_DRAW) hal_play_sound(SND_DRAW);
            last_result_sfx = game.result;
        }

        if (game.result == RESULT_WIN) {
            if (!confetti_running) {
                confetti_init(confetti);
                confetti_running = true;
            }
            confetti_update(confetti);
        } else {
            confetti_running = false;
        }

        if (game.result == RESULT_NONE) table_dust_update(table_dust, frame_dt_ms);

        bool career_broke = (mode_session.mode == MODE_CAREER &&
                              pending_action == MODE_ACTION_CAREER_GAME_OVER);
        unsigned int current_bg = bg_color;
        if (game.result != RESULT_NONE) {
            if (career_broke) current_bg = 0x550000;
            else if (game.result == RESULT_WIN) current_bg = 0x1D551D;
            else if (game.result == RESULT_LOSE) current_bg = TABLE_BG_A;
            else current_bg = 0x224455;
        }
        /* Lighting is now applied while the table scene is drawn. The exact
         * dithered gain field is cached once per lighting preset, and DMA2D
         * renders the lit felt from that L8 map. Cards, shadows and dust use
         * the same cached gain values, eliminating the old fullscreen CPU pass. */
        hal_set_table_sprite_lighting(true);
        if (game.result == RESULT_NONE || game.result == RESULT_LOSE) {
            hal_clear_checkerboard(TABLE_BG_A, TABLE_BG_B);
        } else {
            hal_clear_screen(current_bg);
        }
        if (game.result == RESULT_NONE) table_dust_draw(table_dust);

        int ai_count = game.players[1].card_count;
        ai_hand_visual_update(&game.players[1], frame_dt_ms);
        /* Full shadow pass for every card. Existing cards retain their physical
         * position while the hand closes, expands or reorders. */
        const bool normal_deal_active = anim_phase == 2 || anim_phase == 4;
        if (hal_shadows_enabled() && !normal_deal_active &&
            !ai_hand_visual_sorting) {
            for (int i = 0; i < ai_count; i++) {
                const int x = ai_hand_visual_x(i);
                const int y = ai_hand_visual_y(i);
                hal_draw_sprite_shadow(card_back, 0, 0, CARD_WIDTH, CARD_HEIGHT,
                                       x + 1, y + 2, SHADOW_FAR_COLOR);
            }
        }
        for (int i = 0; i < ai_count; i++) {
            draw_back_sprite_full(card_back, ai_hand_visual_x(i), ai_hand_visual_y(i));
        }

        const bool normal_throw_hint = !is_animating &&
            normal_resolve_action == NORMAL_RESOLVE_NONE &&
            (normal_npc_taking ||
             (game.is_player_turn && game.table_pair_count > 0 && !need_defense));
        if (normal_throw_hint && !normal_throw_hint_was_active)
            normal_throw_hint_started_ms = now_loop_ms;
        if (!normal_throw_hint)
            normal_throw_hint_started_ms = 0u;
        normal_throw_hint_was_active = normal_throw_hint;
        /* Start each explicit throw-in window at the bright half of the pulse
         * instead of inheriting an arbitrary global clock phase. */
        const uint32_t normal_throw_hint_pulse_ms = normal_throw_hint
            ? (now_loop_ms - normal_throw_hint_started_ms + 450u) : 0u;

        int cards_left = DECK_SIZE - game.deck.top_index;
        if (cards_left > 0) {
            /* Android layout: keep the trump at full 40x60 size and vertical,
             * then cover it with the deck.  The 11-pixel exposed rank/suit
             * strip is 27.5% of the card width (Android exposes 34/120). */
            draw_normal_trump_android_style(cards_tex, game.trump_card,
                                            NORMAL_TRUMP_X, NORMAL_TRUMP_Y);
            if (cards_left > 1)
                draw_draw_pile_without_shadow(card_back, NORMAL_DECK_X, 92);
        }
        draw_empty_stock_trump_badge(cards_tex, game.trump_card,
                                     cards_left, now_loop_ms);

        if (!hide_table) {
            /* Table pairs are rendered in two passes so the center pile follows
             * real-world shadowing: all cards keep a downward ground shadow,
             * while only the outer exposed card receives a lateral cast shadow
             * in the left/right light presets. The centered preset keeps only
             * straight-down cast shadows. */
            for (int draw_i = 0; draw_i < game.table_pair_count; ++draw_i) {
                const int i = normal_table_draw_pair_index(game.table_pair_count, draw_i);
                int table_x = normal_table_x(&game, i, false);
                bool has_defense = game.table_defense[i].rank >= 6;
                int sdx_probe, sdy_probe;
                card_shadow_offset(table_x, 0, &sdx_probe, &sdy_probe);
                bool attack_side = (!has_defense) || (sdx_probe < 0);
                bool defense_side = has_defense && (sdx_probe > 0);

                draw_table_card_shadow(cards_tex, game.table_attack[i], table_x, normal_table_y(&game, i, false), attack_side);
                if (has_defense) {
                    draw_table_card_shadow(cards_tex, game.table_defense[i], normal_table_x(&game, i, true), normal_table_y(&game, i, true), defense_side);
                }
            }
            for (int draw_i = 0; draw_i < game.table_pair_count; ++draw_i) {
                const int i = normal_table_draw_pair_index(game.table_pair_count, draw_i);
                int table_x = normal_table_x(&game, i, false);
                const int attack_y = normal_table_y(&game, i, false);
                bool has_defense = game.table_defense[i].rank >= 6;
                draw_table_card_sprite(cards_tex, game.table_attack[i], table_x, attack_y);
                if (normal_throw_hint &&
                    player_has_legal_throw_rank(&game, game.table_attack[i].rank))
                    draw_card_action_mask(cards_tex, game.table_attack[i],
                                          table_x, attack_y, normal_throw_hint_pulse_ms);
                if (has_defense) {
                    const int defense_x = normal_table_x(&game, i, true);
                    const int defense_y = normal_table_y(&game, i, true);
                    draw_table_card_sprite(cards_tex, game.table_defense[i], defense_x, defense_y);
                    if (normal_throw_hint &&
                        player_has_legal_throw_rank(&game, game.table_defense[i].rank))
                        draw_card_action_mask(cards_tex, game.table_defense[i],
                                              defense_x, defense_y, normal_throw_hint_pulse_ms);
                }
            }
        }

        int active_anim_count = 0;
        for (int i = 0; i < MAX_ANIMS; i++) {
            if (anims[i].active) active_anim_count++;
        }
        bool fast_anim_render = active_anim_count > 4;

        for (int i = 0; i < MAX_ANIMS; i++) {
            if (!anims[i].active) continue;
            int ax = (int)anims[i].cx;
            int ay = (int)anims[i].cy;
            int anim_w = CARD_WIDTH, anim_h = CARD_HEIGHT;
            visual_anim_draw_size(&anims[i], &anim_w, &anim_h);
            if (fast_anim_render) {
                if (anims[i].hide_card)
                    draw_back_in_flight_fast(card_back, ax, ay, anim_w, anim_h,
                                             &anims[i]);
                else
                    draw_card_in_flight_fast(cards_tex, anims[i].card,
                                             ax, ay, anim_w, anim_h, &anims[i]);
            } else {
                if (anims[i].hide_card)
                    draw_back_in_flight(card_back, ax, ay,
                                        anim_w, anim_h, &anims[i]);
                else
                    draw_card_in_flight(cards_tex, anims[i].card,
                                        ax, ay, anim_w, anim_h, &anims[i]);
            }
        }

        p_count = game.players[0].card_count;
        bool raise_selected = (p_count > 0 && game.result == RESULT_NONE && !is_animating);

        /* Selection changes Y only.  The complete hand is rebuilt every frame,
         * so moving the cursor cannot leave a rectangular hole in a neighbour. */
        hand_visual_update(&game.players[0], selected_card_idx, raise_selected, frame_dt_ms);

        /* Draw every card shadow first, then every card. This preserves the
         * complete lower silhouette even when more than five cards overlap. */
        if (hal_shadows_enabled() && !normal_deal_active &&
            !hand_visual_sorting) {
            for (int i = 0; i < p_count; ++i) {
                const int x = hand_visual_x(i);
                const int y = hand_visual_y(i);
                const Card c = game.players[0].hand[i];
                const int sx = card_atlas_index(c) * CARD_WIDTH;
                hal_draw_sprite_shadow(cards_tex, sx, 0, CARD_WIDTH, CARD_HEIGHT,
                                       x + 1, y + 2, SHADOW_FAR_COLOR);
            }
        }

        /* Rebuild the fan in physical left-to-right z-order with full sprites.
         * A transparent rounded corner naturally shows the card below it, or
         * the felt when no lower card occupies that pixel. */
        for (int i = 0; i < p_count; i++) {
            const int card_x = hand_visual_x(i);
            const int card_y = hand_visual_y(i);
            const Card card = game.players[0].hand[i];
            draw_card_sprite_full(cards_tex, card, card_x, card_y);
            if (normal_throw_hint && game_can_attack_with(&game, card))
                draw_card_action_mask(cards_tex, card, card_x, card_y,
                                      normal_throw_hint_pulse_ms);
        }

        if (game.result != RESULT_NONE) {
            if (career_broke) {
                draw_back_with_shadow(card_back, 110, 90); draw_back_with_shadow(card_back, 140, 90);
            } else if (game.result == RESULT_WIN) {
                Card vic_card = {SUIT_HEARTS, RANK_A}; draw_card_with_shadow(cards_tex, vic_card, 140, 90);
            } else if (game.result == RESULT_LOSE) {
                Card lose_card = {SUIT_SPADES, RANK_6}; draw_card_with_shadow(cards_tex, lose_card, 140, 90);
            }
        }

        /* Table scene is already lit pixel-for-pixel through the cached gain
         * field. Disable table lighting before grayscale/results/UI overlays,
         * preserving the old render ordering for portraits and HUD. */
        hal_set_table_sprite_lighting(false);

        // Поразка: вся ігрова сцена стає чорно-білою й трохи темнішою.
        // UI малюється після цього проходу, тому ім'я, портрет і CASH лишаються кольоровими.
        if (game.result == RESULT_LOSE) {
            hal_apply_grayscale_darkened();
        }

        // Перемога: процедурні піксельні конфеті без PNG і без додаткового framebuffer.
        if (game.result == RESULT_WIN && confetti_running) {
            confetti_draw(confetti);
        }

        const bool portrait_turn_visible = game.result == RESULT_NONE && !is_animating;
        const bool player_portrait_active = portrait_turn_visible && human_actionable;
        const bool npc_portrait_active = portrait_turn_visible && !human_actionable;

        if (font_tex) {
            if (game.current_opponent.name[0] != '\0') {
                char opp_str[32];
                if (mode_session.mode == MODE_RANDOM_BATTLE)
                    snprintf(opp_str, sizeof(opp_str), "%s", game.current_opponent.name);
                else
                    snprintf(opp_str, sizeof(opp_str), "%s %s%d",
                             ui_ai_name(game.current_opponent.face_index),
                             ui_tr(UI_STR_LEVEL_SHORT), game.current_opponent.level);
                int text_width = (int)strlen(opp_str) * 8;
                int opp_x = SCREEN_WIDTH - 3 - text_width;
                if (opp_x < 3) opp_x = 3;
                draw_text(font_tex, opp_str, opp_x, 6);

                HalTexture* opponent_faces_tex = mode_session.mode == MODE_RANDOM_BATTLE
                                               ? player_faces_tex : faces_tex;
                if (opponent_faces_tex) {
                    int face_idx = game.current_opponent.face_index;
                    draw_face_with_shadow(opponent_faces_tex, face_idx, HUD_FACE_X, OPP_FACE_Y,
                                          player_portrait_active);
                    if (npc_portrait_active)
                        draw_face_turn_frame(HUD_FACE_X, OPP_FACE_Y, now_loop_ms);
                }
                dialogue_draw(&dialogue, dialog_tex, dialogue_font_tex);
            }

            /* Compact status lives in the dedicated 20-pixel gap between the
             * raised deck and the lowered player portrait. It never enters the
             * hand fan or battlefield. */
            if (dialogue_font_tex) {
                const int hud_cx = HUD_FACE_X + FACE_WIDTH / 2;
                if (mode_session.mode == MODE_CAREER) {
                    char money_str[16], level_str[16];
                    snprintf(money_str, sizeof(money_str), "$%d", game.player_money);
                    snprintf(level_str, sizeof(level_str), "%s%d/20",
                             ui_tr(UI_STR_LEVEL_SHORT), game.current_level);
                    draw_text_6x10_centered(dialogue_font_tex, money_str, hud_cx, 153, 9);
                    draw_text_6x10_centered(dialogue_font_tex, level_str, hud_cx, 163, 9);
                } else if (mode_session.mode == MODE_TOURNAMENT) {
                    char round_str[20];
                    int round_no = tournament.round + 1;
                    if (round_no > 3) round_no = 3;
                    snprintf(round_str, sizeof(round_str), "%s%d/3", ui_tr(UI_STR_ROUND), round_no);
                    draw_text_6x10_centered(dialogue_font_tex, round_str, hud_cx, 158, 9);
                } else {
                    draw_text_6x10_centered(dialogue_font_tex,
                                            ui_mode_name_localized(MODE_RANDOM_BATTLE),
                                            hud_cx, 158, 9);
                }
            }

            if (player_faces_tex) {
                draw_face_with_shadow(player_faces_tex, mode_session.player_face & 1, HUD_FACE_X,
                                      PLAYER_FACE_Y, npc_portrait_active);
                if (player_portrait_active)
                    draw_face_turn_frame(HUD_FACE_X, PLAYER_FACE_Y, now_loop_ms);
            }

            char deck_str[20];
            snprintf(deck_str, sizeof(deck_str), "%s:%d", ui_tr(UI_STR_DECK), cards_left);
            if (dialogue_font_tex) {
                int deck_x = SCREEN_WIDTH - 3 - (int)strlen(deck_str) * DIALOGUE_FONT_WIDTH;
                if (deck_x < 225) deck_x = 225;
                draw_text_6x10_white(dialogue_font_tex, deck_str, deck_x, 82);
            }

            // Центральне повідомлення результату поверх освітлення.
            // Тінь на 1 px тримає текст читабельним і на світлій карті, і в тіні.
            if (game.result == RESULT_WIN) {
                const char* result_msg = ui_tr(UI_STR_YOU_WIN);
                int result_x = (SCREEN_WIDTH - ((int)strlen(result_msg) * 8)) / 2;
                draw_text_shadowed(font_tex, result_msg, result_x, 116);
            } else if (game.result == RESULT_LOSE) {
                const char* result_msg = ui_tr(UI_STR_YOU_LOST);
                int result_x = (SCREEN_WIDTH - ((int)strlen(result_msg) * 8)) / 2;
                draw_text_shadowed(font_tex, result_msg, result_x, 116);
            }

            if (pending_action == MODE_ACTION_CAREER_GAME_OVER)
                draw_text_shadowed(font_tex, ui_tr(UI_STR_GAME_OVER), (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_GAME_OVER)) * 8) / 2, 132);
            else if (pending_action == MODE_ACTION_TOURNAMENT_OVER)
                draw_text_shadowed(font_tex, ui_tr(UI_STR_TOURNAMENT_OVER), (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_TOURNAMENT_OVER)) * 8) / 2, 132);
            else if (pending_action == MODE_ACTION_CAREER_COMPLETE)
                draw_text_shadowed(font_tex, ui_tr(UI_STR_CAREER_COMPLETE), (SCREEN_WIDTH - (int)strlen(ui_tr(UI_STR_CAREER_COMPLETE)) * 8) / 2, 132);
        }

        snow_draw(snow);

        if (cats_reward_active) {
            const uint32_t reward_elapsed = now_loop_ms - cats_reward_started_ms;
            HalTexture* cats_reward_tex = card_style_tex[
                deck_style_for_language(DECK_STYLE_CATS_EU, g_settings.language)];
            draw_cats_reward_flight(cats_reward_tex, reward_elapsed);
            hal_fill_rect(54, 92, 212, 48, 0x15110Fu);
            hal_fill_rect(56, 94, 208, 44, 0x3A2418u);
            const char* reward_line_1 = ui_cats_reward_line(0);
            const char* reward_line_2 = ui_cats_reward_line(1);
            draw_text_shadowed(font_tex, reward_line_1,
                               (SCREEN_WIDTH - (int)strlen(reward_line_1) * 8) / 2,
                               101);
            draw_text_shadowed(font_tex, reward_line_2,
                               (SCREEN_WIDTH - (int)strlen(reward_line_2) * 8) / 2,
                               121);
        }

        if (game.result == RESULT_NONE && !in_pause && !is_animating && anim_phase == 0 &&
            normal_resolve_action == NORMAL_RESOLVE_NONE &&
            !visual_anims_any(anims)) {
            const bool take_hint = game.table_pair_count > 0 &&
                                   !game.is_player_turn && need_defense;
            const bool beat_hint = game.table_pair_count > 0 &&
                                   game.is_player_turn && !need_defense;
            if (normal_npc_taking) {
                if (((now_loop_ms / 450u) & 1u) == 0u)
                    draw_action_hint(dialogue_font_tex, ui_tr(UI_STR_THROW_IN_DONE), 150);
            } else if ((take_hint || beat_hint) &&
                       action_hint_visible(now_loop_ms, take_hint, &b_action_hold)) {
                draw_action_hint(dialogue_font_tex,
                                 ui_tr(take_hint ? UI_STR_ACTION_TAKE
                                                : UI_STR_ACTION_BEAT),
                                 150);
            }
        }
        present_with_transition(UI_SCREEN_GAME, &transition_last_screen, &transition_fade_level);
        hal_delay(16);
    }

    if (g_settings_dirty) settings_save();
    if (mode_session.mode == MODE_CAREER)
        (void)career_progress_save(&mode_session, &game);
    g_live_mode_session = NULL;
    g_live_game = NULL;
    hal_music_stop();
    if (menu_bg) hal_destroy_texture(menu_bg);
    if (card_back_classic_tex) hal_destroy_texture(card_back_classic_tex);
    if (card_back_poker_tex) hal_destroy_texture(card_back_poker_tex);
    if (card_back_cats_tex) hal_destroy_texture(card_back_cats_tex);
    if (cards_uk_tex) hal_destroy_texture(cards_uk_tex);
    if (cards_lat_tex) hal_destroy_texture(cards_lat_tex);
    if (cards_atlas_uk_tex) hal_destroy_texture(cards_atlas_uk_tex);
    if (cards_atlas_lat_tex) hal_destroy_texture(cards_atlas_lat_tex);
    if (cards_cats_uk_tex) hal_destroy_texture(cards_cats_uk_tex);
    if (cards_cats_lat_tex) hal_destroy_texture(cards_cats_lat_tex);
    /* BR pointers alias the canonical textures above. Never destroy aliases
     * a second time. */
    br_cards_tex = NULL;
    br_card_back = NULL;
    card_back = NULL;
    if (splash_tex) hal_destroy_texture(splash_tex);
    if (font_ascii_tex) hal_destroy_texture(font_ascii_tex);
    if (font_uk_tex) hal_destroy_texture(font_uk_tex);
    if (faces_tex) hal_destroy_texture(faces_tex);
    if (br_faces_tex) hal_destroy_texture(br_faces_tex);
    if (player_faces_tex) hal_destroy_texture(player_faces_tex);
    if (br_player_faces_tex) hal_destroy_texture(br_player_faces_tex);
    if (career_map_tex) hal_destroy_texture(career_map_tex);
    if (dialogue_font_ascii_tex) hal_destroy_texture(dialogue_font_ascii_tex);
    if (dialogue_font_uk_tex) hal_destroy_texture(dialogue_font_uk_tex);


    hal_shutdown();
    return 0;
#endif
}
#endif /* DUREN_EXCLUDE_APPLICATION_MAIN */
