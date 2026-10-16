#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "WoW/Source/WowServices/WowConnectionNet.h"

// The network thread on macOS. It waits on the connections' sockets with poll() where Windows waits on
// WSAEventSelect events, and wakes on a pipe where Windows sets m_event; otherwise the loop is the
// Windows one: it skips connections being serviced, signals a worker with the same read (2), write
// (1) and disconnect (8) flags, and keeps the same timeouts. poll() is level-triggered, so write
// readiness is only asked for while there is something to send, as the Windows version selects
// FD_WRITE only then.

struct WowConnectionWakeup {
    int32_t m_read;
    int32_t m_write;
};

static WowConnectionWakeup* Wakeup(void* event) {
    return static_cast<WowConnectionWakeup*>(event);
}

static void SignalWakeup(void* event) {
    WowConnectionWakeup* wakeup = Wakeup(event);

    if (wakeup) {
        uint8_t byte = 1;

        while (write(wakeup->m_write, &byte, 1) < 0 && errno == EINTR) {
        }
    }
}

static void DrainWakeup(void* event) {
    WowConnectionWakeup* wakeup = Wakeup(event);
    uint8_t buffer[64];

    while (read(wakeup->m_read, buffer, sizeof(buffer)) > 0) {
    }
}

static short PollEvents(WowConnection* connection, bool* include) {
    *include = true;

    switch (connection->m_connState) {
        case WOWC_CONNECTING:
            return POLLOUT;

        case WOWC_LISTENING:
            return POLLIN;

        case WOWC_CONNECTED:
            return POLLIN | (connection->m_sendList.Head() || connection->m_notifyWriteReady ? POLLOUT : 0);

        case WOWC_DISCONNECTING:
            // Windows keeps the socket's last event selection while it disconnects; readiness then
            // gets the connection serviced.
            return POLLIN | POLLOUT;

        default:
            *include = false;
            return 0;
    }
}

void WowConnectionNet::PlatformChangeState(WowConnection* connection, WOW_CONN_STATE state) {
    // The poll set is rebuilt every pass; waking the thread makes it pick up the new interest.
    SignalWakeup(this->m_event);
}

void WowConnectionNet::PlatformRun() {
    this->m_networkStarted = true;

    int32_t timeout = 500;

    if (this->m_stop) {
        return;
    }

    static const uint32_t kMaxConnections = 64;

    do {
        WowConnection* connections[kMaxConnections];
        pollfd fds[kMaxConnections];

        this->m_connectionsLock.Enter();

        connections[0] = nullptr;
        fds[0].fd = Wakeup(this->m_event)->m_read;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        uint32_t count = 1;

        for (WowConnection* connection = this->m_connections.Head(); connection; connection = this->m_connections.Next(connection)) {
            if (connection->m_serviceCount) {
                continue;
            }

            bool include;
            short events = PollEvents(connection, &include);

            if (!include) {
                continue;
            }

            if (connection->m_connState == WOWC_DISCONNECTING) {
                timeout = 0;
            }

            if (count < kMaxConnections) {
                connection->m_refCount.Increment();
                fds[count].fd = connection->m_sock;
                fds[count].events = events;
                fds[count].revents = 0;
                connections[count] = connection;
                count++;

                if (connection->m_lagTestRecvList.Head()) {
                    timeout = 0;
                }
            }
        }

        this->m_connectionsLock.Leave();

        int32_t result;

        do {
            result = poll(fds, count, timeout);
        } while (result < 0 && errno == EINTR);

        // As on Windows, connections are looked at once anything is signaled.
        if (result > 0) {
            if (fds[0].revents & POLLIN) {
                DrainWakeup(this->m_event);
            }

            for (uint32_t i = 1; i < count; i++) {
                WowConnection* connection = connections[i];
                short revents = fds[i].revents;
                uint32_t flags = 0;

                if (revents & (POLLIN | POLLHUP | POLLERR)) {
                    flags = 2;
                }

                if (revents & (POLLOUT | POLLHUP | POLLERR)) {
                    flags |= 1;
                }

                if (connection->m_connState == WOWC_DISCONNECTING) {
                    flags |= 8;
                }

                if (WowConnection::s_lagTestDelayMin || WowConnection::s_lagTestDelayMax) {
                    flags |= 3;
                    timeout = 10;
                } else {
                    timeout = 500;
                }

                if (flags) {
                    this->SignalWorker(connection, flags);
                }
            }
        }

        for (uint32_t i = 1; i < count; i++) {
            connections[i]->Release();
        }
    } while (!this->m_stop);
}

void WowConnectionNet::PlatformInit(bool useEngine) {
    int32_t fds[2];

    if (pipe(fds) != 0) {
        return;
    }

    for (int32_t fd : fds) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    WowConnectionWakeup* wakeup = new WowConnectionWakeup;
    wakeup->m_read = fds[0];
    wakeup->m_write = fds[1];
    this->m_event = wakeup;
}

void WowConnectionNet::PlatformDestruct(WowConnection* connection) {
    connection->m_event = nullptr;
}

void WowConnectionNet::PlatformWorkerReady() {
    SignalWakeup(this->m_event);
}

void WowConnectionNet::PlatformAdd(WowConnection* connection) {
    int32_t enable = 1;
    setsockopt(connection->m_sock, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

    // A send on a socket the peer closed raises SIGPIPE on macOS; Winsock reports an error instead,
    // which is what the connection code expects.
    setsockopt(connection->m_sock, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));

    SignalWakeup(this->m_event);
}

void WowConnectionNet::PlatformRemove(WowConnection* connection) {
    SignalWakeup(this->m_event);
}

void WowConnectionNet::PlatformStop() {
    SignalWakeup(this->m_event);
}

void WowConnectionNet::PlatformDestroy() {
    WowConnectionWakeup* wakeup = Wakeup(this->m_event);

    if (wakeup) {
        close(wakeup->m_read);
        close(wakeup->m_write);
        delete wakeup;
        this->m_event = nullptr;
    }
}
