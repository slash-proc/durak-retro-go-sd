#include "modes.h"
#include <stdlib.h>
#include "rng.h"

static int clamp_level(int level) {
    if (level < 1) return 1;
    if (level > 20) return 20;
    return level;
}

void modes_init(ModeSession* s) {
    s->mode = MODE_CAREER;
    s->difficulty = DIFFICULTY_NORMAL;
    s->player_face = 0;
    s->random_face = 1;
    s->random_level = 1;
    s->current_location = 1u;
    s->highest_defeated_opponent = 0u;
    s->next_unlocked = false;
    s->career_complete = false;
}

const char* modes_mode_name(GameMode mode) {
    switch (mode) {
        case MODE_CAREER: return "CAREER";
        case MODE_TOURNAMENT: return "TOURNAMENT";
        case MODE_RANDOM_BATTLE: return "ONLINE";
        case MODE_BATTLE_ROYAL: return "BATTLE ROYAL";
        default: return "MODE";
    }
}

const char* modes_difficulty_name(Difficulty difficulty) {
    switch (difficulty) {
        case DIFFICULTY_EASY: return "EASY";
        case DIFFICULTY_HARD: return "HARD";
        case DIFFICULTY_NORMAL:
        default: return "NORMAL";
    }
}

int modes_effective_ai_level(int base_level, Difficulty difficulty) {
    int level = base_level;
    /* Easy must remain visibly forgiving even against late Career opponents.
     * The behavior profile below adds human-like mistakes; this larger level
     * offset also reduces the precision of all score-based choices. */
    if (difficulty == DIFFICULTY_EASY) level -= 8;
    else if (difficulty == DIFFICULTY_HARD) level += 4;
    return clamp_level(level);
}


static int random_opponent_face(int player_face) {
    int face = duren_rand() % 20;
    if (face == player_face) face = (face + 1 + (duren_rand() % 19)) % 20;
    return face;
}

void modes_begin(ModeSession* s, GameMode mode, int player_face, Difficulty difficulty,
                 GameState* game, TournamentState* tournament) {
    s->mode = mode;
    s->difficulty = difficulty;
    s->player_face = player_face;

    if (mode == MODE_CAREER) {
        game_init_global(game);
        s->current_location = 1u;
        s->highest_defeated_opponent = 0u;
        s->next_unlocked = false;
        s->career_complete = false;
    } else if (mode == MODE_TOURNAMENT) {
        game_init_global(game);
        tournament_init(tournament, player_face);
    } else if (mode == MODE_RANDOM_BATTLE) {
        game_init_global(game);
        s->random_face = random_opponent_face(player_face);
        s->random_level = 1 + (duren_rand() % 20);
    } else {
        game_init_global(game);
    }
}

bool modes_prepare_match(ModeSession* s, GameState* game, TournamentState* tournament) {
    int base_level = 1;
    int face = 0;

    if (s->mode == MODE_BATTLE_ROYAL) return false;

    if (s->mode == MODE_CAREER) {
        base_level = clamp_level((int)s->current_location);
        face = (base_level - 1) % 20;
    } else if (s->mode == MODE_TOURNAMENT) {
        int opponent = tournament_get_player_opponent(tournament);
        if (opponent < 0) return false;
        TournamentPlayer* p = &tournament->players[opponent];
        base_level = clamp_level(p->level);
        face = p->face_idx;
    } else {
        base_level = clamp_level(s->random_level);
        face = s->random_face;
    }

    /* Every mode now respects the opponent's own level. Difficulty shifts
     * that level instead of replacing every opponent with fixed 4/10/18 AI. */
    int effective_level = modes_effective_ai_level(base_level, s->difficulty);
    game_set_opponent_face(game, face, effective_level);
    game->current_opponent.difficulty_profile = (uint8_t)s->difficulty;
    /* Keep progression/HUD level separate from effective AI strength. */
    game->current_level = base_level;

    game_init(game);
    game_shuffle_deck(game);
    game_deal_cards(game);
    return true;
}

ModeAction modes_process_result(ModeSession* s, GameState* game, TournamentState* tournament) {
    if (!s || !game || game->result == RESULT_NONE) return MODE_ACTION_NONE;

    if (s->mode == MODE_CAREER) {
        if (game->result == RESULT_LOSE) {
            /* Match the Android Career economy: charge one $50 loss penalty,
             * exactly once when the result is processed, and never underflow. */
            game->player_money -= 50;
            if (game->player_money < 0) game->player_money = 0;
        } else if (game->result == RESULT_WIN) {
            game->player_money += 100;
            if (s->current_location > s->highest_defeated_opponent)
                s->highest_defeated_opponent = s->current_location;
            if (s->current_location >= 20u) {
                s->next_unlocked = false;
                s->career_complete = true;
            } else {
                s->next_unlocked = true;
            }
        }
        /* Win, loss and draw all return to the map. Only a win mutates the
         * progression fields above; no next opponent starts automatically. */
        return MODE_ACTION_SHOW_CAREER_MAP;
    }

    if (s->mode == MODE_TOURNAMENT) {
        if (game->result == RESULT_DRAW) return MODE_ACTION_REPLAY_MATCH;
        if (game->result == RESULT_LOSE) {
            tournament_player_lose(tournament);
            return MODE_ACTION_TOURNAMENT_OVER;
        }

        tournament_player_win(tournament);
        if (tournament_is_champion(tournament)) return MODE_ACTION_TOURNAMENT_CHAMPION;
        return MODE_ACTION_SHOW_BRACKET;
    }

    return MODE_ACTION_RETURN_SPLASH;
}

void modes_career_arrive_next(ModeSession* s, GameState* game) {
    if (!s || s->mode != MODE_CAREER || !s->next_unlocked ||
        s->current_location >= 20u) return;
    ++s->current_location;
    s->next_unlocked = false;
    if (game) game->current_level = (int)s->current_location;
}
