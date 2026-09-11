#ifndef UI_MODES_H
#define UI_MODES_H

#include "hal.h"
#include "tournament.h"

typedef enum {
    UI_LANG_ENGLISH = 0,
    UI_LANG_UKRAINIAN,
    UI_LANG_GERMAN,
    UI_LANG_POLISH,
    UI_LANG_SPANISH,
    UI_LANG_FRENCH,
    UI_LANG_ITALIAN,
    UI_LANG_PORTUGUESE,
    UI_LANG_COUNT
} UiLanguage;

typedef enum {
    UI_STR_SELECT_LANGUAGE,
    UI_STR_NAV_UPDOWN_SELECT,
    UI_STR_NAV_DPAD_SELECT,
    UI_STR_DIFFICULTY,
    UI_STR_EASY,
    UI_STR_NORMAL,
    UI_STR_HARD,
    UI_STR_SELECT_MODE,
    UI_STR_CAREER,
    UI_STR_TOURNAMENT,
    UI_STR_RANDOM_BATTLE,
    UI_STR_BATTLE_ROYAL,
    UI_STR_PLAYERS_3,
    UI_STR_PLAYERS_4,
    UI_STR_CHOOSE_FACE,
    UI_STR_FACE,
    UI_STR_FINAL,
    UI_STR_START_MATCH,
    UI_STR_WIN,
    UI_STR_CHAMPION,
    UI_STR_PRESS_ANY_KEY,
    UI_STR_PAUSED,
    UI_STR_CONTINUE,
    UI_STR_SAVE_GAME,
    UI_STR_LOAD_GAME,
    UI_STR_GO_BACK_MENU,
    UI_STR_WAIT_ACTION,
    UI_STR_GAME_SAVED,
    UI_STR_SAVE_FAILED,
    UI_STR_BAD_SAVE,
    UI_STR_EMPTY_SLOT,
    UI_STR_LOAD_FAILED,
    UI_STR_BACK,
    UI_STR_OVERWRITE_SLOT,
    UI_STR_LOAD_SLOT,
    UI_STR_CURRENT_PROGRESS_LOST,
    UI_STR_YES,
    UI_STR_NO,
    UI_STR_SLOT,
    UI_STR_EMPTY,
    UI_STR_INCOMPATIBLE,
    UI_STR_YOU,
    UI_STR_CASH,
    UI_STR_LEVEL_SHORT,
    UI_STR_ROUND,
    UI_STR_DECK,
    UI_STR_YOU_WIN,
    UI_STR_YOU_LOST,
    UI_STR_GAME_OVER,
    UI_STR_TOURNAMENT_OVER,
    UI_STR_CAREER_COMPLETE,
    UI_STR_BR_WINNER,
    UI_STR_PRESS_A_RETURN,
    UI_STR_OUT,
    UI_STR_DONE,
    UI_STR_LOSER,
    UI_STR_PRESS_A_WINNER,
    UI_STR_PRESS_A_START,
    UI_STR_NEXT_ROUND,
    UI_STR_MODE,
    UI_STR_QUARTERFINAL,
    UI_STR_SEMIFINAL,
    UI_STR_ACTION_BEAT,
    UI_STR_ACTION_TAKE,
    UI_STR_OPTIONS,
    UI_STR_SOUND,
    UI_STR_GRAPHICS,
    UI_STR_EFFECTS,
    UI_STR_MUSIC,
    UI_STR_SHADOWS,
    UI_STR_PARTICLES,
    UI_STR_SNOW,
    UI_STR_SNOWFLAKES,
    UI_STR_ON,
    UI_STR_OFF,
    UI_STR_HOLD_A_FAST,
    UI_STR_FAST_FORWARD_X4,
    UI_STR_FINISHED_FIRST,
    UI_STR_AI_NAME_0,
    UI_STR_AI_NAME_1,
    UI_STR_AI_NAME_2,
    UI_STR_AI_NAME_3,
    UI_STR_AI_NAME_4,
    UI_STR_AI_NAME_5,
    UI_STR_AI_NAME_6,
    UI_STR_AI_NAME_7,
    UI_STR_AI_NAME_8,
    UI_STR_AI_NAME_9,
    UI_STR_AI_NAME_10,
    UI_STR_AI_NAME_11,
    UI_STR_AI_NAME_12,
    UI_STR_AI_NAME_13,
    UI_STR_AI_NAME_14,
    UI_STR_AI_NAME_15,
    UI_STR_AI_NAME_16,
    UI_STR_AI_NAME_17,
    UI_STR_AI_NAME_18,
    UI_STR_AI_NAME_19,
    UI_STR_DIALOGUES,
    UI_STR_LANGUAGE,
    UI_STR_TABLE_COLOUR,
    UI_STR_DARK,
    UI_STR_LIGHT,
    UI_STR_GAMEPLAY,
    UI_STR_AUTO_SORT,
    UI_STR_SOFT_MOSS,
    UI_STR_SUPER_LIGHT,
    UI_STR_BLUE,
    UI_STR_THROW_IN_DONE,
    UI_STR_GAME_SPEED,
    UI_STR_MENU_CAREER_DESC_1,
    UI_STR_MENU_CAREER_DESC_2,
    UI_STR_MENU_ONLINE_DESC_1,
    UI_STR_MENU_ONLINE_DESC_2,
    UI_STR_MENU_TOURNAMENT_DESC_1,
    UI_STR_MENU_TOURNAMENT_DESC_2,
    UI_STR_MENU_BR_DESC_1,
    UI_STR_MENU_BR_DESC_2,
    UI_STR_CARDS_LABEL,
    UI_STR_DECK_ANGLO_FR,
    UI_STR_DECK_ATLAS,
    UI_STR_DECK_CATS,
    UI_STR_COUNT
} UiStringId;

void ui_set_language(int language);
int ui_get_language(void);
bool ui_language_uses_cp1251(int language);
const char* ui_tr(UiStringId id);
const char* ui_mode_name_localized(int mode);
const char* ui_difficulty_name_localized(int difficulty);
const char* ui_ai_name(int face_index);
const char* ui_online_bot_name(int seed);
const char* ui_cats_reward_line(int line);
const char* ui_language_label(int language);
const char* ui_tournament_round_name_localized(const TournamentState* t);

void ui_draw_language_select(HalTexture* font_tex, int selected_language);
void ui_draw_difficulty_select(HalTexture* font_tex, int selected_difficulty);
void ui_draw_deck_size_select(HalTexture* font_tex, int selected_deck_size);
void ui_draw_mode_select(HalTexture* font_tex, int selected_mode);
void ui_draw_battle_royal_players(HalTexture* font_tex, int selected_count);
void ui_draw_avatar_select(HalTexture* font_tex, HalTexture* faces_tex,
                           int selected_face, int scroll_q8);
void ui_draw_tournament_bracket(HalTexture* font_tex, HalTexture* faces_tex,
                                HalTexture* player_faces_tex,
                                const TournamentState* t);
void ui_draw_tournament_champion(HalTexture* font_tex, HalTexture* faces_tex,
                                 HalTexture* player_faces_tex,
                                 const TournamentState* t, int frame);

#endif
