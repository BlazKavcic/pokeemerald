#include "showdown/sd_link.h"

#define MB_MAGIC 0x42474453u // "SDGB"
#define MB_VERSION 1
#define MB_HOST_ATTACHED 1
#define MB_BUF 1024

struct SdMailbox
{
    u32 magic;
    u16 version;
    u16 hostFlags;
    u16 inHead;   // host writes
    u16 inTail;   // GBA reads
    u16 outHead;  // GBA writes
    u16 outTail;  // host reads
    u32 gbaFrame;
    u32 hostFrame;
    u8 inBuf[MB_BUF];
    u8 outBuf[MB_BUF];
};

static EWRAM_DATA ALIGNED(4) volatile struct SdMailbox sMailbox = {0};
static EWRAM_DATA struct SdDecoder sDecoder = {0};
static EWRAM_DATA u32 sLastHostFrame = 0;
static EWRAM_DATA u32 sFramesSinceHost = 0;
static EWRAM_DATA u8 sSeq = 0;

void SdLinkInit(void)
{
    sMailbox.version = MB_VERSION;
    sMailbox.hostFlags = 0;
    sMailbox.inHead = sMailbox.inTail = 0;
    sMailbox.outHead = sMailbox.outTail = 0;
    sMailbox.gbaFrame = 0;
    sMailbox.hostFrame = 0;
    sMailbox.magic = MB_MAGIC; // last: a scanning host must never see a half-built header
    SdDecoderReset(&sDecoder);
    sLastHostFrame = 0;
    sFramesSinceHost = 0xFFFF;
    sSeq = 0;
}

void SdLinkUpdate(void)
{
    u32 hostFrame;
    sMailbox.gbaFrame = sMailbox.gbaFrame + 1;
    hostFrame = sMailbox.hostFrame;
    if (hostFrame != sLastHostFrame)
    {
        sLastHostFrame = hostFrame;
        sFramesSinceHost = 0;
    }
    else if (sFramesSinceHost < 0xFFFF)
    {
        sFramesSinceHost++;
    }
}

bool8 SdLinkIsUp(void)
{
    return (sMailbox.hostFlags & MB_HOST_ATTACHED) && sFramesSinceHost < 180;
}

u8 SdLinkNextSeq(void)
{
    return sSeq++;
}

bool8 SdLinkPoll(struct SdFrame *out)
{
    while (((sMailbox.inHead - sMailbox.inTail) & (MB_BUF - 1)) != 0)
    {
        u16 tail = sMailbox.inTail;
        u8 byte = sMailbox.inBuf[tail];
        sMailbox.inTail = (tail + 1) & (MB_BUF - 1);
        if (SdDecoderPush(&sDecoder, byte, out))
            return TRUE;
    }
    return FALSE;
}

bool8 SdLinkSend(const struct SdFrame *f)
{
    u8 bytes[SD_MAX_FRAME_SIZE];
    u32 n = SdEncodeFrame(f, bytes, sizeof(bytes));
    u16 free = (MB_BUF - 1) - ((sMailbox.outHead - sMailbox.outTail) & (MB_BUF - 1));
    u16 head;
    u32 i;
    if (n == 0 || free < n)
        return FALSE;
    head = sMailbox.outHead;
    for (i = 0; i < n; i++)
    {
        sMailbox.outBuf[head] = bytes[i];
        head = (head + 1) & (MB_BUF - 1);
    }
    sMailbox.outHead = head; // publish after the bytes are in place
    return TRUE;
}

static u32 PutNumber(char *dst, s32 value)
{
    char tmp[12];
    u32 n = 0, i = 0;
    u32 v;
    if (value < 0) { dst[i++] = '-'; v = (u32)(-value); } else v = (u32)value;
    do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
    while (n) dst[i++] = tmp[--n];
    return i;
}

void SdDebug(const char *tag, s32 a, s32 b)
{
    char buf[100];
    u32 i = 0;
    struct SdFrame f;
    while (tag[i] && i < 60) { buf[i] = tag[i]; i++; }
    buf[i++] = ' ';
    i += PutNumber(buf + i, a);
    buf[i++] = ' ';
    i += PutNumber(buf + i, b);
    buf[i] = 0;
    SdEncodeDebug(&f, sSeq++, buf);
    SdLinkSend(&f);
}
