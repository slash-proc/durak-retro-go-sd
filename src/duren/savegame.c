#include "savegame.h"
#include <stdio.h>
#include <string.h>

#ifdef PLATFORM_RETRO_GO
#include "rg_storage.h"
#endif

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_size;
    uint32_t checksum;
    uint32_t mode;
    uint32_t play_time_ms;
} SaveHeader;

#ifdef PLATFORM_RETRO_GO
static const char* const slot_paths[SAVEGAME_SLOT_COUNT] = {
    "/saves/duren_slot1.sav", "/saves/duren_slot2.sav", "/saves/duren_slot3.sav"
};
static const char* const legacy_slot_paths[SAVEGAME_SLOT_COUNT] = {
    "/saves/durak_slot1.sav", "/saves/durak_slot2.sav", "/saves/durak_slot3.sav"
};
static const char* const relative_duren_paths[SAVEGAME_SLOT_COUNT] = {
    "duren_slot1.sav", "duren_slot2.sav", "duren_slot3.sav"
};
static const char* const relative_durak_paths[SAVEGAME_SLOT_COUNT] = {
    "durak_slot1.sav", "durak_slot2.sav", "durak_slot3.sav"
};
#else
static const char* const slot_paths[SAVEGAME_SLOT_COUNT] = {
    "duren_slot1.sav", "duren_slot2.sav", "duren_slot3.sav"
};
static const char* const legacy_slot_paths[SAVEGAME_SLOT_COUNT] = {
    "durak_slot1.sav", "durak_slot2.sav", "durak_slot3.sav"
};
#endif

static int g_last_error = SAVEGAME_ERROR_NONE;
int savegame_last_error(void) { return g_last_error; }
static void set_error(int error) { g_last_error = error; }

const char* savegame_slot_path(int slot) {
    if (slot < 0 || slot >= SAVEGAME_SLOT_COUNT) return NULL;
    return slot_paths[slot];
}

static uint32_t checksum_bytes(const void* data, size_t size) {
    const unsigned char* p = (const unsigned char*)data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool payload_shape_valid(const SaveGamePayload* payload) {
    return payload && payload->mode_session.mode >= MODE_CAREER &&
           payload->mode_session.mode <= MODE_BATTLE_ROYAL &&
           (payload->game.deck_mode == DECK_MODE_36 ||
            payload->game.deck_mode == DECK_MODE_52) &&
           (payload->battle_royal.deck_mode == 0u ||
            payload->battle_royal.deck_mode == DECK_MODE_36 ||
            payload->battle_royal.deck_mode == DECK_MODE_52);
}

static bool standard_human_turn_ready(const GameState* game) {
    if (!game || game->result != RESULT_NONE) return false;
    if (game->players[0].card_count <= 0) return false;
    if (game->table_pair_count < 0 || game->table_pair_count > MAX_TABLE) return false;
    const bool need_defense = game->table_pair_count > 0 &&
        game->table_defense[game->table_pair_count - 1].rank < RANK_6;
    return (game->is_player_turn && !need_defense) ||
           (!game->is_player_turn && need_defense);
}

bool savegame_human_turn_ready(GameMode mode, const GameState* game,
                               const BattleRoyalState* battle_royal) {
    if (mode == MODE_BATTLE_ROYAL) {
        if (!battle_royal || battle_royal->event_pending ||
            battle_royal->auto_resolve_pending ||
            battle_royal->decision_player != 0) return false;
        if (battle_royal->phase != BR_PHASE_ATTACK &&
            battle_royal->phase != BR_PHASE_DEFEND &&
            battle_royal->phase != BR_PHASE_THROW) return false;
        const BRPlayer* human = &battle_royal->players[0];
        return human->present && human->human && !human->eliminated &&
               !human->round_out && human->hand.card_count > 0;
    }
    if (mode < MODE_CAREER || mode > MODE_RANDOM_BATTLE) return false;
    return standard_human_turn_ready(game);
}

bool savegame_payload_human_turn_ready(const SaveGamePayload* payload) {
    if (!payload) return false;
    return savegame_human_turn_ready(payload->mode_session.mode,
                                     &payload->game, &payload->battle_royal);
}

static bool read_payload_file(const char* path, SaveGamePayload* payload,
                              SaveHeader* header_out) {
    FILE* f = fopen(path, "rb");
    if (!f) { set_error(SAVEGAME_ERROR_OPEN_READ); return false; }

    SaveHeader h;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h);
    if (!ok) set_error(SAVEGAME_ERROR_READ_HEADER);
    if (ok && (h.magic != SAVEGAME_MAGIC || h.version != SAVEGAME_VERSION ||
               h.payload_size != sizeof(*payload) || h.mode > MODE_BATTLE_ROYAL)) {
        ok = false;
        set_error(SAVEGAME_ERROR_INCOMPATIBLE);
    }
    if (ok && fread(payload, 1, sizeof(*payload), f) != sizeof(*payload)) {
        ok = false;
        set_error(SAVEGAME_ERROR_READ_PAYLOAD);
    }
    if (fclose(f) != 0 && ok) { ok = false; set_error(SAVEGAME_ERROR_CLOSE); }
    if (!ok) return false;
    if (checksum_bytes(payload, sizeof(*payload)) != h.checksum) {
        set_error(SAVEGAME_ERROR_CHECKSUM);
        return false;
    }
    if (!payload_shape_valid(payload)) {
        set_error(SAVEGAME_ERROR_BAD_PAYLOAD);
        return false;
    }
    if (header_out) *header_out = h;
    set_error(SAVEGAME_ERROR_NONE);
    return true;
}

bool savegame_write_path(const char* path, const SaveGamePayload* payload) {
    set_error(SAVEGAME_ERROR_NONE);
    if (!path || !payload) { set_error(SAVEGAME_ERROR_BAD_ARGUMENT); return false; }
    if (!payload_shape_valid(payload)) { set_error(SAVEGAME_ERROR_BAD_PAYLOAD); return false; }

    SaveHeader h;
    h.magic = SAVEGAME_MAGIC;
    h.version = SAVEGAME_VERSION;
    h.payload_size = (uint32_t)sizeof(*payload);
    h.checksum = checksum_bytes(payload, sizeof(*payload));
    h.mode = (uint32_t)payload->mode_session.mode;
    h.play_time_ms = payload->play_time_ms;

    FILE* f = fopen(path, "wb");
    if (!f) { set_error(SAVEGAME_ERROR_OPEN_WRITE); return false; }

    bool ok = true;
    if (fwrite(&h, 1, sizeof(h), f) != sizeof(h)) {
        ok = false; set_error(SAVEGAME_ERROR_WRITE_HEADER);
    }
    if (ok && fwrite(payload, 1, sizeof(*payload), f) != sizeof(*payload)) {
        ok = false; set_error(SAVEGAME_ERROR_WRITE_PAYLOAD);
    }
    /* Do not call fflush(FILE*) here. This Retro-Go tree wraps fflush with
     * an fd-style function, so an explicit stdio fflush returns EBADF/EIO.
     * All working cores use fwrite() followed by fclose(); fclose performs
     * the required buffered write and FatFs close/sync. */
    if (fclose(f) != 0 && ok) { ok = false; set_error(SAVEGAME_ERROR_CLOSE); }
    if (!ok) { remove(path); return false; }

    SaveGamePayload verify;
    SaveHeader verify_header;
    if (!read_payload_file(path, &verify, &verify_header)) {
        if (g_last_error == SAVEGAME_ERROR_OPEN_READ) set_error(SAVEGAME_ERROR_VERIFY_OPEN);
        return false;
    }
    if (verify_header.checksum != h.checksum ||
        memcmp(&verify, payload, sizeof(verify)) != 0) {
        set_error(SAVEGAME_ERROR_VERIFY_MISMATCH);
        return false;
    }
    set_error(SAVEGAME_ERROR_NONE);
    return true;
}

bool savegame_read_path(const char* path, SaveGamePayload* payload) {
    set_error(SAVEGAME_ERROR_NONE);
    if (!path || !payload) { set_error(SAVEGAME_ERROR_BAD_ARGUMENT); return false; }
    return read_payload_file(path, payload, NULL);
}

bool savegame_write_slot(int slot, const SaveGamePayload* payload) {
    const char* path = savegame_slot_path(slot);
    if (!path) { set_error(SAVEGAME_ERROR_BAD_ARGUMENT); return false; }
#ifdef PLATFORM_RETRO_GO
    /* Existing directory is not an error. Retro-Go does the same. */
    (void)rg_storage_mkdir("/saves");
#endif
    if (!savegame_write_path(path, payload)) return false;
#ifdef PLATFORM_RETRO_GO
    rg_storage_commit();
#endif
    return true;
}

static bool read_slot_candidate(const char* path, SaveGamePayload* payload) {
    return path && savegame_read_path(path, payload);
}

bool savegame_read_slot(int slot, SaveGamePayload* payload) {
    if (slot < 0 || slot >= SAVEGAME_SLOT_COUNT || !payload) {
        set_error(SAVEGAME_ERROR_BAD_ARGUMENT);
        return false;
    }
    if (read_slot_candidate(slot_paths[slot], payload)) return true;
    if (read_slot_candidate(legacy_slot_paths[slot], payload)) return true;
#ifdef PLATFORM_RETRO_GO
    if (read_slot_candidate(relative_duren_paths[slot], payload)) return true;
    if (read_slot_candidate(relative_durak_paths[slot], payload)) return true;
#endif
    return false;
}

static FILE* open_meta_candidate(int slot, const char** selected_path) {
    const char* candidates[4];
    int count = 0;
    candidates[count++] = slot_paths[slot];
    candidates[count++] = legacy_slot_paths[slot];
#ifdef PLATFORM_RETRO_GO
    candidates[count++] = relative_duren_paths[slot];
    candidates[count++] = relative_durak_paths[slot];
#endif
    for (int i = 0; i < count; ++i) {
        FILE* f = fopen(candidates[i], "rb");
        if (f) {
            if (selected_path) *selected_path = candidates[i];
            return f;
        }
    }
    return NULL;
}

SaveGameMeta savegame_read_meta(int slot) {
    SaveGameMeta m;
    memset(&m, 0, sizeof(m));
    if (slot < 0 || slot >= SAVEGAME_SLOT_COUNT) return m;

    const char* selected_path = NULL;
    FILE* f = open_meta_candidate(slot, &selected_path);
    if (!f) return m;
    m.exists = true;

    SaveHeader h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) {
        m.corrupted = true;
        fclose(f);
        return m;
    }
    m.mode = (GameMode)h.mode;
    m.play_time_ms = h.play_time_ms;
    m.compatible = h.magic == SAVEGAME_MAGIC && h.version == SAVEGAME_VERSION &&
                   h.payload_size == sizeof(SaveGamePayload) && h.mode <= MODE_BATTLE_ROYAL;
    fclose(f);
    if (!m.compatible) {
        m.corrupted = h.magic != SAVEGAME_MAGIC;
        return m;
    }

    SaveGamePayload payload;
    if (!savegame_read_path(selected_path, &payload)) {
        m.compatible = false;
        m.corrupted = savegame_last_error() == SAVEGAME_ERROR_CHECKSUM ||
                      savegame_last_error() == SAVEGAME_ERROR_READ_HEADER ||
                      savegame_last_error() == SAVEGAME_ERROR_READ_PAYLOAD;
    }
    return m;
}
