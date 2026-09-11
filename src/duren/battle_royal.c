#include "battle_royal.h"
#include <stdlib.h>
#include <string.h>
#include "rng.h"

#define BR_RESOLVE_HOLD_MS 500u

static bool valid_card(Card c) { return card_canonical_id(c) >= 0; }
static int br_card_id(Card c) { return card_canonical_id(c); }

static void br_discard_claim(BattleRoyalState* br, Card c) {
    const int id = br_card_id(c);
    if (br && id >= 0) br->discard_mask |= (((uint64_t)1u) << id);
}

static bool br_card_in_logical_state(const BattleRoyalState* br, Card card) {
    const int cid = br_card_id(card);
    if (!br || cid < 0) return false;
    if (br->discard_mask & (((uint64_t)1u) << cid)) return true;
    for (int i = br->deck.top_index; i < DECK_SIZE; ++i)
        if (br_card_id(br->deck.cards[i]) == cid) return true;
    for (int p = 0; p < BR_MAX_PLAYERS; ++p) {
        if (!br->players[p].present) continue;
        for (int i = 0; i < br->players[p].hand.card_count; ++i)
            if (br_card_id(br->players[p].hand.hand[i]) == cid) return true;
    }
    for (int i = 0; i < br->table_pair_count; ++i) {
        if (br_card_id(br->table_attack[i]) == cid) return true;
        if (valid_card(br->table_defense[i]) &&
            br_card_id(br->table_defense[i]) == cid) return true;
    }
    return false;
}
static void maybe_finish_round(BattleRoyalState* br);
static void schedule_resolve_event(BattleRoyalState* br, uint32_t now_ms);
static void start_resolve_event_now(BattleRoyalState* br);
static void auto_skip_unplayable_throwers(BattleRoyalState* br, uint32_t now_ms);

static void clear_table(BattleRoyalState* br) {
    br->table_pair_count = 0;
    for (int i = 0; i < BR_MAX_ATTACKS; ++i) {
        br->table_attack[i].rank = 0;
        br->table_attack[i].suit = 0;
        br->table_defense[i].rank = 0;
        br->table_defense[i].suit = 0;
        br->table_attack_owner[i] = -1;
        br->table_defense_owner[i] = -1;
    }
}

static bool br_player_in_round(const BattleRoyalState* br, int i) {
    return i >= 0 && i < BR_MAX_PLAYERS && br->players[i].present &&
           !br->players[i].eliminated && !br->players[i].round_out;
}

int br_next_active_clockwise(const BattleRoyalState* br, int from) {
    for (int step = 1; step <= BR_MAX_PLAYERS; ++step) {
        int i = (from + step) % BR_MAX_PLAYERS;
        if (br_player_in_round(br, i)) return i;
    }
    return -1;
}

int br_prev_active_clockwise(const BattleRoyalState* br, int from) {
    for (int step = 1; step <= BR_MAX_PLAYERS; ++step) {
        int i = (from - step + BR_MAX_PLAYERS * 2) % BR_MAX_PLAYERS;
        if (br->players[i].present && !br->players[i].eliminated) return i;
    }
    return -1;
}

int br_cards_left(const BattleRoyalState* br) {
    int left = DECK_SIZE - br->deck.top_index;
    return left < 0 ? 0 : left;
}

int br_active_money_players(const BattleRoyalState* br) {
    int n = 0;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i)
        if (br->players[i].present && !br->players[i].eliminated) ++n;
    return n;
}

int br_attack_limit(const BattleRoyalState* br) {
    int limit = br->defender_start_count;
    if (limit > BR_MAX_ATTACKS) limit = BR_MAX_ATTACKS;
    if (limit < 1) limit = 1;
    return limit;
}

static int thrower_count(const BattleRoyalState* br) {
    int n = 0;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i)
        if (br_player_in_round(br, i) && i != br->defender) ++n;
    return n;
}

static int next_thrower(const BattleRoyalState* br, int from) {
    int i = from;
    for (int step = 0; step < BR_MAX_PLAYERS; ++step) {
        if (i < 0) i = 0;
        if (br_player_in_round(br, i) && i != br->defender) return i;
        i = (i + 1) % BR_MAX_PLAYERS;
    }
    return -1;
}

static void set_ai_delay(BattleRoyalState* br, uint32_t now_ms, bool long_delay) {
    /* Decisions are immediate in every mode. Visible pacing belongs to card
     * motion and the explicit 500 ms table-inspection hold, never AI thinking. */
    br->ai_delay_long = long_delay;
    br->ai_ready_at_ms = now_ms;
}

static void emit_event(BattleRoyalState* br, BREventType type, int actor, int target,
                       int table_index, Card card) {
    br->event.type = type;
    br->event.actor = actor;
    br->event.target = target;
    br->event.table_index = table_index;
    br->event.card = card;
    br->event.source_hand_index = -1;
    br->event.source_hand_count = 0;
    br->event_pending = true;
}

static void emit_card_event(BattleRoyalState* br, BREventType type,
                            int actor, int target, int table_index, Card card,
                            int source_hand_index, int source_hand_count) {
    emit_event(br, type, actor, target, table_index, card);
    br->event.source_hand_index = source_hand_index;
    br->event.source_hand_count = source_hand_count;
}

static Card remove_hand_card(BRPlayer* p, int index) {
    Card c = {0, 0};
    if (!p || index < 0 || index >= p->hand.card_count) return c;
    c = p->hand.hand[index];
    for (int i = index; i + 1 < p->hand.card_count; ++i)
        p->hand.hand[i] = p->hand.hand[i + 1];
    p->hand.card_count--;
    return c;
}

static bool can_attack_rank(const BattleRoyalState* br, Card c) {
    if (br->table_pair_count == 0) return true;
    if (br->table_pair_count >= br_attack_limit(br)) return false;
    for (int i = 0; i < br->table_pair_count; ++i) {
        if (br->table_attack[i].rank == c.rank) return true;
        if (valid_card(br->table_defense[i]) && br->table_defense[i].rank == c.rank) return true;
    }
    return false;
}

static bool player_has_legal_throw(const BattleRoyalState* br, int player_index) {
    if (!br_player_in_round(br, player_index)) return false;
    if (br->phase != BR_PHASE_THROW || br->table_pair_count <= 0 ||
        br->table_pair_count >= br_attack_limit(br)) return false;
    const BRPlayer* p = &br->players[player_index];
    for (int i = 0; i < p->hand.card_count; ++i) {
        if (can_attack_rank(br, p->hand.hand[i])) return true;
    }
    return false;
}

static bool can_defend(const BattleRoyalState* br, Card attack, Card defense) {
    if (defense.suit == attack.suit &&
        card_rank_strength_u8(defense.rank) > card_rank_strength_u8(attack.rank)) return true;
    if (defense.suit == br->trump_suit && attack.suit != br->trump_suit) return true;
    return false;
}

static int card_value(const BattleRoyalState* br, Card c) {
    int value = card_rank_strength_u8(c.rank);
    if (c.suit == br->trump_suit) value += 20;
    return value;
}

static void record_seen(BattleRoyalState* br, Card c) {
    if (!valid_card(c) || c.suit >= SUIT_COUNT) return;
    int r = card_rank_strength_u8(c.rank) - 2;
    if (r < 0 || r >= 13) return;
    if (br->seen_cards[c.suit][r] < 255u) br->seen_cards[c.suit][r]++;
    if (br->cards_seen_total < 255u) br->cards_seen_total++;
}

static int seen_rank_total(const BattleRoyalState* br, int rank) {
    int r = card_rank_strength_u8((uint8_t)rank) - 2;
    if (r < 0 || r >= 13) return 0;
    int n = 0;
    for (int s = 0; s < SUIT_COUNT; ++s) n += br->seen_cards[s][r];
    return n;
}

static int own_rank_count(const BRPlayer* p, int rank) {
    int n = 0;
    for (int i = 0; i < p->hand.card_count; ++i)
        if (p->hand.hand[i].rank == rank) ++n;
    return n;
}

static int difficulty_mistake(Difficulty d) {
    if (d == DIFFICULTY_EASY) return 60;
    if (d == DIFFICULTY_HARD) return 0;
    return 8;
}

int br_ai_choose_attack(const BattleRoyalState* br, int player_index) {
    if (!br_player_in_round(br, player_index)) return -1;
    const BRPlayer* p = &br->players[player_index];
    int valid[MAX_HAND];
    int n = 0;
    for (int i = 0; i < p->hand.card_count; ++i)
        if (can_attack_rank(br, p->hand.hand[i])) valid[n++] = i;
    if (n == 0) return -1;
    if ((duren_rand() % 100) < difficulty_mistake(br->difficulty)) {
        if (br->difficulty == DIFFICULTY_EASY && n > 1) {
            int worst = valid[0];
            for (int i = 1; i < n; ++i)
                if (card_value(br, p->hand.hand[valid[i]]) >
                    card_value(br, p->hand.hand[worst])) worst = valid[i];
            return worst;
        }
        return valid[duren_rand() % n];
    }
    int best = valid[0];
    int best_score = card_value(br, p->hand.hand[best]);
    if (br->difficulty != DIFFICULTY_EASY)
        best_score -= (own_rank_count(p, p->hand.hand[best].rank) - 1) * 3;
    if (br->difficulty == DIFFICULTY_HARD)
        best_score -= seen_rank_total(br, p->hand.hand[best].rank);
    for (int i = 1; i < n; ++i) {
        int idx = valid[i];
        Card c = p->hand.hand[idx];
        int score = card_value(br, c);
        if (br->difficulty != DIFFICULTY_EASY)
            score -= (own_rank_count(p, c.rank) - 1) * 3;
        if (br->difficulty == DIFFICULTY_HARD)
            score -= seen_rank_total(br, c.rank);
        if (score < best_score) { best = idx; best_score = score; }
    }
    return best;
}

int br_ai_choose_defense(const BattleRoyalState* br, int player_index) {
    if (!br_player_in_round(br, player_index) || br->table_pair_count <= 0) return -1;
    const BRPlayer* p = &br->players[player_index];
    Card attack = br->table_attack[br->table_pair_count - 1];
    int valid[MAX_HAND];
    int n = 0;
    for (int i = 0; i < p->hand.card_count; ++i)
        if (can_defend(br, attack, p->hand.hand[i])) valid[n++] = i;
    if (n == 0) return -1;
    if ((duren_rand() % 100) < difficulty_mistake(br->difficulty)) {
        if (br->difficulty == DIFFICULTY_EASY && n > 1) {
            int worst = valid[0];
            for (int i = 1; i < n; ++i)
                if (card_value(br, p->hand.hand[valid[i]]) >
                    card_value(br, p->hand.hand[worst])) worst = valid[i];
            return worst;
        }
        return valid[duren_rand() % n];
    }
    int best = valid[0];
    for (int i = 1; i < n; ++i)
        if (card_value(br, p->hand.hand[valid[i]]) < card_value(br, p->hand.hand[best])) best = valid[i];
    return best;
}

bool br_ai_should_throw(const BattleRoyalState* br, int player_index, int hand_index) {
    if (hand_index < 0) return false;
    Card c = br->players[player_index].hand.hand[hand_index];
    int chance = 68;
    if (c.suit == br->trump_suit) chance -= 45;
    if (card_rank_strength_u8(c.rank) >= card_rank_strength_u8(RANK_K)) chance -= 18;
    if (br->players[br->defender].hand.card_count <= 2) chance += 16;
    if (br->defender_taking) chance += 20;
    if (br->difficulty == DIFFICULTY_EASY) chance -= 18;
    if (br->difficulty == DIFFICULTY_NORMAL) {
        if (own_rank_count(&br->players[player_index], c.rank) > 1) chance += 6;
    }
    if (br->difficulty == DIFFICULTY_HARD) {
        chance += 8;
        chance += seen_rank_total(br, c.rank) * 3;
        if (c.suit == br->trump_suit) {
            int seen_trumps = 0;
            for (int r = 0; r < 13; ++r) seen_trumps += br->seen_cards[br->trump_suit][r];
            if (seen_trumps < 6) chance -= 12;
        }
    }
    if (chance < 8) chance = 8;
    if (chance > 92) chance = 92;
    return (duren_rand() % 100) < chance;
}

static void shuffle_deck(BattleRoyalState* br) {
    int k = 0;
    for (int r = RANK_2; r <= RANK_5; ++r)
        for (int s = 0; s < SUIT_COUNT; ++s) {
            br->deck.cards[k].rank = (uint8_t)r;
            br->deck.cards[k].suit = (uint8_t)s;
            ++k;
        }
    for (int r = RANK_6; r <= RANK_A; ++r)
        for (int s = 0; s < SUIT_COUNT; ++s) {
            br->deck.cards[k].rank = (uint8_t)r;
            br->deck.cards[k].suit = (uint8_t)s;
            ++k;
        }
    const int start = deck_mode_start_index(br->deck_mode);
    for (int i = DECK_SIZE - 1; i > start; --i) {
        int j = start + duren_rand() % (i - start + 1);
        Card t = br->deck.cards[i];
        br->deck.cards[i] = br->deck.cards[j];
        br->deck.cards[j] = t;
    }
    br->deck.top_index = start;
    br->trump_card = br->deck.cards[DECK_SIZE - 1];
    br->trump_suit = (Suit)br->trump_card.suit;
}

static void draw_one(BattleRoyalState* br, int player_index) {
    BRPlayer* p = &br->players[player_index];
    if (p->hand.card_count >= MAX_HAND || br->deck.top_index >= DECK_SIZE) return;
    p->hand.hand[p->hand.card_count++] = br->deck.cards[br->deck.top_index++];
}

static void deal_initial(BattleRoyalState* br) {
    bool gave = true;
    while (gave) {
        gave = false;
        for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
            if (!br->players[i].present || br->players[i].eliminated) continue;
            if (br->players[i].hand.card_count < 6 && br->deck.top_index < DECK_SIZE) {
                draw_one(br, i);
                gave = true;
            }
        }
    }
}

static int find_lowest_trump_holder(const BattleRoyalState* br) {
    int best_player = -1;
    int best_rank = 999;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
        if (!br_player_in_round(br, i)) continue;
        for (int j = 0; j < br->players[i].hand.card_count; ++j) {
            Card c = br->players[i].hand.hand[j];
            if (c.suit == br->trump_suit && card_rank_strength_u8(c.rank) < best_rank) {
                best_rank = card_rank_strength_u8(c.rank);
                best_player = i;
            }
        }
    }
    if (best_player >= 0) return best_player;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i) if (br_player_in_round(br, i)) return i;
    return 0;
}

static void begin_battle(BattleRoyalState* br, int attacker, uint32_t now_ms) {
    if (!br_player_in_round(br, attacker)) attacker = br_next_active_clockwise(br, attacker);
    if (attacker < 0) { maybe_finish_round(br); return; }
    clear_table(br);
    br->primary_attacker = attacker;
    br->attacker = attacker;
    br->defender = br_next_active_clockwise(br, attacker);
    br->decision_player = attacker;
    br->throw_cursor = attacker;
    br->consecutive_passes = 0;
    br->defender_taking = false;
    br->auto_resolve_pending = false;
    br->auto_resolve_at_ms = 0u;
    br->defender_start_count = br->defender >= 0 ? br->players[br->defender].hand.card_count : 1;
    br->phase = BR_PHASE_ATTACK;
    set_ai_delay(br, now_ms, true);
}

static void randomize_faces(BattleRoyalState* br, int player_face) {
    bool used[20] = {false};
    used[player_face % 20] = true;
    for (int i = 1; i < BR_MAX_PLAYERS; ++i) {
        if (!br->players[i].present) continue;
        int face;
        do { face = duren_rand() % 20; } while (used[face]);
        used[face] = true;
        br->players[i].face_index = face;
    }
}

void br_init(BattleRoyalState* br, int player_count, int player_face, Difficulty difficulty) {
    memset(br, 0, sizeof(*br));
    if (player_count != 4) player_count = 3;
    br->player_count = player_count;
    br->difficulty = difficulty;
    br->deck_mode = game_get_default_deck_mode();
    br->round_number = 1;
    br->round_loser = -1;
    br->next_round_starter = -1;
    br->champion = -1;
    br->selected_card = 0;

    /* Clockwise fixed positions: 0 RB player, 1 LB NPC3, 2 LT NPC2, 3 RT NPC1. */
    for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
        br->players[i].present = (player_count == 4) || (i != 1);
        br->players[i].human = (i == 0);
        br->players[i].money = BR_START_MONEY;
        br->players[i].face_index = (i == 0) ? player_face : 0;
        br->players[i].hand.is_ai = (i != 0);
    }
    randomize_faces(br, player_face);
    br->spectator = false;
    br->human_full_bottom = (player_count == 3 || !br->players[1].present);
    br->phase = BR_PHASE_INACTIVE;
}

static void reset_round(BattleRoyalState* br) {
    shuffle_deck(br);
    clear_table(br);
    br->discard_mask = 0u;
    br->exit_serial = 0;
    memset(br->seen_cards, 0, sizeof(br->seen_cards));
    br->cards_seen_total = 0;
    br->round_loser = -1;
    br->champion = -1;
    br->spectator = br->players[0].eliminated;
    br->event_pending = false;
    br->event.type = BR_EVENT_NONE;
    br->pending_next_attacker = -1;
    br->pending_resolve_took = false;
    br->auto_resolve_pending = false;
    br->auto_resolve_at_ms = 0u;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
        br->players[i].hand.card_count = 0;
        br->players[i].round_out = br->players[i].eliminated || !br->players[i].present;
        br->players[i].out_order = 0;
    }
    /* NPC3 leaving one round temporarily expands the human lane, but the
     * split must be restored before the next deal unless NPC3 is permanently
     * absent or eliminated. */
    br->human_full_bottom = (br->player_count == 3 ||
                             !br->players[1].present ||
                             br->players[1].eliminated);
}

void br_start_first_round(BattleRoyalState* br, uint32_t now_ms) {
    reset_round(br);
    /* Initial cards remain in the deck until the visible deal lands. */
    br->next_round_starter = -1;
    br->phase = BR_PHASE_ROUND_BANNER;
    br->phase_until_ms = UINT32_MAX;
    br->last_tick_ms = now_ms;
    Card none = {0, 0};
    emit_event(br, BR_EVENT_DEAL, -1, -1, -1, none);
}

void br_start_next_round(BattleRoyalState* br, uint32_t now_ms) {
    int starter = br->next_round_starter;
    reset_round(br);
    if (starter < 0 || br->players[starter].eliminated || !br->players[starter].present)
        starter = -1;
    br->next_round_starter = starter;
    br->phase = BR_PHASE_ROUND_BANNER;
    br->phase_until_ms = UINT32_MAX;
    br->last_tick_ms = now_ms;
    Card none = {0, 0};
    emit_event(br, BR_EVENT_DEAL, -1, -1, -1, none);
}

void br_update_clock(BattleRoyalState* br, uint32_t now_ms) {
    if (br->last_tick_ms != 0 && now_ms >= br->last_tick_ms) br->play_time_ms += now_ms - br->last_tick_ms;
    br->last_tick_ms = now_ms;
    if (br->phase == BR_PHASE_ROUND_BANNER && now_ms >= br->phase_until_ms) {
        begin_battle(br, br->next_round_starter, now_ms);
    }
    auto_skip_unplayable_throwers(br, now_ms);
    if (br->auto_resolve_pending && !br->event_pending &&
        (int32_t)(now_ms - br->auto_resolve_at_ms) >= 0) {
        start_resolve_event_now(br);
    }
}

static void note_empty_hand(BattleRoyalState* br, int player_index) {
    BRPlayer* p = &br->players[player_index];
    if (br_cards_left(br) == 0 && p->hand.card_count == 0 && !p->round_out) {
        p->round_out = true;
        p->out_order = ++br->exit_serial;
        if (player_index == 1) br->human_full_bottom = true;
        if (p->human) br->spectator = true;
    }
}

static void start_throw_cycle(BattleRoyalState* br, int start, uint32_t now_ms) {
    br->phase = BR_PHASE_THROW;
    br->throw_cursor = next_thrower(br, start);
    br->decision_player = br->throw_cursor;
    br->consecutive_passes = 0;
    if (br->throw_cursor < 0 || thrower_count(br) <= 0 ||
        br->table_pair_count >= br_attack_limit(br)) {
        schedule_resolve_event(br, now_ms);
        return;
    }
    set_ai_delay(br, now_ms, false);
}

static void schedule_resolve_event(BattleRoyalState* br, uint32_t now_ms) {
    if (!br || br->event_pending || br->auto_resolve_pending) return;
    br->auto_resolve_pending = true;
    br->auto_resolve_at_ms = now_ms + BR_RESOLVE_HOLD_MS;
    br->phase_until_ms = br->auto_resolve_at_ms;
    br->decision_player = -1;
}

static void start_resolve_event_now(BattleRoyalState* br) {
    br->auto_resolve_pending = false;
    br->auto_resolve_at_ms = 0u;
    BREventType t = br->defender_taking ? BR_EVENT_RESOLVE_TAKE : BR_EVENT_RESOLVE_BEAT;
    Card none = {0, 0};
    emit_event(br, t, br->primary_attacker, br->defender, -1, none);
}

static void advance_throw_after_pass(BattleRoyalState* br, uint32_t now_ms) {
    int total = thrower_count(br);
    if (total <= 0 || br->consecutive_passes >= total || br->table_pair_count >= br_attack_limit(br)) {
        schedule_resolve_event(br, now_ms);
        return;
    }
    br->throw_cursor = next_thrower(br, (br->throw_cursor + 1) % BR_MAX_PLAYERS);
    br->decision_player = br->throw_cursor;
    set_ai_delay(br, now_ms, false);
}

/* Do not stop the game on a participant who physically has no legal rank to
 * throw.  A pass caused by an empty legal set counts exactly like pressing B,
 * but happens immediately and without the 1-1.5 s AI thinking delay. */
static void auto_skip_unplayable_throwers(BattleRoyalState* br, uint32_t now_ms) {
    int guard = 0;
    while (!br->event_pending && !br->auto_resolve_pending &&
           br->phase == BR_PHASE_THROW &&
           guard++ < BR_MAX_PLAYERS) {
        int actor = br->decision_player;
        if (actor < 0 || actor >= BR_MAX_PLAYERS ||
            !br_player_in_round(br, actor) || actor == br->defender) {
            br->consecutive_passes++;
            advance_throw_after_pass(br, now_ms);
            continue;
        }
        if (player_has_legal_throw(br, actor)) break;
        br->consecutive_passes++;
        advance_throw_after_pass(br, now_ms);
    }
}

bool br_human_can_act(const BattleRoyalState* br) {
    if (!br || br->event_pending || br->auto_resolve_pending ||
        br->decision_player != 0 ||
        !br_player_in_round(br, 0)) return false;
    /* During THROW the human must always be allowed to press B/DONE, even
     * when there is no legal card to add. */
    return br->phase == BR_PHASE_ATTACK || br->phase == BR_PHASE_DEFEND ||
           br->phase == BR_PHASE_THROW;
}

bool br_human_card_legal(const BattleRoyalState* br, int hand_index) {
    if (!br_human_can_act(br)) return false;
    if (hand_index < 0 || hand_index >= br->players[0].hand.card_count) return false;
    const Card c = br->players[0].hand.hand[hand_index];
    if (br->phase == BR_PHASE_DEFEND) {
        if (br->table_pair_count <= 0) return false;
        return can_defend(br, br->table_attack[br->table_pair_count - 1], c);
    }
    return can_attack_rank(br, c) && br->table_pair_count < br_attack_limit(br);
}

bool br_human_play(BattleRoyalState* br, int hand_index, uint32_t now_ms) {
    if (!br_human_can_act(br)) return false;
    BRPlayer* p = &br->players[0];
    if (hand_index < 0 || hand_index >= p->hand.card_count) return false;
    Card c = p->hand.hand[hand_index];
    if (!br_human_card_legal(br, hand_index)) return false;

    if (br->phase == BR_PHASE_DEFEND) {
        Card attack = br->table_attack[br->table_pair_count - 1];
        if (!can_defend(br, attack, c)) return false;
        const int source_count = p->hand.card_count;
        c = remove_hand_card(p, hand_index);
        const int slot = br->table_pair_count - 1;
        const int attack_owner = br->table_attack_owner[slot] >= 0
                               ? br->table_attack_owner[slot]
                               : br->primary_attacker;
        /* Transactional BR rule: source hand is detached now, destination
         * table is committed only by br_ack_event after the visual landing. */
        emit_card_event(br, BR_EVENT_DEFEND, 0, attack_owner, slot, c,
                        hand_index, source_count);
        return true;
    }

    if (!can_attack_rank(br, c) || br->table_pair_count >= br_attack_limit(br)) return false;
    const int source_count = p->hand.card_count;
    c = remove_hand_card(p, hand_index);
    const int slot = br->table_pair_count;
    emit_card_event(br, BR_EVENT_ATTACK, 0, br->defender, slot, c,
                    hand_index, source_count);
    (void)now_ms;
    return true;
}

bool br_human_pass(BattleRoyalState* br, uint32_t now_ms) {
    if (!br_human_can_act(br) || br->phase != BR_PHASE_THROW) return false;
    br->consecutive_passes++;
    advance_throw_after_pass(br, now_ms);
    return true;
}

bool br_human_take(BattleRoyalState* br, uint32_t now_ms) {
    if (!br_human_can_act(br) || br->phase != BR_PHASE_DEFEND) return false;
    br->defender_taking = true;
    Card none = {0, 0};
    emit_event(br, BR_EVENT_TAKE_DECLARED, 0, br->primary_attacker, -1, none);
    (void)now_ms;
    return true;
}

static bool ai_play_attack(BattleRoyalState* br, int actor, uint32_t now_ms) {
    int idx = br_ai_choose_attack(br, actor);
    if (idx < 0) return false;
    const int source_count = br->players[actor].hand.card_count;
    Card c = remove_hand_card(&br->players[actor], idx);
    const int slot = br->table_pair_count;
    emit_card_event(br, BR_EVENT_ATTACK, actor, br->defender, slot, c,
                    idx, source_count);
    (void)now_ms;
    return true;
}

static bool ai_play_defense(BattleRoyalState* br, int actor, uint32_t now_ms) {
    int idx = br_ai_choose_defense(br, actor);
    if (idx < 0) return false;
    const int source_count = br->players[actor].hand.card_count;
    Card c = remove_hand_card(&br->players[actor], idx);
    const int slot = br->table_pair_count - 1;
    const int attack_owner = br->table_attack_owner[slot] >= 0
                           ? br->table_attack_owner[slot]
                           : br->primary_attacker;
    emit_card_event(br, BR_EVENT_DEFEND, actor, attack_owner, slot, c,
                    idx, source_count);
    (void)now_ms;
    return true;
}

static bool commit_card_event(BattleRoyalState* br, const BREvent* e) {
    if (!br || !e || !valid_card(e->card) || br_card_id(e->card) < 0) return false;
    if (br_card_in_logical_state(br, e->card)) return false;

    if (e->type == BR_EVENT_ATTACK) {
        if (e->table_index != br->table_pair_count ||
            e->table_index < 0 || e->table_index >= BR_MAX_ATTACKS ||
            br->table_pair_count >= br_attack_limit(br)) return false;
        const int slot = e->table_index;
        br->table_attack[slot] = e->card;
        br->table_defense[slot].rank = 0;
        br->table_defense[slot].suit = 0;
        br->table_attack_owner[slot] = (int8_t)e->actor;
        br->table_defense_owner[slot] = -1;
        ++br->table_pair_count;
    } else if (e->type == BR_EVENT_DEFEND) {
        if (e->table_index < 0 || e->table_index >= br->table_pair_count ||
            valid_card(br->table_defense[e->table_index])) return false;
        br->table_defense[e->table_index] = e->card;
        br->table_defense_owner[e->table_index] = (int8_t)e->actor;
    } else {
        return false;
    }

    record_seen(br, e->card);
    note_empty_hand(br, e->actor);
    return true;
}

void br_update_ai(BattleRoyalState* br, uint32_t now_ms) {
    if (br->event_pending || br->auto_resolve_pending ||
        br->phase < BR_PHASE_ATTACK || br->phase > BR_PHASE_THROW) return;
    auto_skip_unplayable_throwers(br, now_ms);
    if (br->event_pending || br->auto_resolve_pending ||
        (br->phase != BR_PHASE_ATTACK && br->phase != BR_PHASE_DEFEND &&
         br->phase != BR_PHASE_THROW)) return;
    int actor = br->decision_player;
    if (actor < 0 || actor >= BR_MAX_PLAYERS || br->players[actor].human) return;
    if (now_ms < br->ai_ready_at_ms) return;

    if (br->phase == BR_PHASE_ATTACK) {
        if (!ai_play_attack(br, actor, now_ms)) {
            /* With an empty deck, a player with no cards has left the round. */
            note_empty_hand(br, actor);
            int next = br_next_active_clockwise(br, actor);
            if (next >= 0) begin_battle(br, next, now_ms);
        }
        return;
    }

    if (br->phase == BR_PHASE_DEFEND) {
        int idx = br_ai_choose_defense(br, actor);
        bool should_take = (idx < 0);
        if (!should_take && br->difficulty == DIFFICULTY_EASY && (duren_rand() % 100) < 35) should_take = true;
        if (!should_take && br->difficulty == DIFFICULTY_NORMAL && (duren_rand() % 100) < 2) should_take = true;
        if (!should_take && br->difficulty == DIFFICULTY_HARD && idx >= 0) {
            Card cover = br->players[actor].hand.hand[idx];
            Card attack = br->table_attack[br->table_pair_count - 1];
            if (attack.suit != br->trump_suit && cover.suit == br->trump_suit &&
                card_rank_strength_u8(cover.rank) >= card_rank_strength_u8(RANK_Q) &&
                br_cards_left(br) > 8 &&
                br->table_pair_count >= 2) should_take = true;
        }
        if (should_take) {
            br->defender_taking = true;
            Card none = {0, 0};
            emit_event(br, BR_EVENT_TAKE_DECLARED, actor, br->primary_attacker, -1, none);
        } else {
            ai_play_defense(br, actor, now_ms);
        }
        return;
    }

    if (br->phase == BR_PHASE_THROW) {
        int idx = br_ai_choose_attack(br, actor);
        if (idx >= 0 && br_ai_should_throw(br, actor, idx)) {
            ai_play_attack(br, actor, now_ms);
        } else {
            br->consecutive_passes++;
            advance_throw_after_pass(br, now_ms);
        }
    }
}

static void draw_after_battle(BattleRoyalState* br) {
    int order[BR_MAX_PLAYERS];
    int n = 0;
    int i = br->primary_attacker;
    for (int step = 0; step < BR_MAX_PLAYERS; ++step) {
        if (br->players[i].present && !br->players[i].eliminated && i != br->defender) order[n++] = i;
        i = (i + 1) % BR_MAX_PLAYERS;
    }
    if (br->players[br->defender].present && !br->players[br->defender].eliminated) order[n++] = br->defender;

    /* Same fairness rule as 1v1: distribute one card per eligible player per
     * pass. No player may consume all remaining deck cards before the next
     * player in draw order gets a chance. */
    bool dealt_any = true;
    while (dealt_any && br->deck.top_index < DECK_SIZE) {
        dealt_any = false;
        for (int k = 0; k < n; ++k) {
            BRPlayer* p = &br->players[order[k]];
            if (p->hand.card_count >= 6 || br->deck.top_index >= DECK_SIZE)
                continue;
            draw_one(br, order[k]);
            dealt_any = true;
        }
    }
}

static int choose_round_loser(BattleRoyalState* br) {
    int remaining = -1;
    int count = 0;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
        if (!br->players[i].present || br->players[i].eliminated) continue;
        if (!br->players[i].round_out) { remaining = i; ++count; }
    }
    if (count == 1) return remaining;
    if (count == 0) {
        int last = -1, order = -1;
        for (int i = 0; i < BR_MAX_PLAYERS; ++i) {
            if (!br->players[i].present || br->players[i].eliminated) continue;
            if (br->players[i].out_order > order) { order = br->players[i].out_order; last = i; }
        }
        return last;
    }
    return -1;
}

static void maybe_finish_round(BattleRoyalState* br) {
    if (br_cards_left(br) != 0) return;
    for (int i = 0; i < BR_MAX_PLAYERS; ++i) note_empty_hand(br, i);
    int loser = choose_round_loser(br);
    if (loser < 0) return;

    br->round_loser = loser;
    br->players[loser].money -= BR_LOSS_PENALTY;
    if (br->players[loser].money <= 0) {
        br->players[loser].money = 0;
        br->players[loser].eliminated = true;
        br->players[loser].round_out = true;
        /* Eliminating a player must not make the cards still in that hand
         * ownerless during the result screen. They become round-discarded
         * atomically; reset_round rebuilds the complete 36-card deck. */
        for (int i = 0; i < br->players[loser].hand.card_count; ++i)
            br_discard_claim(br, br->players[loser].hand.hand[i]);
        br->players[loser].hand.card_count = 0;
        if (loser == 1) br->human_full_bottom = true;
        if (loser == 0) br->spectator = true;
    }
    br->next_round_starter = br_prev_active_clockwise(br, loser);

    int alive = br_active_money_players(br);
    if (alive <= 1) {
        for (int i = 0; i < BR_MAX_PLAYERS; ++i)
            if (br->players[i].present && !br->players[i].eliminated) br->champion = i;
    }
    br->phase = BR_PHASE_ROUND_RESULT;
    Card none = {0, 0};
    emit_event(br, BR_EVENT_ROUND_FINISHED, loser, -1, -1, none);
}

static void resolve_battle(BattleRoyalState* br, bool took, uint32_t now_ms) {
    int old_defender = br->defender;
    if (took) {
        BRPlayer* p = &br->players[old_defender];
        for (int i = 0; i < br->table_pair_count; ++i) {
            if (valid_card(br->table_attack[i]) && p->hand.card_count < MAX_HAND)
                p->hand.hand[p->hand.card_count++] = br->table_attack[i];
            if (valid_card(br->table_defense[i]) && p->hand.card_count < MAX_HAND)
                p->hand.hand[p->hand.card_count++] = br->table_defense[i];
        }
    } else {
        for (int i = 0; i < br->table_pair_count; ++i) {
            br_discard_claim(br, br->table_attack[i]);
            if (valid_card(br->table_defense[i]))
                br_discard_claim(br, br->table_defense[i]);
        }
    }
    clear_table(br);

    int next_attacker = took ? br_next_active_clockwise(br, old_defender) : old_defender;
    if (!took && !br_player_in_round(br, next_attacker))
        next_attacker = br_next_active_clockwise(br, old_defender);
    br->pending_next_attacker = next_attacker;
    br->pending_resolve_took = took;

    /* Deal is its own transaction. Cards stay in the deck while their visual
     * copies fly, then BR_EVENT_DEAL commits the whole group in one step. */
    Card none = {0, 0};
    emit_event(br, BR_EVENT_DEAL, -1, -1, -1, none);
    (void)now_ms;
}

void br_ack_event(BattleRoyalState* br, uint32_t now_ms) {
    if (!br->event_pending) return;
    BREvent e = br->event;
    br->event_pending = false;
    br->event.type = BR_EVENT_NONE;

    switch (e.type) {
        case BR_EVENT_ATTACK:
            if (!commit_card_event(br, &e)) {
                /* Never advance phases from a card that failed its single
                 * atomic destination commit. Return it to its source hand so
                 * the 36-card invariant is preserved instead of duplicating or
                 * losing it. */
                BRPlayer* source = e.actor >= 0 && e.actor < BR_MAX_PLAYERS
                                 ? &br->players[e.actor] : NULL;
                if (source && source->hand.card_count < MAX_HAND)
                    source->hand.hand[source->hand.card_count++] = e.card;
                break;
            }
            if (br->defender_taking) {
                br->consecutive_passes = 0;
                br->throw_cursor = next_thrower(br, (e.actor + 1) % BR_MAX_PLAYERS);
                br->decision_player = br->throw_cursor;
                br->phase = BR_PHASE_THROW;
                if (br->throw_cursor < 0 || thrower_count(br) <= 0 ||
                    br->table_pair_count >= br_attack_limit(br))
                    schedule_resolve_event(br, now_ms);
                else
                    set_ai_delay(br, now_ms, false);
            } else {
                br->phase = BR_PHASE_DEFEND;
                br->decision_player = br->defender;
                set_ai_delay(br, now_ms, true);
            }
            break;
        case BR_EVENT_DEFEND:
            if (!commit_card_event(br, &e)) {
                BRPlayer* source = e.actor >= 0 && e.actor < BR_MAX_PLAYERS
                                 ? &br->players[e.actor] : NULL;
                if (source && source->hand.card_count < MAX_HAND)
                    source->hand.hand[source->hand.card_count++] = e.card;
                break;
            }
            start_throw_cycle(br, br->primary_attacker, now_ms);
            break;
        case BR_EVENT_TAKE_DECLARED:
            start_throw_cycle(br, br->primary_attacker, now_ms);
            break;
        case BR_EVENT_RESOLVE_BEAT:
            resolve_battle(br, false, now_ms);
            break;
        case BR_EVENT_RESOLVE_TAKE:
            resolve_battle(br, true, now_ms);
            break;
        case BR_EVENT_DEAL: {
            if (br->phase == BR_PHASE_ROUND_BANNER) {
                deal_initial(br);
                if (br->next_round_starter < 0 ||
                    br->players[br->next_round_starter].eliminated ||
                    !br->players[br->next_round_starter].present)
                    br->next_round_starter = find_lowest_trump_holder(br);
                /* The battle timer starts after every initial-deal card has
                 * visibly landed, never underneath the animation. */
                br->phase_until_ms = now_ms + 1875u;
                break;
            }
            const int next_attacker = br->pending_next_attacker;
            draw_after_battle(br);
            br->pending_next_attacker = -1;
            br->pending_resolve_took = false;
            maybe_finish_round(br);
            if (br->phase == BR_PHASE_ROUND_RESULT) break;
            if (next_attacker >= 0) begin_battle(br, next_attacker, now_ms);
            else maybe_finish_round(br);
            break;
        }
        case BR_EVENT_ROUND_FINISHED:
            br->phase = BR_PHASE_ROUND_RESULT;
            break;
        default:
            break;
    }
}

void br_confirm_round_result(BattleRoyalState* br) {
    if (br->phase == BR_PHASE_ROUND_RESULT && !br->event_pending) br->phase = BR_PHASE_NEXT_PROMPT;
}

void br_confirm_next_prompt(BattleRoyalState* br, uint32_t now_ms) {
    if (br->phase != BR_PHASE_NEXT_PROMPT) return;
    if (br->champion >= 0 || br_active_money_players(br) <= 1) {
        br->phase = BR_PHASE_CHAMPION;
        return;
    }
    br->round_number++;
    br_start_next_round(br, now_ms);
}
