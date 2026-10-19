#include "showdown/sd_proto.h"

enum { ST_SYNC, ST_LEN, ST_TYPE, ST_SEQ, ST_PAYLOAD, ST_CRC };

u8 SdCrc8(const u8 *data, u32 n)
{
    u8 crc = 0;
    u32 i;
    s32 bit;
    for (i = 0; i < n; i++)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (u8)((crc << 1) ^ 0x07) : (u8)(crc << 1);
    }
    return crc;
}

u32 SdEncodeFrame(const struct SdFrame *frame, u8 *out, u32 cap)
{
    u32 total, i;
    if (frame->len > SD_MAX_PAYLOAD)
        return 0;
    total = 4 + frame->len + 1;
    if (cap < total)
        return 0;
    out[0] = SD_SYNC_BYTE;
    out[1] = frame->len;
    out[2] = frame->type;
    out[3] = frame->seq;
    for (i = 0; i < frame->len; i++)
        out[4 + i] = frame->payload[i];
    out[4 + frame->len] = SdCrc8(out + 1, 3 + frame->len);
    return total;
}

void SdDecoderReset(struct SdDecoder *d)
{
    d->state = ST_SYNC;
    d->received = 0;
}

bool8 SdDecoderPush(struct SdDecoder *d, u8 byte, struct SdFrame *out)
{
    switch (d->state)
    {
    case ST_SYNC:
        if (byte == SD_SYNC_BYTE)
            d->state = ST_LEN;
        return FALSE;
    case ST_LEN:
        if (byte > SD_MAX_PAYLOAD)
        {
            d->state = (byte == SD_SYNC_BYTE) ? ST_LEN : ST_SYNC;
            return FALSE;
        }
        d->frame.len = byte;
        d->state = ST_TYPE;
        return FALSE;
    case ST_TYPE:
        d->frame.type = byte;
        d->state = ST_SEQ;
        return FALSE;
    case ST_SEQ:
        d->frame.seq = byte;
        d->received = 0;
        d->state = d->frame.len ? ST_PAYLOAD : ST_CRC;
        return FALSE;
    case ST_PAYLOAD:
        d->frame.payload[d->received++] = byte;
        if (d->received == d->frame.len)
            d->state = ST_CRC;
        return FALSE;
    case ST_CRC:
    {
        u8 header[3];
        u8 crc;
        u32 i;
        s32 bit;
        header[0] = d->frame.len;
        header[1] = d->frame.type;
        header[2] = d->frame.seq;
        crc = SdCrc8(header, 3);
        for (i = 0; i < d->frame.len; i++)
        {
            crc ^= d->frame.payload[i];
            for (bit = 0; bit < 8; bit++)
                crc = (crc & 0x80) ? (u8)((crc << 1) ^ 0x07) : (u8)(crc << 1);
        }
        d->state = ST_SYNC;
        if (crc != byte)
        {
            d->crcErrors++;
            return FALSE;
        }
        *out = d->frame;
        return TRUE;
    }
    }
    d->state = ST_SYNC;
    return FALSE;
}

// ---- payload reader ---------------------------------------------------------------------

struct Reader { const struct SdFrame *f; u32 pos; bool8 under; };

static u8 R8(struct Reader *r)
{
    if (r->pos >= r->f->len) { r->under = TRUE; return 0; }
    return r->f->payload[r->pos++];
}

static u16 R16(struct Reader *r)
{
    u16 lo = R8(r);
    u16 hi = R8(r);
    return lo | (hi << 8);
}

static void RStr(struct Reader *r, u8 *out, u32 cap)
{
    u8 n = R8(r);
    u32 written = 0, i;
    for (i = 0; i < n; i++)
    {
        u8 c = R8(r);
        if (written + 1 < cap)
            out[written++] = c;
    }
    if (cap)
        out[written] = 0;
}

bool8 SdDecodeMessage(const struct SdFrame *frame, struct SdMessage *out)
{
    struct Reader r = { frame, 0, FALSE };
    s32 i;
    out->type = frame->type;
    switch (frame->type)
    {
    case SD_MSG_HELLO:
        return TRUE;
    case SD_MSG_STATUS:
        out->phase = R8(&r);
        return !r.under;
    case SD_MSG_BATTLE_START:
        out->battleStart.mySide = R8(&r);
        out->battleStart.format = R8(&r);
        RStr(&r, out->battleStart.opponent, sizeof(out->battleStart.opponent));
        return !r.under;
    case SD_MSG_PARTY_SLOT:
    {
        struct SdPartySlot *p = &out->partySlot;
        p->slot = R8(&r);
        p->species = R16(&r);
        p->level = R8(&r);
        p->gender = R8(&r);
        p->hp = R16(&r);
        p->maxHp = R16(&r);
        p->status = R8(&r);
        p->item = R16(&r);
        p->ability = R16(&r);
        for (i = 0; i < SD_MOVE_SLOTS; i++)
        {
            p->moves[i].id = R16(&r);
            p->moves[i].pp = R8(&r);
            p->moves[i].maxPp = R8(&r);
        }
        return !r.under && p->slot < SD_PARTY_SIZE;
    }
    case SD_MSG_HP_UPDATE:
        out->hpUpdate.side = R8(&r);
        out->hpUpdate.slot = R8(&r);
        out->hpUpdate.hp = R16(&r);
        out->hpUpdate.maxHp = R16(&r);
        out->hpUpdate.status = R8(&r);
        return !r.under && out->hpUpdate.slot < SD_PARTY_SIZE;
    case SD_MSG_OPP_REVEAL:
        out->oppReveal.slot = R8(&r);
        out->oppReveal.species = R16(&r);
        out->oppReveal.level = R8(&r);
        out->oppReveal.gender = R8(&r);
        return !r.under && out->oppReveal.slot < SD_PARTY_SIZE;
    case SD_MSG_OPP_MOVE_SEEN:
        out->oppMoveSeen.slot = R8(&r);
        out->oppMoveSeen.move = R16(&r);
        return !r.under && out->oppMoveSeen.slot < SD_PARTY_SIZE;
    case SD_MSG_BOOSTS:
        out->boosts.side = R8(&r);
        for (i = 0; i < 7; i++)
            out->boosts.values[i] = (s8)R8(&r);
        return !r.under;
    case SD_MSG_FIELD:
        out->field.weather = R8(&r);
        out->field.turns = R8(&r);
        out->field.myHazards = R8(&r);
        out->field.oppHazards = R8(&r);
        return !r.under;
    case SD_MSG_SWITCH_IN:
    case SD_MSG_FAINT:
        out->switchIn.side = R8(&r);
        out->switchIn.slot = R8(&r);
        return !r.under && out->switchIn.slot < SD_PARTY_SIZE;
    case SD_MSG_TEXT:
        out->text.flags = R8(&r);
        RStr(&r, out->text.line, sizeof(out->text.line));
        return !r.under;
    case SD_MSG_ANIM:
        out->anim.kind = R8(&r);
        out->anim.side = R8(&r);
        out->anim.move = (frame->len >= 4) ? R16(&r) : 0;
        return !r.under;
    case SD_MSG_REQUEST:
        out->request.rqid = R16(&r);
        out->request.kind = R8(&r);
        out->request.moveMask = R8(&r);
        out->request.disabledMask = R8(&r);
        out->request.switchMask = R8(&r);
        out->request.trapped = R8(&r);
        return !r.under;
    case SD_MSG_CHOICE_REJECTED:
        out->rejected.rqid = R16(&r);
        RStr(&r, out->rejected.reason, sizeof(out->rejected.reason));
        return !r.under;
    case SD_MSG_TIMER:
        out->timer.seconds = R16(&r);
        out->timer.active = R8(&r);
        return !r.under;
    case SD_MSG_BATTLE_END:
        out->result = R8(&r);
        return !r.under;
    case SD_MSG_ERROR:
        out->error.code = R8(&r);
        RStr(&r, out->error.text, sizeof(out->error.text));
        return !r.under;
    default:
        return FALSE;
    }
}

// ---- GBA -> bridge ----------------------------------------------------------------------

static void Begin(struct SdFrame *f, u8 type, u8 seq)
{
    f->type = type;
    f->seq = seq;
    f->len = 0;
}

static void W8(struct SdFrame *f, u8 v)
{
    if (f->len < SD_MAX_PAYLOAD)
        f->payload[f->len++] = v;
}

static void W16(struct SdFrame *f, u16 v)
{
    W8(f, v & 0xFF);
    W8(f, v >> 8);
}

void SdEncodeGbaHello(struct SdFrame *f, u8 seq, u8 romVersion)
{
    Begin(f, SD_MSG_GBA_HELLO, seq);
    W8(f, SD_PROTO_VERSION);
    W8(f, romVersion);
}

void SdEncodeSearch(struct SdFrame *f, u8 seq, u8 format)
{
    Begin(f, SD_MSG_SEARCH, seq);
    W8(f, format);
}

void SdEncodeCancelSearch(struct SdFrame *f, u8 seq) { Begin(f, SD_MSG_CANCEL_SEARCH, seq); }

void SdEncodeChooseMove(struct SdFrame *f, u8 seq, u16 rqid, u8 slot)
{
    Begin(f, SD_MSG_CHOOSE_MOVE, seq);
    W16(f, rqid);
    W8(f, slot);
}

void SdEncodeChooseSwitch(struct SdFrame *f, u8 seq, u16 rqid, u8 slot)
{
    Begin(f, SD_MSG_CHOOSE_SWITCH, seq);
    W16(f, rqid);
    W8(f, slot);
}

void SdEncodeTimerToggle(struct SdFrame *f, u8 seq) { Begin(f, SD_MSG_TIMER_TOGGLE, seq); }
void SdEncodeForfeit(struct SdFrame *f, u8 seq) { Begin(f, SD_MSG_FORFEIT, seq); }
