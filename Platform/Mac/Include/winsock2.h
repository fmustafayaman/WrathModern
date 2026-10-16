// A curated subset of <winsock2.h> for the macOS build: the Winsock names that shared Win64/ socket
// code (WowConnection) uses, mapped onto BSD sockets with Winsock semantics. Like windows.h here, it
// only grows when a shared file needs more.

#ifndef PLATFORM_MAC_INCLUDE_WINSOCK2_H
#define PLATFORM_MAC_INCLUDE_WINSOCK2_H

#include <cerrno>
#include <cstdint>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int SOCKET;
typedef unsigned long u_long;
typedef unsigned short u_short;

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)

// Winsock error codes the shared code compares against.
#define WSAEINTR 10004
#define WSAEWOULDBLOCK 10035

// WSAGetLastError reports the Winsock code for the last socket call on this thread. A non-blocking
// connect in progress is WSAEWOULDBLOCK on Windows and EINPROGRESS on macOS.
static inline int WSAGetLastError() {
    switch (errno) {
        case EWOULDBLOCK:
        case EINPROGRESS:
            return WSAEWOULDBLOCK;
        case EINTR:
            return WSAEINTR;
        default:
            return errno;
    }
}

static inline int closesocket(SOCKET sock) {
    return close(sock);
}

static inline int ioctlsocket(SOCKET sock, unsigned long command, u_long* argument) {
    if (command == FIONBIO) {
        int flags = fcntl(sock, F_GETFL, 0);

        if (flags < 0) {
            return SOCKET_ERROR;
        }

        return fcntl(sock, F_SETFL, *argument ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) < 0 ? SOCKET_ERROR : 0;
    }

    return ioctl(sock, command, argument);
}

// Winsock takes int lengths where BSD sockets take socklen_t.

static inline int getpeername(SOCKET sock, sockaddr* address, int* length) {
    socklen_t size = static_cast<socklen_t>(*length);
    int result = ::getpeername(sock, address, &size);
    *length = static_cast<int>(size);

    return result;
}

static inline int getsockname(SOCKET sock, sockaddr* address, int* length) {
    socklen_t size = static_cast<socklen_t>(*length);
    int result = ::getsockname(sock, address, &size);
    *length = static_cast<int>(size);

    return result;
}

static inline SOCKET accept(SOCKET sock, sockaddr* address, int* length) {
    socklen_t size = static_cast<socklen_t>(*length);
    SOCKET result = ::accept(sock, address, &size);
    *length = static_cast<int>(size);

    return result;
}

static inline int getsockopt(SOCKET sock, int level, int name, char* value, int* length) {
    socklen_t size = static_cast<socklen_t>(*length);
    int result = ::getsockopt(sock, level, name, value, &size);
    *length = static_cast<int>(size);

    return result;
}

#endif
