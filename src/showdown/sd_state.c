#include "showdown/sd_state.h"
#include "showdown/sd_charmap.h"
#include "showdown/showdown.h"
#include "battle.h"
#include "pokemon.h"
#include "string_util.h"
#include "constants/characters.h"
#include "constants/moves.h"
#include "constants/pokemon.h"

EWRAM_DATA struct SdState gSdState = {0};

void SdStateReset(void)
{
    s32 i;
    memset(&gSdState, 0, sizeof(gSdState));
    gSdState.myActive = -1;
    gSdState.oppActive = -1;
    for (i = 0; i < SD_PARTY_SIZE; i++)
        gSdState.opp[i].hpPercent = 100;
}

bool8 SdStateApply(const struct SdMessage *m)
{
    switch (m->type)
    {
    case SD_MSG_STATUS:
        gSdState.phase = m->phase;
        return TRUE;
    case SD_MSG_BATTLE_START:
    {
        u8 phase = gSdState.phase;
        SdStateReset();
        gSdState.phase = phase;
        gSdState.inBattle = TRUE;
        memcpy(gSdState.opponentName, m->battleStart.opponent, sizeof(gSdState.opponentName));
        return TRUE;
    }
    case SD_MSG_PARTY_SLOT:
        gSdState.party[m->partySlot.slot] = m->partySlot;
        gSdState.partyKnown[m->partySlot.slot] = TRUE;
        return TRUE;
    case SD_MSG_HP_UPDATE:
        if (m->hpUpdate.side == SD_SIDE_ME)
        {
            gSdState.party[m->hpUpdate.slot].hp = m->hpUpdate.hp;
            if (m->hpUpdate.maxHp)
                gSdState.party[m->hpUpdate.slot].maxHp = m->hpUpdate.maxHp;
            gSdState.party[m->hpUpdate.slot].status = m->hpUpdate.status;
        }
        else
        {
            gSdState.opp[m->hpUpdate.slot].hpPercent = m->hpUpdate.hp > 100 ? 100 : m->hpUpdate.hp;
            gSdState.opp[m->hpUpdate.slot].status = m->hpUpdate.status;
        }
        return TRUE;
    case SD_MSG_OPP_REVEAL:
        gSdState.opp[m->oppReveal.slot].species = m->oppReveal.species;
        gSdState.opp[m->oppReveal.slot].level = m->oppReveal.level;
        gSdState.opp[m->oppReveal.slot].gender = m->oppReveal.gender;
        gSdState.opp[m->oppReveal.slot].revealed = TRUE;
        return TRUE;
    case SD_MSG_SWITCH_IN:
        if (m->switchIn.side == SD_SIDE_ME)
            gSdState.myActive = m->switchIn.slot;
        else
            gSdState.oppActive = m->switchIn.slot;
        return TRUE;
    case SD_MSG_FAINT:
        if (m->faint.side == SD_SIDE_ME)
        {
            gSdState.party[m->faint.slot].hp = 0;
            gSdState.party[m->faint.slot].status = SD_STATUS_FAINTED;
            if (gSdState.myActive == m->faint.slot)
                gSdState.myActive = -1;
        }
        else
        {
            gSdState.opp[m->faint.slot].hpPercent = 0;
            gSdState.opp[m->faint.slot].status = SD_STATUS_FAINTED;
            if (gSdState.oppActive == m->faint.slot)
                gSdState.oppActive = -1;
        }
        return TRUE;
    case SD_MSG_REQUEST:
        gSdState.request = m->request;
        gSdState.haveRequest = (m->request.kind == SD_REQUEST_MOVE || m->request.kind == SD_REQUEST_FORCE_SWITCH);
        return TRUE;
    case SD_MSG_BATTLE_END:
        gSdState.result = m->result;
        gSdState.inBattle = FALSE;
        gSdState.haveRequest = FALSE;
        return TRUE;
    default:
        return TRUE;
    }
}

// ---- parties ----------------------------------------------------------------------------

static u16 DexToSpecies(u16 dex)
{
    if (dex >= 387 && dex <= 389) dex = 351; // Castform forms
    else if (dex >= 390 && dex <= 392) dex = 386; // Deoxys forms
    if (dex < 1 || dex > 386)
        return SPECIES_NONE;
    return NationalPokedexNumToSpecies(dex);
}

static u8 ToMonGender(u8 g)
{
    if (g == SD_GENDER_MALE) return MON_MALE;
    if (g == SD_GENDER_FEMALE) return MON_FEMALE;
    return MON_GENDERLESS;
}

static u32 ToStatus1(u8 s)
{
    switch (s)
    {
    case SD_STATUS_BURN: return STATUS1_BURN;
    case SD_STATUS_FREEZE: return STATUS1_FREEZE;
    case SD_STATUS_PARALYSIS: return STATUS1_PARALYSIS;
    case SD_STATUS_POISON: return STATUS1_POISON;
    case SD_STATUS_TOXIC: return STATUS1_TOXIC_POISON;
    case SD_STATUS_SLEEP: return 2; // sleep turns
    default: return 0;
    }
}

u32 SdStatus1FromShowdown(u8 s)
{
    return ToStatus1(s);
}

void SdWritePlayerPartySlot(u8 slot)
{
    const struct SdPartySlot *p = &gSdState.party[slot];
    struct Pokemon *mon = &gPlayerParty[slot];
    u16 species = DexToSpecies(p->species);
    u8 gender = ToMonGender(p->gender);
    u32 status;
    u8 ppBonuses = 0xFF; // three PP Ups on every move, matching Showdown's max PP
    u8 abilityNum;
    s32 i;
    u16 hp, maxHp;

    if (species == SPECIES_NONE)
    {
        ZeroMonData(mon);
        return;
    }
    // Showdown's gender is the truth; Emerald derives it from the personality value.
    if (gender == MON_GENDERLESS || GetGenderFromSpeciesAndPersonality(species, 0) == MON_GENDERLESS)
        CreateMon(mon, species, p->level, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
    else
        CreateMonWithGenderNatureLetter(mon, species, p->level, MAX_PER_STAT_IVS, gender, NATURE_HARDY, 0);

    for (i = 0; i < SD_MOVE_SLOTS; i++)
    {
        u16 move = p->moves[i].id <= MOVES_COUNT ? p->moves[i].id : MOVE_NONE;
        u8 pp = p->moves[i].pp;
        SetMonData(mon, MON_DATA_MOVE1 + i, &move);
        SetMonData(mon, MON_DATA_PP1 + i, &pp);
    }
    SetMonData(mon, MON_DATA_PP_BONUSES, &ppBonuses);
    abilityNum = (p->ability != 0 && gSpeciesInfo[species].abilities[1] == p->ability) ? 1 : 0;
    SetMonData(mon, MON_DATA_ABILITY_NUM, &abilityNum);
    maxHp = p->maxHp ? p->maxHp : 1;
    hp = p->hp > maxHp ? maxHp : p->hp;
    SetMonData(mon, MON_DATA_MAX_HP, &maxHp);
    SetMonData(mon, MON_DATA_HP, &hp);
    status = (p->status == SD_STATUS_FAINTED) ? 0 : ToStatus1(p->status);
    SetMonData(mon, MON_DATA_STATUS, &status);
}

void SdWriteEnemyPartySlot(u8 slot)
{
    const struct SdOppMon *o = &gSdState.opp[slot];
    struct Pokemon *mon = &gEnemyParty[slot];
    u16 species = DexToSpecies(o->species);
    u8 gender = ToMonGender(o->gender);
    u16 maxHp = 100, hp;
    u32 status;
    u16 none = MOVE_NONE;
    s32 i;

    if (!o->revealed || species == SPECIES_NONE)
    {
        ZeroMonData(mon);
        return;
    }
    if (gender == MON_GENDERLESS || GetGenderFromSpeciesAndPersonality(species, 0) == MON_GENDERLESS)
        CreateMon(mon, species, o->level, USE_RANDOM_IVS, FALSE, 0, OT_ID_PRESET, 0x12345678);
    else
        CreateMonWithGenderNatureLetter(mon, species, o->level, MAX_PER_STAT_IVS, gender, NATURE_HARDY, 0);
    for (i = 0; i < SD_MOVE_SLOTS; i++)
        SetMonData(mon, MON_DATA_MOVE1 + i, &none);
    hp = o->hpPercent > 100 ? 100 : o->hpPercent;
    SetMonData(mon, MON_DATA_MAX_HP, &maxHp);
    SetMonData(mon, MON_DATA_HP, &hp);
    status = (o->status == SD_STATUS_FAINTED) ? 0 : ToStatus1(o->status);
    SetMonData(mon, MON_DATA_STATUS, &status);
}

void SdBuildParties(void)
{
    s32 i;
    for (i = 0; i < SD_PARTY_SIZE; i++)
    {
        if (gSdState.partyKnown[i])
            SdWritePlayerPartySlot(i);
        else
            ZeroMonData(&gPlayerParty[i]);
        SdWriteEnemyPartySlot(i);
    }
    gPlayerPartyCount = 0;
    for (i = 0; i < SD_PARTY_SIZE; i++)
        if (gSdState.partyKnown[i]) gPlayerPartyCount++;
}

// ---- text ---------------------------------------------------------------------------------

#define SD_LINE_MAX 28

void SdConvertText(const u8 *ascii, u8 *dst, u32 dstCap)
{
    u32 out = 0, lineLen = 0, lines = 0;
    u32 i = 0;
    while (ascii[i] && out + 2 < dstCap)
    {
        // measure the next word
        u32 w = 0;
        while (ascii[i + w] && ascii[i + w] != ' ') w++;
        if (lineLen > 0 && lineLen + 1 + w > SD_LINE_MAX)
        {
            dst[out++] = (lines == 0) ? CHAR_NEWLINE : CHAR_PROMPT_SCROLL;
            lines++;
            lineLen = 0;
        }
        else if (lineLen > 0)
        {
            dst[out++] = CHAR_SPACE;
            lineLen++;
        }
        while (w-- && out + 1 < dstCap)
        {
            u8 c = ascii[i++];
            dst[out++] = (c < 128) ? sAsciiToEmerald[c] : CHAR_QUESTION_MARK;
            lineLen++;
        }
        while (ascii[i] == ' ') i++;
    }
    dst[out] = EOS;
}
