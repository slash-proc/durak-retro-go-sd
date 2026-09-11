#ifndef BATTLE_ROYAL_UI_H
#define BATTLE_ROYAL_UI_H

#include "battle_royal.h"
#include "dialogue_data.h"
#include "hal.h"
#include <stdint.h>

#define BR_UI_MAX_DIALOGUES_PER_ROUND 3

typedef struct {
    uint8_t owner_index;
    uint8_t trigger;
    uint8_t priority;
    uint8_t round_serial;
    uint32_t queued_ms;
} BRUiDialogueEvent;

#define BR_UI_DIALOGUE_QUEUE_CAPACITY 2

typedef struct {
    const char* text;
    uint32_t visible_until_ms;
    uint32_t visible_started_ms;
    uint32_t last_dialogue_ms;
    uint32_t last_by_owner_ms[BR_MAX_PLAYERS];
    uint32_t decision_started_ms;
    int face_index;
    int owner_index;
    uint8_t shown_count;
    uint8_t ordinary_shown_count;
    uint8_t round_shown_count;
    uint8_t stalling_match_count;
    uint8_t shown_by_owner[BR_MAX_PLAYERS];
    uint8_t used_mask[BR_MAX_PLAYERS][DIALOGUE_TRIGGER_COUNT];
    int8_t last_variant[BR_MAX_PLAYERS][DIALOGUE_TRIGGER_COUNT];
    BRUiDialogueEvent queue[BR_UI_DIALOGUE_QUEUE_CAPACITY];
    uint8_t queue_count;
    uint8_t round_serial;
    uint8_t throw_count[BR_MAX_PLAYERS];
    uint8_t stable_hand_count[BR_MAX_PLAYERS];
    uint8_t pre_resolve_hand_count[BR_MAX_PLAYERS];
    uint8_t pre_resolve_table_cards;
    int8_t pre_resolve_defender;
    bool pre_resolve_valid;
    bool pre_resolve_took;
    bool stalling_fired;
    int blink_frame;
    bool throw_hint_was_active;
} BRUiState;

void br_ui_reset(BRUiState* ui, uint32_t now_ms);

void br_ui_set_dialogues_enabled(bool enabled);
void br_ui_set_game_speed_step(int speed_step);
void br_ui_sync_hands(BRUiState* ui, const BattleRoyalState* br);
void br_ui_after_event_ack(BRUiState* ui, const BattleRoyalState* br,
                           BREventType event_type, uint32_t now_ms);
void br_ui_update_stalling(BRUiState* ui, const BattleRoyalState* br,
                           bool actionable, bool input_happened, uint32_t now_ms);
void br_ui_new_round(BRUiState* ui, uint32_t now_ms);
void br_ui_react_event(BRUiState* ui, const BattleRoyalState* br, const BREvent* event,
                       uint32_t now_ms);
void br_ui_draw(const BattleRoyalState* br, BRUiState* ui,
                HalTexture* cards, HalTexture* card_back, HalTexture* faces,
                HalTexture* player_faces,
                HalTexture* font6x10, HalTexture* bubble_right,
                bool hide_table);
void br_ui_actor_anchor(int player_index, int* x, int* y);
void br_ui_table_card_size(const BattleRoyalState* br, int* width, int* height);
void br_ui_table_anchor(const BattleRoyalState* br, int table_index, bool defense,
                        int* x, int* y);
void br_ui_hand_anchor(const BattleRoyalState* br, int player_index, int* x, int* y,
                       int* width);
void br_ui_hand_card_anchor(const BattleRoyalState* br, int player_index,
                            int hand_count, int hand_index, bool selected,
                            int* x, int* y);
unsigned int br_ui_hand_dim_percent(const BattleRoyalState* br,
                                    int player_index);

#endif
