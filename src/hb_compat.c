/*
 * Small stubs for symbols the Duren HAL expects from the old firmware
 * overlay build, but which are not (yet) on the freestanding GWHB ABI.
 */

#include <stddef.h>
#include <stdbool.h>
#include <math.h>

#include "gw_malloc.h"
#include "odroid_sdcard.h"

#include "gw_core_bridge.h"

/* Overlay builds used a dedicated AHB bump; map to the freeable AHB heap. */
void *ahb_only_malloc(size_t size)
{
    return ahb_malloc(size);
}

/* cpumon_busy / cpumon_reset are firmware-side; only cpumon_sleep is on ABI. */
void cpumon_busy(void)
{
}

void cpumon_reset(void)
{
}

/* ABI exposes cosf/sqrtf but not sinf/roundf/hypotf. */
float sinf(float x)
{
    return cosf(x - 1.57079632679489661923f);
}

float roundf(float x)
{
    return (float)(int)(x >= 0.0f ? x + 0.5f : x - 0.5f);
}

float hypotf(float x, float y)
{
    return sqrtf(x * x + y * y);
}

/* Declared in rg_storage.h but not on the freestanding ABI. */
bool rg_storage_mkdir(const char *dir)
{
    if (!dir || !dir[0])
        return false;
    /* odroid_sdcard_mkdir returns 0 on success (FatFs FR_OK). */
    return odroid_sdcard_mkdir(dir) == 0;
}

void rg_storage_commit(void)
{
    /* FatFs on this firmware flushes on fclose; no separate commit slot. */
}
