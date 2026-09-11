#ifndef DUREN_PAK_H
#define DUREN_PAK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Combined resource container for Duren.
 *
 * Device path (only): /homebrews/Duren.pak  (same folder as Duren.bin)
 *
 * Members (inner files, unchanged formats):
 *   "gfx"   → former duren_gfx.pak   (DGFX)
 *   "audio" → former duren_audio.pak (DAUD)
 *   "lang"  → former duren_lang.pak  (DLG7)
 */

#define DUREN_PAK_MAGIC        "DPK1"
#define DUREN_PAK_VERSION      1u
#define DUREN_PAK_MEMBER_GFX   "gfx"
#define DUREN_PAK_MEMBER_AUDIO "audio"
#define DUREN_PAK_MEMBER_LANG  "lang"

/* Open Duren.pak and locate a named member.
 * On success: *out_file is open (caller fclose), *out_base is the absolute
 * byte offset of the member start inside the container. The file position
 * is left at *out_base (ready to read the inner pack header). */
bool duren_pak_open_member(const char *member, FILE **out_file, uint32_t *out_base);

/* Human-readable reason for the last open failure (never NULL). */
const char *duren_pak_last_error(void);

#endif /* DUREN_PAK_H */
