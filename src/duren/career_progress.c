#include "career_progress.h"
#include <stdio.h>
#include <string.h>

#define CAREER_PROGRESS_MAGIC 0x43525047u /* CRPG */
#define CAREER_PROGRESS_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t sequence;
    uint8_t current_location;
    uint8_t highest_defeated_opponent;
    uint8_t next_unlocked;
    uint8_t career_complete;
    int32_t player_money;
    uint32_t checksum;
} CareerProgressFile;

static const char* const k_progress_paths[2] = {
    "duren_career_a.dat", "duren_career_b.dat"
};
static uint32_t g_progress_sequence = 0u;

static uint32_t progress_checksum(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static bool progress_shape_valid(const CareerProgressFile* p) {
    if (!p || p->magic != CAREER_PROGRESS_MAGIC ||
        p->version != CAREER_PROGRESS_VERSION ||
        p->current_location < 1u || p->current_location > 20u ||
        p->highest_defeated_opponent > 20u ||
        p->next_unlocked > 1u || p->career_complete > 1u ||
        p->player_money < 0) return false;
    if (p->current_location > p->highest_defeated_opponent + 1u) return false;
    if (p->next_unlocked &&
        (p->current_location >= 20u ||
         p->highest_defeated_opponent < p->current_location)) return false;
    if (p->career_complete &&
        (p->current_location != 20u || p->highest_defeated_opponent != 20u))
        return false;
    return p->checksum == progress_checksum(
        p, sizeof(*p) - sizeof(p->checksum));
}

static bool progress_read(const char* path, CareerProgressFile* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    CareerProgressFile p;
    const bool exact = fread(&p, 1, sizeof(p), f) == sizeof(p) &&
                       fgetc(f) == EOF && !ferror(f);
    const bool closed = fclose(f) == 0;
    if (!exact || !closed || !progress_shape_valid(&p)) return false;
    if (out) *out = p;
    return true;
}

bool career_progress_load(ModeSession* session, GameState* game) {
    if (!session || !game) return false;
    CareerProgressFile a, b;
    const bool have_a = progress_read(k_progress_paths[0], &a);
    const bool have_b = progress_read(k_progress_paths[1], &b);
    if (!have_a && !have_b) return false;
    const CareerProgressFile* best = have_a && have_b
        ? (a.sequence >= b.sequence ? &a : &b)
        : (have_a ? &a : &b);
    session->current_location = best->current_location;
    session->highest_defeated_opponent = best->highest_defeated_opponent;
    session->next_unlocked = best->next_unlocked != 0u;
    session->career_complete = best->career_complete != 0u;
    game->current_level = (int)best->current_location;
    game->player_money = (int)best->player_money;
    g_progress_sequence = best->sequence;
    return true;
}

bool career_progress_save(const ModeSession* session, const GameState* game) {
    if (!session || !game || session->mode != MODE_CAREER) return false;
    CareerProgressFile p;
    memset(&p, 0, sizeof(p));
    p.magic = CAREER_PROGRESS_MAGIC;
    p.version = CAREER_PROGRESS_VERSION;
    p.sequence = ++g_progress_sequence;
    p.current_location = session->current_location;
    p.highest_defeated_opponent = session->highest_defeated_opponent;
    p.next_unlocked = session->next_unlocked ? 1u : 0u;
    p.career_complete = session->career_complete ? 1u : 0u;
    p.player_money = game->player_money;
    p.checksum = progress_checksum(&p, sizeof(p) - sizeof(p.checksum));
    if (!progress_shape_valid(&p)) return false;

    /* Two alternating journal files make a power loss recoverable without
     * depending on rename()/directory semantics in the Retro-Go FAT wrapper. */
    const char* path = k_progress_paths[p.sequence & 1u];
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(&p, 1, sizeof(p), f) == sizeof(p);
    if (fclose(f) != 0) ok = false;
    CareerProgressFile verify;
    if (ok) ok = progress_read(path, &verify) &&
                 memcmp(&p, &verify, sizeof(p)) == 0;
    return ok;
}
