#include "game.h"
#include <stdlib.h>
#include <string.h>
#include "rng.h"

static int game_card_id(Card c) { return card_canonical_id(c); }

static uint8_t g_default_deck_mode = DECK_MODE_36;
void game_set_default_deck_mode(int mode) { g_default_deck_mode = deck_mode_sanitize(mode); }
uint8_t game_get_default_deck_mode(void) { return g_default_deck_mode; }

static void game_discard_claim(GameState* state, Card c) {
    const int id = game_card_id(c);
    if (state && id >= 0) state->discard_mask |= (((uint64_t)1u) << id);
}

static const char* ai_names[] = {
    "Doc", "Pip", "Bruno", "Nils", "Milo", "Rolf", "Mara", "Hank", "Ivan", "Kane",
    "Karl", "Glim", "Lyra", "Rook", "Oleg", "Fitz", "Nyx", "Morn", "Stan", "Jeff"
};

void game_set_opponent_face(GameState* state, int face_index, int level) {
    if (face_index < 0) face_index = 0;
    face_index %= 20;
    if (level < 1) level = 1;
    if (level > 20) level = 20;

    state->current_level = level;
    strncpy(state->current_opponent.name, ai_names[face_index], MAX_AI_NAME - 1);
    state->current_opponent.name[MAX_AI_NAME - 1] = '\0';
    state->current_opponent.level = level;
    state->current_opponent.face_index = face_index;
    if (state->current_opponent.difficulty_profile > 2u)
        state->current_opponent.difficulty_profile = 1u;
}

const char* game_get_ai_name(int face_index) {
    if (face_index < 0) face_index = 0;
    return ai_names[face_index % 20];
}

void game_set_opponent(GameState* state, int level) {
    int idx = (level - 1) % 20;
    if (idx < 0) idx += 20;
    game_set_opponent_face(state, idx, level);
}

void game_init_global(GameState* state) {
    memset(state, 0, sizeof(GameState));
    state->deck_mode = g_default_deck_mode;
    state->player_money = 500;
    game_set_opponent(state, 1);
    state->current_opponent.difficulty_profile = 1u;
    state->result = RESULT_NONE;
}

void game_check_status(GameState* state) {
    if (state->result != RESULT_NONE) return; 
    int cards_left = DECK_SIZE - state->deck.top_index;
    
    if (cards_left == 0 && state->players[0].card_count == 0 && state->players[1].card_count == 0) {
        state->result = RESULT_DRAW;
    } else if (cards_left == 0 && state->players[0].card_count == 0) {
        state->result = RESULT_WIN;
    } else if (cards_left == 0 && state->players[1].card_count == 0) {
        state->result = RESULT_LOSE;
    }
}

void game_init(GameState* state) {
    if (!state) return;
    state->deck_mode = deck_mode_sanitize(state->deck_mode);
    int idx = 0;
    for (int r = RANK_2; r <= RANK_5; r++) {
        for (int s = 0; s < SUIT_COUNT; s++) {
            state->deck.cards[idx].rank = (uint8_t)r;
            state->deck.cards[idx].suit = (uint8_t)s;
            idx++;
        }
    }
    for (int r = RANK_6; r <= RANK_A; r++) {
        for (int s = 0; s < SUIT_COUNT; s++) {
            state->deck.cards[idx].rank = r;
            state->deck.cards[idx].suit = s;
            idx++;
        }
    }
    state->deck.top_index = deck_mode_start_index(state->deck_mode);
    state->players[0].card_count = 0;
    state->players[0].is_ai = false;
    state->players[1].card_count = 0;
    state->players[1].is_ai = true;
    state->table_pair_count = 0;
    state->discard_mask = 0u;
    state->result = RESULT_NONE;
    
    for (int i = 0; i < MAX_TABLE; i++) {
        state->table_attack[i].rank = 0;
        state->table_defense[i].rank = 0;
    }
}

void game_shuffle_deck(GameState* state) {
    const int start = deck_mode_start_index(state->deck_mode);
    state->deck.top_index = start;
    for (int i = DECK_SIZE - 1; i > start; i--) {
        int j = start + duren_rand() % (i - start + 1);
        Card temp = state->deck.cards[i];
        state->deck.cards[i] = state->deck.cards[j];
        state->deck.cards[j] = temp;
    }
    state->trump_card = state->deck.cards[DECK_SIZE - 1];
    state->trump_suit = state->trump_card.suit;
    state->is_player_turn = true; /* finalized after initial deal */
}

static void deal_player_to_six(GameState* state, int player_index) {
    while (state->players[player_index].card_count < 6 &&
           state->deck.top_index < DECK_SIZE) {
        state->players[player_index].hand[state->players[player_index].card_count++] =
            state->deck.cards[state->deck.top_index++];
    }
}

static void deal_players_round_robin(GameState* state,
                                     int first_player,
                                     int second_player) {
    if (!state || first_player < 0 || first_player > 1 ||
        second_player < 0 || second_player > 1 ||
        first_player == second_player) return;
    const int order[2] = {first_player, second_player};
    bool dealt_any = true;
    while (dealt_any && state->deck.top_index < DECK_SIZE) {
        dealt_any = false;
        for (int oi = 0; oi < 2; ++oi) {
            const int pidx = order[oi];
            if (state->players[pidx].card_count >= 6 ||
                state->deck.top_index >= DECK_SIZE)
                continue;
            state->players[pidx].hand[state->players[pidx].card_count++] =
                state->deck.cards[state->deck.top_index++];
            dealt_any = true;
        }
    }
}

void game_deal_cards(GameState* state) {
    /* Initial deal remains unchanged from R113.11. */
    deal_player_to_six(state, 0);
    deal_player_to_six(state, 1);

    int player_low = 999;
    int ai_low = 999;
    for (int i = 0; i < state->players[0].card_count; ++i) {
        const Card c = state->players[0].hand[i];
        const int strength = card_rank_strength_u8(c.rank);
        if (c.suit == state->trump_suit && strength < player_low) player_low = strength;
    }
    for (int i = 0; i < state->players[1].card_count; ++i) {
        const Card c = state->players[1].hand[i];
        const int strength = card_rank_strength_u8(c.rank);
        if (c.suit == state->trump_suit && strength < ai_low) ai_low = strength;
    }
    if (player_low != 999 || ai_low != 999)
        state->is_player_turn = player_low <= ai_low;
    else
        state->is_player_turn = (duren_rand() & 1) != 0;
}

bool game_can_attack_with(GameState* state, Card card) {
    if (!state) return false;

    /* A bout may contain no more cards than the defender held when it began.
     * The original count is reconstructed without adding save-state fields:
     * current defender hand + cards already used to defend. */
    const int defender_idx = state->is_player_turn ? 1 : 0;
    int defended_count = 0;
    for (int i = 0; i < state->table_pair_count; ++i)
        if (card_rank_valid_u8(state->table_defense[i].rank)) ++defended_count;
    int attack_limit = state->players[defender_idx].card_count + defended_count;
    if (attack_limit > MAX_TABLE) attack_limit = MAX_TABLE;
    if (attack_limit < 1) return false;
    if (state->table_pair_count >= attack_limit) return false;

    if (state->table_pair_count == 0) return true;
    for (int i = 0; i < state->table_pair_count; i++) {
        if (state->table_attack[i].rank == card.rank) return true;
        if (state->table_defense[i].rank == card.rank) return true;
    }
    return false;
}

bool game_can_defend_with(GameState* state, Card attack_card, Card defend_card) {
    if (defend_card.suit == attack_card.suit)
        return card_rank_strength_u8(defend_card.rank) >
               card_rank_strength_u8(attack_card.rank);
    if (defend_card.suit == state->trump_suit &&
        attack_card.suit != state->trump_suit) return true;
    return false;
}

bool game_table_fully_defended(const GameState* state) {
    if (!state || state->table_pair_count <= 0) return false;
    for (int i = 0; i < state->table_pair_count; ++i) {
        if (!card_rank_valid_u8(state->table_attack[i].rank) ||
            !card_rank_valid_u8(state->table_defense[i].rank))
            return false;
    }
    return true;
}

bool game_attacker_has_legal_throw(const GameState* state) {
    if (!state || state->table_pair_count <= 0) return false;
    const int attacker_idx = state->is_player_turn ? 0 : 1;
    const Player* attacker = &state->players[attacker_idx];
    for (int i = 0; i < attacker->card_count; ++i) {
        if (game_can_attack_with((GameState*)state, attacker->hand[i]))
            return true;
    }
    return false;
}

bool game_should_auto_discard(const GameState* state) {
    return game_table_fully_defended(state) &&
           !game_attacker_has_legal_throw(state);
}

// =========================================================
// DUREN AI Tension (V2.5): stable V2 core + public memory + confidence
//
// Design goals:
// - never inspect the human hand or future deck order;
// - never create difficulty by choosing obviously catastrophic cards;
// - Easy/Normal/Hard differ by how narrowly they choose among close moves;
// - top trumps are protected while the stock is rich;
// - once the stock is empty, discard/table memory gives exact public inference.
// =========================================================

typedef enum {
    AI_STYLE_CAUTIOUS = 0,
    AI_STYLE_BALANCED = 1,
    AI_STYLE_AGGRESSIVE = 2
} AiStyle;

static int ai_level(const GameState* state) {
    int level = state ? state->current_opponent.level : 10;
    if (level < 1) level = 1;
    if (level > 20) level = 20;
    return level;
}

static int ai_profile(const GameState* state) {
    int profile = state ? (int)state->current_opponent.difficulty_profile : 1;
    if (profile < 0 || profile > 2) profile = 1;
    return profile;
}

static AiStyle ai_style(const GameState* state) {
    int face = state ? state->current_opponent.face_index : 1;
    if (face < 0) face = -face;
    return (AiStyle)(face % 3);
}

static int hand_rank_count(const Player* p, int rank) {
    int count = 0;
    if (!p) return 0;
    for (int i = 0; i < p->card_count; ++i)
        if (p->hand[i].rank == rank) ++count;
    return count;
}

static int ai_deck_cards_left(const GameState* state) {
    int left = state ? DECK_SIZE - state->deck.top_index : 0;
    if (left < 0) left = 0;
    if (left > DECK_SIZE) left = DECK_SIZE;
    return left;
}

static uint64_t ai_card_bit(Card c) {
    const int id = game_card_id(c);
    return id >= 0 ? (((uint64_t)1u) << id) : 0u;
}

static Card ai_card_from_id(int id) { return card_from_canonical_id(id); }

/* Only useful once the deck is empty. At that point the cards that are not
 * discarded, not on the table and not in the AI hand are exactly the
 * opponent's cards. This is deduction from public information, not peeking. */
static int ai_collect_endgame_unknown(const GameState* state,
                                      Card* out_cards, int capacity) {
    if (!state || ai_deck_cards_left(state) != 0) return 0;
    uint64_t known = state->discard_mask;
    for (int i = 0; i < state->players[1].card_count; ++i)
        known |= ai_card_bit(state->players[1].hand[i]);
    for (int i = 0; i < state->table_pair_count; ++i) {
        if (card_rank_valid_u8(state->table_attack[i].rank))
            known |= ai_card_bit(state->table_attack[i]);
        if (card_rank_valid_u8(state->table_defense[i].rank))
            known |= ai_card_bit(state->table_defense[i]);
    }

    int count = 0;
    for (int id = 0; id < DECK_SIZE; ++id) {
        if (!card_id_active_for_mode(id, state->deck_mode)) continue;
        if ((known & (((uint64_t)1u) << id)) != 0u) continue;
        if (out_cards && count < capacity) out_cards[count] = ai_card_from_id(id);
        ++count;
    }
    return count;
}

static bool ai_is_top_trump(const GameState* state, Card c) {
    return state && c.suit == state->trump_suit &&
           card_rank_strength_u8(c.rank) >= card_rank_strength_u8(RANK_Q);
}

static int ai_base_attack_score(const GameState* state, Card c, int profile) {
    const Player* ai = &state->players[1];
    const Player* defender = &state->players[0];
    const int duplicates = hand_rank_count(ai, c.rank) - 1;
    int score = (card_rank_strength_u8(c.rank) - 2) * 12;

    /* Even Easy respects trumps. Difficulty is not implemented as stupidity. */
    if (c.suit == state->trump_suit)
        score += profile == 0 ? 58 : (profile == 1 ? 70 : 82);

    if (duplicates > 0) {
        int pressure = profile == 0 ? 6 : (profile == 1 ? 11 : 16);
        const AiStyle style = ai_style(state);
        if (style == AI_STYLE_AGGRESSIVE) pressure += 5;
        else if (style == AI_STYLE_CAUTIOUS) pressure -= 3;
        if (pressure < 2) pressure = 2;
        score -= duplicates * pressure;
    }
    if (profile >= 1 && defender->card_count <= 2)
        score -= duplicates * (profile == 2 ? 12 : 6);
    return score;
}

static int ai_endgame_attack_adjustment(const GameState* state, Card attack) {
    Card unknown[DECK_SIZE];
    const int n = ai_collect_endgame_unknown(state, unknown, DECK_SIZE);
    if (n <= 0) return 0;

    int min_cover_cost = 100000;
    for (int i = 0; i < n; ++i) {
        if (!game_can_defend_with((GameState*)state, attack, unknown[i])) continue;
        int cost = (card_rank_strength_u8(unknown[i].rank) - 2) * 12;
        if (unknown[i].suit == state->trump_suit) cost += 82;
        if (cost < min_cover_cost) min_cover_cost = cost;
    }
    if (min_cover_cost == 100000) return -100; /* Publicly known forced take. */
    return -(min_cover_cost / 3);
}

static int ai_tension_attack_score(const GameState* state, Card c,
                                   int profile) {
    int score = ai_base_attack_score(state, c, profile);
    const int left = ai_deck_cards_left(state);
    const int duplicates = hand_rank_count(&state->players[1], c.rank) - 1;
    const AiStyle style = ai_style(state);

    /* Regression guard: Q/K/A trump must remain a precious resource early. */
    if (ai_is_top_trump(state, c) && left > 8)
        score += 60 + (card_rank_strength_u8(c.rank) -
                       card_rank_strength_u8(RANK_Q)) * 12;

    if (duplicates > 0 && c.suit != state->trump_suit &&
        state->players[0].card_count <= 3) {
        int bonus = 16;
        if (style == AI_STYLE_AGGRESSIVE) bonus = 24;
        else if (style == AI_STYLE_CAUTIOUS) bonus = 10;
        score -= duplicates * bonus;
    }

    if (left == 0) score += ai_endgame_attack_adjustment(state, c);
    return score;
}

static int ai_table_matching_unknown_count(const GameState* state, Card cover) {
    Card unknown[DECK_SIZE];
    const int n = ai_collect_endgame_unknown(state, unknown, DECK_SIZE);
    if (n <= 0) return 0;
    uint16_t ranks = 0u;
    for (int i = 0; i < state->table_pair_count; ++i) {
        if (card_rank_valid_u8(state->table_attack[i].rank))
            ranks |= (uint16_t)(1u << (state->table_attack[i].rank - RANK_6));
        if (card_rank_valid_u8(state->table_defense[i].rank))
            ranks |= (uint16_t)(1u << (state->table_defense[i].rank - RANK_6));
    }
    if (card_rank_valid_u8(cover.rank))
        ranks |= (uint16_t)(1u << (cover.rank - RANK_6));

    int count = 0;
    for (int i = 0; i < n; ++i) {
        const int bit = (int)unknown[i].rank - RANK_6;
        if (bit >= 0 && bit < 16 && (ranks & (uint16_t)(1u << bit))) ++count;
    }
    return count;
}

static int ai_tension_defense_score(const GameState* state, Card attack,
                                    Card c, int profile) {
    int score = (card_rank_strength_u8(c.rank) - 2) * 12;
    const bool spends_trump = c.suit == state->trump_suit &&
                              attack.suit != state->trump_suit;
    if (spends_trump)
        score += profile == 0 ? 58 : (profile == 1 ? 72 : 85);

    const int left = ai_deck_cards_left(state);
    if (spends_trump && card_rank_strength_u8(c.rank) >=
        card_rank_strength_u8(RANK_Q) && left > 8)
        score += 30 + (card_rank_strength_u8(c.rank) -
                       card_rank_strength_u8(RANK_Q)) * 10;

    if (left > 0 && hand_rank_count(&state->players[1], c.rank) >= 2)
        score += 6;

    /* In the exact-information endgame, avoid a cover that opens many ranks
     * the attacker is publicly known to hold and can immediately throw in. */
    if (left == 0)
        score += ai_table_matching_unknown_count(state, c) * 26;
    return score;
}

static bool ai_has_safe_attack_alternative(const GameState* state,
                                           const int* valid, int count) {
    for (int i = 0; i < count; ++i) {
        const Card c = state->players[1].hand[valid[i]];
        if (!ai_is_top_trump(state, c)) return true;
    }
    return false;
}

/* Difficulty controls the width of the acceptable decision band. Alternatives
 * outside that band are never selected. This prevents "Easy" from becoming a
 * random self-sabotaging bot while still keeping its play less precise. */
static int ai_choose_close_candidate(const int* valid, const int* scores,
                                     int count, int best_pos,
                                     int profile, int level) {
    if (!valid || !scores || count <= 0) return -1;
    if (best_pos < 0 || best_pos >= count) best_pos = 0;
    if (count == 1) return valid[best_pos];

    int window;
    int best_chance;
    if (profile == 0) {
        window = 96 - level;           /* 76..95: accepts many legal moves */
        best_chance = 18 + level;      /* 19..38%: visibly forgiving */
    } else if (profile == 1) {
        window = 42 - level / 3;       /* 36..42: rarely misses the optimum */
        best_chance = 88 + level / 2;  /* 88..98% */
    } else {
        window = 8 - level / 5;        /* 4..8 */
        if (window < 3) window = 3;
        best_chance = 92 + level / 3;  /* 92..98% */
    }
    if (best_chance > 99) best_chance = 99;

    const int limit = scores[best_pos] + window;
    int close[MAX_HAND];
    int close_count = 0;
    for (int i = 0; i < count; ++i)
        if (scores[i] <= limit) close[close_count++] = i;
    if (close_count <= 1 || (duren_rand() % 100) < best_chance)
        return valid[best_pos];

    int alternatives[MAX_HAND];
    int alt_count = 0;
    for (int i = 0; i < close_count; ++i)
        if (close[i] != best_pos) alternatives[alt_count++] = close[i];
    if (alt_count <= 0) return valid[best_pos];
    return valid[alternatives[duren_rand() % alt_count]];
}

int game_ai_choose_attack(GameState* state) {
    if (!state) return -1;
    int valid_indices[MAX_HAND];
    int valid_count = 0;
    for (int i = 0; i < state->players[1].card_count; ++i)
        if (game_can_attack_with(state, state->players[1].hand[i]))
            valid_indices[valid_count++] = i;
    if (valid_count == 0) return -1;

    const int profile = ai_profile(state);
    const int level = ai_level(state);

    /* Hard guard for all difficulty levels: while the stock is rich, remove
     * Q/K/A trumps from consideration whenever a sane legal alternative exists. */
    if (ai_deck_cards_left(state) > 8 &&
        ai_has_safe_attack_alternative(state, valid_indices, valid_count)) {
        int write = 0;
        for (int i = 0; i < valid_count; ++i) {
            const int idx = valid_indices[i];
            if (!ai_is_top_trump(state, state->players[1].hand[idx]))
                valid_indices[write++] = idx;
        }
        if (write > 0) valid_count = write;
    }

    int scores[MAX_HAND];
    int best_pos = 0;
    for (int i = 0; i < valid_count; ++i) {
        scores[i] = ai_tension_attack_score(
            state, state->players[1].hand[valid_indices[i]], profile);
        if (scores[i] < scores[best_pos]) best_pos = i;
    }

    /* Do not throw an expensive resource merely because its rank matches the
     * developed table. Passing is a legitimate and often stronger action. */
    if (state->table_pair_count > 0) {
        const Card best = state->players[1].hand[valid_indices[best_pos]];
        if (ai_is_top_trump(state, best) && ai_deck_cards_left(state) > 4)
            return -1;
    }

    return ai_choose_close_candidate(valid_indices, scores, valid_count,
                                     best_pos, profile, level);
}

int game_ai_choose_defend(GameState* state, Card attack_card) {
    if (!state) return -1;
    int valid_indices[MAX_HAND];
    int valid_count = 0;
    for (int i = 0; i < state->players[1].card_count; ++i)
        if (game_can_defend_with(state, attack_card, state->players[1].hand[i]))
            valid_indices[valid_count++] = i;
    if (valid_count == 0) return -1;

    const int profile = ai_profile(state);
    const int level = ai_level(state);
    /* On Easy the opponent sometimes chooses to take even though a cover is
     * available. This is intentional and makes the lowest setting clearly
     * distinct without giving Normal/Hard hidden information or bonuses. */
    if (profile == 0 && (duren_rand() % 100) < 20) return -1;
    int scores[MAX_HAND];
    int best_pos = 0;
    for (int i = 0; i < valid_count; ++i) {
        scores[i] = ai_tension_defense_score(
            state, attack_card, state->players[1].hand[valid_indices[i]], profile);
        if (scores[i] < scores[best_pos]) best_pos = i;
    }

    const Card best = state->players[1].hand[valid_indices[best_pos]];
    const int left = ai_deck_cards_left(state);
    if (attack_card.suit != state->trump_suit &&
        best.suit == state->trump_suit &&
        card_rank_strength_u8(best.rank) >= card_rank_strength_u8(RANK_Q) &&
        left > 8) {
        /* Stronger bots recognize sooner that taking a developed bout is
         * cheaper than burning a premium trump. */
        const int take_pair_threshold = profile == 2 ? 2 :
                                        (profile == 1 ? 3 : 4);
        if (state->table_pair_count >= take_pair_threshold) return -1;
    }

    return ai_choose_close_candidate(valid_indices, scores, valid_count,
                                     best_pos, profile, level);
}

static int card_sort_key(Card c, Suit trump_suit) {
    /* Visible hand order: all ordinary cards by ascending rank, then suit;
     * trumps form one separate ascending group on the far right. */
    const int rank_key = card_rank_strength_u8(c.rank) - 2;
    if ((Suit)c.suit == trump_suit)
        return 1024 + rank_key * SUIT_COUNT + (int)c.suit;
    return rank_key * SUIT_COUNT + (int)c.suit;
}

bool game_sort_hand(Player* player, Suit trump_suit) {
    if (!player || player->card_count < 2) return false;
    bool changed = false;
    for (int i = 1; i < player->card_count; ++i) {
        Card key = player->hand[i];
        const int key_value = card_sort_key(key, trump_suit);
        int j = i - 1;
        while (j >= 0 && card_sort_key(player->hand[j], trump_suit) > key_value) {
            player->hand[j + 1] = player->hand[j];
            --j;
            changed = true;
        }
        player->hand[j + 1] = key;
    }
    return changed;
}

void game_end_turn_precollected_no_deal(GameState* state) {
    if (!state) return;
    /* The defender already owns every table card. Preserve the attacker and
     * clear only the logical table; R96 online publishes this TAKE_COMMIT phase
     * before the separate alternating DEAL transaction. */
    state->table_pair_count = 0;
    for (int i = 0; i < MAX_TABLE; ++i) {
        state->table_attack[i].rank = 0;
        state->table_defense[i].rank = 0;
    }
}

void game_end_turn_precollected(GameState* state) {
    if (!state) return;
    const int attacker_idx = state->is_player_turn ? 0 : 1;
    const int defender_idx = state->is_player_turn ? 1 : 0;

    game_end_turn_precollected_no_deal(state);
    deal_players_round_robin(state, attacker_idx, defender_idx);
    game_check_status(state);
}

int game_reserve_draw_to_six(GameState* state, int player_index,
                                 Card* out_cards, int out_capacity) {
    if (!state || !out_cards || out_capacity <= 0 ||
        player_index < 0 || player_index > 1) return 0;
    int count = 0;
    const int current = state->players[player_index].card_count;
    while (current + count < 6 && state->deck.top_index < DECK_SIZE &&
           count < out_capacity) {
        out_cards[count++] = state->deck.cards[state->deck.top_index++];
    }
    return count;
}

int game_reserve_draw_round_robin(GameState* state, int first_player,
                                  int second_player, Card out_cards[2][6],
                                  int out_counts[2]) {
    if (!state || !out_cards || !out_counts || first_player < 0 ||
        first_player > 1 || second_player < 0 || second_player > 1 ||
        first_player == second_player) return 0;
    out_counts[0] = 0;
    out_counts[1] = 0;
    int final_count[2] = {
        state->players[0].card_count,
        state->players[1].card_count
    };
    const int order[2] = {first_player, second_player};
    int total = 0;
    bool drew_any = true;
    while (drew_any && state->deck.top_index < DECK_SIZE) {
        drew_any = false;
        for (int oi = 0; oi < 2; ++oi) {
            const int pidx = order[oi];
            if (final_count[pidx] >= 6 || out_counts[pidx] >= 6) continue;
            Card one = {0};
            if (game_reserve_draw_to_six(state, pidx, &one, 1) == 1) {
                out_cards[pidx][out_counts[pidx]++] = one;
                ++final_count[pidx];
                ++total;
                drew_any = true;
            }
        }
    }
    return total;
}

void game_end_turn_no_deal(GameState* state, bool took) {
    if (!state) return;
    const int defender_idx = state->is_player_turn ? 1 : 0;
    if (took) {
        for (int i = 0; i < state->table_pair_count; i++) {
            if (card_rank_valid_u8(state->table_attack[i].rank) &&
                state->players[defender_idx].card_count < MAX_HAND) {
                state->players[defender_idx].hand[state->players[defender_idx].card_count++] =
                    state->table_attack[i];
            }
            if (card_rank_valid_u8(state->table_defense[i].rank) &&
                state->players[defender_idx].card_count < MAX_HAND) {
                state->players[defender_idx].hand[state->players[defender_idx].card_count++] =
                    state->table_defense[i];
            }
        }
    } else {
        for (int i = 0; i < state->table_pair_count; ++i) {
            game_discard_claim(state, state->table_attack[i]);
            if (card_rank_valid_u8(state->table_defense[i].rank))
                game_discard_claim(state, state->table_defense[i]);
        }
        state->is_player_turn = !state->is_player_turn;
    }

    state->table_pair_count = 0;
    for (int i = 0; i < MAX_TABLE; i++) {
        state->table_attack[i].rank = 0;
        state->table_defense[i].rank = 0;
    }
}

void game_end_turn(GameState* state, bool took) {
    if (!state) return;
    const int attacker_idx = state->is_player_turn ? 0 : 1;
    const int defender_idx = state->is_player_turn ? 1 : 0;
    game_end_turn_no_deal(state, took);

    /* Durak draw order: one card at a time. The attacker of the bout draws
     * first, then the defender, and the cycle repeats until hands reach six or
     * the deck is empty. This keeps the last cards fair in every local mode. */
    deal_players_round_robin(state, attacker_idx, defender_idx);
    game_check_status(state);
}
