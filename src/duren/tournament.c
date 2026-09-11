#include "tournament.h"
#include "game.h"
#include <stdlib.h>
#include <string.h>
#include "rng.h"

static int weighted_winner(const TournamentState* t, int a, int b) {
    if (a < 0) return b;
    if (b < 0) return a;

    int diff = t->players[a].level - t->players[b].level;
    int chance_a = 50 + diff * 2;
    if (chance_a < 20) chance_a = 20;
    if (chance_a > 80) chance_a = 80;
    return ((duren_rand() % 100) < chance_a) ? a : b;
}

static void copy_name(char dst[16], const char* src) {
    if (!src) src = "AI";
    strncpy(dst, src, 15);
    dst[15] = '\0';
}

void tournament_init(TournamentState* t, int player_face) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < TOURNAMENT_QF_MATCHES; i++) t->qf_winner[i] = -1;
    for (int i = 0; i < TOURNAMENT_SF_MATCHES; i++) t->sf_winner[i] = -1;
    t->champion = -1;
    t->round = 0;
    t->eliminated = false;

    int used_face[20] = {0};
    if (player_face < 0) player_face = 0;
    if (player_face > 19) player_face = 19;

    t->player_slot = duren_rand() % TOURNAMENT_PLAYERS;
    t->player_index = t->player_slot;

    for (int slot = 0; slot < TOURNAMENT_PLAYERS; slot++) {
        TournamentPlayer* p = &t->players[slot];
        p->is_player = (slot == t->player_slot);
        if (p->is_player) {
            p->face_idx = player_face;
            p->level = 10;
            copy_name(p->name, "YOU");
            used_face[player_face] = 1;
        } else {
            int face;
            do { face = duren_rand() % 20; } while (used_face[face]);
            used_face[face] = 1;
            p->face_idx = face;
            p->level = 3 + (duren_rand() % 18); /* 3..20 */
            copy_name(p->name, game_get_ai_name(face));
        }
    }
}

int tournament_get_player_opponent(const TournamentState* t) {
    if (!t || t->eliminated || t->round >= 3) return -1;
    int player = t->player_index;

    if (t->round == 0) {
        int pair_base = (t->player_slot / 2) * 2;
        return (pair_base == player) ? pair_base + 1 : pair_base;
    }

    if (t->round == 1) {
        int qf_match = t->player_slot / 2;
        int sf_group = qf_match / 2;
        int a = t->qf_winner[sf_group * 2];
        int b = t->qf_winner[sf_group * 2 + 1];
        return (a == player) ? b : a;
    }

    {
        int a = t->sf_winner[0];
        int b = t->sf_winner[1];
        return (a == player) ? b : a;
    }
}

void tournament_player_win(TournamentState* t) {
    if (!t || t->eliminated || t->round >= 3) return;
    int player = t->player_index;

    if (t->round == 0) {
        int player_match = t->player_slot / 2;
        t->qf_winner[player_match] = player;

        for (int m = 0; m < TOURNAMENT_QF_MATCHES; m++) {
            if (m == player_match) continue;
            t->qf_winner[m] = weighted_winner(t, m * 2, m * 2 + 1);
        }
        t->round = 1;
        return;
    }

    if (t->round == 1) {
        int player_qf = t->player_slot / 2;
        int player_sf = player_qf / 2;
        t->sf_winner[player_sf] = player;

        int other_sf = 1 - player_sf;
        int a = t->qf_winner[other_sf * 2];
        int b = t->qf_winner[other_sf * 2 + 1];
        t->sf_winner[other_sf] = weighted_winner(t, a, b);
        t->round = 2;
        return;
    }

    t->champion = player;
    t->round = 3;
}

void tournament_player_lose(TournamentState* t) {
    if (!t) return;
    t->eliminated = true;
}

bool tournament_is_champion(const TournamentState* t) {
    return t && t->round == 3 && t->champion == t->player_index;
}

const char* tournament_round_name(const TournamentState* t) {
    if (!t) return "TOURNAMENT";
    if (t->round == 0) return "QUARTERFINAL";
    if (t->round == 1) return "SEMIFINAL";
    if (t->round == 2) return "FINAL";
    return "CHAMPION";
}
