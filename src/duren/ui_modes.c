#include "ui_modes.h"
#include "dialogue_data.h"
#include "game.h"
#include "modes.h"
#include <stdio.h>
#include <string.h>

#define FACE_W 55
#define FACE_H 55

static int g_ui_language = UI_LANG_ENGLISH;

_Static_assert(UI_STR_COUNT == DIALOGUE_UI_STRING_COUNT, "UI string count mismatch");

void ui_set_language(int language) {
    if (language < 0 || language >= UI_LANG_COUNT) language = UI_LANG_ENGLISH;
    g_ui_language = language;
}

int ui_get_language(void) {
    return g_ui_language;
}

bool ui_language_uses_cp1251(int language) {
    return language == UI_LANG_UKRAINIAN;
}

const char* ui_tr(UiStringId id) {
    if (id < 0 || id >= UI_STR_COUNT) return "?";
    return dialogue_get_ui_string(g_ui_language, (int)id);
}

const char* ui_mode_name_localized(int mode) {
    static const char* const online_labels[UI_LANG_COUNT] = {
        "ONLINE",
        "\xCE\xCD\xCB\xC0\xC9\xCD", /* ОНЛАЙН, CP1251 */
        "ONLINE",
        "ONLINE",
        "EN LINEA",
        "EN LIGNE",
        "ONLINE",
        "ONLINE"
    };
    switch (mode) {
        case MODE_CAREER: return ui_tr(UI_STR_CAREER);
        case MODE_TOURNAMENT: return ui_tr(UI_STR_TOURNAMENT);
        case MODE_RANDOM_BATTLE: return online_labels[g_ui_language];
        case MODE_BATTLE_ROYAL: return ui_tr(UI_STR_BATTLE_ROYAL);
        default: return ui_tr(UI_STR_MODE);
    }
}

const char* ui_difficulty_name_localized(int difficulty) {
    switch (difficulty) {
        case DIFFICULTY_EASY: return ui_tr(UI_STR_EASY);
        case DIFFICULTY_HARD: return ui_tr(UI_STR_HARD);
        default: return ui_tr(UI_STR_NORMAL);
    }
}


const char* ui_language_label(int language) {
    static const char* const labels[UI_LANG_COUNT] = {
        "ENGLISH", "UKRAINIAN", "DEUTSCH", "POLSKI", "ESPA\x8D" "OL",
        "FRAN\x86" "AIS", "ITALIANO", "PORTUGU\x89" "S"
    };
    if (language < 0 || language >= UI_LANG_COUNT) language = UI_LANG_ENGLISH;
    return labels[language];
}

const char* ui_ai_name(int face_index) {
    if (face_index < 0) face_index = 0;
    face_index %= 20;
    return ui_tr((UiStringId)(UI_STR_AI_NAME_0 + face_index));
}

const char* ui_online_bot_name(int seed) {
    /* 600 distinct handles per language from 10 x 10 x 6 components.  This
     * preserves the Android-sized pool without storing 4,800 full strings in
     * scarce Game & Watch memory. All results fit the compact opponent HUD. */
    static const char* const roots[UI_LANG_COUNT][10] = {
        {"Ace","Maya","Sam","Riley","Lucky","Jack","Robin","Max","Amy","Kate"},
        {"Taras","Oksa","Lesya","Ivan","Yurko","Vira","Bohd","Myko","Katya","Andr"},
        {"Lena","Lukas","Anna","Felix","Mia","Tom","Klara","Hans","Nina","Ole"},
        {"Kuba","Zosia","Marek","Ania","Ola","Piotr","Kasia","Janek","Ewa","Iga"},
        {"Lucas","Sofia","Diego","Lucia","Mateo","Carme","Pablo","Alba","Nico","Lola"},
        {"Lucas","Emma","Hugo","Lea","Jules","Chloe","Theo","Manon","Louis","Ines"},
        {"Luca","Giul","Marco","Anna","Leo","Sofia","Paolo","Mia","Enzo","Sara"},
        {"Joao","Ana","Tiago","Rita","Migu","Bia","Nuno","Lara","Rui","Ines"}
    };
    static const char* const motifs[UI_LANG_COUNT][10] = {
        {"Fox","Card","Deck","Win","Owl","Spade","Heart","Club","Gold","Night"},
        {"Kozak","Kyiv","Lviv","Sich","Dnpr","Lutsk","Step","Ukr","Volyn","Mavka"},
        {"Rhein","Karo","Herz","Pik","Nord","Karte","Berl","Konig","Wald","As"},
        {"Karty","Wawa","Kier","Trefl","Gdan","Krak","As","Orzel","Wisla","Zloty"},
        {"Sol","As","Sur","Luna","Oro","Naipe","Madrid","Carta","Rey","Costa"},
        {"Paris","Coeur","Lyon","Pique","Nord","Carte","Roi","Lune","As","Loire"},
        {"Roma","Asso","Cuori","Milan","Carte","Fiori","Re","Luna","Oro","Venez"},
        {"Lisbo","Copas","Rei","Porto","Carta","Ouro","As","Lusa","Tejo","Norte"}
    };
    static const char* const tags[UI_LANG_COUNT][6] = {
        {"7","9","X","Q","Pro","Go"}, {"UA","7","X","Pro","Win","Go"},
        {"DE","7","X","Pro","Top","Go"}, {"PL","7","X","Pro","Top","Gra"},
        {"ES","7","X","Pro","Top","Go"}, {"FR","7","X","Pro","Top","Jeu"},
        {"IT","7","X","Pro","Top","Re"}, {"PT","7","X","Pro","Top","Jg"}
    };
    static char result[24];
    unsigned int id = seed < 0 ? (unsigned int)(-(long long)seed)
                               : (unsigned int)seed;
    id %= 600u;
    snprintf(result, sizeof(result), "%s%s%s",
             roots[g_ui_language][id % 10u],
             motifs[g_ui_language][(id / 10u) % 10u],
             tags[g_ui_language][(id / 100u) % 6u]);
    return result;
}

const char* ui_cats_reward_line(int line) {
    static const char* const reward[UI_LANG_COUNT][2] = {
        {"CAT DECK", "UNLOCKED!"},
        {"\xCA\xCE\xD2\xDF\xD7\xD3 \xCA\xCE\xCB\xCE\xC4\xD3",
         "\xD0\xCE\xC7\xC1\xCB\xCE\xCA\xCE\xC2\xC0\xCD\xCE!"},
        {"KATZENDECK", "FREIGESCHALTET!"},
        {"TALIA KOTOW", "ODBLOKOWANA!"},
        {"BARAJA DE GATOS", "DESBLOQUEADA!"},
        {"JEU DE CHATS", "DEBLOQUE!"},
        {"MAZZO DI GATTI", "SBLOCCATO!"},
        {"BARALHO DE GATOS", "DESBLOQUEADO!"}
    };
    return reward[g_ui_language][line == 1 ? 1 : 0];
}

const char* ui_tournament_round_name_localized(const TournamentState* t) {
    if (!t) return ui_tr(UI_STR_TOURNAMENT);
    if (t->round == 0) return ui_tr(UI_STR_QUARTERFINAL);
    if (t->round == 1) return ui_tr(UI_STR_SEMIFINAL);
    if (t->round == 2) return ui_tr(UI_STR_FINAL);
    return ui_tr(UI_STR_CHAMPION);
}


static void ui_draw_glyph8(HalTexture* font, unsigned int glyph, int x, int y) {
    const int sx = (int)(glyph % 16u) * 8;
    const int sy = (int)(glyph / 16u) * 8;
    hal_draw_sprite(font, sx, sy, 8, 8, x, y);
}

/* Language names use three single-byte extension markers.  Some old SDL and
 * Retro-Go asset paths silently skipped atlas cells above ASCII even though
 * their advance was still applied.  Compose these three glyphs from the
 * always-present ASCII letters, so ESPAÑOL / FRANÇAIS / PORTUGUÊS render
 * identically on desktop and hardware without depending on extended cells. */
static void ui_draw_language_glyph(HalTexture* font, unsigned int glyph,
                                   int x, int y) {
    const unsigned int white = 0xFFFFFFu;
    if (glyph == 0x8Du) {                 /* Ñ */
        ui_draw_glyph8(font, (unsigned int)'N', x, y + 1);
        hal_fill_rect(x + 2, y, 2, 1, white);
        hal_fill_rect(x + 4, y + 1, 2, 1, white);
        return;
    }
    if (glyph == 0x86u) {                 /* Ç */
        ui_draw_glyph8(font, (unsigned int)'C', x, y);
        hal_fill_rect(x + 3, y + 7, 2, 1, white);
        hal_fill_rect(x + 4, y + 6, 1, 1, white);
        return;
    }
    if (glyph == 0x89u) {                 /* Ê */
        ui_draw_glyph8(font, (unsigned int)'E', x, y + 1);
        hal_fill_rect(x + 3, y, 1, 1, white);
        hal_fill_rect(x + 2, y + 1, 1, 1, white);
        hal_fill_rect(x + 4, y + 1, 1, 1, white);
        return;
    }
    ui_draw_glyph8(font, glyph, x, y);
}

static void ui_text(HalTexture* font, const char* text, int x, int y) {
    if (!font || !text) return;
    for (int i = 0; text[i]; i++) {
        const unsigned int glyph = (unsigned char)text[i];
        ui_draw_language_glyph(font, glyph, x + i * 8, y);
    }
}

static void ui_text_center(HalTexture* font, const char* text, int cx, int y) {
    ui_text(font, text, cx - ((int)strlen(text) * 8) / 2, y);
}

static int ui_portrait_corner_cut(int edge) {
    if (edge <= 0) return 3;
    if (edge == 1) return 2;
    if (edge == 2) return 1;
    return 0;
}

static void ui_rounded_outline(int x, int y, int w, int h,
                               unsigned int color) {
    if (w < 6 || h < 6) return;
    for (int row = 0; row < h; ++row) {
        int edge = row;
        if (h - 1 - row < edge) edge = h - 1 - row;
        int cut = ui_portrait_corner_cut(edge);
        if (cut * 2 >= w) cut = 0;
        if (row == 0 || row == h - 1) {
            hal_fill_rect(x + cut, y + row, w - cut * 2, 1, color);
        } else {
            hal_fill_rect(x + cut, y + row, 1, 1, color);
            hal_fill_rect(x + w - 1 - cut, y + row, 1, 1, color);
        }
    }
}

static void ui_face(HalTexture* faces, int face, int x, int y, int w, int h) {
    if (!faces) return;
    if (face < 0) face = 0;
    face %= 20;
    hal_draw_sprite_scaled(faces, face * FACE_W, 0, FACE_W, FACE_H, x, y, w, h);
    ui_rounded_outline(x - 1, y - 1, w + 2, h + 2, 0x603517u);
}

static void ui_player_face(HalTexture* faces, int face,
                           int x, int y, int w, int h) {
    if (!faces) return;
    if (face < 0) face = 0;
    face %= 2;
    hal_draw_sprite_scaled(faces, face * FACE_W, 0, FACE_W, FACE_H, x, y, w, h);
    ui_rounded_outline(x - 1, y - 1, w + 2, h + 2, 0x603517u);
}

static void ui_line_h(int x1, int x2, int y, unsigned int c) {
    if (x2 < x1) { int t=x1; x1=x2; x2=t; }
    hal_fill_rect(x1, y, x2 - x1 + 1, 1, c);
}

static void ui_line_v(int x, int y1, int y2, unsigned int c) {
    if (y2 < y1) { int t=y1; y1=y2; y2=t; }
    hal_fill_rect(x, y1, 1, y2 - y1 + 1, c);
}

void ui_draw_language_select(HalTexture* font, int selected_language) {
    const int ys[UI_LANG_COUNT] = { 48, 68, 88, 108, 128, 148, 168, 188 };

    if (selected_language < 0 || selected_language >= UI_LANG_COUNT)
        selected_language = UI_LANG_ENGLISH;
    ui_text_center(font, "SELECT LANGUAGE", SCREEN_WIDTH / 2, 22);
    for (int i = 0; i < UI_LANG_COUNT; ++i) {
        int w = (int)strlen(ui_language_label(i)) * 8;
        int x = (SCREEN_WIDTH - w) / 2;
        if (i == selected_language) ui_text(font, ">", x - 16, ys[i]);
        ui_text(font, ui_language_label(i), x, ys[i]);
    }
    ui_text_center(font, "UP/DOWN  A SELECT", SCREEN_WIDTH / 2, 218);
}

void ui_draw_difficulty_select(HalTexture* font, int selected_difficulty) {
    const char* labels[3] = { ui_tr(UI_STR_EASY), ui_tr(UI_STR_NORMAL), ui_tr(UI_STR_HARD) };
    const int ys[3] = { 96, 120, 144 };

    if (selected_difficulty < 0 || selected_difficulty > 2) selected_difficulty = 1;
    ui_text_center(font, ui_tr(UI_STR_DIFFICULTY), SCREEN_WIDTH / 2, 52);
    for (int i = 0; i < 3; i++) {
        int w = (int)strlen(labels[i]) * 8;
        int x = (SCREEN_WIDTH - w) / 2;
        if (i == selected_difficulty) ui_text(font, ">", x - 16, ys[i]);
        ui_text(font, labels[i], x, ys[i]);
    }
    ui_text_center(font, ui_tr(UI_STR_NAV_DPAD_SELECT), SCREEN_WIDTH / 2, 204);
}

void ui_draw_deck_size_select(HalTexture* font, int selected_deck_size) {
    const char* labels[2] = { "36", "52" };
    const int ys[2] = { 108, 140 };
    const int selected = selected_deck_size == DECK_MODE_52 ? 1 : 0;
    ui_text_center(font, ui_tr(UI_STR_DECK), SCREEN_WIDTH / 2, 52);
    for (int i = 0; i < 2; ++i) {
        int w = (int)strlen(labels[i]) * 8;
        int x = (SCREEN_WIDTH - w) / 2;
        if (i == selected) ui_text(font, ">", x - 16, ys[i]);
        ui_text(font, labels[i], x, ys[i]);
    }
    ui_text_center(font, ui_tr(UI_STR_NAV_UPDOWN_SELECT), SCREEN_WIDTH / 2, 204);
}

void ui_draw_mode_select(HalTexture* font, int selected_mode) {
    const char* labels[4] = {
        ui_tr(UI_STR_CAREER), ui_tr(UI_STR_TOURNAMENT),
        ui_mode_name_localized(MODE_RANDOM_BATTLE), ui_tr(UI_STR_BATTLE_ROYAL)
    };
    const int ys[4] = { 84, 108, 132, 156 };

    if (selected_mode < 0 || selected_mode > 3) selected_mode = 0;
    ui_text_center(font, ui_tr(UI_STR_SELECT_MODE), SCREEN_WIDTH / 2, 52);
    for (int i = 0; i < 4; i++) {
        int w = (int)strlen(labels[i]) * 8;
        int x = (SCREEN_WIDTH - w) / 2;
        if (i == selected_mode) ui_text(font, ">", x - 16, ys[i]);
        ui_text(font, labels[i], x, ys[i]);
    }
    ui_text_center(font, ui_tr(UI_STR_NAV_UPDOWN_SELECT), SCREEN_WIDTH / 2, 204);
}


void ui_draw_battle_royal_players(HalTexture* font, int selected_count) {
    const char* labels[2] = { ui_tr(UI_STR_PLAYERS_3), ui_tr(UI_STR_PLAYERS_4) };
    const int ys[2] = { 108, 140 };
    if (selected_count != 4) selected_count = 3;
    ui_text_center(font, ui_tr(UI_STR_BATTLE_ROYAL), SCREEN_WIDTH / 2, 52);
    for (int i = 0; i < 2; ++i) {
        int count = i + 3;
        int w = (int)strlen(labels[i]) * 8;
        int x = (SCREEN_WIDTH - w) / 2;
        if (count == selected_count) ui_text(font, ">", x - 16, ys[i]);
        ui_text(font, labels[i], x, ys[i]);
    }
    ui_text_center(font, ui_tr(UI_STR_NAV_UPDOWN_SELECT), SCREEN_WIDTH / 2, 204);
}

static int ui_floor_q8(int value) {
    if (value >= 0) return value / 256;
    return -((-value + 255) / 256);
}

static int ui_face_wrap(int index) {
    index %= 2;
    if (index < 0) index += 2;
    return index;
}

void ui_draw_avatar_select(HalTexture* font, HalTexture* faces,
                           int selected_face, int scroll_q8) {
    ui_text_center(font, ui_tr(UI_STR_CHOOSE_FACE), SCREEN_WIDTH / 2, 24);

    const int center_x = SCREEN_WIDTH / 2;
    const int face_y = 82;
    const int spacing = 61;
    const int base = ui_floor_q8(scroll_q8);

    /* The player profile intentionally has only the two neutral Android
     * silhouettes: male and female. Story NPC portraits never enter here. */
    for (int offset = -2; offset <= 2; ++offset) {
        const int unwrapped = base + offset;
        const int rel_q8 = unwrapped * 256 - scroll_q8;
        const int x = center_x - FACE_W / 2 +
                      (rel_q8 * spacing) / 256;
        if (x + FACE_W < 0 || x >= SCREEN_WIDTH) continue;
        ui_player_face(faces, ui_face_wrap(unwrapped),
                       x, face_y, FACE_W, FACE_H);
    }

    ui_rounded_outline(center_x - FACE_W / 2 - 2, face_y - 2,
                       FACE_W + 4, FACE_H + 4, 0xFFD84Au);

    char buf[32];
    snprintf(buf, sizeof(buf), "%s %d / 2",
             ui_tr(UI_STR_YOU), ui_face_wrap(selected_face) + 1);
    ui_text_center(font, buf, SCREEN_WIDTH / 2, 154);
    ui_text_center(font, ui_tr(UI_STR_NAV_DPAD_SELECT),
                   SCREEN_WIDTH / 2, 204);
}


static void draw_name_short(HalTexture* font, const TournamentPlayer* player, int x, int y) {
    const char* shown = (player && player->is_player) ? ui_tr(UI_STR_YOU)
                                                    : (player ? ui_ai_name(player->face_idx) : "?");
    char buf[8];
    int i = 0;
    while (shown && shown[i] && i < 7) { buf[i] = shown[i]; i++; }
    buf[i] = '\0';
    ui_text(font, buf, x, y);
}

static void draw_tournament_face(HalTexture* faces, HalTexture* player_faces,
                                 const TournamentPlayer* player,
                                 int x, int y, int w, int h) {
    if (!player) return;
    if (player->is_player)
        ui_player_face(player_faces, player->face_idx & 1, x, y, w, h);
    else
        ui_face(faces, player->face_idx, x, y, w, h);
}

void ui_draw_tournament_bracket(HalTexture* font, HalTexture* faces,
                                HalTexture* player_faces,
                                const TournamentState* t) {
    const unsigned int line = 0x65707A;
    const unsigned int active = 0xFFD84A;
    ui_text_center(font, ui_tournament_round_name_localized(t), SCREEN_WIDTH / 2, 4);
    ui_text(font, "QF", 6, 18);
    ui_text(font, "SF", 110, 18);
    ui_text(font, ui_tr(UI_STR_FINAL), 196, 18);

    /* Quarterfinal entrants: 8 rows, each adjacent pair is a match. */
    for (int i = 0; i < 8; i++) {
        int y = 30 + i * 25;
        const TournamentPlayer* p = &t->players[i];
        draw_tournament_face(faces, player_faces, p, 4, y, 18, 18);
        draw_name_short(font, p, 25, y + 5);
        if (p->is_player) {
            hal_fill_rect(2, y - 1, 1, 20, active);
        }
    }

    for (int m = 0; m < 4; m++) {
        int y1 = 39 + (m * 2) * 25;
        int y2 = 39 + (m * 2 + 1) * 25;
        int ym = (y1 + y2) / 2;
        ui_line_h(74, 91, y1, line);
        ui_line_h(74, 91, y2, line);
        ui_line_v(91, y1, y2, line);
        ui_line_h(91, 106, ym, line);

        int w = t->qf_winner[m];
        if (w >= 0) {
            draw_tournament_face(faces, player_faces, &t->players[w],
                                 108, ym - 10, 20, 20);
            draw_name_short(font, &t->players[w], 130, ym - 4);
        }
    }

    /* Semifinal connectors and winners. */
    for (int m = 0; m < 2; m++) {
        int y1 = 64 + m * 100;
        int y2 = 114 + m * 100;
        int ym = (y1 + y2) / 2;
        ui_line_h(174, 186, y1, line);
        ui_line_h(174, 186, y2, line);
        ui_line_v(186, y1, y2, line);
        ui_line_h(186, 198, ym, line);

        int w = t->sf_winner[m];
        if (w >= 0) {
            draw_tournament_face(faces, player_faces, &t->players[w],
                                 200, ym - 11, 22, 22);
            draw_name_short(font, &t->players[w], 224, ym - 4);
        }
    }

    /* Final connector and optional champion. */
    ui_line_h(270, 278, 89, line);
    ui_line_h(270, 278, 189, line);
    ui_line_v(278, 89, 189, line);
    ui_line_h(278, 286, 139, line);
    if (t->champion >= 0) {
        draw_tournament_face(faces, player_faces, &t->players[t->champion],
                             288, 125, 28, 28);
    }

    if (t->round < 3) ui_text_center(font, ui_tr(UI_STR_START_MATCH), SCREEN_WIDTH / 2, 230);
}

void ui_draw_tournament_champion(HalTexture* font, HalTexture* faces,
                                 HalTexture* player_faces,
                                 const TournamentState* t, int frame) {
    static const int pos[][2] = {
        {16, 26}, {132, 20}, {244, 34}, {54, 78}, {206, 82},
        {12, 150}, {136, 166}, {246, 146}, {58, 208}, {218, 208}
    };
    int n = (int)(sizeof(pos) / sizeof(pos[0]));
    for (int i = 0; i < n; i++) {
        if (((frame / 8) + i) % 3 != 0) ui_text(font, ui_tr(UI_STR_WIN), pos[i][0], pos[i][1]);
    }

    ui_text_center(font, ui_tr(UI_STR_CHAMPION), SCREEN_WIDTH / 2, 104);
    if (t && t->champion >= 0) {
        draw_tournament_face(faces, player_faces, &t->players[t->champion],
                             133, 120, 55, 55);
    }
    ui_text_center(font, ui_tr(UI_STR_PRESS_ANY_KEY), SCREEN_WIDTH / 2, 224);
}
