#ifndef MODES_H
#define MODES_H

#include "game.h"
#include "tournament.h"

typedef enum {
    MODE_CAREER = 0,
    MODE_TOURNAMENT = 1,
    MODE_RANDOM_BATTLE = 2,
    MODE_BATTLE_ROYAL = 3
} GameMode;

typedef enum {
    DIFFICULTY_EASY = 0,
    DIFFICULTY_NORMAL = 1,
    DIFFICULTY_HARD = 2
} Difficulty;

typedef enum {
    MODE_ACTION_NONE = 0,
    MODE_ACTION_NEXT_MATCH,
    MODE_ACTION_REPLAY_MATCH,
    MODE_ACTION_SHOW_BRACKET,
    MODE_ACTION_TOURNAMENT_OVER,
    MODE_ACTION_TOURNAMENT_CHAMPION,
    MODE_ACTION_RETURN_SPLASH,
    MODE_ACTION_CAREER_GAME_OVER,
    MODE_ACTION_CAREER_COMPLETE,
    MODE_ACTION_SHOW_CAREER_MAP
} ModeAction;

typedef struct {
    GameMode mode;
    Difficulty difficulty;
    int player_face;
    int random_face;
    int random_level;
    uint8_t current_location;            /* 1..20 */
    uint8_t highest_defeated_opponent;   /* 0..20 */
    bool next_unlocked;                  /* route current -> current+1 */
    bool career_complete;
} ModeSession;

void modes_init(ModeSession* s);
void modes_begin(ModeSession* s, GameMode mode, int player_face, Difficulty difficulty,
                 GameState* game, TournamentState* tournament);
bool modes_prepare_match(ModeSession* s, GameState* game, TournamentState* tournament);
ModeAction modes_process_result(ModeSession* s, GameState* game, TournamentState* tournament);
const char* modes_mode_name(GameMode mode);
const char* modes_difficulty_name(Difficulty difficulty);
int modes_effective_ai_level(int base_level, Difficulty difficulty);
void modes_career_arrive_next(ModeSession* s, GameState* game);

#endif
