#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>
#include "Engine/Source/Os/OsTcp.h"
#include "Engine/Source/Storm/H/storm.h"

// Network addresses are sockaddr_in in NETADDR storage, as on Windows. The Windows TCP layer behind
// OsNetPump (an IOCP connection manager) is never started by the client, which networks through
// WowConnection; pumping it only waits.

void OsNetAddrMake(uint32_t address, uint16_t port, NETADDR* addr) {
    memset(addr, 0, sizeof(NETADDR));

    sockaddr_in* in = reinterpret_cast<sockaddr_in*>(addr);
    in->sin_len = sizeof(sockaddr_in);
    in->sin_family = AF_INET;
    in->sin_port = htons(port);
    in->sin_addr.s_addr = address;
}

uint32_t OsNetAddrGetAddress(const NETADDR* addr, uint16_t* port) {
    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(addr);

    if (port) {
        *port = ntohs(in->sin_port);
    }

    return in->sin_addr.s_addr;
}

const char* OsNetAddrToStr(const NETADDR* addr, char* buffer, uint32_t size) {
    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(addr);
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
    SStrPrintf(buffer, size, "%s:%u", text, ntohs(in->sin_port));

    return buffer;
}

void OsNetPump(uint32_t timeout) {
    usleep(static_cast<useconds_t>(timeout) * 1000);
}
