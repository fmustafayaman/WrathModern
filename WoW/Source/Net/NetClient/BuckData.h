#ifndef WOW_SOURCE_NET_NETCLIENT_BUCKDATA_H
#define WOW_SOURCE_NET_NETCLIENT_BUCKDATA_H

#include <cstdint>

class NetClient;
class CDataStore;

/*
 * Warmane's buck-data stream. After the Warden handshake the realm (and later the world)
 * delivers x86 code over unused opcodes (0x2E9 / 0x4B8 / 0x4B9); answering it with the buck
 * requests (0x41D / 0x41C / 0x41B) is what gates the character list, and the in-game challenges
 * (0x41B, carrying code) keep the session alive. The official client executes the delivered
 * code; this runs it in the BuckCodeEmu machine (Engine/Source/Os/Mac/BuckCodeEmu.h) so the
 * answers are the delivered code's own, never a canned value.
 *
 * Both functions return 1 when they handled the message, 0 when the caller should dispatch it
 * normally; without the emulator they always return 0.
 */

// Runs the delivered ClientInitializeGame content hook and commits its real DBC edits.
// False means the hook failed; the caller must not continue an inconsistent world login.
bool BuckDataApplyContent();

int32_t BuckDataProcessMessage(NetClient* connection, uint32_t msgId, CDataStore* msg);
int32_t BuckDataHandleCustomCode(NetClient* connection, const uint8_t* data, uint32_t size);

#endif
