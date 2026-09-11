#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include <stdbool.h>

#define LEGACY_DECK_SIZE 36
#define DECK_SIZE   52
#define MAX_HAND    52
#define DECK_36_START_INDEX (DECK_SIZE - LEGACY_DECK_SIZE)
#define MAX_TABLE   10
#define MAX_AI_NAME 16

typedef enum { SUIT_SPADES, SUIT_CLUBS, SUIT_DIAMONDS, SUIT_HEARTS, SUIT_COUNT } Suit;
typedef enum {
    /* Preserve the old 6..A values for save compatibility and append 2..5. */
    RANK_6 = 6, RANK_7, RANK_8, RANK_9, RANK_10, RANK_J, RANK_Q, RANK_K, RANK_A,
    RANK_2, RANK_3, RANK_4, RANK_5
} Rank;
typedef enum { RESULT_NONE, RESULT_WIN, RESULT_LOSE, RESULT_DRAW } GameResult;
typedef enum { DECK_MODE_36 = 36, DECK_MODE_52 = 52 } DeckMode;

typedef struct { uint8_t suit; uint8_t rank; } Card;

static inline bool card_rank_valid_u8(uint8_t rank) {
    return (rank >= RANK_6 && rank <= RANK_A) ||
           (rank >= RANK_2 && rank <= RANK_5);
}

static inline int card_rank_strength_u8(uint8_t rank) {
    if (rank >= RANK_6 && rank <= RANK_A) return (int)rank;
    if (rank >= RANK_2 && rank <= RANK_5) return 2 + ((int)rank - RANK_2);
    return 0;
}

/* Atlas identity stays 0..35=6..A and 36..51=2..5. */
static inline int card_canonical_id(Card c) {
    if (c.suit >= SUIT_COUNT) return -1;
    if (c.rank >= RANK_6 && c.rank <= RANK_A)
        return ((int)c.rank - RANK_6) * SUIT_COUNT + (int)c.suit;
    if (c.rank >= RANK_2 && c.rank <= RANK_5)
        return LEGACY_DECK_SIZE + ((int)c.rank - RANK_2) * SUIT_COUNT + (int)c.suit;
    return -1;
}

static inline Card card_from_canonical_id(int id) {
    Card c = {0, 0};
    if (id < 0 || id >= DECK_SIZE) return c;
    c.suit = (uint8_t)(id % SUIT_COUNT);
    c.rank = (uint8_t)(id < LEGACY_DECK_SIZE
        ? RANK_6 + id / SUIT_COUNT
        : RANK_2 + (id - LEGACY_DECK_SIZE) / SUIT_COUNT);
    return c;
}

static inline uint8_t deck_mode_sanitize(int mode) {
    return mode == DECK_MODE_52 ? (uint8_t)DECK_MODE_52 : (uint8_t)DECK_MODE_36;
}

static inline int deck_mode_start_index(int mode) {
    return mode == DECK_MODE_52 ? 0 : DECK_36_START_INDEX;
}

static inline bool card_id_active_for_mode(int id, int mode) {
    return id >= 0 && id < DECK_SIZE &&
           (mode == DECK_MODE_52 || id < LEGACY_DECK_SIZE);
}
typedef struct { Card cards[DECK_SIZE]; int top_index; } Deck;
typedef struct { Card hand[MAX_HAND]; int card_count; bool is_ai; } Player;

typedef struct {
    char name[MAX_AI_NAME];
    int level;
    int face_index;
    uint8_t difficulty_profile; /* 0=Easy, 1=Normal, 2=Hard */
} Opponent;

typedef struct {
    Deck deck;
    Player players[2]; 
    Suit trump_suit;   
    Card trump_card;   
    bool is_player_turn; 
    Card table_attack[MAX_TABLE];  
    Card table_defense[MAX_TABLE]; 
    int table_pair_count;
    /* Public, canonical memory of cards that reached the discard pile.
     * AI V3 uses it only for legal endgame deduction; it never reads the
     * human hand or future deck order. */
    uint64_t discard_mask;
    GameResult result;          
    int player_money;
    int current_level;
    Opponent current_opponent;
    uint8_t deck_mode;
} GameState;

void game_set_default_deck_mode(int mode);
uint8_t game_get_default_deck_mode(void);
void game_init_global(GameState* state);
void game_init(GameState* state);
void game_set_opponent(GameState* state, int level);
void game_set_opponent_face(GameState* state, int face_index, int level);
const char* game_get_ai_name(int face_index);
void game_check_status(GameState* state);

// =========================================================
// ВИПРАВЛЕННЯ: Додані прототипи відсутніх функцій
// =========================================================
void game_shuffle_deck(GameState* state);
void game_deal_cards(GameState* state);
int game_ai_choose_attack(GameState* state);
int game_ai_choose_defend(GameState* state, Card attack_card);
bool game_can_attack_with(GameState* state, Card card);
bool game_can_defend_with(GameState* state, Card attack_card, Card defend_card);
bool game_table_fully_defended(const GameState* state);
bool game_attacker_has_legal_throw(const GameState* state);
bool game_should_auto_discard(const GameState* state);
int game_reserve_draw_to_six(GameState* state, int player_index,
                             Card* out_cards, int out_capacity);
int game_reserve_draw_round_robin(GameState* state, int first_player,
                                  int second_player, Card out_cards[2][6],
                                  int out_counts[2]);
void game_end_turn_no_deal(GameState* state, bool took);
void game_end_turn(GameState* state, bool took);
void game_end_turn_precollected(GameState* state);
void game_end_turn_precollected_no_deal(GameState* state);
bool game_sort_hand(Player* player, Suit trump_suit);

#endif
