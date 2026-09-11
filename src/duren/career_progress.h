#ifndef CAREER_PROGRESS_H
#define CAREER_PROGRESS_H

#include "modes.h"

bool career_progress_load(ModeSession* session, GameState* game);
bool career_progress_save(const ModeSession* session, const GameState* game);

#endif
