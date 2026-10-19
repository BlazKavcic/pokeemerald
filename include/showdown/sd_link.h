// Transport for the Showdown mode inside pokeemerald: the EWRAM mailbox serviced by an
// emulator script (scripts/mgba-bridge.lua, scripts/bizhawk-bridge.lua). Same layout as
// gba/include/transport/mailbox_transport.h so the scripts need no changes. The link-port
// UART transport for real hardware is added later.
#ifndef GUARD_SHOWDOWN_SD_LINK_H
#define GUARD_SHOWDOWN_SD_LINK_H

#include "global.h"
#include "showdown/sd_proto.h"

void SdLinkInit(void);
void SdLinkUpdate(void);                    // once per frame
bool8 SdLinkPoll(struct SdFrame *out);      // one decoded frame if available
bool8 SdLinkSend(const struct SdFrame *f);
bool8 SdLinkIsUp(void);
u8 SdLinkNextSeq(void);
// Sends "tag a b" to the bridge's log (no-op if the link is down).
void SdDebug(const char *tag, s32 a, s32 b);

#endif // GUARD_SHOWDOWN_SD_LINK_H
