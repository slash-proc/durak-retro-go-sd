#ifndef TOURNAMENT_H
#define TOURNAMENT_H

#include <stdbool.h>

#define TOURNAMENT_PLAYERS 8
#define TOURNAMENT_QF_MATCHES 4
#define TOURNAMENT_SF_MATCHES 2

typedef struct {
    int face_idx;
    int level;
    bool is_player;
    char name[16];
} TournamentPlayer;

typedef struct {
    TournamentPlayer players[TOURNAMENT_PLAYERS];
    int player_index;
    int player_slot;
    int round; /* 0=quarterfinal, 1=semifinal, 2=final, 3=champion */
    int qf_winner[TOURNAMENT_QF_MATCHES];
    int sf_winner[TOURNAMENT_SF_MATCHES];
    int champion;
    bool eliminated;
} TournamentState;

void tournament_init(TournamentState* t, int player_face);
int tournament_get_player_opponent(const TournamentState* t);
void tournament_player_win(TournamentState* t);
void tournament_player_lose(TournamentState* t);
bool tournament_is_champion(const TournamentState* t);
const char* tournament_round_name(const TournamentState* t);

#endif
