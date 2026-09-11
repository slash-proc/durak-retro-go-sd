/*
 * Duren (Durak) — Retro-Go SD GWHB homebrew entry.
 *
 * Game logic lives under src/duren/ (ported from the Discord handoff overlay).
 * Combined resources live next to the GWHB binary:
 *   /homebrews/Duren.bin
 *   /homebrews/Duren.pak   (gfx + audio + lang)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "common.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "rom_manager.h"
#include "odroid_system.h"
#include "odroid_overlay.h"
#include "appid.h"
#include "gw_malloc.h"

#include "duren_pak.h"
#include "gw_core_bridge.h"

#define APP_ID       APPID_HOMEBREW
#define SAMPLE_RATE  8000
#define FPS          60

int durak_run(void);
bool durak_system_SaveState(const char *save_path);
bool durak_system_LoadState(const char *save_path);
void hal_on_retro_go_wakeup(void);
void duren_system_PreSleep(void);

static const char *g_boot_error;

static void present_error_frame(void)
{
    uint16_t *fb = lcd_get_active_buffer();
    const char *detail = g_boot_error ? g_boot_error : duren_pak_last_error();
    int y = 40;

    memset(fb, 0, GW_LCD_WIDTH * GW_LCD_HEIGHT * sizeof(uint16_t));
    odroid_overlay_draw_text(8, y, 0, "Duren - failed to start", 0xFFFF, 0x0000);
    y += 20;
    odroid_overlay_draw_text(8, y, 0, "Put Duren.pak in:", 0xFFFF, 0x0000);
    y += 16;
    odroid_overlay_draw_text(8, y, 0, "  /homebrews/Duren.pak", 0xFFE0, 0x0000);
    y += 24;
    if (detail && detail[0]) {
        char line[48];
        snprintf(line, sizeof(line), "%.46s", detail);
        odroid_overlay_draw_text(8, y, 0, line, 0xF800, 0x0000);
        y += 20;
    }
    if (ACTIVE_FILE && ACTIVE_FILE->path[0]) {
        char line[48];
        snprintf(line, sizeof(line), "bin: %.40s", ACTIVE_FILE->path);
        odroid_overlay_draw_text(8, y, 0, line, 0x8410, 0x0000);
        y += 16;
    }
    y += 8;
    odroid_overlay_draw_text(8, y, 0, "PAUSE: Retro-Go menu", 0x07FF, 0x0000);
    common_ingame_overlay();
}

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    if (g_boot_error)
        present_error_frame();
    return lcd_get_active_buffer();
}

static void Shutdown(void)
{
    duren_system_PreSleep();
}

static void SleepWake(void)
{
    odroid_audio_init(SAMPLE_RATE);
    audio_clear_buffers();
    if (!g_boot_error)
        hal_on_retro_go_wakeup();
}

static void SramSave(void)
{
}

/* Keep MENU / POWER alive forever — never spin on wdog alone. */
static void fatal_error_loop(const char *msg)
{
    odroid_gamepad_state_t joystick;
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };

    g_boot_error = msg ? msg : duren_pak_last_error();
    printf("duren: fatal: %s\n", g_boot_error);

    common_emu_state.frame_time_10us = (uint16_t)(100000 / FPS + 0.5f);
    lcd_set_refresh_rate(FPS);

    while (1) {
        wdog_refresh();
        (void)common_emu_frame_loop();
        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &present_error_frame);
        present_error_frame();
        lcd_swap();
    }
}

void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    int rc;

    gw_core_bridge_init();
    g_boot_error = NULL;

    /* Firmware seeds ram_start after load; bump past our own BSS so
     * ram_malloc does not overlap static caches in the HAL. */
    ram_start = (uint32_t)&__CORE_BSS_END__;

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    common_emu_state.frame_time_10us = (uint16_t)(100000 / FPS + 0.5f);
    lcd_set_refresh_rate(FPS);

    /* Early on-screen heartbeat so a hang before pack load is visible. */
    {
        uint16_t *fb = lcd_get_active_buffer();
        memset(fb, 0, GW_LCD_WIDTH * GW_LCD_HEIGHT * sizeof(uint16_t));
        odroid_overlay_draw_text(8, 100, 0, "Duren loading...", 0xFFFF, 0x0000);
        lcd_swap();
    }

    /* 8 kHz matches the embedded PCM packs. */
    odroid_system_init(APP_ID, SAMPLE_RATE);
    odroid_system_emu_init(&durak_system_LoadState,
                           &durak_system_SaveState,
                           &Screenshot,
                           &Shutdown,
                           &SleepWake,
                           &SramSave,
                           NULL);

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    printf("duren: ACTIVE_FILE=%s\n",
           (ACTIVE_FILE && ACTIVE_FILE->path[0]) ? ACTIVE_FILE->path : "(null)");

    rc = durak_run();
    if (rc != 0) {
        fatal_error_loop(duren_pak_last_error());
    }

    /* Normal quit path — keep MENU responsive instead of a bare spin. */
    fatal_error_loop("Duren exited");
}
