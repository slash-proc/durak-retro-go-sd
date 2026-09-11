#include "career_map.h"
#include "ui_modes.h"
#include <stdlib.h>
#include <string.h>

#define MAP_W 320
#define MAP_H 240
#define FOG_CELL 8
#define FOG_COLS 40
#define FOG_ROWS 30
#define ROUTE_MAX_POINTS 12

typedef struct { int16_t x, y; } CareerPoint;
typedef struct {
    uint8_t count;
    CareerPoint point[ROUTE_MAX_POINTS];
} CareerRoute;

/* Centres were authored against the clean 320x240 Game & Watch map. The
 * numbered HD image is only a semantic 1..20 reference and is never loaded. */
static const CareerPoint k_points[20] = {
    {35,44}, {84,46}, {126,66}, {160,50}, {170,23},
    {233,34}, {293,47}, {266,75}, {284,108}, {22,91},
    {50,136}, {91,108}, {132,121}, {174,101}, {226,109},
    {17,181}, {73,184}, {139,194}, {202,181}, {268,177}
};

static const CareerRoute k_routes[19] = {
    {4, {{35,44},{53,50},{70,51},{84,46}}},
    {4, {{84,46},{91,57},{107,64},{126,66}}},
    {4, {{126,66},{139,64},{151,56},{160,50}}},
    {3, {{160,50},{165,40},{170,23}}},
    {5, {{170,23},{186,23},{205,28},{220,34},{233,34}}},
    {4, {{233,34},{251,42},{270,44},{293,47}}},
    {4, {{293,47},{286,60},{276,67},{266,75}}},
    {4, {{266,75},{276,84},{282,96},{284,108}}},
    {10,{{284,108},{292,84},{279,65},{245,54},{205,58},
         {165,66},{126,75},{82,79},{43,86},{22,91}}},
    {4, {{22,91},{32,107},{44,121},{50,136}}},
    {4, {{50,136},{62,127},{78,118},{91,108}}},
    {4, {{91,108},{107,114},{121,120},{132,121}}},
    {4, {{132,121},{144,115},{159,108},{174,101}}},
    {4, {{174,101},{192,106},{210,112},{226,109}}},
    {8, {{226,109},{205,130},{176,145},{140,151},
         {105,158},{70,165},{40,173},{17,181}}},
    {4, {{17,181},{34,190},{54,190},{73,184}}},
    {4, {{73,184},{93,191},{116,195},{139,194}}},
    {4, {{139,194},{159,196},{180,190},{202,181}}},
    {4, {{202,181},{222,181},{245,179},{268,177}}}
};

static int point_distance_sq(int x0, int y0, int x1, int y1) {
    const int dx = x0 - x1;
    const int dy = y0 - y1;
    return dx * dx + dy * dy;
}

static uint32_t fog_hash(int x, int y) {
    uint32_t h = (uint32_t)x * 0x9E3779B9u ^ (uint32_t)y * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0xC2B2AE35u;
    return h ^ (h >> 13);
}

static void fog_stamp(CareerMapUi* ui, int x, int y, int radius) {
    const int outer2 = radius * radius;
    const int middle = radius > 5 ? radius - 4 : radius;
    const int inner = radius > 10 ? radius - 9 : radius;
    const int middle2 = middle * middle;
    const int inner2 = inner * inner;
    for (int row = 0; row < FOG_ROWS; ++row) {
        const int cy = row * FOG_CELL + FOG_CELL / 2;
        for (int col = 0; col < FOG_COLS; ++col) {
            const int cx = col * FOG_CELL + FOG_CELL / 2;
            const int d2 = point_distance_sq(cx, cy, x, y);
            uint8_t level = 0u;
            if (d2 <= inner2) level = 3u;
            else if (d2 <= middle2) level = 2u;
            else if (d2 <= outer2 && (fog_hash(col, row) & 3u) != 0u)
                level = 1u;
            if (level > ui->fog[row][col]) ui->fog[row][col] = level;
        }
    }
}

static void fog_route(CareerMapUi* ui, const CareerRoute* route) {
    if (!ui || !route || route->count < 2u) return;
    for (int i = 0; i + 1 < route->count; ++i) {
        const CareerPoint a = route->point[i];
        const CareerPoint b = route->point[i + 1];
        const int dx = (int)b.x - (int)a.x;
        const int dy = (int)b.y - (int)a.y;
        int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (steps < 1) steps = 1;
        for (int s = 0; s <= steps; s += 5) {
            const int x = a.x + (dx * s) / steps;
            const int y = a.y + (dy * s) / steps;
            fog_stamp(ui, x, y, 11);
        }
        fog_stamp(ui, b.x, b.y, 11);
    }
}

static void rebuild_fog(CareerMapUi* ui, const ModeSession* session) {
    memset(ui->fog, 0, sizeof(ui->fog));
    int current = session ? (int)session->current_location : 1;
    int highest = session ? (int)session->highest_defeated_opponent : 0;
    if (current < 1) current = 1;
    if (current > 20) current = 20;
    int visible_points = current;
    if (highest > visible_points) visible_points = highest;
    for (int i = 0; i < visible_points && i < 20; ++i)
        fog_stamp(ui, k_points[i].x, k_points[i].y, 25);
    for (int i = 0; i + 1 < current && i < 19; ++i)
        fog_route(ui, &k_routes[i]);
    if (session && session->next_unlocked && current < 20) {
        fog_route(ui, &k_routes[current - 1]);
        fog_stamp(ui, k_points[current].x, k_points[current].y, 22);
    }
    if (session && session->career_complete) {
        for (int i = 0; i < 20; ++i)
            fog_stamp(ui, k_points[i].x, k_points[i].y, 30);
        for (int i = 0; i < 19; ++i) fog_route(ui, &k_routes[i]);
    }
}

static int route_length(const CareerRoute* route) {
    int total = 0;
    if (!route) return 1;
    for (int i = 0; i + 1 < route->count; ++i) {
        const int dx = abs((int)route->point[i + 1].x - route->point[i].x);
        const int dy = abs((int)route->point[i + 1].y - route->point[i].y);
        total += dx > dy ? dx : dy;
    }
    return total > 0 ? total : 1;
}

static CareerPoint route_position(const CareerRoute* route, int distance) {
    CareerPoint out = route && route->count ? route->point[0]
                                            : (CareerPoint){0, 0};
    if (!route) return out;
    for (int i = 0; i + 1 < route->count; ++i) {
        const CareerPoint a = route->point[i];
        const CareerPoint b = route->point[i + 1];
        const int dx = abs((int)b.x - a.x);
        const int dy = abs((int)b.y - a.y);
        int length = dx > dy ? dx : dy;
        if (length < 1) length = 1;
        if (distance <= length) {
            out.x = (int16_t)(a.x + ((int)b.x - a.x) * distance / length);
            out.y = (int16_t)(a.y + ((int)b.y - a.y) * distance / length);
            return out;
        }
        distance -= length;
        out = b;
    }
    return out;
}

static void cursor_snap_to_point(CareerMapUi* ui, int point_index) {
    if (!ui) return;
    if (point_index < 0) point_index = 0;
    if (point_index > 19) point_index = 19;
    ui->cursor_point = (uint8_t)point_index;
    ui->cursor_x = k_points[point_index].x;
    /* Store the actual node centre. draw_green_cursor() treats this as the
     * arrow-tip anchor; there is no second hidden +8/-8 coordinate system. */
    ui->cursor_y = k_points[point_index].y;
}

void career_map_init(CareerMapUi* ui, const ModeSession* session,
                     uint32_t now_ms) {
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    int location = session ? session->current_location : 1;
    if (location < 1 || location > 20) location = 1;
    const int cursor_point = session && session->next_unlocked && location < 20
                           ? location : location - 1;
    cursor_snap_to_point(ui, cursor_point);
    ui->firefly_x_q8 = k_points[location - 1].x << 8;
    ui->firefly_y_q8 = k_points[location - 1].y << 8;
    ui->travel_started_ms = now_ms;
    rebuild_fog(ui, session);
}

bool career_map_move_cursor(CareerMapUi* ui, const ModeSession* session,
                            int dx, int dy) {
    if (!ui || !session || ui->traveling || (!dx && !dy)) return false;
    int location = session->current_location;
    if (location < 1 || location > 20) return false;

    /* The button adaptation is node based, not a free mouse cursor. Only the
     * current city and its unlocked destination can be selected, and the
     * arrow tip is always reconstructed from the exact career_points entry. */
    const int current_point = location - 1;
    const int next_point = session->next_unlocked && location < 20
                         ? location : current_point;
    int selected = ui->cursor_point;
    if (selected != current_point && selected != next_point)
        selected = current_point;

    const int candidate = selected == current_point ? next_point : current_point;
    if (candidate == selected) {
        cursor_snap_to_point(ui, selected);
        return false;
    }
    const int vx = (int)k_points[candidate].x - k_points[selected].x;
    const int vy = (int)k_points[candidate].y - k_points[selected].y;
    if (vx * dx + vy * dy <= 0) {
        cursor_snap_to_point(ui, selected);
        return false;
    }
    cursor_snap_to_point(ui, candidate);
    return true;
}

CareerMapAction career_map_activate(CareerMapUi* ui,
                                    const ModeSession* session,
                                    uint32_t now_ms) {
    if (!ui || !session || ui->traveling || session->career_complete)
        return CAREER_MAP_ACTION_NONE;
    int location = session->current_location;
    if (location < 1 || location > 20) return CAREER_MAP_ACTION_NONE;
    if (session->next_unlocked && location < 20 &&
        ui->cursor_point == (uint8_t)location) {
        ui->traveling = true;
        ui->route_index = (uint8_t)(location - 1);
        ui->travel_started_ms = now_ms;
        const int length = route_length(&k_routes[ui->route_index]);
        uint32_t duration = (uint32_t)length * 22u;
        if (duration < 900u) duration = 900u;
        if (duration > 4200u) duration = 4200u;
        ui->travel_duration_ms = duration;
        return CAREER_MAP_ACTION_TRAVEL_STARTED;
    }
    if (session->highest_defeated_opponent < location &&
        ui->cursor_point == (uint8_t)(location - 1))
        return CAREER_MAP_ACTION_START_BATTLE;
    return CAREER_MAP_ACTION_NONE;
}

bool career_map_update(CareerMapUi* ui, ModeSession* session,
                       GameState* game, uint32_t now_ms) {
    if (!ui || !session || !ui->traveling) return false;
    uint32_t elapsed = now_ms - ui->travel_started_ms;
    if (elapsed > ui->travel_duration_ms) elapsed = ui->travel_duration_ms;
    const CareerRoute* route = &k_routes[ui->route_index];
    const int total = route_length(route);
    const int distance = (int)((uint64_t)total * elapsed /
                               (ui->travel_duration_ms ? ui->travel_duration_ms : 1u));
    const CareerPoint p = route_position(route, distance);
    ui->firefly_x_q8 = p.x << 8;
    ui->firefly_y_q8 = p.y << 8;
    if (elapsed < ui->travel_duration_ms) return false;

    ui->traveling = false;
    modes_career_arrive_next(session, game);
    const int location = session->current_location;
    cursor_snap_to_point(ui, location - 1);
    rebuild_fog(ui, session);
    return true;
}

static void draw_glyph8(HalTexture* font, unsigned int glyph, int x, int y) {
    if (!font) return;
    hal_draw_sprite(font, (int)(glyph % 16u) * 8,
                    (int)(glyph / 16u) * 8, 8, 8, x, y);
}

static void draw_text8(HalTexture* font, const char* text, int x, int y) {
    if (!font || !text) return;
    for (int i = 0; text[i]; ++i)
        draw_glyph8(font, (unsigned char)text[i], x + i * 8, y);
}

static void draw_fog_reveal(HalTexture* map, const CareerMapUi* ui) {
    if (!map || !ui) return;
    static const unsigned int dim_for_level[4] = {65u, 44u, 20u, 0u};
    hal_draw_texture_dimmed(map, 0, 0, dim_for_level[0]);
    for (int row = 0; row < FOG_ROWS; ++row) {
        for (uint8_t level = 1u; level <= 3u; ++level) {
            int col = 0;
            while (col < FOG_COLS) {
                while (col < FOG_COLS && ui->fog[row][col] != level) ++col;
                const int start = col;
                while (col < FOG_COLS && ui->fog[row][col] == level) ++col;
                if (start >= col) continue;
                const int x = start * FOG_CELL;
                const int y = row * FOG_CELL;
                const int w = (col - start) * FOG_CELL;
                if (level == 3u)
                    hal_draw_sprite(map, x, y, w, FOG_CELL, x, y);
                else
                    hal_draw_sprite_dimmed(map, x, y, w, FOG_CELL,
                                           x, y, dim_for_level[level]);
            }
        }
    }
}

static void draw_firefly(int x, int y, uint32_t now_ms) {
    const int pulse = (int)((now_ms / 110u) % 6u);
    const int halo = 5 + (pulse <= 3 ? pulse : 6 - pulse);
    hal_fill_rect(x - halo, y, halo * 2 + 1, 1, 0x70530Au);
    hal_fill_rect(x - halo + 2, y - 2, (halo - 2) * 2 + 1, 5, 0x9C7412u);
    hal_fill_rect(x - 2, y - 2, 5, 5, 0xE6B82Eu);
    hal_fill_rect(x - 1, y - 1, 3, 3, 0xFFE77Au);
    hal_fill_rect(x, y, 1, 1, 0xFFF4BCu);
}

static void draw_green_cursor(int x, int y, uint32_t now_ms) {
    const int bob = (int)((now_ms / 120u) % 8u);
    const int offset = bob <= 4 ? bob : 8 - bob;
    const int tip_y = y - offset;
    const unsigned int dark = 0x174C24u;
    const unsigned int green = 0x52D66Au;
    hal_fill_rect(x - 1, tip_y - 10, 3, 5, dark);
    hal_fill_rect(x, tip_y - 10, 1, 5, green);
    for (int row = 0; row < 5; ++row) {
        const int width = 9 - row * 2;
        hal_fill_rect(x - width / 2, tip_y - 5 + row, width, 1, dark);
        if (row < 4 && width > 2)
            hal_fill_rect(x - width / 2 + 1, tip_y - 5 + row,
                          width - 2, 1, green);
    }
}

void career_map_draw(HalTexture* map, HalTexture* font,
                     CareerMapUi* ui, const ModeSession* session,
                     uint32_t now_ms) {
    if (!ui || !session) return;
    draw_fog_reveal(map, ui);
    draw_firefly(ui->firefly_x_q8 >> 8, ui->firefly_y_q8 >> 8, now_ms);
    if (!ui->traveling && !session->career_complete)
        draw_green_cursor(ui->cursor_x, ui->cursor_y, now_ms);

    hal_fill_rect(0, 0, 64, 12, 0x06140Bu);
    draw_text8(font, "B ", 3, 2);
    draw_text8(font, ui_tr(UI_STR_BACK), 19, 2);
    const char* options = ui_tr(UI_STR_OPTIONS);
    int width = ((int)strlen(options) + 7) * 8;
    if (width > 152) width = 152;
    hal_fill_rect(MAP_W - width, 0, width, 12, 0x06140Bu);
    draw_text8(font, "SELECT ", MAP_W - width + 3, 2);
    draw_text8(font, options, MAP_W - width + 59, 2);
    if (session->career_complete) {
        const char* done = ui_tr(UI_STR_CAREER_COMPLETE);
        int x = (MAP_W - (int)strlen(done) * 8) / 2;
        if (x < 2) x = 2;
        hal_fill_rect(x - 2, MAP_H - 14,
                      (int)strlen(done) * 8 + 4, 12, 0x06140Bu);
        draw_text8(font, done, x, MAP_H - 12);
    }
}
