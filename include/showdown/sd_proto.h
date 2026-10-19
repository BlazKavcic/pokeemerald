// showdownGBA wire protocol, C port of gba/include/proto/frame.h + battle/messages.h.
// Keep in sync with the Butano client and bridge/src/gbaproto.
#ifndef GUARD_SHOWDOWN_SD_PROTO_H
#define GUARD_SHOWDOWN_SD_PROTO_H

#include "global.h"

#define SD_PROTO_VERSION    1
#define SD_SYNC_BYTE        0xA5
#define SD_MAX_PAYLOAD      120
#define SD_MAX_FRAME_SIZE   (4 + SD_MAX_PAYLOAD + 1)

enum SdMsgType
{
    SD_MSG_HELLO = 0x01,
    SD_MSG_STATUS = 0x02,
    SD_MSG_BATTLE_START = 0x10,
    SD_MSG_PARTY_SLOT = 0x11,
    SD_MSG_HP_UPDATE = 0x12,
    SD_MSG_OPP_REVEAL = 0x13,
    SD_MSG_OPP_MOVE_SEEN = 0x14,
    SD_MSG_BOOSTS = 0x15,
    SD_MSG_FIELD = 0x16,
    SD_MSG_SWITCH_IN = 0x17,
    SD_MSG_FAINT = 0x18,
    SD_MSG_TEXT = 0x20,
    SD_MSG_ANIM = 0x21,
    SD_MSG_REQUEST = 0x30,
    SD_MSG_CHOICE_REJECTED = 0x31,
    SD_MSG_TIMER = 0x32,
    SD_MSG_BATTLE_END = 0x40,
    SD_MSG_ERROR = 0x7F,
    SD_MSG_GBA_HELLO = 0x81,
    SD_MSG_SEARCH = 0x82,
    SD_MSG_CANCEL_SEARCH = 0x83,
    SD_MSG_CHOOSE_MOVE = 0x90,
    SD_MSG_CHOOSE_SWITCH = 0x91,
    SD_MSG_TIMER_TOGGLE = 0x92,
    SD_MSG_FORFEIT = 0x93,
    SD_MSG_ACK = 0xA0,
};

enum SdSide { SD_SIDE_ME = 0, SD_SIDE_OPP = 1 };
enum SdGender { SD_GENDER_NONE = 0, SD_GENDER_MALE = 1, SD_GENDER_FEMALE = 2 };
enum SdStatus { SD_STATUS_NONE = 0, SD_STATUS_BURN, SD_STATUS_FREEZE, SD_STATUS_PARALYSIS, SD_STATUS_POISON, SD_STATUS_TOXIC, SD_STATUS_SLEEP, SD_STATUS_FAINTED };
enum SdWeather { SD_WEATHER_NONE = 0, SD_WEATHER_SUN, SD_WEATHER_RAIN, SD_WEATHER_SANDSTORM, SD_WEATHER_HAIL };
enum SdRequestKind { SD_REQUEST_NONE = 0, SD_REQUEST_MOVE = 1, SD_REQUEST_FORCE_SWITCH = 2, SD_REQUEST_WAIT = 3 };
enum SdPhase { SD_PHASE_OFFLINE = 0, SD_PHASE_CONNECTING, SD_PHASE_IDLE, SD_PHASE_SEARCHING, SD_PHASE_IN_BATTLE, SD_PHASE_ENDED };
enum SdResult { SD_RESULT_NONE = 0, SD_RESULT_WIN, SD_RESULT_LOSS, SD_RESULT_TIE, SD_RESULT_FORFEIT };
enum SdAnim { SD_ANIM_NONE = 0, SD_ANIM_ATTACK, SD_ANIM_HIT, SD_ANIM_FAINT, SD_ANIM_SWITCH_IN, SD_ANIM_STATUS, SD_ANIM_HEAL };

#define SD_PARTY_SIZE 6
#define SD_MOVE_SLOTS 4
#define SD_TEXT_MAX 60
#define SD_NAME_MAX 18

struct SdFrame
{
    u8 type;
    u8 seq;
    u8 len;
    u8 payload[SD_MAX_PAYLOAD];
};

struct SdDecoder
{
    u8 state;
    u8 received;
    struct SdFrame frame;
    u32 crcErrors;
};

struct SdMoveSlot { u16 id; u8 pp; u8 maxPp; };

struct SdPartySlot
{
    u8 slot;
    u16 species;   // National Dex number (forms 387+)
    u8 level;
    u8 gender;
    u16 hp;
    u16 maxHp;
    u8 status;
    u16 item;      // Showdown item number
    u16 ability;   // Showdown ability number (= Gen 3 ability id)
    struct SdMoveSlot moves[SD_MOVE_SLOTS];
};

struct SdHpUpdate { u8 side; u8 slot; u16 hp; u16 maxHp; u8 status; };
struct SdOppReveal { u8 slot; u16 species; u8 level; u8 gender; };
struct SdOppMoveSeen { u8 slot; u16 move; };
struct SdBoosts { u8 side; s8 values[7]; };
struct SdField { u8 weather; u8 turns; u8 myHazards; u8 oppHazards; };
struct SdSwitchIn { u8 side; u8 slot; };
struct SdText { u8 flags; u8 line[SD_TEXT_MAX + 1]; };
struct SdAnimMsg { u8 kind; u8 side; u16 move; };
struct SdRequest { u16 rqid; u8 kind; u8 moveMask; u8 disabledMask; u8 switchMask; u8 trapped; };
struct SdChoiceRejected { u16 rqid; u8 reason[SD_TEXT_MAX + 1]; };
struct SdTimer { u16 seconds; u8 active; };
struct SdBattleStart { u8 mySide; u8 format; u8 opponent[SD_NAME_MAX + 1]; };

struct SdMessage
{
    u8 type;
    union
    {
        u8 phase;
        struct SdBattleStart battleStart;
        struct SdPartySlot partySlot;
        struct SdHpUpdate hpUpdate;
        struct SdOppReveal oppReveal;
        struct SdOppMoveSeen oppMoveSeen;
        struct SdBoosts boosts;
        struct SdField field;
        struct SdSwitchIn switchIn;
        struct SdSwitchIn faint;
        struct SdText text;
        struct SdAnimMsg anim;
        struct SdRequest request;
        struct SdChoiceRejected rejected;
        struct SdTimer timer;
        u8 result;
        struct { u8 code; u8 text[SD_TEXT_MAX + 1]; } error;
    };
};

u8 SdCrc8(const u8 *data, u32 n);
u32 SdEncodeFrame(const struct SdFrame *frame, u8 *out, u32 cap);
void SdDecoderReset(struct SdDecoder *d);
bool8 SdDecoderPush(struct SdDecoder *d, u8 byte, struct SdFrame *out);
bool8 SdDecodeMessage(const struct SdFrame *frame, struct SdMessage *out);

// GBA -> bridge frames
void SdEncodeGbaHello(struct SdFrame *f, u8 seq, u8 romVersion);
void SdEncodeSearch(struct SdFrame *f, u8 seq, u8 format);
void SdEncodeCancelSearch(struct SdFrame *f, u8 seq);
void SdEncodeChooseMove(struct SdFrame *f, u8 seq, u16 rqid, u8 slot);
void SdEncodeChooseSwitch(struct SdFrame *f, u8 seq, u16 rqid, u8 slot);
void SdEncodeTimerToggle(struct SdFrame *f, u8 seq);
void SdEncodeForfeit(struct SdFrame *f, u8 seq);

#endif // GUARD_SHOWDOWN_SD_PROTO_H
