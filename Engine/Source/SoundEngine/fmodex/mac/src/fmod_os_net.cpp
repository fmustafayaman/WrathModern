/*
    FMOD Ex networking for macOS (internet streams), with the behaviour of the Win32 layer: connects
    with FMOD_Net_Timeout, non-blocking listen and accept, blocking reads and writes.
*/

#include "fmod_net.h"
#include "fmod_os_misc.h"
#include "fmod_time.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool              FMOD_OS_Net_Initialised = false;
FMOD_OS_CRITICALSECTION *gResolveCrit            = 0;

static void *SocketHandle(int sock)
{
    return (void *)(intptr_t)sock;
}

static int SocketDescriptor(const void *handle)
{
    return (int)(intptr_t)handle;
}

static void SetNonBlocking(int sock, bool nonblocking)
{
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

FMOD_RESULT FMOD_OS_Net_Init()
{
    if (!FMOD_OS_Net_Initialised)
    {
        FMOD_RESULT result = FMOD_OS_CriticalSection_Create(&gResolveCrit);

        if (result != FMOD_OK)
        {
            return result;
        }

        FMOD_OS_Net_Initialised = true;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Shutdown()
{
    if (FMOD_OS_Net_Initialised)
    {
        FMOD_OS_CriticalSection_Free(gResolveCrit);
        gResolveCrit = 0;
        FMOD_OS_Net_Initialised = false;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Connect(const char *host, const unsigned short port, void **handle)
{
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_len = sizeof(server);
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server.sin_addr) != 1)
    {
        FMOD_RESULT result = FMOD_OS_CriticalSection_Enter(gResolveCrit);

        if (result != FMOD_OK)
        {
            return result;
        }

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo *info = 0;
        int resolved = getaddrinfo(host, 0, &hints, &info);

        if (resolved == 0 && info)
        {
            server.sin_addr = ((struct sockaddr_in *)info->ai_addr)->sin_addr;
            freeaddrinfo(info);
        }

        FMOD_OS_CriticalSection_Leave(gResolveCrit);

        if (resolved != 0)
        {
            return FMOD_ERR_NET_URL;
        }
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
    {
        return FMOD_ERR_MEMORY;
    }

    int enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));

    *handle = SocketHandle(sock);

    SetNonBlocking(sock, true);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0 && errno != EINPROGRESS)
    {
        close(sock);
        return FMOD_ERR_NET_CONNECT;
    }

    /*
        Wait up to the timeout for the connection to complete.
    */
    struct pollfd fd;
    fd.fd = sock;
    fd.events = POLLOUT;
    fd.revents = 0;

    int ready;

    do
    {
        ready = poll(&fd, 1, FMOD_Net_Timeout);
    } while (ready < 0 && errno == EINTR);

    int error = 0;
    socklen_t length = sizeof(error);

    if (ready <= 0 || getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error)
    {
        close(sock);
        return FMOD_ERR_NET_CONNECT;
    }

    SetNonBlocking(sock, false);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Listen(const unsigned short port, void **listenhandle)
{
    int listensocket = socket(AF_INET, SOCK_STREAM, 0);

    if (listensocket < 0)
    {
        return FMOD_ERR_MEMORY;
    }

    SetNonBlocking(listensocket, true);

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listensocket, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        close(listensocket);
        return FMOD_ERR_NET_SOCKET_ERROR;
    }

    if (listen(listensocket, 3) < 0)
    {
        close(listensocket);
        return FMOD_ERR_NET_SOCKET_ERROR;
    }

    *listenhandle = SocketHandle(listensocket);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Accept(const void *listenhandle, void **clienthandle)
{
    if (listenhandle == (void *)-1)
    {
        return FMOD_ERR_NET_SOCKET_ERROR;
    }

    int clientsocket = accept(SocketDescriptor(listenhandle), 0, 0);

    if (clientsocket < 0)
    {
        return errno == EWOULDBLOCK ? FMOD_ERR_NET_WOULD_BLOCK : FMOD_ERR_NET_SOCKET_ERROR;
    }

    int enable = 1;
    setsockopt(clientsocket, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));

    *clienthandle = SocketHandle(clientsocket);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Close(const void *handle)
{
    int sock = SocketDescriptor(handle);

    if (sock != -1)
    {
        close(sock);
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Write(const void *handle, const char *buf, const unsigned int len, unsigned int *byteswritten)
{
    if (handle == (void *)-1)
    {
        return FMOD_ERR_NET_SOCKET_ERROR;
    }

    int sock = SocketDescriptor(handle);
    unsigned int bytestowrite = len;
    *byteswritten = 0;

    while (bytestowrite)
    {
        ssize_t written = send(sock, buf, bytestowrite, 0);

        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return errno == EWOULDBLOCK ? FMOD_ERR_NET_WOULD_BLOCK : FMOD_ERR_NET_SOCKET_ERROR;
        }

        *byteswritten += (unsigned int)written;
        bytestowrite -= (unsigned int)written;
        buf += written;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_Read(const void *handle, char *buf, const unsigned int len, unsigned int *bytesread)
{
    if (handle == (void *)-1)
    {
        return FMOD_ERR_NET_SOCKET_ERROR;
    }

    if (!buf || (len <= 0))
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    int sock = SocketDescriptor(handle);
    unsigned int bytestoread = len;
    *bytesread = 0;

    while (bytestoread)
    {
        ssize_t read = recv(sock, buf, bytestoread, 0);

        if (read < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return errno == EWOULDBLOCK ? FMOD_ERR_NET_WOULD_BLOCK : FMOD_ERR_NET_SOCKET_ERROR;
        }

        if (read == 0)
        {
            /*
                Connection closed gracefully
            */
            return FMOD_ERR_FILE_EOF;
        }

        *bytesread += (unsigned int)read;
        bytestoread -= (unsigned int)read;
        buf += read;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Net_ReadLine(const void *handle, char *buf, const unsigned int len)
{
    unsigned int pos, read;
    char c = 0;

    if (handle == (void *)-1)
    {
        return FMOD_ERR_NET_SOCKET_ERROR;
    }

    if (!buf || (len <= 0))
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    buf[0] = 0;
    pos = 0;

    while (pos < len)
    {
        FMOD_RESULT result = FMOD_OS_Net_Read(handle, &c, 1, &read);

        if (result == FMOD_ERR_NET_WOULD_BLOCK)
        {
            return FMOD_ERR_NET_WOULD_BLOCK;
        }

        if (read == 1)
        {
            if (c == '\n')
            {
                break;
            }
            else if (c != '\r')
            {
                buf[pos++] = c;
            }
        }
        else
        {
            break;
        }
    }

    if (pos >= len)
    {
        pos = len - 1;
    }

    buf[pos] = 0;

    return FMOD_OK;
}
