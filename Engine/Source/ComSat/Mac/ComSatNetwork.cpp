#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "Engine/Source/ComSat/ComSatNetwork.h"

// ComSat (voice chat) UDP sockets on macOS. ComSat::Network::Address stores a Windows sockaddr_in
// from m_family on: a 16-bit family, then port and address. A BSD sockaddr_in starts with a length
// byte and a family byte, so the calls below build one from the fields rather than reinterpreting
// the storage.

static sockaddr_in ToSockaddr(const ComSat::Network::Address& address) {
    sockaddr_in in = {};
    in.sin_len = sizeof(sockaddr_in);
    in.sin_family = AF_INET;
    in.sin_port = address.m_port;
    in.sin_addr.s_addr = address.m_ip;

    return in;
}

static void FromSockaddr(const sockaddr_in& in, ComSat::Network::Address& address) {
    address.m_family = AF_INET;
    address.m_port = in.sin_port;
    address.m_ip = in.sin_addr.s_addr;
    memset(address.m_zero, 0, sizeof(address.m_zero));
}

void ComSat::Network::Address::SetPort(int32_t port) {
    this->m_port = htons(static_cast<uint16_t>(port));
}

int32_t ComSat::Network::Address::GetPort() const {
    return ntohs(this->m_port);
}

void ComSat::Network::Address::SetAddress(const char* address) {
    const char* colon = strchr(address, ':');

    if (colon) {
        if (strlen(address) < 128) {
            char host[128];
            strcpy(host, address);
            host[colon - address] = '\0';
            this->m_ip = inet_addr(host);
            this->m_port = htons(static_cast<uint16_t>(strtol(colon + 1, nullptr, 10)));
        }
    } else {
        this->m_ip = inet_addr(address);
    }
}

ComSat::Network::SocketPosix::SocketPosix() {
    int32_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    this->m_socket = static_cast<uintptr_t>(sock);

    if (sock < 0) {
        return;
    }

    // Windows turns off UDP connection-reset reports (SIO_UDP_CONNRESET) here; BSD sockets do not
    // report ICMP port-unreachable on unconnected sockets to begin with.
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    int32_t size = 0x8000;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

ComSat::Network::SocketPosix::~SocketPosix() {
    close(static_cast<int32_t>(this->m_socket));
}

bool ComSat::Network::SocketPosix::Bind(int32_t port) {
    ComSat::Network::Address address;
    address.SetPort(port);

    int32_t reuse = 1;
    setsockopt(static_cast<int32_t>(this->m_socket), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in in = ToSockaddr(address);

    return bind(static_cast<int32_t>(this->m_socket), reinterpret_cast<const sockaddr*>(&in), sizeof(in)) == 0;
}

int32_t ComSat::Network::SocketPosix::SendTo(const ComSat::Network::Address& address, const uint8_t* data, uint32_t size) {
    sockaddr_in in = ToSockaddr(address);

    return static_cast<int32_t>(sendto(static_cast<int32_t>(this->m_socket), data, size, 0, reinterpret_cast<const sockaddr*>(&in), sizeof(in)));
}

bool ComSat::Network::SocketPosix::RecvFrom(ComSat::Network::Address& address, uint8_t* data, uint32_t& size) {
    memset(data, 0, size);

    sockaddr_in in = {};
    socklen_t length = sizeof(in);
    ssize_t received = recvfrom(static_cast<int32_t>(this->m_socket), data, size, 0, reinterpret_cast<sockaddr*>(&in), &length);

    if (received <= 0) {
        return false;
    }

    FromSockaddr(in, address);
    address.m_length = sizeof(sockaddr_in);
    size = static_cast<uint32_t>(received);

    return true;
}
