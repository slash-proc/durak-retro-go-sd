#ifndef SAVEGAME_H
#define SAVEGAME_H

#include "game.h"
#include "modes.h"
#include "tournament.h"
#include "battle_royal.h"
#include <stdbool.h>
#include <stdint.h>

#define SAVEGAME_SLOT_COUNT 3
#define SAVEGAME_MAGIC 0x4455524Bu /* DURK */
#define SAVEGAME_VERSION 9u

typedef struct {
    GameState game;
    ModeSession mode_session;
    TournamentState tournament;
    BattleRoyalState battle_royal;
    int selected_difficulty;
    int selected_mode;
    int selected_face;
    int selected_br_players;
    int selected_card_idx;
    int pending_action;
    bool result_processed;
    int last_result_sfx;
    uint32_t play_time_ms;
    uint32_t rng_state;
} SaveGamePayload;


typedef enum {
    SAVEGAME_ERROR_NONE = 0,
    SAVEGAME_ERROR_BAD_ARGUMENT = 1,
    SAVEGAME_ERROR_BAD_PAYLOAD = 2,
    SAVEGAME_ERROR_OPEN_WRITE = 3,
    SAVEGAME_ERROR_WRITE_HEADER = 4,
    SAVEGAME_ERROR_WRITE_PAYLOAD = 5,
    SAVEGAME_ERROR_FLUSH = 6,
    SAVEGAME_ERROR_CLOSE = 7,
    SAVEGAME_ERROR_VERIFY_OPEN = 8,
    SAVEGAME_ERROR_VERIFY_MISMATCH = 9,
    SAVEGAME_ERROR_OPEN_READ = 10,
    SAVEGAME_ERROR_READ_HEADER = 11,
    SAVEGAME_ERROR_INCOMPATIBLE = 12,
    SAVEGAME_ERROR_READ_PAYLOAD = 13,
    SAVEGAME_ERROR_CHECKSUM = 14
} SaveGameError;

typedef struct {
    bool exists;
    bool compatible;
    bool corrupted;
    GameMode mode;
    uint32_t play_time_ms;
} SaveGameMeta;

bool savegame_human_turn_ready(GameMode mode, const GameState* game,
                               const BattleRoyalState* battle_royal);
bool savegame_payload_human_turn_ready(const SaveGamePayload* payload);
bool savegame_write_slot(int slot, const SaveGamePayload* payload);
bool savegame_read_slot(int slot, SaveGamePayload* payload);
SaveGameMeta savegame_read_meta(int slot);
bool savegame_write_path(const char* path, const SaveGamePayload* payload);
bool savegame_read_path(const char* path, SaveGamePayload* payload);
const char* savegame_slot_path(int slot);
int savegame_last_error(void);

#endif
