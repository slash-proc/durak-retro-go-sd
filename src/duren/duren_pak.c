#include "duren_pak.h"

#include <stdio.h>
#include <string.h>

#ifdef PLATFORM_RETRO_GO
#include "gw_core_bridge.h"
#endif

#define DUREN_PAK_HEADER_SIZE  12u
#define DUREN_PAK_ENTRY_SIZE   32u
#define DUREN_PAK_MAX_ENTRIES  8u
#define DUREN_PAK_NAME_SIZE    16u

#define DUREN_PAK_DEVICE_PATH  "/homebrews/Duren.pak"

static const char *g_duren_pak_last_error = "Duren.pak not opened";

const char *duren_pak_last_error(void)
{
    return g_duren_pak_last_error ? g_duren_pak_last_error : "unknown";
}

static uint16_t duren_pak_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t duren_pak_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static FILE *duren_pak_fopen_container(void)
{
#ifdef PLATFORM_RETRO_GO
    FILE *f = fopen(DUREN_PAK_DEVICE_PATH, "rb");
    if (f) {
        printf("duren: opened %s\n", DUREN_PAK_DEVICE_PATH);
        g_duren_pak_last_error = NULL;
        return f;
    }
    g_duren_pak_last_error = "Duren.pak not found in /homebrews";
    printf("duren: %s\n", g_duren_pak_last_error);
    return NULL;
#else
    /* Host SDL: cwd is typically host_data/. */
    static const char *const host_paths[] = {
        "Duren.pak",
        "./Duren.pak",
        "host_data/Duren.pak",
    };
    unsigned i;

    for (i = 0; i < sizeof(host_paths) / sizeof(host_paths[0]); ++i) {
        FILE *f = fopen(host_paths[i], "rb");
        if (f) {
            printf("duren: opened %s\n", host_paths[i]);
            g_duren_pak_last_error = NULL;
            return f;
        }
    }
    g_duren_pak_last_error = "Duren.pak not found (host_data)";
    printf("duren: %s\n", g_duren_pak_last_error);
    return NULL;
#endif
}

bool duren_pak_open_member(const char *member, FILE **out_file, uint32_t *out_base)
{
    uint8_t header[DUREN_PAK_HEADER_SIZE];
    uint16_t version;
    uint16_t count;
    uint16_t i;

    if (!member || !out_file || !out_base)
        return false;

    *out_file = NULL;
    *out_base = 0;

    FILE *f = duren_pak_fopen_container();
    if (!f)
        return false;

    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        g_duren_pak_last_error = "Duren.pak truncated header";
        goto fail;
    }
    if (memcmp(header, DUREN_PAK_MAGIC, 4) != 0) {
        g_duren_pak_last_error = "Duren.pak bad magic (need DPK1)";
        goto fail;
    }

    version = duren_pak_u16le(header + 4);
    count = duren_pak_u16le(header + 6);
    if (version != DUREN_PAK_VERSION || count == 0 || count > DUREN_PAK_MAX_ENTRIES) {
        g_duren_pak_last_error = "Duren.pak unsupported version/count";
        goto fail;
    }

    for (i = 0; i < count; ++i) {
        uint8_t raw[DUREN_PAK_ENTRY_SIZE];
        char name[DUREN_PAK_NAME_SIZE];
        uint32_t offset;
        uint32_t size;

        if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
            g_duren_pak_last_error = "Duren.pak truncated TOC";
            goto fail;
        }

        memcpy(name, raw, DUREN_PAK_NAME_SIZE - 1);
        name[DUREN_PAK_NAME_SIZE - 1] = '\0';
        offset = duren_pak_u32le(raw + 16);
        size = duren_pak_u32le(raw + 20);

        if (strcmp(name, member) != 0)
            continue;
        if (size == 0 || offset < DUREN_PAK_HEADER_SIZE) {
            g_duren_pak_last_error = "Duren.pak bad member extent";
            goto fail;
        }
        if (fseek(f, (long)offset, SEEK_SET) != 0) {
            g_duren_pak_last_error = "Duren.pak seek failed";
            goto fail;
        }

        *out_file = f;
        *out_base = offset;
        g_duren_pak_last_error = NULL;
        return true;
    }

    g_duren_pak_last_error = "Duren.pak missing member";
fail:
    fclose(f);
    printf("duren: %s (want '%s')\n", duren_pak_last_error(), member ? member : "?");
    return false;
}
