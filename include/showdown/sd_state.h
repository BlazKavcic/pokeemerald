// Shared state between the lobby (pre-battle) and the in-battle driver.
#ifndef GUARD_SHOWDOWN_SD_STATE_H
#define GUARD_SHOWDOWN_SD_STATE_H

#include "global.h"
#include "showdown/sd_proto.h"

struct SdOppMon
{
    u16 species;   // National Dex number
    u8 level;
    u8 gender;
    u8 hpPercent;
    u8 status;
    bool8 revealed;
};

struct SdState
{
    u8 phase;                          // SdPhase from the bridge
    bool8 inBattle;
    bool8 haveRequest;                 // a decision request is pending for the driver
    struct SdRequest request;
    struct SdPartySlot party[SD_PARTY_SIZE];
    bool8 partyKnown[SD_PARTY_SIZE];
    struct SdOppMon opp[SD_PARTY_SIZE];
    s8 myActive;                       // party slot on the field, -1 none
    s8 oppActive;
    u8 opponentName[SD_NAME_MAX + 1];  // ASCII
    u8 result;                         // SdResult once the battle ended
};

extern struct SdState gSdState;

// Applies a message to gSdState (pure bookkeeping, no display). Returns FALSE if ignored.
bool8 SdStateApply(const struct SdMessage *m);
void SdStateReset(void);

// Mirrors gSdState into gPlayerParty / gEnemyParty and gBattlerPartyIndexes.
void SdBuildParties(void);
void SdWritePlayerPartySlot(u8 slot);
void SdWriteEnemyPartySlot(u8 slot);

// ASCII -> Emerald text with word wrapping for the battle message box.
void SdConvertText(const u8 *ascii, u8 *dst, u32 dstCap);
u32 SdStatus1FromShowdown(u8 showdownStatus);

#endif // GUARD_SHOWDOWN_SD_STATE_H
