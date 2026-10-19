// Showdown battle mode: the engine's display layer driven by Pokémon Showdown through
// the showdownGBA bridge. See docs/ARCHITECTURE.md in the parent repository.
#ifndef GUARD_SHOWDOWN_SHOWDOWN_H
#define GUARD_SHOWDOWN_SHOWDOWN_H

#include "global.h"

// Battle string id handled specially in BufferStringBattle: prints gShowdownText verbatim.
#define STRINGID_SHOWDOWN_TEXT 0xFFFE

extern bool8 gShowdownBattle;              // TRUE while a Showdown-driven battle runs
extern u8 gShowdownText[160];                 // Emerald-encoded line(s) for STRINGID_SHOWDOWN_TEXT
extern u8 gShowdownOpponentName[20];         // Emerald-encoded opponent name (B_TXT_TRAINER1_NAME)

void Showdown_StartFromMainMenu(void);     // SELECT on the main menu
void CB2_ShowdownLobby(void);
void ShowdownBattleMain(void);             // replaces the turn state machine (gBattleMainFunc)
void ShowdownFinishBattle(u8 outcome);     // implemented in battle_main.c

#endif // GUARD_SHOWDOWN_SHOWDOWN_H
