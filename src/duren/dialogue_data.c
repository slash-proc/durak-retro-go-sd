#include "dialogue_data.h"
#include "duren_pak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIALOGUE_TOTAL_LINES \
    (DIALOGUE_CHARACTER_COUNT * DIALOGUE_TRIGGER_COUNT * DIALOGUE_VARIANTS_PER_TRIGGER)
#define DIALOGUE_PACK_VERSION 3u
#define DIALOGUE_PACK_HEADER_SIZE 12u
#define DIALOGUE_PACK_ENTRY_SIZE 16u
#define DIALOGUE_ACTIVE_BUFFER_SIZE 49152u
#define DIALOGUE_RAW_HEADER_BYTES 8u
#define DIALOGUE_UI_OFFSETS_BYTES (DIALOGUE_UI_STRING_COUNT * 2u)
#define DIALOGUE_LINE_OFFSETS_BYTES (DIALOGUE_TOTAL_LINES * 2u)
#define DIALOGUE_UI_OFFSETS_POS DIALOGUE_RAW_HEADER_BYTES
#define DIALOGUE_LINE_OFFSETS_POS \
    (DIALOGUE_UI_OFFSETS_POS + DIALOGUE_UI_OFFSETS_BYTES)
#define DIALOGUE_BLOBS_POS \
    (DIALOGUE_LINE_OFFSETS_POS + DIALOGUE_LINE_OFFSETS_BYTES)

static unsigned char g_dialogue_active[DIALOGUE_ACTIVE_BUFFER_SIZE];
static uint32_t g_dialogue_active_size;
static int g_dialogue_loaded_language = -1;
static const char g_dialogue_fallback[] = "...";

static uint16_t read_u16_le(const unsigned char* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const unsigned char* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t fnv1a32(const unsigned char* data, uint32_t size) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Small raw-LZ4 block decoder. The pack is generated with store_size=False,
 * so no frame parser, heap dictionary, or streaming state is needed. */
static uint32_t dialogue_lz4_depack(const unsigned char* src, uint32_t packed_size,
                                    unsigned char* dst, uint32_t dst_capacity) {
    uint32_t in = 0;
    uint32_t out = 0;

    while (in < packed_size) {
        uint32_t token = src[in++];
        uint32_t literal_len = token >> 4;
        uint32_t match_len = (token & 0x0Fu) + 4u;

        if (literal_len == 15u) {
            unsigned int extra;
            do {
                if (in >= packed_size) return 0;
                extra = src[in++];
                literal_len += extra;
            } while (extra == 255u);
        }

        if (literal_len > packed_size - in || literal_len > dst_capacity - out)
            return 0;
        memcpy(dst + out, src + in, literal_len);
        in += literal_len;
        out += literal_len;

        if (in == packed_size) break;
        if (packed_size - in < 2u) return 0;

        uint32_t offset = (uint32_t)src[in] | ((uint32_t)src[in + 1] << 8);
        in += 2;
        if (offset == 0u || offset > out) return 0;

        if (match_len == 19u) {
            unsigned int extra;
            do {
                if (in >= packed_size) return 0;
                extra = src[in++];
                match_len += extra;
            } while (extra == 255u);
        }

        if (match_len > dst_capacity - out) return 0;
        for (uint32_t i = 0; i < match_len; ++i) {
            dst[out] = dst[out - offset];
            ++out;
        }
    }

    return out;
}

static FILE* dialogue_open_pack(uint32_t* out_base) {
    FILE* f = NULL;
    uint32_t base = 0;
    if (!duren_pak_open_member(DUREN_PAK_MEMBER_LANG, &f, &base))
        return NULL;
    if (out_base) *out_base = base;
    return f;
}

static bool dialogue_validate_active(uint32_t unpacked_size) {
    if (unpacked_size <= DIALOGUE_BLOBS_POS ||
        unpacked_size > DIALOGUE_ACTIVE_BUFFER_SIZE)
        return false;

    const uint16_t ui_count = read_u16_le(g_dialogue_active + 0);
    const uint16_t line_count = read_u16_le(g_dialogue_active + 2);
    const uint16_t ui_blob_size = read_u16_le(g_dialogue_active + 4);
    if (ui_count != DIALOGUE_UI_STRING_COUNT ||
        line_count != DIALOGUE_TOTAL_LINES)
        return false;

    if ((uint32_t)ui_blob_size > unpacked_size - DIALOGUE_BLOBS_POS)
        return false;
    const uint32_t dialogue_blob_size =
        unpacked_size - DIALOGUE_BLOBS_POS - (uint32_t)ui_blob_size;
    if (dialogue_blob_size == 0u) return false;

    uint16_t previous = 0;
    for (int i = 0; i < DIALOGUE_UI_STRING_COUNT; ++i) {
        const uint16_t offset = read_u16_le(
            g_dialogue_active + DIALOGUE_UI_OFFSETS_POS + i * 2u);
        if ((uint32_t)offset >= (uint32_t)ui_blob_size) return false;
        if (i > 0 && offset < previous) return false;
        const unsigned char* text =
            g_dialogue_active + DIALOGUE_BLOBS_POS + offset;
        if (memchr(text, 0, (uint32_t)ui_blob_size - offset) == NULL)
            return false;
        previous = offset;
    }

    previous = 0;
    for (int i = 0; i < DIALOGUE_TOTAL_LINES; ++i) {
        const uint16_t offset = read_u16_le(
            g_dialogue_active + DIALOGUE_LINE_OFFSETS_POS + i * 2u);
        if ((uint32_t)offset >= dialogue_blob_size) return false;
        if (i > 0 && offset < previous) return false;
        const unsigned char* text = g_dialogue_active + DIALOGUE_BLOBS_POS +
                                    (uint32_t)ui_blob_size + offset;
        if (memchr(text, 0, dialogue_blob_size - offset) == NULL)
            return false;
        previous = offset;
    }
    return true;
}

bool dialogue_load_language(int language) {
    if (language < 0 || language >= DIALOGUE_LANGUAGE_COUNT)
        language = DIALOGUE_LANGUAGE_ENGLISH;
    if (g_dialogue_loaded_language == language) return true;

    uint32_t pack_base = 0;
    FILE* f = dialogue_open_pack(&pack_base);
    if (!f) return false;

    unsigned char header[DIALOGUE_PACK_HEADER_SIZE];
    bool ok = false;
    unsigned char* packed = NULL;

    if (fread(header, 1, sizeof(header), f) != sizeof(header)) goto cleanup;
    if (memcmp(header, "DLG7", 4) != 0) goto cleanup;
    if (read_u16_le(header + 4) != DIALOGUE_PACK_VERSION) goto cleanup;
    if (read_u16_le(header + 6) != DIALOGUE_LANGUAGE_COUNT) goto cleanup;
    if (read_u16_le(header + 8) != DIALOGUE_TOTAL_LINES) goto cleanup;

    const long entry_pos = (long)pack_base + (long)DIALOGUE_PACK_HEADER_SIZE +
                           (long)language * (long)DIALOGUE_PACK_ENTRY_SIZE;
    if (fseek(f, entry_pos, SEEK_SET) != 0) goto cleanup;

    unsigned char entry[DIALOGUE_PACK_ENTRY_SIZE];
    if (fread(entry, 1, sizeof(entry), f) != sizeof(entry)) goto cleanup;

    const uint32_t offset = pack_base + read_u32_le(entry + 0);
    const uint32_t packed_size = read_u32_le(entry + 4);
    const uint32_t unpacked_size = read_u32_le(entry + 8);
    const uint32_t expected_checksum = read_u32_le(entry + 12);

    if (packed_size == 0u || packed_size > 65536u ||
        unpacked_size <= DIALOGUE_BLOBS_POS ||
        unpacked_size > DIALOGUE_ACTIVE_BUFFER_SIZE)
        goto cleanup;

    packed = (unsigned char*)malloc(packed_size);
    if (!packed) goto cleanup;
    if (fseek(f, (long)offset, SEEK_SET) != 0) goto cleanup;
    if (fread(packed, 1, packed_size, f) != packed_size) goto cleanup;

    const uint32_t actual_size = dialogue_lz4_depack(
        packed, packed_size, g_dialogue_active, DIALOGUE_ACTIVE_BUFFER_SIZE);
    if (actual_size != unpacked_size) goto cleanup;
    if (fnv1a32(g_dialogue_active, actual_size) != expected_checksum) goto cleanup;
    if (!dialogue_validate_active(actual_size)) goto cleanup;

    g_dialogue_active_size = actual_size;
    g_dialogue_loaded_language = language;
    ok = true;

cleanup:
    free(packed);
    fclose(f);
    if (!ok) {
        g_dialogue_active_size = 0;
        g_dialogue_loaded_language = -1;
    }
    return ok;
}

int dialogue_loaded_language(void) {
    return g_dialogue_loaded_language;
}

const char* dialogue_get_ui_string(int language, int string_id) {
    if (language < 0 || language >= DIALOGUE_LANGUAGE_COUNT)
        language = DIALOGUE_LANGUAGE_ENGLISH;
    if (g_dialogue_loaded_language != language && !dialogue_load_language(language))
        return g_dialogue_fallback;
    if (string_id < 0 || string_id >= DIALOGUE_UI_STRING_COUNT)
        return g_dialogue_fallback;

    const uint16_t ui_blob_size = read_u16_le(g_dialogue_active + 4);
    const uint16_t offset = read_u16_le(
        g_dialogue_active + DIALOGUE_UI_OFFSETS_POS + string_id * 2u);
    if ((uint32_t)offset >= (uint32_t)ui_blob_size)
        return g_dialogue_fallback;
    return (const char*)(g_dialogue_active + DIALOGUE_BLOBS_POS + offset);
}

const char* dialogue_get_line(int language, int character, int trigger, int variant) {
    if (language < 0 || language >= DIALOGUE_LANGUAGE_COUNT)
        language = DIALOGUE_LANGUAGE_ENGLISH;
    if (g_dialogue_loaded_language != language && !dialogue_load_language(language))
        return g_dialogue_fallback;

    if (character < 0) character = 0;
    if (character >= DIALOGUE_CHARACTER_COUNT)
        character %= DIALOGUE_CHARACTER_COUNT;
    if (trigger < 0 || trigger >= DIALOGUE_TRIGGER_COUNT)
        trigger = DIALOGUE_BAD_DRAW;
    if (variant < 0 || variant >= DIALOGUE_VARIANTS_PER_TRIGGER)
        variant = 0;

    const int index = (character * DIALOGUE_TRIGGER_COUNT + trigger) *
                      DIALOGUE_VARIANTS_PER_TRIGGER + variant;
    const uint16_t ui_blob_size = read_u16_le(g_dialogue_active + 4);
    const uint16_t offset = read_u16_le(
        g_dialogue_active + DIALOGUE_LINE_OFFSETS_POS + index * 2u);
    const uint32_t dialogue_blob_size = g_dialogue_active_size -
        DIALOGUE_BLOBS_POS - (uint32_t)ui_blob_size;
    if ((uint32_t)offset >= dialogue_blob_size)
        return g_dialogue_fallback;
    return (const char*)(g_dialogue_active + DIALOGUE_BLOBS_POS +
                         (uint32_t)ui_blob_size + offset);
}
