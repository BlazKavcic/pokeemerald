// Pre-battle screen: shows the bridge status, searches the ladder on START, collects the
// battle setup messages, builds the parties and launches the engine's battle intro.
#include "global.h"
#include "showdown/showdown.h"
#include "showdown/sd_link.h"
#include "showdown/sd_proto.h"
#include "showdown/sd_state.h"
#include "battle.h"
#include "battle_setup.h"
#include "bg.h"
#include "gpu_regs.h"
#include "main.h"
#include "main_menu.h"
#include "save.h"
#include "strings.h"
#include "malloc.h"
#include "menu.h"
#include "new_game.h"
#include "palette.h"
#include "pokemon.h"
#include "scanline_effect.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "task.h"
#include "text.h"
#include "text_window.h"
#include "window.h"
#include "constants/opponents.h"
#include "constants/rgb.h"
#include "constants/songs.h"

enum { WIN_TITLE, WIN_STATUS };

static const struct WindowTemplate sWinTemplates[] =
{
    [WIN_TITLE] = { .bg = 1, .tilemapLeft = 2, .tilemapTop = 1, .width = 26, .height = 2, .paletteNum = 1, .baseBlock = 2 },
    [WIN_STATUS] = { .bg = 0, .tilemapLeft = 2, .tilemapTop = 5, .width = 26, .height = 14, .paletteNum = 1, .baseBlock = 0x36 },
    DUMMY_WIN_TEMPLATE
};

static const struct BgTemplate sBgTemplates[] =
{
    { .bg = 1, .charBaseIndex = 1, .mapBaseIndex = 30, .screenSize = 0, .paletteMode = 0, .priority = 0, .baseTile = 0 },
    { .bg = 0, .charBaseIndex = 1, .mapBaseIndex = 31, .screenSize = 0, .paletteMode = 0, .priority = 1, .baseTile = 0 },
};

static const u16 sBgPal[] = { RGB(17, 18, 31) };
static const u16 sTextPal[] = INCBIN_U16("graphics/interface/option_menu_text.gbapal");

static const u8 sText_Title[] = _("showdownGBA   Gen 3 Random Battle");
static const u8 sText_Offline[] = _("Link: waiting for the emulator\nscript or adapter...");
static const u8 sText_Connecting[] = _("Bridge: connecting...");
static const u8 sText_Ready[] = _("Ready.\n\nSTART: find a battle\nB: back to the menu");
static const u8 sText_Searching[] = _("Searching for an opponent...\n\nB: cancel");
static const u8 sText_Starting[] = _("Opponent found!\nSetting up the battle...");
static const u8 sText_Ended[] = _("Battle over.\n\nSTART: find another\nB: back to the menu");

static EWRAM_DATA u8 sShownPhase = 0xFF;
static EWRAM_DATA bool8 sShownLink = 0xFF;
static EWRAM_DATA bool8 sHelloSent = FALSE;
static EWRAM_DATA bool8 sStarting = FALSE;

void ShowdownBattleDriverReset(void);

static void DrawStatus(void)
{
    const u8 *text;
    bool8 link = SdLinkIsUp();
    if (link == sShownLink && gSdState.phase == sShownPhase)
        return;
    sShownLink = link;
    sShownPhase = gSdState.phase;
    if (!link)
        text = sText_Offline;
    else switch (gSdState.phase)
    {
    case SD_PHASE_IDLE: text = sText_Ready; break;
    case SD_PHASE_SEARCHING: text = sText_Searching; break;
    case SD_PHASE_IN_BATTLE: text = sText_Starting; break;
    case SD_PHASE_ENDED: text = sText_Ended; break;
    default: text = sText_Connecting; break;
    }
    FillWindowPixelBuffer(WIN_STATUS, PIXEL_FILL(1));
    AddTextPrinterParameterized(WIN_STATUS, FONT_NORMAL, text, 8, 1, TEXT_SKIP_DRAW, NULL);
    CopyWindowToVram(WIN_STATUS, COPYWIN_FULL);
}

static void SendHello(void)
{
    struct SdFrame f;
    SdEncodeGbaHello(&f, SdLinkNextSeq(), 2);
    SdLinkSend(&f);
    sHelloSent = TRUE;
}

static void StartBattle(void)
{
    s32 i;
    SdBuildParties();
    SdConvertText(gSdState.opponentName, gShowdownOpponentName, sizeof(gShowdownOpponentName));
    if (gShowdownOpponentName[0] == EOS)
        StringCopy(gShowdownOpponentName, gText_ExpandedPlaceholder_Brendan);
    gBattlerPartyIndexes[0] = gSdState.myActive >= 0 ? gSdState.myActive : 0;
    gBattlerPartyIndexes[1] = gSdState.oppActive >= 0 ? gSdState.oppActive : 0;
    // The intro sends out party index 0 on each side; put the leads first.
    if (gBattlerPartyIndexes[0] != 0)
    {
        struct Pokemon tmp = gPlayerParty[0];
        gPlayerParty[0] = gPlayerParty[gBattlerPartyIndexes[0]];
        gPlayerParty[gBattlerPartyIndexes[0]] = tmp;
    }
    if (gBattlerPartyIndexes[1] != 0)
    {
        struct Pokemon tmp = gEnemyParty[0];
        gEnemyParty[0] = gEnemyParty[gBattlerPartyIndexes[1]];
        gEnemyParty[gBattlerPartyIndexes[1]] = tmp;
    }
    for (i = 0; i < PARTY_SIZE; i++)
        ; // party order on the Showdown side is always "active first", so this matches
    gShowdownBattle = TRUE;
    gBattleTypeFlags = BATTLE_TYPE_TRAINER;
    gTrainerBattleOpponent_A = TRAINER_BRENDAN_ROUTE_103_TREECKO;
    gMain.savedCallback = CB2_ShowdownLobby;
    ShowdownBattleDriverReset();
    PlayBattleBGM();
    SetMainCallback2(CB2_InitBattle);
}

// Pumps the bridge; returns TRUE once the battle can start.
static bool8 Pump(void)
{
    struct SdFrame frame;
    struct SdMessage msg;
    u8 n = 0;
    while (n++ < 16 && SdLinkPoll(&frame))
    {
        if (!SdDecodeMessage(&frame, &msg))
            continue;
        SdStateApply(&msg);
        if (msg.type == SD_MSG_REQUEST && gSdState.inBattle && gSdState.haveRequest
            && gSdState.myActive >= 0 && gSdState.oppActive >= 0)
            return TRUE;
    }
    return FALSE;
}

static void Task_Lobby(u8 taskId)
{
    if (gPaletteFade.active || sStarting)
        return;
    if (SdLinkIsUp() && !sHelloSent)
        SendHello();
    if (!SdLinkIsUp())
        sHelloSent = FALSE;

    if (Pump())
    {
        sStarting = TRUE;
        PlaySE(SE_SELECT);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
        gTasks[taskId].func = NULL;
        DestroyTask(taskId);
        CreateTask(0, 0); // placeholder, replaced below
        return;
    }
    DrawStatus();

    if (JOY_NEW(START_BUTTON) && SdLinkIsUp() && (gSdState.phase == SD_PHASE_IDLE || gSdState.phase == SD_PHASE_ENDED))
    {
        struct SdFrame f;
        PlaySE(SE_SELECT);
        SdEncodeSearch(&f, SdLinkNextSeq(), 1);
        SdLinkSend(&f);
    }
    else if (JOY_NEW(B_BUTTON) && gSdState.phase == SD_PHASE_SEARCHING)
    {
        struct SdFrame f;
        PlaySE(SE_SELECT);
        SdEncodeCancelSearch(&f, SdLinkNextSeq());
        SdLinkSend(&f);
    }
    else if (JOY_NEW(B_BUTTON) && (gSdState.phase == SD_PHASE_IDLE || gSdState.phase == SD_PHASE_ENDED))
    {
        PlaySE(SE_SELECT);
        gMain.savedCallback = CB2_InitMainMenu;
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
        gTasks[taskId].func = NULL;
        DestroyTask(taskId);
        CreateTask(0, 0);
        return;
    }
}

static void Task_FadeAndGo(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        DestroyTask(taskId);
        if (sStarting)
            StartBattle();
        else
            SetMainCallback2(gMain.savedCallback);
    }
}

static void MainCB2(void)
{
    SdLinkUpdate();
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
    UpdatePaletteFade();
    // tasks with func NULL are the "fade then go" placeholders created above
    {
        u8 i;
        for (i = 0; i < NUM_TASKS; i++)
            if (gTasks[i].isActive && gTasks[i].func == 0)
                gTasks[i].func = Task_FadeAndGo;
    }
}

static void VBlankCB(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

void CB2_ShowdownLobby(void)
{
    switch (gMain.state)
    {
    default:
    case 0:
        SetVBlankCallback(NULL);
        gShowdownBattle = FALSE;
        sStarting = FALSE;
        gMain.state++;
        break;
    case 1:
        DmaClearLarge16(3, (void *)(VRAM), VRAM_SIZE, 0x1000);
        DmaClear32(3, OAM, OAM_SIZE);
        DmaClear16(3, PLTT, PLTT_SIZE);
        SetGpuReg(REG_OFFSET_DISPCNT, 0);
        ResetBgsAndClearDma3BusyFlags(0);
        InitBgsFromTemplates(0, sBgTemplates, ARRAY_COUNT(sBgTemplates));
        ChangeBgX(0, 0, BG_COORD_SET);
        ChangeBgY(0, 0, BG_COORD_SET);
        ChangeBgX(1, 0, BG_COORD_SET);
        ChangeBgY(1, 0, BG_COORD_SET);
        InitWindows(sWinTemplates);
        DeactivateAllTextPrinters();
        SetGpuReg(REG_OFFSET_WIN0H, 0);
        SetGpuReg(REG_OFFSET_WIN0V, 0);
        SetGpuReg(REG_OFFSET_WININ, 0);
        SetGpuReg(REG_OFFSET_WINOUT, 0);
        SetGpuReg(REG_OFFSET_BLDCNT, 0);
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
        ShowBg(0);
        ShowBg(1);
        gMain.state++;
        break;
    case 2:
        ResetPaletteFade();
        ScanlineEffect_Stop();
        ResetTasks();
        ResetSpriteData();
        gMain.state++;
        break;
    case 3:
        LoadBgTiles(1, GetWindowFrameTilesPal(gSaveBlock2Ptr->optionsWindowFrameType)->tiles, 0x120, 0x1A2);
        LoadPalette(sBgPal, BG_PLTT_ID(0), sizeof(sBgPal));
        LoadPalette(GetWindowFrameTilesPal(gSaveBlock2Ptr->optionsWindowFrameType)->pal, BG_PLTT_ID(7), PLTT_SIZE_4BPP);
        LoadPalette(sTextPal, BG_PLTT_ID(1), sizeof(sTextPal));
        gMain.state++;
        break;
    case 4:
        PutWindowTilemap(WIN_TITLE);
        FillWindowPixelBuffer(WIN_TITLE, PIXEL_FILL(1));
        AddTextPrinterParameterized(WIN_TITLE, FONT_NORMAL, sText_Title, 8, 1, TEXT_SKIP_DRAW, NULL);
        CopyWindowToVram(WIN_TITLE, COPYWIN_FULL);
        PutWindowTilemap(WIN_STATUS);
        sShownPhase = 0xFF;
        sShownLink = 0xFF;
        DrawStatus();
        gMain.state++;
        break;
    case 5:
        CreateTask(Task_Lobby, 0);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
        SetVBlankCallback(VBlankCB);
        SetMainCallback2(MainCB2);
        return;
    }
}

void Showdown_StartFromMainMenu(void)
{
    if (gSaveFileStatus == SAVE_STATUS_EMPTY || gSaveFileStatus == SAVE_STATUS_CORRUPT || gSaveFileStatus == SAVE_STATUS_NO_FLASH)
        NewGameInitData();
    SdLinkInit();
    SdStateReset();
    sHelloSent = FALSE;
    gMain.state = 0;
    SetMainCallback2(CB2_ShowdownLobby);
}
