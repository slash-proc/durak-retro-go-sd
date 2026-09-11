#include "battle_royal_ui.h"
#include "dialogue_data.h"
#include "dialogue_font_metrics.h"
#include "ui_modes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rng.h"

#define CARD_W 40
#define CARD_H 60
#define FACE_W 45
#define FACE_H 45

/* Battle Royal uses the same native 40x60 source art as every other mode.
 * Hand/table cards stay native; only the exposed deck trump is intentionally
 * shown at 50% so it reads as a compact suit indicator. */
#define BR_CARD_W 40
#define BR_CARD_H 60
#define BR_FACE_W 45
#define BR_FACE_H 45
#define FONT_W 6
#define FONT_H 10
#define FRAME_COLOR 0xFFD84A
#define HUD_TEXT_COLOR 0xFFFFFF
#define SHADOW_COLOR 0x101218
#define DIM_PERCENT 40u
#define BUBBLE_MIN_W 104
#define BUBBLE_MAX_W 160
#define BUBBLE_MAX_H 64
#define BUBBLE_TEXT_MAX_W 144
#define COMPACT_FONT_W 6
#define COMPACT_FONT_H 10
#define COMPACT_LINE_H 12
#define BUBBLE_PAD_X 7
#define BUBBLE_PAD_Y 7
#define MAX_LINE_CHARS 48
#define MAX_LINES 4
#define BR_DIALOGUE_COOLDOWN_MS 7000u
#define BR_DIALOGUE_OWNER_COOLDOWN_MS 15000u
#define BR_DIALOGUE_STALE_MS 4000u
#define BR_DIALOGUE_MAX_ORDINARY_MATCH 12u
#define BR_DIALOGUE_PUFF_MS 170u
#define BR_SELECTED_LIFT_Y 7
#define BR_BOTTOM_HAND_LEFT_X 54
#define BR_BOTTOM_HAND_RIGHT_X 266
#define BR_BOTTOM_HAND_FENCE_GAP 7
#define BR_PLAYER_HAND_W 150
#define BR_NPC3_HAND_W 55
#define BR_BOTTOM_HAND_DEFAULT_W 96

/* Native 40x60 battlefield cards. Seven pairs share the defender-side
 * row with hand-like dynamic overlap; the eighth pair starts a second row. */
#define BR_TABLE_CARD_W BR_CARD_W
#define BR_TABLE_CARD_H BR_CARD_H
#define BR_TABLE_PAIR_W (BR_CARD_W + 12)
#define BR_TABLE_ROW_CAPACITY 7
#define BR_TABLE_FIELD_LEFT 3
#define BR_DECK_X 280
#define BR_TRUMP_X (BR_DECK_X - 11)
#define BR_TRUMP_Y 89
#define BR_TABLE_FIELD_RIGHT (BR_TRUMP_X - 6)
#define BR_TABLE_TOP_Y 68
#define BR_TABLE_BOTTOM_Y 98
#define BR_TABLE_ROW_OFFSET 30
#define BR_TABLE_DEFENSE_SHIFT_X 12
#define BR_TABLE_DEFENSE_SHIFT_Y 8

/* Hand and battlefield shadows are contact shadows.  The global 6 x 8 cast
 * shadow is intentionally not used here because it looked like a detached
 * black shelf below every fan. */
#define BR_CONTACT_SHADOW_X 1
#define BR_CONTACT_SHADOW_Y 2

static void draw_text6_color(HalTexture* font, const char* text, int x, int y,
                             unsigned int color, bool solid_tint) {
    if (!font || !text) return;
    for (int i = 0; text[i]; ++i) {
        unsigned int ch = (unsigned char)text[i];
        int sx = (int)(ch % 16u) * FONT_W;
        int sy = (int)(ch / 16u) * FONT_H;
        if (solid_tint)
            hal_draw_sprite_shadow(font, sx, sy, FONT_W, FONT_H,
                                   x + i * FONT_W, y, color);
        else
            hal_draw_sprite(font, sx, sy, FONT_W, FONT_H, x + i * FONT_W, y);
    }
}

static void draw_text6_white(HalTexture* font, const char* text, int x, int y) {
    draw_text6_color(font, text, x, y, HUD_TEXT_COLOR, false);
}

static void draw_text6_center_white(HalTexture* font, const char* text, int cx, int y) {
    int w = text ? (int)strlen(text) * FONT_W : 0;
    draw_text6_white(font, text, cx - w / 2, y);
}

static bool g_br_dialogues_enabled = true;
static int g_br_game_speed_step = 0;
static const uint8_t g_br_game_speed_percent[11] = {
    40u, 46u, 52u, 58u, 64u, 70u, 78u, 86u, 95u, 105u, 115u
};

#define BR_HAND_LAYOUT_TAU_MS 34u
static Card g_br_hand_visual_cards[BR_MAX_PLAYERS][MAX_HAND];
static int32_t g_br_hand_visual_x_q16[BR_MAX_PLAYERS][MAX_HAND];
static int32_t g_br_hand_visual_y_q16[BR_MAX_PLAYERS][MAX_HAND];
static int g_br_hand_visual_count[BR_MAX_PLAYERS] = {-1, -1, -1, -1};
static bool g_br_hand_visual_sorting[BR_MAX_PLAYERS] = {false, false, false, false};
static uint32_t g_br_hand_visual_last_ms = 0u;

static bool br_visual_card_same(Card a, Card b) {
    return a.rank == b.rank && a.suit == b.suit;
}

static uint32_t br_scaled_motion_ms(uint32_t neutral_ms) {
    int step = g_br_game_speed_step;
    if (step < -5) step = -5;
    if (step > 5) step = 5;
    const uint32_t percent = g_br_game_speed_percent[step + 5];
    uint32_t result = (neutral_ms * 100u + percent - 1u) / percent;
    return result > 0u ? result : 1u;
}

static int32_t br_hand_visual_approach(int32_t current, int32_t target,
                                       uint32_t dt_ms) {
    if (dt_ms < 1u) dt_ms = 1u;
    if (dt_ms > 50u) dt_ms = 50u;
    const uint32_t tau_ms = br_scaled_motion_ms(BR_HAND_LAYOUT_TAU_MS);
    const uint32_t alpha_q16 =
        (uint32_t)(((uint64_t)dt_ms << 16) / (tau_ms + dt_ms));
    const int64_t delta = (int64_t)target - current;
    int32_t next = current + (int32_t)((delta * alpha_q16) >> 16);
    if (next == current && current != target) next += target > current ? 1 : -1;
    return next;
}

static void br_hand_visual_reset_all(uint32_t now_ms) {
    for (int p = 0; p < BR_MAX_PLAYERS; ++p) {
        g_br_hand_visual_count[p] = -1;
        g_br_hand_visual_sorting[p] = false;
    }
    g_br_hand_visual_last_ms = now_ms;
}

void br_ui_set_game_speed_step(int speed_step) {
    if (speed_step < -5) speed_step = -5;
    if (speed_step > 5) speed_step = 5;
    g_br_game_speed_step = speed_step;
}

static uint32_t dialogue_duration(const char* text) {
    int chars = text ? (int)strlen(text) : 0;
    if (chars <= 10) return 3000u;
    if (chars <= 20) return 4000u;
    if (chars <= 32) return 5000u;
    if (chars <= 46) return 6000u;
    return 7000u;
}

static const DialogueGlyphMetric* br_active_dialogue_metrics(void) {
    return dialogue_glyph_metrics(ui_language_uses_cp1251(ui_get_language()));
}

static int br_dialogue_char_advance(unsigned char ch) {
    return br_active_dialogue_metrics()[ch].advance;
}

static int br_dialogue_text_width(const char* text) {
    int width = 0;
    if (!text) return 0;
    for (int i = 0; text[i]; ++i)
        width += br_dialogue_char_advance((unsigned char)text[i]);
    return width;
}

static int br_dialogue_word_width(const char* text, int length) {
    int width = 0;
    for (int i = 0; i < length; ++i)
        width += br_dialogue_char_advance((unsigned char)text[i]);
    return width;
}

static int wrap_dialogue(const char* text, char out[MAX_LINES][MAX_LINE_CHARS + 1]) {
    for (int i = 0; i < MAX_LINES; ++i) out[i][0] = '\0';
    if (!text || !*text) return 0;
    int line = 0, len = 0, width = 0;
    const char* p = text;
    while (*p && line < MAX_LINES) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char* word = p;
        int wl = 0;
        while (p[wl] && p[wl] != ' ') ++wl;
        const int ww = br_dialogue_word_width(word, wl);
        const int space = len ? br_dialogue_char_advance(' ') : 0;
        if (len && width + space + ww > BUBBLE_TEXT_MAX_W) {
            ++line; len = 0; width = 0;
            if (line >= MAX_LINES) break;
        }
        if (ww > BUBBLE_TEXT_MAX_W) {
            for (int i = 0; i < wl && line < MAX_LINES; ++i) {
                const int adv = br_dialogue_char_advance((unsigned char)word[i]);
                if (len && width + adv > BUBBLE_TEXT_MAX_W) {
                    ++line; len = 0; width = 0;
                    if (line >= MAX_LINES) break;
                }
                if (len < MAX_LINE_CHARS) {
                    out[line][len++] = word[i];
                    out[line][len] = '\0';
                    width += adv;
                }
            }
        } else {
            if (len && len < MAX_LINE_CHARS) {
                out[line][len++] = ' ';
                width += space;
            }
            int copy = wl;
            if (copy > MAX_LINE_CHARS - len) copy = MAX_LINE_CHARS - len;
            memcpy(out[line] + len, word, (size_t)copy);
            len += copy;
            out[line][len] = '\0';
            width += br_dialogue_word_width(word, copy);
        }
        p += wl;
    }
    return line + 1;
}

static void br_draw_compact_text(HalTexture* font, const char* text, int x, int y) {
    if (!font || !text) return;
    int dx = x;
    for (int i = 0; text[i]; ++i) {
        const unsigned int ch = (unsigned char)text[i];
        const int sx = (int)(ch % 16u) * FONT_W;
        const int sy = (int)(ch / 16u) * FONT_H;
        /* Native semibold 6x10 avoids the blurred, eye-straining 7x11
         * scaling while keeping proportional spacing. */
        const DialogueGlyphMetric metric = br_active_dialogue_metrics()[ch];
        /* Bubble text is content, not an optional table shadow.  Keep it
         * visible when the player disables graphical shadows. */
        hal_draw_sprite_tinted(font, sx, sy, FONT_W, FONT_H,
                               dx - metric.left, y, 0x121212u, 100u);
        dx += metric.advance;
    }
}

static uint8_t br_dialogue_priority(DialogueTrigger trigger) {
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

static bool br_dialogue_is_critical(DialogueTrigger trigger) {
    return trigger == DIALOGUE_NPC_WINS || trigger == DIALOGUE_NPC_LOSES ||
           trigger == DIALOGUE_PLAYER_LAST_CARD || trigger == DIALOGUE_NPC_LAST_CARD;
}

static bool br_dialogue_passes_frequency(DialogueTrigger trigger) {
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

static void br_dialogue_clear_visible(BRUiState* ui) {
    if (!ui) return;
    ui->text = NULL;
    ui->visible_until_ms = 0u;
    ui->visible_started_ms = 0u;
    ui->face_index = -1;
    ui->owner_index = -1;
}

static void br_dialogue_clear_queue(BRUiState* ui) {
    if (!ui) return;
    ui->queue_count = 0u;
    memset(ui->throw_count, 0, sizeof(ui->throw_count));
    ui->decision_started_ms = 0u;
    ui->stalling_fired = false;
    br_dialogue_clear_visible(ui);
}

void br_ui_set_dialogues_enabled(bool enabled) {
    g_br_dialogues_enabled = enabled;
}

void br_ui_reset(BRUiState* ui, uint32_t now_ms) {
    if (!ui) return;
    br_hand_visual_reset_all(now_ms);
    memset(ui, 0, sizeof(*ui));
    memset(ui->last_variant, -1, sizeof(ui->last_variant));
    ui->owner_index = -1;
    ui->face_index = -1;
    ui->pre_resolve_defender = -1;
    ui->last_dialogue_ms = now_ms >= BR_DIALOGUE_COOLDOWN_MS
                         ? now_ms - BR_DIALOGUE_COOLDOWN_MS : 0u;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i)
        ui->last_by_owner_ms[i] = now_ms >= BR_DIALOGUE_OWNER_COOLDOWN_MS
                                ? now_ms - BR_DIALOGUE_OWNER_COOLDOWN_MS : 0u;
}

void br_ui_sync_hands(BRUiState* ui, const BattleRoyalState* br) {
    if (!ui || !br) return;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i)
        ui->stable_hand_count[i] = (uint8_t)br->players[i].hand.card_count;
}

void br_ui_new_round(BRUiState* ui, uint32_t now_ms) {
    if (!ui) return;
    const int blink = ui->blink_frame;
    uint8_t masks[BR_MAX_PLAYERS][DIALOGUE_TRIGGER_COUNT];
    int8_t last[BR_MAX_PLAYERS][DIALOGUE_TRIGGER_COUNT];
    uint32_t last_owner[BR_MAX_PLAYERS];
    memcpy(masks, ui->used_mask, sizeof(masks));
    memcpy(last, ui->last_variant, sizeof(last));
    memcpy(last_owner, ui->last_by_owner_ms, sizeof(last_owner));
    const uint8_t next_serial = (uint8_t)(ui->round_serial + 1u);
    const uint8_t ordinary_match = ui->ordinary_shown_count;
    const uint8_t stall_match = ui->stalling_match_count;
    const uint32_t last_dialogue = ui->last_dialogue_ms;
    br_ui_reset(ui, now_ms);
    memcpy(ui->used_mask, masks, sizeof(masks));
    memcpy(ui->last_variant, last, sizeof(last));
    memcpy(ui->last_by_owner_ms, last_owner, sizeof(last_owner));
    ui->ordinary_shown_count = ordinary_match;
    ui->stalling_match_count = stall_match;
    ui->last_dialogue_ms = last_dialogue;
    ui->round_serial = next_serial;
    ui->blink_frame = blink;
}

static int br_first_npc(const BattleRoyalState* br, int preferred) {
    if (!br) return -1;
    if (preferred >= 0 && preferred < BR_MAX_PLAYERS &&
        br->players[preferred].present && !br->players[preferred].human)
        return preferred;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
        if (br->players[i].present && !br->players[i].human)
            return i;
    }
    return -1;
}

static bool br_queue_dialogue(BRUiState* ui, const BattleRoyalState* br,
                              int owner, DialogueTrigger trigger) {
    if (!ui || !br || !g_br_dialogues_enabled) return false;
    if (owner < 0 || owner >= BR_MAX_PLAYERS) return false;
    if (br->players[owner].human || !br->players[owner].present) return false;
    if (trigger < 0 || trigger >= DIALOGUE_TRIGGER_COUNT) return false;
    if (trigger == DIALOGUE_PLAYER_STALLING &&
        (ui->stalling_fired || ui->stalling_match_count >= 2u)) return false;
    if (!br_dialogue_is_critical(trigger) && !br_dialogue_passes_frequency(trigger))
        return false;

    const uint8_t priority = br_dialogue_priority(trigger);
    if (trigger == DIALOGUE_NPC_WINS || trigger == DIALOGUE_NPC_LOSES)
        br_dialogue_clear_queue(ui);

    for (uint8_t i = 0; i < ui->queue_count; ++i) {
        const BRUiDialogueEvent* e = &ui->queue[i];
        if (e->owner_index == (uint8_t)owner &&
            e->trigger == (uint8_t)trigger &&
            e->round_serial == ui->round_serial)
            return false;
    }

    if (ui->queue_count >= BR_UI_DIALOGUE_QUEUE_CAPACITY) {
        uint8_t lowest = 0u;
        for (uint8_t i = 1u; i < ui->queue_count; ++i)
            if (ui->queue[i].priority < ui->queue[lowest].priority) lowest = i;
        if (priority <= ui->queue[lowest].priority) return false;
        ui->queue[lowest] = ui->queue[ui->queue_count - 1u];
        ui->queue_count--;
    }

    BRUiDialogueEvent* e = &ui->queue[ui->queue_count++];
    e->owner_index = (uint8_t)owner;
    e->trigger = (uint8_t)trigger;
    e->priority = priority;
    e->round_serial = ui->round_serial;
    e->queued_ms = hal_get_ticks_ms();
    return true;
}

static bool br_choose_line(BRUiState* ui, const BattleRoyalState* br,
                           int owner, DialogueTrigger trigger,
                           const char** out_text) {
    int face = br->players[owner].face_index;
    if (face < 0) face = 0;
    face %= DIALOGUE_CHARACTER_COUNT;
    uint8_t mask = ui->used_mask[owner][trigger] & 0x07u;
    if (mask == 0x07u) mask = 0u;
    const int last = ui->last_variant[owner][trigger];
    int choices[DIALOGUE_VARIANTS_PER_TRIGGER], n = 0;
    for (int i = 0; i < DIALOGUE_VARIANTS_PER_TRIGGER; ++i)
        if ((mask & (1u << i)) == 0u && i != last) choices[n++] = i;
    if (n == 0)
        for (int i = 0; i < DIALOGUE_VARIANTS_PER_TRIGGER; ++i)
            if ((mask & (1u << i)) == 0u) choices[n++] = i;
    if (n == 0) return false;

    const int first = duren_rand() % n;
    for (int attempt = 0; attempt < n; ++attempt) {
        const int variant = choices[(first + attempt) % n];
        const char* candidate = dialogue_get_line(ui_get_language(), face,
                                                  trigger, variant);
        char wrapped[MAX_LINES][MAX_LINE_CHARS + 1];
        if (!candidate || !candidate[0] ||
            wrap_dialogue(candidate, wrapped) < 1 || !wrapped[0][0])
            continue;
        ui->used_mask[owner][trigger] = (uint8_t)(mask | (1u << variant));
        ui->last_variant[owner][trigger] = (int8_t)variant;
        ui->face_index = face;
        *out_text = candidate;
        return true;
    }
    return false;
}

static void br_dialogue_update(BRUiState* ui, const BattleRoyalState* br,
                               uint32_t now_ms) {
    if (!ui || !br) return;
    if (!g_br_dialogues_enabled) {
        br_dialogue_clear_queue(ui);
        return;
    }
    if (ui->text && now_ms < ui->visible_until_ms) return;
    if (ui->text) br_dialogue_clear_visible(ui);

    for (uint8_t i = 0; i < ui->queue_count;) {
        if (ui->queue[i].priority < 90u &&
            now_ms - ui->queue[i].queued_ms > BR_DIALOGUE_STALE_MS) {
            ui->queue[i] = ui->queue[ui->queue_count - 1u];
            ui->queue_count--;
        } else {
            ++i;
        }
    }
    if (ui->queue_count == 0u) return;

    uint8_t best = 0u;
    for (uint8_t i = 1u; i < ui->queue_count; ++i)
        if (ui->queue[i].priority > ui->queue[best].priority) best = i;
    const BRUiDialogueEvent e = ui->queue[best];
    const bool urgent = e.priority >= 90u;
    if (!urgent) {
        if (ui->ordinary_shown_count >= BR_DIALOGUE_MAX_ORDINARY_MATCH ||
            ui->round_shown_count >= BR_UI_MAX_DIALOGUES_PER_ROUND) {
            ui->queue[best] = ui->queue[ui->queue_count - 1u];
            ui->queue_count--;
            return;
        }
        if (now_ms - ui->last_dialogue_ms < BR_DIALOGUE_COOLDOWN_MS) return;
        if (now_ms - ui->last_by_owner_ms[e.owner_index] < BR_DIALOGUE_OWNER_COOLDOWN_MS)
            return;
    }

    ui->queue[best] = ui->queue[ui->queue_count - 1u];
    ui->queue_count--;
    const char* candidate = NULL;
    if (!br_choose_line(ui, br, e.owner_index,
                        (DialogueTrigger)e.trigger, &candidate))
        return;

    ui->text = candidate;
    ui->owner_index = e.owner_index;
    ui->visible_started_ms = now_ms;
    ui->visible_until_ms = now_ms + BR_DIALOGUE_PUFF_MS + dialogue_duration(candidate);
    ui->last_dialogue_ms = now_ms;
    ui->last_by_owner_ms[e.owner_index] = now_ms;
    ui->shown_count++;
    ui->shown_by_owner[e.owner_index]++;
    if (!urgent) {
        ui->ordinary_shown_count++;
        ui->round_shown_count++;
    }
    if (e.trigger == DIALOGUE_PLAYER_STALLING) ui->stalling_match_count++;
}

static void br_capture_pre_resolve(BRUiState* ui, const BattleRoyalState* br,
                                   bool took) {
    ui->pre_resolve_valid = true;
    ui->pre_resolve_took = took;
    ui->pre_resolve_defender = (int8_t)br->defender;
    int table_cards = 0;
    for (int i = 0; i < br->table_pair_count; ++i) {
        if (br->table_attack[i].rank >= RANK_6) table_cards++;
        if (br->table_defense[i].rank >= RANK_6) table_cards++;
    }
    ui->pre_resolve_table_cards = (uint8_t)table_cards;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i)
        ui->pre_resolve_hand_count[i] = (uint8_t)br->players[i].hand.card_count;
}

static void br_flush_throw_summaries(BRUiState* ui, const BattleRoyalState* br) {
    const int player_speaker = br_first_npc(br,
        (!br->players[br->defender].human) ? br->defender : br->primary_attacker);
    if (ui->throw_count[0] == 1u)
        br_queue_dialogue(ui, br, player_speaker, DIALOGUE_PLAYER_THROWS_ONE);
    else if (ui->throw_count[0] >= 4u)
        br_queue_dialogue(ui, br, player_speaker, DIALOGUE_PLAYER_THROWS_MANY);
    else if (ui->throw_count[0] >= 2u)
        br_queue_dialogue(ui, br, player_speaker, DIALOGUE_PLAYER_THROWS_SEVERAL);

    for (int i = 1; i < BR_MAX_PLAYERS; ++i) {
        if (ui->throw_count[i] == 1u)
            br_queue_dialogue(ui, br, i, DIALOGUE_NPC_THROWS_ONE);
        else if (ui->throw_count[i] >= 2u)
            br_queue_dialogue(ui, br, i, DIALOGUE_NPC_THROWS_SEVERAL);
    }
    memset(ui->throw_count, 0, sizeof(ui->throw_count));
}

static void br_react_draw(BRUiState* ui, const BattleRoyalState* br,
                          int owner, int first_drawn) {
    if (owner <= 0 || owner >= BR_MAX_PLAYERS || !br->players[owner].present) return;
    const int end = br->players[owner].hand.card_count;
    if (first_drawn < 0) first_drawn = 0;
    if (first_drawn >= end) return;
    int score = 0, count = 0;
    for (int i = first_drawn; i < end; ++i) {
        Card c = br->players[owner].hand.hand[i];
        if (c.suit == br->trump_suit) score += 4;
        if (c.rank == RANK_A) score += 4;
        else if (c.rank == RANK_K) score += 3;
        else if (c.rank == RANK_Q) score += 2;
        else if (c.rank == RANK_J || c.rank == RANK_10) score += 1;
        else if (card_rank_strength_u8(c.rank) <= 7) score -= 2;
        count++;
    }
    if (count > 0 && score >= count * 2)
        br_queue_dialogue(ui, br, owner, DIALOGUE_GOOD_DRAW);
    else if (count > 0 && score <= 0)
        br_queue_dialogue(ui, br, owner, DIALOGUE_BAD_DRAW);
}

void br_ui_after_event_ack(BRUiState* ui, const BattleRoyalState* br,
                           BREventType event_type, uint32_t now_ms) {
    (void)now_ms;
    if (!ui || !br) return;
    /* R113-style resolution is split into visible TAKE/BEAT and DEAL events.
     * Evaluate the draw only after DEAL has committed the landed cards. */
    if (event_type == BR_EVENT_DEAL && ui->pre_resolve_valid) {
        for (int i = 1; i < BR_MAX_PLAYERS; ++i) {
            int first_drawn = ui->pre_resolve_hand_count[i];
            if (ui->pre_resolve_took && i == ui->pre_resolve_defender)
                first_drawn += ui->pre_resolve_table_cards;
            br_react_draw(ui, br, i, first_drawn);
        }
        ui->pre_resolve_valid = false;
    }

    if (br_cards_left(br) == 0) {
        for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
            const uint8_t now = (uint8_t)br->players[i].hand.card_count;
            if (ui->stable_hand_count[i] >= 2u && now == 1u) {
                if (i == 0) {
                    const int speaker = br_first_npc(br, br->primary_attacker);
                    br_queue_dialogue(ui, br, speaker, DIALOGUE_PLAYER_LAST_CARD);
                } else {
                    br_queue_dialogue(ui, br, i, DIALOGUE_NPC_LAST_CARD);
                }
            }
        }
    }
    br_ui_sync_hands(ui, br);
}

void br_ui_update_stalling(BRUiState* ui, const BattleRoyalState* br,
                           bool actionable, bool input_happened, uint32_t now_ms) {
    if (!ui || !br || !g_br_dialogues_enabled || !actionable) {
        if (ui) { ui->decision_started_ms = 0u; ui->stalling_fired = false; }
        return;
    }
    if (input_happened) {
        ui->decision_started_ms = now_ms;
        ui->stalling_fired = false;
        return;
    }
    if (ui->text || ui->queue_count > 0u) {
        ui->decision_started_ms = now_ms;
        return;
    }
    if (ui->decision_started_ms == 0u) ui->decision_started_ms = now_ms;
    if (!ui->stalling_fired && ui->stalling_match_count < 2u &&
        now_ms - ui->decision_started_ms >= 9500u) {
        const int speaker = br_first_npc(br, br->primary_attacker);
        if (br_queue_dialogue(ui, br, speaker, DIALOGUE_PLAYER_STALLING))
            ui->stalling_fired = true;
    }
}

void br_ui_react_event(BRUiState* ui, const BattleRoyalState* br, const BREvent* e,
                       uint32_t now_ms) {
    (void)now_ms;
    if (!ui || !br || !e || !g_br_dialogues_enabled) return;
    switch (e->type) {
        case BR_EVENT_ATTACK:
            if (e->table_index == 0 && e->actor > 0)
                br_queue_dialogue(ui, br, e->actor, DIALOGUE_NPC_STARTS_ATTACK);
            else if (e->table_index > 0 && e->actor >= 0 && e->actor < BR_MAX_PLAYERS &&
                     ui->throw_count[e->actor] < 255u)
                ui->throw_count[e->actor]++;
            break;
        case BR_EVENT_RESOLVE_BEAT:
            br_capture_pre_resolve(ui, br, false);
            br_flush_throw_summaries(ui, br);
            if (br->defender > 0)
                br_queue_dialogue(ui, br, br->defender, DIALOGUE_NPC_DEFENDED);
            else
                br_queue_dialogue(ui, br, br_first_npc(br, br->primary_attacker),
                                  DIALOGUE_PLAYER_DEFENDED);
            break;
        case BR_EVENT_RESOLVE_TAKE:
            br_capture_pre_resolve(ui, br, true);
            br_flush_throw_summaries(ui, br);
            if (br->defender > 0)
                br_queue_dialogue(ui, br, br->defender, DIALOGUE_NPC_TAKES);
            else
                br_queue_dialogue(ui, br, br_first_npc(br, br->primary_attacker),
                                  DIALOGUE_PLAYER_TAKES);
            break;
        case BR_EVENT_ROUND_FINISHED:
            if (br->champion >= 0) {
                if (br->players[br->champion].human) {
                    const int loser = br_first_npc(br, br->round_loser);
                    br_queue_dialogue(ui, br, loser, DIALOGUE_NPC_LOSES);
                } else {
                    br_queue_dialogue(ui, br, br->champion, DIALOGUE_NPC_WINS);
                }
            }
            break;
        default:
            break;
    }
}

void br_ui_actor_anchor(int i, int* x, int* y) {
    /* Five-pixel screen margin around every scaled portrait. */
    static const int xs[4] = {270, 5, 5, 270};
    static const int ys[4] = {190, 190, 5, 5};
    if (i < 0 || i >= 4) i = 0;
    if (x) *x = xs[i];
    if (y) *y = ys[i];
}

static int hand_step(int count, int width) {
    if (count <= 1) return 0;
    int step = (width - BR_CARD_W) / (count - 1);
    if (step < 1) step = 1;
    if (step > BR_CARD_W) step = BR_CARD_W;
    return step;
}

void br_ui_hand_anchor(const BattleRoyalState* br, int i, int* x, int* y, int* width) {
    int hx = 170, hy = 184, hw = BR_BOTTOM_HAND_DEFAULT_W;

    /* Hand sectors are permanent for the whole round. A player leaving the
     * round does not donate screen space to the remaining NPCs, so no fan
     * jumps or expands while the human watches in spectator mode. */
    switch (i) {
        case 0: /* human: lower-right, 150 px */
            hx = br && br->human_full_bottom
               ? BR_BOTTOM_HAND_LEFT_X
               : BR_BOTTOM_HAND_LEFT_X + BR_NPC3_HAND_W + BR_BOTTOM_HAND_FENCE_GAP;
            hy = 175;
            hw = br && br->human_full_bottom
               ? BR_BOTTOM_HAND_RIGHT_X - BR_BOTTOM_HAND_LEFT_X
               : BR_PLAYER_HAND_W;
            break;
        case 1: /* NPC3: lower-left, compact 55 px */
            hx = BR_BOTTOM_HAND_LEFT_X;
            hy = 175;
            hw = BR_NPC3_HAND_W;
            break;
        case 2: /* upper-left */
            hx = 54;
            hy = 5;
            hw = BR_BOTTOM_HAND_DEFAULT_W;
            break;
        case 3: /* upper-right */
            hx = 170;
            hy = 5;
            hw = BR_BOTTOM_HAND_DEFAULT_W;
            break;
        default:
            break;
    }

    if (x) *x = hx;
    if (y) *y = hy;
    if (width) *width = hw;
}

void br_ui_table_card_size(const BattleRoyalState* br, int* width, int* height) {
    (void)br;
    if (width) *width = BR_TABLE_CARD_W;
    if (height) *height = BR_TABLE_CARD_H;
}

/* The actual pair count drives each row. The first row is capped at seven,
 * so adding pair eight cannot make the original seven jump. */
static int br_visible_pair_count(const BattleRoyalState* br, int requested_slot) {
    int count = br ? br->table_pair_count : 0;
    if (requested_slot + 1 > count) count = requested_slot + 1;
    if (count < 1) count = 1;
    if (count > BR_MAX_ATTACKS) count = BR_MAX_ATTACKS;
    return count;
}

static int br_row_pair_count(int total_pairs, int row) {
    int count = total_pairs - row * BR_TABLE_ROW_CAPACITY;
    if (count < 1) count = 1;
    if (count > BR_TABLE_ROW_CAPACITY) count = BR_TABLE_ROW_CAPACITY;
    return count;
}

static int br_pair_step(int row_pair_count) {
    if (row_pair_count <= 1) return 0;
    const int field_w = BR_TABLE_FIELD_RIGHT - BR_TABLE_FIELD_LEFT + 1;
    int step = (field_w - BR_TABLE_PAIR_W) / (row_pair_count - 1);
    if (step > 70) step = 70;
    if (step < 1) step = 1;
    return step;
}

/* Draw row two first and the defender-side row last. Within a row, the order
 * follows the defender side so an attack/defence pair remains one layer. */
static int br_table_draw_pair_index(const BattleRoyalState* br,
                                    int pair_count, int draw_index) {
    if (pair_count < 0) pair_count = 0;
    if (pair_count > BR_MAX_ATTACKS) pair_count = BR_MAX_ATTACKS;
    if (draw_index < 0 || draw_index >= pair_count) return 0;

    const int defender = br ? br->defender : 2;
    const bool right = defender == 0 || defender == 3;
    const int first_count = pair_count < BR_TABLE_ROW_CAPACITY
                          ? pair_count : BR_TABLE_ROW_CAPACITY;
    const int second_count = pair_count - first_count;

    if (draw_index < second_count) {
        const int local = draw_index;
        return BR_TABLE_ROW_CAPACITY +
               (right ? second_count - 1 - local : local);
    }

    const int local = draw_index - second_count;
    return right ? first_count - 1 - local : local;
}

void br_ui_table_anchor(const BattleRoyalState* br, int slot, bool defense,
                        int* x, int* y) {
    if (slot < 0) slot = 0;
    if (slot >= BR_MAX_ATTACKS) slot = BR_MAX_ATTACKS - 1;

    const int defender = br ? br->defender : 2;
    const bool bottom = defender == 0 || defender == 1;
    const bool right = defender == 0 || defender == 3;
    const int total = br_visible_pair_count(br, slot);
    const int row = slot / BR_TABLE_ROW_CAPACITY;
    const int row_slot = slot % BR_TABLE_ROW_CAPACITY;
    const int row_count = br_row_pair_count(total, row);
    const int step = br_pair_step(row_count);

    int tx;
    if (right)
        tx = BR_TABLE_FIELD_RIGHT - BR_TABLE_PAIR_W + 1 - row_slot * step;
    else
        tx = BR_TABLE_FIELD_LEFT + row_slot * step;
    int ty = bottom
           ? BR_TABLE_BOTTOM_Y - row * BR_TABLE_ROW_OFFSET
           : BR_TABLE_TOP_Y + row * BR_TABLE_ROW_OFFSET;

    if (defense) {
        tx += BR_TABLE_DEFENSE_SHIFT_X;
        ty += BR_TABLE_DEFENSE_SHIFT_Y;
    }
    if (x) *x = tx;
    if (y) *y = ty;
}


/* Selection never changes horizontal spacing. The chosen card keeps its
 * normal X coordinate and is drawn last, lifted only on the Y axis. */
static void hand_layout(int count, int width, int offsets[MAX_HAND]) {
    if (count <= 0) return;
    if (count > MAX_HAND) count = MAX_HAND;
    if (width < BR_CARD_W) width = BR_CARD_W;

    const int step = hand_step(count, width);
    for (int i = 0; i < count; ++i) offsets[i] = step * i;
}

static void br_hand_visual_update(const BattleRoyalState* br, int player_index,
                                  int hx, int hy, int hw, int selected_index,
                                  uint32_t dt_ms) {
    if (!br || player_index < 0 || player_index >= BR_MAX_PLAYERS) return;
    g_br_hand_visual_sorting[player_index] = false;
    int count = br->players[player_index].hand.card_count;
    if (count <= 0) {
        g_br_hand_visual_count[player_index] = -1;
        return;
    }
    if (count > MAX_HAND) count = MAX_HAND;

    Card old_cards[MAX_HAND];
    int32_t old_x[MAX_HAND];
    int32_t old_y[MAX_HAND];
    const int old_count = g_br_hand_visual_count[player_index] > 0
                        ? g_br_hand_visual_count[player_index] : 0;
    for (int i = 0; i < old_count; ++i) {
        old_cards[i] = g_br_hand_visual_cards[player_index][i];
        old_x[i] = g_br_hand_visual_x_q16[player_index][i];
        old_y[i] = g_br_hand_visual_y_q16[player_index][i];
    }

    int offsets[MAX_HAND] = {0};
    hand_layout(count, hw, offsets);
    for (int i = 0; i < count; ++i) {
        const Card card = br->players[player_index].hand.hand[i];
        const int32_t tx = (hx + offsets[i]) << 16;
        const int32_t ty = (hy - (i == selected_index ? BR_SELECTED_LIFT_Y : 0)) << 16;
        int old_idx = -1;
        for (int j = 0; j < old_count; ++j) {
            if (br_visual_card_same(card, old_cards[j])) {
                old_idx = j;
                break;
            }
        }
        if (old_idx >= 0) {
            g_br_hand_visual_x_q16[player_index][i] =
                br_hand_visual_approach(old_x[old_idx], tx, dt_ms);
            g_br_hand_visual_y_q16[player_index][i] =
                br_hand_visual_approach(old_y[old_idx], ty, dt_ms);
            if (g_br_hand_visual_x_q16[player_index][i] != tx)
                g_br_hand_visual_sorting[player_index] = true;
        } else {
            g_br_hand_visual_x_q16[player_index][i] = tx;
            g_br_hand_visual_y_q16[player_index][i] = ty;
        }
        g_br_hand_visual_cards[player_index][i] = card;
    }
    g_br_hand_visual_count[player_index] = count;
}

static int br_hand_visual_x(int player_index, int card_index) {
    return (int)((g_br_hand_visual_x_q16[player_index][card_index] + 32768) >> 16);
}

static int br_hand_visual_y(int player_index, int card_index) {
    return (int)((g_br_hand_visual_y_q16[player_index][card_index] + 32768) >> 16);
}

void br_ui_hand_card_anchor(const BattleRoyalState* br, int player_index,
                            int hand_count, int hand_index, bool selected,
                            int* x, int* y) {
    int hx = 0, hy = 0, hw = BR_CARD_W;
    br_ui_hand_anchor(br, player_index, &hx, &hy, &hw);
    if (hand_count < 1) hand_count = 1;
    if (hand_count > MAX_HAND) hand_count = MAX_HAND;
    if (hand_index < 0) hand_index = 0;
    if (hand_index >= hand_count) hand_index = hand_count - 1;

    int offsets[MAX_HAND] = {0};
    hand_layout(hand_count, hw, offsets);
    if (x) *x = hx + offsets[hand_index];
    if (y) *y = hy - (selected ? BR_SELECTED_LIFT_Y : 0);
}

static int card_source_x(Card c) {
    return card_canonical_id(c) * BR_CARD_W;
}

static void draw_card_face_sprite(HalTexture* cards, Card c, int x, int y,
                                  bool dimmed) {
    if (!cards || c.rank < RANK_6) return;
    /* Complete canonical sprite: fan overlap is painter order only. */
    if (dimmed) {
        hal_draw_sprite_dimmed(cards, card_source_x(c), 0, BR_CARD_W, BR_CARD_H,
                               x, y, DIM_PERCENT);
    } else {
        hal_draw_sprite(cards, card_source_x(c), 0, BR_CARD_W, BR_CARD_H, x, y);
    }
}

static void draw_table_card(HalTexture* cards, Card c, int x, int y,
                            int draw_w, int draw_h) {
    (void)draw_w;
    (void)draw_h;
    if (!cards || c.rank < RANK_6) return;
    hal_draw_sprite_shadow(cards, card_source_x(c), 0, BR_CARD_W, BR_CARD_H,
                           x + BR_CONTACT_SHADOW_X,
                           y + BR_CONTACT_SHADOW_Y, SHADOW_COLOR);
    hal_draw_sprite(cards, card_source_x(c), 0, BR_CARD_W, BR_CARD_H, x, y);
}

static void draw_card_back_sprite(HalTexture* back, int x, int y,
                                  bool dimmed) {
    if (!back) return;
    if (dimmed) {
        hal_draw_sprite_dimmed(back, 0, 0, BR_CARD_W, BR_CARD_H,
                               x, y, DIM_PERCENT);
    } else {
        hal_draw_sprite(back, 0, 0, BR_CARD_W, BR_CARD_H, x, y);
    }
}

static void draw_trump_native(HalTexture* cards, Card c, int x, int y) {
    if (!cards || c.rank < RANK_6) return;
    int sx = card_source_x(c);
    hal_draw_sprite_shadow(cards, sx, 0, BR_CARD_W, BR_CARD_H,
                           x + BR_CONTACT_SHADOW_X,
                           y + BR_CONTACT_SHADOW_Y, SHADOW_COLOR);
    hal_draw_sprite(cards, sx, 0, BR_CARD_W, BR_CARD_H, x, y);
}

static void draw_deck_native(HalTexture* back, int x, int y) {
    if (!back) return;
    hal_draw_sprite(back, 0, 0, BR_CARD_W, BR_CARD_H, x, y);
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

static void draw_face_box(HalTexture* faces, int face, int x, int y, bool dimmed) {
    if (!faces) return;
    int sx = (face % 20) * FACE_W;
    const unsigned int outer = dimmed ? 0x100804u : 0x241108u;
    const unsigned int inner = dimmed ? 0x34200Fu : 0x603517u;
    hal_draw_sprite_shadow(faces, sx, 0, BR_FACE_W, BR_FACE_H,
                           x + BR_CONTACT_SHADOW_X,
                           y + BR_CONTACT_SHADOW_Y, 0x000000);
    draw_rounded_outline(x - 2, y - 2, BR_FACE_W + 4, BR_FACE_H + 4, outer);
    draw_rounded_outline(x - 1, y - 1, BR_FACE_W + 2, BR_FACE_H + 2, inner);
    /* faces2.png carries the same transparent 3-2-1 corner mask as cards, so
     * the inactive dim pass cannot leave a dark square in the cut corners. */
    if (dimmed)
        hal_draw_sprite_dimmed(faces, sx, 0, BR_FACE_W, BR_FACE_H,
                               x, y, DIM_PERCENT);
    else
        hal_draw_sprite(faces, sx, 0, BR_FACE_W, BR_FACE_H, x, y);
}

static void draw_player_face_box(HalTexture* faces, int face,
                                 int x, int y, bool dimmed) {
    if (!faces) return;
    const int sx = (face & 1) * FACE_W;
    const unsigned int outer = dimmed ? 0x100804u : 0x241108u;
    const unsigned int inner = dimmed ? 0x34200Fu : 0x603517u;
    hal_draw_sprite_shadow(faces, sx, 0, BR_FACE_W, BR_FACE_H,
                           x + BR_CONTACT_SHADOW_X,
                           y + BR_CONTACT_SHADOW_Y, 0x000000);
    draw_rounded_outline(x - 2, y - 2, BR_FACE_W + 4, BR_FACE_H + 4, outer);
    draw_rounded_outline(x - 1, y - 1, BR_FACE_W + 2, BR_FACE_H + 2, inner);
    if (dimmed)
        hal_draw_sprite_dimmed(faces, sx, 0, BR_FACE_W, BR_FACE_H,
                               x, y, DIM_PERCENT);
    else
        hal_draw_sprite(faces, sx, 0, BR_FACE_W, BR_FACE_H, x, y);
}

static unsigned int card_hint_alpha_from_frame(int frame) {
    /* Always visible, but gently pulses from 24% to 46% opacity. */
    const int phase = frame % 54;
    const int tri = phase < 27 ? phase : 53 - phase;
    return 24u + (unsigned int)((tri * 22) / 26);
}

/* Orange translucent mask follows the rounded sprite alpha exactly. It is
 * drawn immediately after its card, so later overlapping cards naturally
 * cover it instead of the mask leaking across the whole hand or pile. */
static void draw_card_blink_mask(HalTexture* cards, Card card,
                                 int x, int y, int frame) {
    if (!cards || card.rank < RANK_6) return;
    hal_draw_sprite_tinted(cards, card_source_x(card), 0, BR_CARD_W, BR_CARD_H,
                           x, y, 0xF07818u, card_hint_alpha_from_frame(frame));
}

static bool br_human_throw_hint_active(const BattleRoyalState* br) {
    return br && !br->event_pending && br->phase == BR_PHASE_THROW &&
           br->decision_player == 0 && br->players[0].present &&
           !br->players[0].round_out && !br->players[0].eliminated;
}

static bool br_human_has_legal_rank(const BattleRoyalState* br, Rank rank) {
    if (!br || br->decision_player != 0 || br->phase != BR_PHASE_THROW) return false;
    for (int i = 0; i < br->players[0].hand.card_count; ++i) {
        if (br->players[0].hand.hand[i].rank == rank && br_human_card_legal(br, i))
            return true;
    }
    return false;
}

static unsigned int blink_frame_color(int frame) {
    const int phase = frame % 54;
    const int tri = phase < 27 ? phase : 53 - phase;
    const unsigned int r = 224u + (31u * (unsigned int)tri) / 26u;
    const unsigned int g = 112u + (104u * (unsigned int)tri) / 26u;
    const unsigned int b = 16u + (34u * (unsigned int)tri) / 26u;
    return (r << 16) | (g << 8) | b;
}

static void draw_blink_frame(int x, int y, int frame) {
    draw_rounded_outline(x - 3, y - 3, BR_FACE_W + 6, BR_FACE_H + 6,
                         blink_frame_color(frame));
}

static bool event_hides_table_card(const BattleRoyalState* br, int slot, bool defense) {
    if (!br->event_pending) return false;
    if (br->event.type == BR_EVENT_ATTACK && !defense && br->event.table_index == slot) return true;
    if (br->event.type == BR_EVENT_DEFEND && defense && br->event.table_index == slot) return true;
    return false;
}

static int br_dialogue_round_inset(int edge) {
    if (edge <= 0) return 7;
    if (edge == 1) return 4;
    if (edge == 2) return 2;
    if (edge == 3) return 1;
    return 0;
}

static void br_dialogue_pill_fill(int x, int y, int w, int h,
                                  unsigned int color) {
    if (w < 16 || h < 12) return;
    for (int row = 0; row < h; ++row) {
        int edge = row;
        if (h - 1 - row < edge) edge = h - 1 - row;
        const int inset = br_dialogue_round_inset(edge);
        const int span = w - inset * 2;
        if (span > 0) hal_fill_rect(x + inset, y + row, span, 1, color);
    }
}

static void br_dialogue_pill(int x, int y, int w, int h) {
    br_dialogue_pill_fill(x, y, w, h, 0x090909u);
    br_dialogue_pill_fill(x + 1, y + 1, w - 2, h - 2, 0xFFFFFFu);
}

static void br_dialogue_short_tail(int body_x, int body_w, int center_y,
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

static int br_dialogue_puff_percent(uint32_t elapsed) {
    if (elapsed < 35u) return 30;
    if (elapsed < 75u) return 58;
    if (elapsed < 115u) return 82;
    if (elapsed < 145u) return 106;
    return 100;
}

static void draw_dialogue(const BattleRoyalState* br, BRUiState* ui,
                          HalTexture* font, HalTexture* bubble_texture) {
    (void)bubble_texture;
    if (!br || !ui || !font) return;
    const uint32_t now_ms = hal_get_ticks_ms();
    br_dialogue_update(ui, br, now_ms);
    if (!ui->text || !ui->text[0] ||
        now_ms >= ui->visible_until_ms || ui->owner_index < 0) return;

    char lines[MAX_LINES][MAX_LINE_CHARS + 1];
    int n = wrap_dialogue(ui->text, lines);
    if (n < 1 || !lines[0][0]) return;
    if (n > MAX_LINES) n = MAX_LINES;

    int max_text_w = 0;
    for (int i = 0; i < n; ++i) {
        const int w = br_dialogue_text_width(lines[i]);
        if (w > max_text_w) max_text_w = w;
    }
    int final_w = max_text_w + BUBBLE_PAD_X * 2;
    if (final_w < BUBBLE_MIN_W) final_w = BUBBLE_MIN_W;
    if (final_w > BUBBLE_MAX_W) final_w = BUBBLE_MAX_W;
    const int text_block_h = n * COMPACT_FONT_H +
                             (n - 1) *
                             (COMPACT_LINE_H - COMPACT_FONT_H);
    int final_h = text_block_h + BUBBLE_PAD_Y * 2;
    if (final_h > BUBBLE_MAX_H) final_h = BUBBLE_MAX_H;

    int fx, fy;
    br_ui_actor_anchor(ui->owner_index, &fx, &fy);
    const bool portrait_left = fx + BR_FACE_W / 2 < SCREEN_WIDTH / 2;
    const int center_y = fy + BR_FACE_H / 2;
    const int target_x = portrait_left ? fx + BR_FACE_W : fx;
    const uint32_t elapsed = now_ms - ui->visible_started_ms;
    const int pct = br_dialogue_puff_percent(elapsed);
    int bw = final_w * pct / 100;
    int bh = final_h * pct / 100;
    if (bw < 14) bw = 14;
    if (bh < 10) bh = 10;
    const int gap = 7;
    int bx = portrait_left ? target_x + gap : target_x - gap - bw;
    int by = center_y - bh / 2;
    if (bx < 1) bx = 1;
    if (bx + bw >= SCREEN_WIDTH) bx = SCREEN_WIDTH - bw - 1;
    if (by < 1) by = 1;
    if (by + bh >= SCREEN_HEIGHT) by = SCREEN_HEIGHT - bh - 1;

    int tail_y = center_y;
    if (tail_y < by + 8) tail_y = by + 8;
    if (tail_y > by + bh - 9) tail_y = by + bh - 9;
    br_dialogue_pill(bx, by, bw, bh);
    br_dialogue_short_tail(bx, bw, tail_y, !portrait_left);
    if (elapsed < BR_DIALOGUE_PUFF_MS) return;

    const int block_h = text_block_h;
    const int ty = by + (bh - block_h) / 2;
    for (int i = 0; i < n; ++i) {
        const int tw = br_dialogue_text_width(lines[i]);
        br_draw_compact_text(font, lines[i], bx + (bw - tw) / 2,
                             ty + i * COMPACT_LINE_H);
    }
}

static bool br_ui_action_phase(const BattleRoyalState* br) {
    return br && br->phase >= BR_PHASE_ATTACK && br->phase <= BR_PHASE_THROW;
}

static bool br_ui_player_lit(const BattleRoyalState* br, int player) {
    if (!br || player < 0 || player >= BR_MAX_PLAYERS) return false;
    if (!br->players[player].present || br->players[player].round_out ||
        br->players[player].eliminated) return false;
    if (!br_ui_action_phase(br) && !br->event_pending) return false;
    if (player == br->decision_player || player == br->defender) return true;
    if (br->event_pending &&
        (player == br->event.actor || player == br->event.target)) return true;
    return false;
}

unsigned int br_ui_hand_dim_percent(const BattleRoyalState* br,
                                    int player_index) {
    if (!br || player_index < 0 || player_index >= BR_MAX_PLAYERS) return 0u;
    const bool action_scene = br_ui_action_phase(br) || br->event_pending;
    return action_scene && !br_ui_player_lit(br, player_index)
         ? DIM_PERCENT : 0u;
}

void br_ui_draw(const BattleRoyalState* br, BRUiState* ui,
                HalTexture* cards, HalTexture* card_back, HalTexture* faces,
                HalTexture* player_faces,
                HalTexture* font6x10, HalTexture* bubble_right,
                bool hide_table) {
    if (!br || !ui) return;
    const uint32_t draw_now_ms = hal_get_ticks_ms();
    uint32_t hand_dt_ms = g_br_hand_visual_last_ms == 0u
                        ? 16u : draw_now_ms - g_br_hand_visual_last_ms;
    if (hand_dt_ms > 50u) hand_dt_ms = 50u;
    g_br_hand_visual_last_ms = draw_now_ms;
    const bool throw_hint_active = br_human_throw_hint_active(br);
    if (throw_hint_active && !ui->throw_hint_was_active)
        ui->blink_frame = 26; /* enter THROW at a clearly visible pulse peak */
    else if (throw_hint_active)
        ++ui->blink_frame;
    else
        ui->blink_frame = 0;
    ui->throw_hint_was_active = throw_hint_active;

    if (br->phase == BR_PHASE_CHAMPION && br->champion >= 0) {
        hal_set_table_sprite_lighting(false);
        hal_clear_checkerboard(0, 0);
        int face = br->players[br->champion].face_index;
        if (br->champion == 0)
            draw_player_face_box(player_faces, face,
                                 (SCREEN_WIDTH - BR_FACE_W) / 2, 68, false);
        else
            draw_face_box(faces, face, (SCREEN_WIDTH - BR_FACE_W) / 2, 68, false);
        const char* name = br->champion == 0 ? ui_tr(UI_STR_YOU) : ui_ai_name(face);
        draw_text6_center_white(font6x10, ui_tr(UI_STR_BR_WINNER), SCREEN_WIDTH / 2, 28);
        draw_text6_center_white(font6x10, name, SCREEN_WIDTH / 2, 121);
        draw_text6_center_white(font6x10, ui_tr(UI_STR_PRESS_A_RETURN), SCREEN_WIDTH / 2, 154);
        return;
    }

    /* Console: DMA2D creates the lit felt and sprite blitters use the cached
     * gain map. Desktop: the cached light overlay is placed immediately after
     * the felt clear. No second full-screen hardware post-process is needed. */
    hal_set_table_sprite_lighting(true);
    hal_clear_checkerboard(0, 0);
    hal_apply_table_lighting();

    /* Android layout: a full-size vertical trump sits under the full-size
     * deck.  Only its left 11-pixel rank/suit strip remains visible. */
    int left = br_cards_left(br);
    if (left > 0) {
        draw_trump_native(cards, br->trump_card, BR_TRUMP_X, BR_TRUMP_Y);
        if (left > 1) draw_deck_native(card_back, BR_DECK_X, 89);
    }

    if (!hide_table) {
        int table_w = CARD_W, table_h = CARD_H;
        br_ui_table_card_size(br, &table_w, &table_h);

        /* Draw wrapped pairs first, then the defender-line pairs. The whole
         * attack/defense pair remains one layer and later cards cannot erase a
         * critical rank corner from the first four pairs. */
        const int visible_pairs = br->table_pair_count < BR_MAX_ATTACKS ? br->table_pair_count : BR_MAX_ATTACKS;
        for (int draw_i = 0; draw_i < visible_pairs; ++draw_i) {
            const int i = br_table_draw_pair_index(br, visible_pairs, draw_i);
            int x, y;
            if (!event_hides_table_card(br, i, false)) {
                br_ui_table_anchor(br, i, false, &x, &y);
                draw_table_card(cards, br->table_attack[i], x, y,
                                table_w, table_h);
                if (throw_hint_active &&
                    br_human_has_legal_rank(br, br->table_attack[i].rank))
                    draw_card_blink_mask(cards, br->table_attack[i], x, y,
                                         ui->blink_frame);
            }
            if (br->table_defense[i].rank >= RANK_6 &&
                !event_hides_table_card(br, i, true)) {
                br_ui_table_anchor(br, i, true, &x, &y);
                draw_table_card(cards, br->table_defense[i], x, y,
                                table_w, table_h);
                if (throw_hint_active &&
                    br_human_has_legal_rank(br, br->table_defense[i].rank))
                    draw_card_blink_mask(cards, br->table_defense[i], x, y,
                                         ui->blink_frame);
            }
        }
    }

    const bool action_scene = br_ui_action_phase(br) || br->event_pending;
    for (int p = 0; p < BR_MAX_PLAYERS; ++p) {
        if (!br->players[p].present) continue;
        if (br->players[p].round_out || br->players[p].eliminated) continue;

        int hx, hy, hw;
        br_ui_hand_anchor(br, p, &hx, &hy, &hw);
        int count = br->players[p].hand.card_count;
        if (count <= 0) continue;
        if (count > MAX_HAND) count = MAX_HAND;

        const bool lit = br_ui_player_lit(br, p);
        const bool dimmed = br_ui_hand_dim_percent(br, p) > 0u;
        const int selected_index =
            (p == 0 && br->decision_player == 0 && lit &&
             br->selected_card >= 0 && br->selected_card < count)
            ? br->selected_card : -1;

        br_hand_visual_update(br, p, hx, hy, hw, selected_index, hand_dt_ms);

        /* Full shadow pass for all cards. The subsequent sprite pass clips
         * covered portions naturally and keeps 6+ card fans visually solid. */
        const bool br_deal_active = br->event_pending &&
                                    br->event.type == BR_EVENT_DEAL;
        if (hal_shadows_enabled() && !br_deal_active &&
            !g_br_hand_visual_sorting[p]) {
            for (int i = 0; i < count; ++i) {
                const int x = br_hand_visual_x(p, i);
                const int y = br_hand_visual_y(p, i);
                if (p == 0) {
                    const Card c = br->players[p].hand.hand[i];
                    hal_draw_sprite_shadow(cards, card_source_x(c), 0,
                                           BR_CARD_W, BR_CARD_H,
                                           x + 1, y + 2, SHADOW_COLOR);
                } else {
                    hal_draw_sprite_shadow(card_back, 0, 0,
                                           BR_CARD_W, BR_CARD_H,
                                           x + 1, y + 2, SHADOW_COLOR);
                }
            }
        }

        /* Draw complete sprites in physical left-to-right z-order. A rounded
         * transparent corner shows the lower card where one exists, otherwise
         * it shows the felt. */
        for (int i = 0; i < count; ++i) {
            const int x = br_hand_visual_x(p, i);
            const int y = br_hand_visual_y(p, i);
            if (p == 0) {
                draw_card_face_sprite(cards, br->players[p].hand.hand[i],
                                      x, y, dimmed && i != selected_index);
                if (throw_hint_active && br_human_card_legal(br, i))
                    draw_card_blink_mask(cards, br->players[p].hand.hand[i],
                                         x, y, ui->blink_frame);
            } else {
                draw_card_back_sprite(card_back, x, y, dimmed);
            }
        }
    }

    /* Portraits and HUD are UI, not part of the felt lighting pass. */
    hal_set_table_sprite_lighting(false);

    for (int p = 0; p < BR_MAX_PLAYERS; ++p) {
        if (!br->players[p].present) continue;
        int fx, fy;
        br_ui_actor_anchor(p, &fx, &fy);

        const bool out = br->players[p].round_out || br->players[p].eliminated;
        const bool lit = br_ui_player_lit(br, p);
        const bool portrait_dimmed = out || (action_scene && !lit);
        if (p == 0)
            draw_player_face_box(player_faces, br->players[p].face_index,
                                 fx, fy, portrait_dimmed);
        else
            draw_face_box(faces, br->players[p].face_index,
                          fx, fy, portrait_dimmed);

        const bool framed = br_ui_action_phase(br) && br->decision_player == p && !out;
        if (framed) draw_blink_frame(fx, fy, ui->blink_frame);

        char money[16];
        snprintf(money, sizeof(money), "$%d", br->players[p].money);
        int my = (p == 0 || p == 1)
               ? (fy - FONT_H - 5)
               : (fy + BR_FACE_H + 5);
        int mx = fx + (BR_FACE_W - (int)strlen(money) * FONT_W) / 2;
        draw_text6_white(font6x10, money, mx, my);
        if (br->players[p].eliminated)
            draw_text6_center_white(font6x10, ui_tr(UI_STR_OUT), fx + BR_FACE_W / 2, fy + 16);
        else if (br->players[p].round_out)
            draw_text6_center_white(font6x10,
                                    ui_tr((p == 0 && br->players[p].out_order == 1)
                                          ? UI_STR_FINISHED_FIRST : UI_STR_DONE),
                                    fx + BR_FACE_W / 2, fy + 16);
    }

    char deck[16];
    snprintf(deck, sizeof(deck), "%s:%d", ui_tr(UI_STR_DECK), left);
    /* Right-align the raised counter so longer translations stay on-screen. */
    int deck_label_x = SCREEN_WIDTH - 2 - (int)strlen(deck) * FONT_W;
    if (deck_label_x < 250) deck_label_x = 250;
    draw_text6_white(font6x10, deck, deck_label_x, 72);

    if (br->phase == BR_PHASE_ROUND_BANNER) {
        char round[20];
        snprintf(round, sizeof(round), "%s %d", ui_tr(UI_STR_ROUND), br->round_number);
        draw_text6_center_white(font6x10, round, SCREEN_WIDTH / 2, 113);
    } else if (br->phase == BR_PHASE_ROUND_RESULT || br->phase == BR_PHASE_NEXT_PROMPT) {
        /* Transparent round result overlay: keep the felt visible and draw
         * only the white statistics/prompt text. */
        int loser = br->round_loser;
        char line1[32];
        const char* name = loser == 0 ? ui_tr(UI_STR_YOU) : ui_ai_name(br->players[loser].face_index);
        snprintf(line1, sizeof(line1), "%s: %s", ui_tr(UI_STR_LOSER), name);
        draw_text6_center_white(font6x10, line1, SCREEN_WIDTH / 2, 84);
        draw_text6_center_white(font6x10, "-$60", SCREEN_WIDTH / 2, 96);
        int row = 110;
        for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
            if (!br->players[i].present) continue;
            char money_line[32];
            const char* pn = i == 0 ? ui_tr(UI_STR_YOU) : ui_ai_name(br->players[i].face_index);
            snprintf(money_line, sizeof(money_line), "%s $%d", pn, br->players[i].money);
            draw_text6_center_white(font6x10, money_line, SCREEN_WIDTH / 2, row);
            row += 11;
        }
        if (br->champion >= 0) {
            draw_text6_center_white(font6x10, ui_tr(UI_STR_PRESS_A_WINNER), SCREEN_WIDTH / 2, 166);
        } else {
            draw_text6_center_white(font6x10, ui_tr(UI_STR_PRESS_A_START), SCREEN_WIDTH / 2, 166);
            draw_text6_center_white(font6x10, ui_tr(UI_STR_NEXT_ROUND), SCREEN_WIDTH / 2, 177);
        }
    }

    draw_dialogue(br, ui, font6x10, bubble_right);
}
