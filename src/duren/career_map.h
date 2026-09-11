#ifndef CAREER_MAP_H
#define CAREER_MAP_H

#include "hal.h"
#include "modes.h"

typedef enum {
    CAREER_MAP_ACTION_NONE = 0,
    CAREER_MAP_ACTION_START_BATTLE,
    CAREER_MAP_ACTION_TRAVEL_STARTED
} CareerMapAction;

typedef struct {
    int cursor_x;
    int cursor_y;
    uint8_t cursor_point;
    int firefly_x_q8;
    int firefly_y_q8;
    bool traveling;
    uint8_t route_index;
    uint32_t travel_started_ms;
    uint32_t travel_duration_ms;
    uint8_t fog[30][40];
} CareerMapUi;

void career_map_init(CareerMapUi* ui, const ModeSession* session,
                     uint32_t now_ms);
bool career_map_move_cursor(CareerMapUi* ui, const ModeSession* session,
                            int dx, int dy);
CareerMapAction career_map_activate(CareerMapUi* ui,
                                    const ModeSession* session,
                                    uint32_t now_ms);
bool career_map_update(CareerMapUi* ui, ModeSession* session,
                       GameState* game, uint32_t now_ms);
void career_map_draw(HalTexture* map, HalTexture* font,
                     CareerMapUi* ui, const ModeSession* session,
                     uint32_t now_ms);

#endif
