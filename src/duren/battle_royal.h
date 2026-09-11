#ifndef BATTLE_ROYAL_H
#define BATTLE_ROYAL_H

#include "game.h"
#include "modes.h"
#include <stdbool.h>
#include <stdint.h>

#define BR_MAX_PLAYERS 4
#define BR_MAX_ATTACKS 8
#define BR_START_MONEY 300
#define BR_LOSS_PENALTY 60

typedef enum {
    BR_PHASE_INACTIVE = 0,
    BR_PHASE_ROUND_BANNER,
    BR_PHASE_ATTACK,
    BR_PHASE_DEFEND,
    BR_PHASE_THROW,
    BR_PHASE_ROUND_RESULT,
    BR_PHASE_NEXT_PROMPT,
    BR_PHASE_CHAMPION
} BRPhase;

typedef enum {
    BR_EVENT_NONE = 0,
    BR_EVENT_ATTACK,
    BR_EVENT_DEFEND,
    BR_EVENT_TAKE_DECLARED,
    BR_EVENT_RESOLVE_BEAT,
    BR_EVENT_RESOLVE_TAKE,
    BR_EVENT_DEAL,
    BR_EVENT_ROUND_FINISHED,
    BR_EVENT_CHAMPION
} BREventType;

typedef struct {
    Player hand;
    int face_index;
    int money;
    int out_order;
    bool present;
    bool human;
    bool eliminated;
    bool round_out;
} BRPlayer;

typedef struct {
    BREventType type;
    int actor;
    int target;
    int table_index;
    Card card;
    /* Exact pre-removal source slot for the flight animation. Non-card events
     * keep source_hand_index=-1 and source_hand_count=0. */
    int source_hand_index;
    int source_hand_count;
} BREvent;

typedef struct {
    BRPlayer players[BR_MAX_PLAYERS];
    int player_count;
    Difficulty difficulty;

    Deck deck;
    Suit trump_suit;
    Card trump_card;

    Card table_attack[BR_MAX_ATTACKS];
    Card table_defense[BR_MAX_ATTACKS];
    int8_t table_attack_owner[BR_MAX_ATTACKS];
    int8_t table_defense_owner[BR_MAX_ATTACKS];
    int table_pair_count;
    uint64_t discard_mask;
    int defender_start_count;

    int primary_attacker;
    int attacker;
    int defender;
    int decision_player;
    int throw_cursor;
    int consecutive_passes;
    bool defender_taking;
    int pending_next_attacker;
    bool pending_resolve_took;

    BRPhase phase;
    uint32_t phase_until_ms;
    uint32_t ai_ready_at_ms;
    bool ai_delay_long;
    bool auto_resolve_pending;
    uint32_t auto_resolve_at_ms;

    BREvent event;
    bool event_pending;

    int round_number;
    int round_loser;
    int next_round_starter;
    int champion;
    int exit_serial;
    uint8_t seen_cards[SUIT_COUNT][13];
    uint8_t cards_seen_total;
    bool spectator;
    bool human_full_bottom;
    int selected_card;

    uint32_t play_time_ms;
    uint32_t last_tick_ms;
    uint8_t deck_mode;
} BattleRoyalState;

void br_init(BattleRoyalState* br, int player_count, int player_face, Difficulty difficulty);
void br_start_first_round(BattleRoyalState* br, uint32_t now_ms);
void br_start_next_round(BattleRoyalState* br, uint32_t now_ms);
void br_update_clock(BattleRoyalState* br, uint32_t now_ms);
void br_update_ai(BattleRoyalState* br, uint32_t now_ms);

bool br_human_can_act(const BattleRoyalState* br);
bool br_human_card_legal(const BattleRoyalState* br, int hand_index);
bool br_human_play(BattleRoyalState* br, int hand_index, uint32_t now_ms);
bool br_human_pass(BattleRoyalState* br, uint32_t now_ms);
bool br_human_take(BattleRoyalState* br, uint32_t now_ms);

void br_ack_event(BattleRoyalState* br, uint32_t now_ms);
void br_confirm_round_result(BattleRoyalState* br);
void br_confirm_next_prompt(BattleRoyalState* br, uint32_t now_ms);

int br_next_active_clockwise(const BattleRoyalState* br, int from);
int br_prev_active_clockwise(const BattleRoyalState* br, int from);
int br_cards_left(const BattleRoyalState* br);
int br_active_money_players(const BattleRoyalState* br);
int br_attack_limit(const BattleRoyalState* br);
int br_ai_choose_attack(const BattleRoyalState* br, int player_index);
int br_ai_choose_defense(const BattleRoyalState* br, int player_index);
bool br_ai_should_throw(const BattleRoyalState* br, int player_index, int hand_index);

#endif
