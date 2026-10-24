// Native transport for the EpochConnection.dll revision documented in Docs/Epoch.md.
// No Windows code is loaded. Other servers keep their original network path.
#ifndef PLATFORM_MAC_EPOCHRELAY_H
#define PLATFORM_MAC_EPOCHRELAY_H

#include <cstdint>
#include <string>

namespace EpochRelay {

// The DLL hooks only this hostname (case-insensitive, optional final dot) on port 3724.
bool IsLoginHost(const char* host, uint16_t port);

// Starts loopback-only auth/world listeners, without making an upstream connection yet.
// Reads and authenticates the original DLL next to the app. Zero means failure: callers
// must report a connection failure, NOT fall back to unencrypted/direct login.
uint16_t LoginPort();
// Epoch advertises build 12341 in BOTH auth protocols (despite PE file version 12340).
// Only sockets connected to our current loopback listeners get that profile. Everything
// else remains stock 12340, including a non-Epoch server used after an Epoch login.
uint16_t ProtocolBuild(uint32_t networkOrderAddress, uint16_t port);
void Shutdown();

// Read-only, revision-pinned configuration import. Never returns the key in an error.
bool ReadCredential(const char* dllPath, std::string& credential, std::string& error);
void ClearCredential(std::string& credential);

#if defined(WRATHMODERN_EPOCH_RELAY_TESTING)
struct TestState {
    uint16_t authPort;
    uint16_t worldPort;
    unsigned active;
    unsigned opened;
    unsigned closed;
};
// Test builds only: fake credentials with a loopback WebSocket server. Not an app setting.
bool StartForTesting(const char* loopbackURL, double handshakeTimeout);
TestState StateForTesting();
#endif

}
#endif
