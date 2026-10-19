// In-battle driver: replaces the turn state machine. Bridge messages become a queue of
// battle-controller commands (the same ones battle scripts emit), executed one at a time
// while gBattleControllerExecFlags is clear, so the display layer behaves exactly as in a
// vanilla battle. Player decisions come back through the player controller's menus and
// are sent to the bridge as choices.
#include "global.h"
#include "showdown/showdown.h"
#include "showdown/sd_link.h"
#include "showdown/sd_proto.h"
#include "showdown/sd_state.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_controllers.h"
#include "battle_message.h"
#include "pokemon.h"
#include "string_util.h"
#include "sound.h"
#include "util.h"
#include "constants/abilities.h"
#include "constants/battle.h"
#include "constants/battle_string_ids.h"
#include "constants/moves.h"
#include "constants/party_menu.h"
#include "constants/songs.h"

EWRAM_DATA bool8 gShowdownBattle = FALSE;
EWRAM_DATA u8 gShowdownText[160] = {0};
EWRAM_DATA u8 gShowdownOpponentName[SD_NAME_MAX + 2] = {0};

enum SdOp
{
    OP_NONE,
    OP_PRINT_RAW,       // arg: text slot
    OP_PRINT_ID,        // arg: string id; battler: attacker for placeholders
    OP_PAUSE,           // arg: frames
    OP_GETMONDATA,      // battler
    OP_COPYMONDATA,     // battler: gBattleBufferB -> gBattleMons (after GETMONDATA)
    OP_RETURN_TO_BALL,  // battler
    OP_SWITCHIN_ANIM,   // battler, arg: party slot
    OP_HIT_ANIM,        // battler
    OP_HPBAR,           // battler, arg2: delta (+damage / -heal)
    OP_SET_HP,          // battler, arg: new hp
    OP_STATUS_ICON,     // battler, arg2: status1
    OP_FAINT_CRY,       // battler
    OP_FAINT_ANIM,      // battler
    OP_MOVE_ANIM,       // battler, arg: move
    OP_CHOOSE_ACTION,
    OP_CHOOSE_MOVE,
    OP_CHOOSE_POKEMON,  // arg: PARTY_ACTION_*
    OP_LINK_STANDBY,
    OP_END_BATTLE,      // arg: outcome
};

struct SdOpItem { u8 op; u8 battler; u16 arg; s32 arg2; };

#define OP_QUEUE 48
#define TEXT_SLOTS 12
#define TEXT_SLOT_LEN 64

enum SdWait { WAIT_NONE, WAIT_EXEC, WAIT_PAUSE, WAIT_ACTION, WAIT_MOVE, WAIT_POKEMON };

static EWRAM_DATA struct SdOpItem sQueue[OP_QUEUE] = {0};
static EWRAM_DATA u8 sQHead = 0;
static EWRAM_DATA u8 sQCount = 0;
static EWRAM_DATA u8 sTexts[TEXT_SLOTS][TEXT_SLOT_LEN] = {0};
static EWRAM_DATA u8 sTextNext = 0;
static EWRAM_DATA u8 sWait = WAIT_NONE;
static EWRAM_DATA u16 sPauseFrames = 0;
static EWRAM_DATA u16 sPendingAttackMove = 0;
static EWRAM_DATA u8 sPendingAttackBattler = 0xFF;
static EWRAM_DATA bool8 sForcedSwitch = FALSE;
static EWRAM_DATA bool8 sEnded = FALSE;
static EWRAM_DATA u16 sLastMyHp[SD_PARTY_SIZE] = {0};
static EWRAM_DATA u8 sLastOppHp[SD_PARTY_SIZE] = {0};
static EWRAM_DATA u8 sLastMyStatus[SD_PARTY_SIZE] = {0};
static EWRAM_DATA u8 sLastOppStatus[SD_PARTY_SIZE] = {0};

#define MY_BATTLER 0
#define OPP_BATTLER 1
#define PAUSE_AFTER_TEXT 40

static void Push(u8 op, u8 battler, u16 arg, s32 arg2)
{
    u8 tail;
    if (sQCount >= OP_QUEUE)
        return;
    tail = (sQHead + sQCount) % OP_QUEUE;
    sQueue[tail].op = op;
    sQueue[tail].battler = battler;
    sQueue[tail].arg = arg;
    sQueue[tail].arg2 = arg2;
    sQCount++;
}

static void PushText(const u8 *ascii)
{
    u8 slot = sTextNext;
    sTextNext = (sTextNext + 1) % TEXT_SLOTS;
    SdConvertText(ascii, sTexts[slot], TEXT_SLOT_LEN);
    Push(OP_PRINT_RAW, MY_BATTLER, slot, 0);
    Push(OP_PAUSE, 0, PAUSE_AFTER_TEXT, 0);
}

static void PushString(u16 stringId, u8 battler)
{
    Push(OP_PRINT_ID, battler, stringId, 0);
    Push(OP_PAUSE, 0, PAUSE_AFTER_TEXT, 0);
}

static u8 BattlerForSide(u8 side)
{
    return side == SD_SIDE_ME ? MY_BATTLER : OPP_BATTLER;
}

// ---- ASCII helpers (the bridge sends plain ASCII) ---------------------------------------

static bool8 AsciiContains(const u8 *s, const char *needle)
{
    u32 i, j;
    for (i = 0; s[i]; i++)
    {
        for (j = 0; needle[j] && s[i + j] == (u8)needle[j]; j++) ;
        if (!needle[j])
            return TRUE;
    }
    return FALSE;
}

static bool8 AsciiStartsWith(const u8 *s, const char *prefix)
{
    u32 i;
    for (i = 0; prefix[i]; i++)
        if (s[i] != (u8)prefix[i])
            return FALSE;
    return TRUE;
}

// ---- message -> ops -----------------------------------------------------------------------

static void SendChoiceMove(u8 slot)
{
    struct SdFrame f;
    SdEncodeChooseMove(&f, SdLinkNextSeq(), gSdState.request.rqid, slot);
    SdLinkSend(&f);
}

static void SendChoiceSwitch(u8 slot)
{
    struct SdFrame f;
    SdEncodeChooseSwitch(&f, SdLinkNextSeq(), gSdState.request.rqid, slot);
    SdLinkSend(&f);
}

static void HandleText(const struct SdText *t)
{
    const u8 *line = t->line;
    SdDebug("text", sPendingAttackBattler, sQCount);
    u8 battler = AsciiStartsWith(line, "The opposing ") ? OPP_BATTLER : MY_BATTLER;

    if (sPendingAttackBattler != 0xFF && AsciiContains(line, " used "))
    {
        // "<mon> used <move>!" -> vanilla string + the real move animation
        Push(OP_PRINT_ID, sPendingAttackBattler, STRINGID_USEDMOVE, sPendingAttackMove);
        Push(OP_MOVE_ANIM, sPendingAttackBattler, sPendingAttackMove, 0);
        sPendingAttackBattler = 0xFF;
        return;
    }
    if (AsciiStartsWith(line, "It's super effective"))      { PushString(STRINGID_SUPEREFFECTIVE, battler); return; }
    if (AsciiStartsWith(line, "It's not very effective"))   { PushString(STRINGID_NOTVERYEFFECTIVE, battler); return; }
    if (AsciiStartsWith(line, "A critical hit"))            { PushString(STRINGID_CRITICALHIT, battler); return; }
    if (AsciiStartsWith(line, "But it failed"))             { PushString(STRINGID_BUTITFAILED, battler); return; }
    if (AsciiContains(line, " fainted!"))                   { PushString(STRINGID_ATTACKERFAINTED, battler); return; }
    if (AsciiContains(line, " avoided the attack"))         { PushString(STRINGID_PKMNAVOIDEDATTACK, battler == MY_BATTLER ? OPP_BATTLER : MY_BATTLER); return; }
    if (AsciiStartsWith(line, "Go! ") || AsciiContains(line, " sent out ") || AsciiContains(line, ", come back!"))
        return; // the switch-in / return ops print the vanilla strings
    if (AsciiStartsWith(line, "Battle started between"))
        return; // the intro already said it
    PushText(line);
}

static void HandleSwitchIn(u8 side, u8 slot)
{
    u8 battler = BattlerForSide(side);
    bool8 intro = (side == SD_SIDE_ME) ? (gBattlerPartyIndexes[MY_BATTLER] == slot && !sEnded && gSdState.phase == SD_PHASE_IN_BATTLE && sQCount == 0 && gBattleMons[MY_BATTLER].species != SPECIES_NONE && gBattleMons[MY_BATTLER].hp == gSdState.party[slot].hp && FALSE) : FALSE;
    (void)intro;
    if (side == SD_SIDE_ME)
    {
        SdWritePlayerPartySlot(slot);
        if (!(gAbsentBattlerFlags & gBitTable[battler]) && gBattleMons[battler].hp != 0)
        {
            // voluntary switch: recall the current one first
            PREPARE_MON_NICK_BUFFER(gBattleTextBuff1, battler, gBattlerPartyIndexes[battler]);
            Push(OP_RETURN_TO_BALL, battler, 0, 0);
        }
        gBattlerPartyIndexes[battler] = slot;
        Push(OP_GETMONDATA, battler, 0, 0);
        Push(OP_COPYMONDATA, battler, 0, 0);
        Push(OP_PRINT_ID, battler, STRINGID_SWITCHINMON, 0);
        Push(OP_SWITCHIN_ANIM, battler, slot, 0);
        sLastMyHp[slot] = gSdState.party[slot].hp;
        sLastMyStatus[slot] = gSdState.party[slot].status;
    }
    else
    {
        SdWriteEnemyPartySlot(slot);
        if (!(gAbsentBattlerFlags & gBitTable[battler]) && gBattleMons[battler].hp != 0)
            Push(OP_RETURN_TO_BALL, battler, 0, 0);
        gBattlerPartyIndexes[battler] = slot;
        Push(OP_GETMONDATA, battler, 0, 0);
        Push(OP_COPYMONDATA, battler, 0, 0);
        Push(OP_PRINT_ID, battler, STRINGID_INTROSENDOUT, 0);
        Push(OP_SWITCHIN_ANIM, battler, slot, 0);
        sLastOppHp[slot] = gSdState.opp[slot].hpPercent;
        sLastOppStatus[slot] = gSdState.opp[slot].status;
    }
}

static void HandleHpUpdate(const struct SdHpUpdate *h)
{
    u8 battler = BattlerForSide(h->side);
    u16 newHp, oldHp;
    u8 newStatus, oldStatus;

    if (h->side == SD_SIDE_ME)
    {
        oldHp = sLastMyHp[h->slot];
        newHp = h->hp;
        oldStatus = sLastMyStatus[h->slot];
        newStatus = h->status;
        sLastMyHp[h->slot] = newHp;
        sLastMyStatus[h->slot] = newStatus;
        if (gBattlerPartyIndexes[battler] != h->slot || gSdState.myActive != h->slot)
        {
            SdWritePlayerPartySlot(h->slot); // bench: keep the party menu accurate
            return;
        }
    }
    else
    {
        oldHp = sLastOppHp[h->slot];
        newHp = h->hp > 100 ? 100 : h->hp;
        oldStatus = sLastOppStatus[h->slot];
        newStatus = h->status;
        sLastOppHp[h->slot] = newHp;
        sLastOppStatus[h->slot] = newStatus;
        if (gBattlerPartyIndexes[battler] != h->slot || gSdState.oppActive != h->slot)
            return;
    }

    if (newHp != oldHp)
    {
        if (newHp < oldHp)
            Push(OP_HIT_ANIM, battler, 0, 0);
        Push(OP_HPBAR, battler, 0, (s32)oldHp - (s32)newHp);
        Push(OP_SET_HP, battler, newHp, 0);
    }
    if (newStatus != oldStatus && newStatus != SD_STATUS_FAINTED)
        Push(OP_STATUS_ICON, battler, 0, (s32)SdStatus1FromShowdown(newStatus));
}

static void HandleFaint(u8 side, u8 slot)
{
    u8 battler = BattlerForSide(side);
    if (gBattlerPartyIndexes[battler] != slot)
        return;
    Push(OP_FAINT_CRY, battler, 0, 0);
    Push(OP_FAINT_ANIM, battler, 0, 0);
}

static void HandleRequest(const struct SdRequest *r)
{
    if (r->kind == SD_REQUEST_MOVE)
    {
        sForcedSwitch = FALSE;
        Push(OP_CHOOSE_ACTION, MY_BATTLER, 0, 0);
    }
    else if (r->kind == SD_REQUEST_FORCE_SWITCH)
    {
        sForcedSwitch = TRUE;
        Push(OP_CHOOSE_POKEMON, MY_BATTLER, PARTY_ACTION_SEND_OUT, 0);
    }
}

static void HandleMessage(const struct SdMessage *m)
{
    SdDebug("msg", m->type, (m->type == SD_MSG_TEXT) ? 0 : (m->type == SD_MSG_SWITCH_IN || m->type == SD_MSG_FAINT) ? (m->switchIn.side << 4 | m->switchIn.slot) : (m->type == SD_MSG_HP_UPDATE) ? m->hpUpdate.hp : (m->type == SD_MSG_REQUEST) ? m->request.kind : 0);
    SdStateApply(m);
    switch (m->type)
    {
    case SD_MSG_TEXT:
        HandleText(&m->text);
        break;
    case SD_MSG_ANIM:
        if (m->anim.kind == SD_ANIM_ATTACK)
        {
            sPendingAttackBattler = BattlerForSide(m->anim.side);
            sPendingAttackMove = (m->anim.move && m->anim.move < MOVES_COUNT) ? m->anim.move : MOVE_NONE;
        }
        break;
    case SD_MSG_PARTY_SLOT:
        if (gBattlerPartyIndexes[MY_BATTLER] != m->partySlot.slot)
            SdWritePlayerPartySlot(m->partySlot.slot);
        else
        {
            // active mon: keep PP and item current without touching HP (the bar owns it)
            struct Pokemon *mon = &gPlayerParty[m->partySlot.slot];
            s32 i;
            for (i = 0; i < SD_MOVE_SLOTS; i++)
            {
                u8 pp = m->partySlot.moves[i].pp;
                SetMonData(mon, MON_DATA_PP1 + i, &pp);
                gBattleMons[MY_BATTLER].pp[i] = pp;
            }
        }
        break;
    case SD_MSG_OPP_REVEAL:
        if (gBattlerPartyIndexes[OPP_BATTLER] != m->oppReveal.slot)
            SdWriteEnemyPartySlot(m->oppReveal.slot);
        break;
    case SD_MSG_SWITCH_IN:
        HandleSwitchIn(m->switchIn.side, m->switchIn.slot);
        break;
    case SD_MSG_HP_UPDATE:
        HandleHpUpdate(&m->hpUpdate);
        break;
    case SD_MSG_FAINT:
        HandleFaint(m->faint.side, m->faint.slot);
        break;
    case SD_MSG_REQUEST:
        HandleRequest(&m->request);
        break;
    case SD_MSG_CHOICE_REJECTED:
        PushText(m->rejected.reason);
        break;
    case SD_MSG_ERROR:
        PushText(m->error.text);
        break;
    case SD_MSG_BATTLE_END:
    {
        u8 outcome = (m->result == SD_RESULT_WIN) ? B_OUTCOME_WON
                   : (m->result == SD_RESULT_TIE) ? B_OUTCOME_DREW
                   : (m->result == SD_RESULT_FORFEIT) ? B_OUTCOME_FORFEITED : B_OUTCOME_LOST;
        Push(OP_END_BATTLE, 0, outcome, 0);
        break;
    }
    default:
        break;
    }
}

// ---- op execution -------------------------------------------------------------------------

static void CopyMonData(u8 battler)
{
    u8 *monData = (u8 *)&gBattleMons[battler];
    s32 i;
    for (i = 0; i < (s32)sizeof(struct BattlePokemon); i++)
        monData[i] = gBattleBufferB[battler][4 + i];
    gBattleMons[battler].types[0] = gSpeciesInfo[gBattleMons[battler].species].types[0];
    gBattleMons[battler].types[1] = gSpeciesInfo[gBattleMons[battler].species].types[1];
    gBattleMons[battler].ability = GetAbilityBySpecies(gBattleMons[battler].species, gBattleMons[battler].abilityNum);
    gActiveBattler = battler;
    SwitchInClearSetData();
    gAbsentBattlerFlags &= ~gBitTable[battler];
}

static void EmitChooseMove(void)
{
    struct ChooseMoveStruct moveInfo;
    s32 i;
    struct Pokemon *mon = &gPlayerParty[gBattlerPartyIndexes[MY_BATTLER]];
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        moveInfo.moves[i] = gBattleMons[MY_BATTLER].moves[i];
        moveInfo.currentPP[i] = gBattleMons[MY_BATTLER].pp[i];
        moveInfo.maxPP[i] = CalculatePPWithBonus(moveInfo.moves[i], GetMonData(mon, MON_DATA_PP_BONUSES), i);
    }
    moveInfo.species = gBattleMons[MY_BATTLER].species;
    moveInfo.monTypes[0] = gBattleMons[MY_BATTLER].types[0];
    moveInfo.monTypes[1] = gBattleMons[MY_BATTLER].types[1];
    BtlController_EmitChooseMove(B_COMM_TO_CONTROLLER, FALSE, FALSE, &moveInfo);
}

static void ExecOp(const struct SdOpItem *it)
{
    SdDebug("op", it->op, it->battler << 16 | (it->arg & 0xFFFF));
    gActiveBattler = it->battler;
    switch (it->op)
    {
    case OP_PRINT_RAW:
        StringCopy(gShowdownText, sTexts[it->arg]);
        gActiveBattler = MY_BATTLER;
        BtlController_EmitPrintString(B_COMM_TO_CONTROLLER, STRINGID_SHOWDOWN_TEXT);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_PRINT_ID:
        gBattlerAttacker = it->battler;
        gBattlerTarget = it->battler ^ 1;
        gBattleScripting.battler = it->battler;
        if (it->arg2)
            gCurrentMove = gChosenMove = it->arg2;
        PREPARE_MON_NICK_BUFFER(gBattleTextBuff1, it->battler, gBattlerPartyIndexes[it->battler]);
        gActiveBattler = MY_BATTLER; // the player controller owns the message window
        BtlController_EmitPrintString(B_COMM_TO_CONTROLLER, it->arg);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_PAUSE:
        sPauseFrames = it->arg;
        sWait = WAIT_PAUSE;
        break;
    case OP_GETMONDATA:
        BtlController_EmitGetMonData(B_COMM_TO_CONTROLLER, REQUEST_ALL_BATTLE, 0);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_COPYMONDATA:
        CopyMonData(it->battler);
        break;
    case OP_RETURN_TO_BALL:
        BtlController_EmitReturnMonToBall(B_COMM_TO_CONTROLLER, FALSE);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_SWITCHIN_ANIM:
        gAbsentBattlerFlags &= ~gBitTable[it->battler];
        BtlController_EmitSwitchInAnim(B_COMM_TO_CONTROLLER, it->arg, FALSE);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_HIT_ANIM:
        BtlController_EmitHitAnimation(B_COMM_TO_CONTROLLER);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_HPBAR:
        BtlController_EmitHealthBarUpdate(B_COMM_TO_CONTROLLER, it->arg2);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_SET_HP:
    {
        u16 hp = it->arg;
        struct Pokemon *mon = (it->battler == MY_BATTLER) ? &gPlayerParty[gBattlerPartyIndexes[MY_BATTLER]]
                                                          : &gEnemyParty[gBattlerPartyIndexes[OPP_BATTLER]];
        gBattleMons[it->battler].hp = hp;
        SetMonData(mon, MON_DATA_HP, &hp);
        break;
    }
    case OP_STATUS_ICON:
    {
        u32 status1 = (u32)it->arg2;
        struct Pokemon *mon = (it->battler == MY_BATTLER) ? &gPlayerParty[gBattlerPartyIndexes[MY_BATTLER]]
                                                          : &gEnemyParty[gBattlerPartyIndexes[OPP_BATTLER]];
        gBattleMons[it->battler].status1 = status1;
        SetMonData(mon, MON_DATA_STATUS, &status1);
        BtlController_EmitStatusIconUpdate(B_COMM_TO_CONTROLLER, status1, gBattleMons[it->battler].status2);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    }
    case OP_FAINT_CRY:
        BtlController_EmitFaintingCry(B_COMM_TO_CONTROLLER);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_FAINT_ANIM:
        gBattleMons[it->battler].hp = 0;
        BtlController_EmitFaintAnimation(B_COMM_TO_CONTROLLER);
        MarkBattlerForControllerExec(gActiveBattler);
        gAbsentBattlerFlags |= gBitTable[it->battler];
        sWait = WAIT_EXEC;
        break;
    case OP_MOVE_ANIM:
        if (it->arg == MOVE_NONE)
            break;
        gBattlerAttacker = it->battler;
        gBattlerTarget = it->battler ^ 1;
        gCurrentMove = gChosenMove = it->arg;
        gBattleScripting.animTurn = 0;
        gBattleMovePower = gBattleMoves[it->arg].power;
        gBattleMoveDamage = 1;
        BtlController_EmitMoveAnimation(B_COMM_TO_CONTROLLER, it->arg, 0, gBattleMovePower, gBattleMoveDamage,
                                        gBattleMons[it->battler].friendship, &gDisableStructs[it->battler], 0);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_CHOOSE_ACTION:
        gActiveBattler = MY_BATTLER;
        BtlController_EmitChooseAction(B_COMM_TO_CONTROLLER, B_ACTION_USE_MOVE, 0);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_ACTION;
        break;
    case OP_CHOOSE_MOVE:
        gActiveBattler = MY_BATTLER;
        EmitChooseMove();
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_MOVE;
        break;
    case OP_CHOOSE_POKEMON:
        gActiveBattler = MY_BATTLER;
        BtlController_EmitChoosePokemon(B_COMM_TO_CONTROLLER, it->arg, PARTY_SIZE, ABILITY_NONE,
                                        gBattleStruct->battlerPartyOrders[MY_BATTLER]);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_POKEMON;
        break;
    case OP_LINK_STANDBY:
        gActiveBattler = MY_BATTLER;
        BtlController_EmitLinkStandbyMsg(B_COMM_TO_CONTROLLER, LINK_STANDBY_MSG_ONLY, FALSE);
        MarkBattlerForControllerExec(gActiveBattler);
        sWait = WAIT_EXEC;
        break;
    case OP_END_BATTLE:
        sEnded = TRUE;
        ShowdownFinishBattle(it->arg);
        break;
    default:
        break;
    }
}

// Results of the player's menus come back in gBattleBufferB[MY_BATTLER].
static void HandleMenuResult(void)
{
    u8 *b = gBattleBufferB[MY_BATTLER];
    SdDebug("menu", sWait, b[1] | b[2] << 8 | b[3] << 16);
    switch (sWait)
    {
    case WAIT_ACTION:
        sWait = WAIT_NONE;
        switch (b[1])
        {
        case B_ACTION_USE_MOVE:
            Push(OP_CHOOSE_MOVE, MY_BATTLER, 0, 0);
            break;
        case B_ACTION_SWITCH:
            if (gSdState.request.switchMask == 0 || gSdState.request.trapped)
            {
                PushText((const u8 *)"Can't switch out right now!");
                Push(OP_CHOOSE_ACTION, MY_BATTLER, 0, 0);
            }
            else
                Push(OP_CHOOSE_POKEMON, MY_BATTLER, PARTY_ACTION_CHOOSE_MON, 0);
            break;
        case B_ACTION_USE_ITEM:
            PushString(STRINGID_ITEMSCANTBEUSEDNOW, MY_BATTLER);
            Push(OP_CHOOSE_ACTION, MY_BATTLER, 0, 0);
            break;
        case B_ACTION_RUN:
        default:
        {
            struct SdFrame f;
            SdEncodeForfeit(&f, SdLinkNextSeq());
            SdLinkSend(&f);
            Push(OP_LINK_STANDBY, MY_BATTLER, 0, 0);
            break;
        }
        }
        break;
    case WAIT_MOVE:
        sWait = WAIT_NONE;
        if (b[2] == 0xFF && b[3] == 0xFF)
        {
            Push(OP_CHOOSE_ACTION, MY_BATTLER, 0, 0); // B: back to the action menu
        }
        else
        {
            u8 index = b[2] & 0x3;
            SendChoiceMove(index + 1);
            Push(OP_LINK_STANDBY, MY_BATTLER, 0, 0);
        }
        break;
    case WAIT_POKEMON:
        sWait = WAIT_NONE;
        if (b[1] >= PARTY_SIZE)
        {
            if (sForcedSwitch)
                Push(OP_CHOOSE_POKEMON, MY_BATTLER, PARTY_ACTION_SEND_OUT, 0);
            else
                Push(OP_CHOOSE_ACTION, MY_BATTLER, 0, 0);
        }
        else
        {
            SendChoiceSwitch(b[1] + 1);
            Push(OP_LINK_STANDBY, MY_BATTLER, 0, 0);
        }
        break;
    default:
        break;
    }
}

void ShowdownBattleMain(void)
{
    struct SdFrame frame;
    struct SdMessage msg;

    SdLinkUpdate();
    if (sEnded)
        return;

    if (sWait == WAIT_PAUSE)
    {
        if (sPauseFrames)
            sPauseFrames--;
        else
            sWait = WAIT_NONE;
        return;
    }
    if (gBattleControllerExecFlags)
        return;
    if (sWait == WAIT_EXEC)
        sWait = WAIT_NONE;
    if (sWait == WAIT_ACTION || sWait == WAIT_MOVE || sWait == WAIT_POKEMON)
    {
        HandleMenuResult();
        return;
    }

    if (sQCount)
    {
        struct SdOpItem it = sQueue[sQHead];
        sQHead = (sQHead + 1) % OP_QUEUE;
        sQCount--;
        ExecOp(&it);
        return;
    }

    // Idle: pull the next message from the bridge.
    if (SdLinkPoll(&frame) && SdDecodeMessage(&frame, &msg))
        HandleMessage(&msg);
}

// Called by the lobby right before CB2_InitBattle, after the parties were built.
void ShowdownBattleDriverReset(void)
{
    s32 i;
    sQHead = sQCount = 0;
    sTextNext = 0;
    sWait = WAIT_NONE;
    sPauseFrames = 0;
    sPendingAttackBattler = 0xFF;
    sPendingAttackMove = 0;
    sForcedSwitch = FALSE;
    sEnded = FALSE;
    for (i = 0; i < SD_PARTY_SIZE; i++)
    {
        sLastMyHp[i] = gSdState.party[i].hp;
        sLastMyStatus[i] = gSdState.party[i].status;
        sLastOppHp[i] = gSdState.opp[i].hpPercent;
        sLastOppStatus[i] = gSdState.opp[i].status;
    }
    // The decision that arrived with the battle setup is answered once the intro is done.
    if (gSdState.haveRequest)
        HandleRequest(&gSdState.request);
}
