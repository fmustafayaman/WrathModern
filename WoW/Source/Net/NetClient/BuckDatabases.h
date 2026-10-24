#ifndef WOW_SOURCE_NET_NETCLIENT_BUCKDATABASES_H
#define WOW_SOURCE_NET_NETCLIENT_BUCKDATABASES_H

#include "Engine/Source/Os/Mac/BuckCodeEmu.h"

// Translate the real native DBC rows to/from the 32-bit layout used by delivered content.
// These functions do not manufacture framework state or challenge responses.
bool BuckReadNativeDatabase(uint32_t address, BuckDatabase& database);
bool BuckCommitNativeDatabase(uint32_t address, const BuckDatabase& database);

#endif
