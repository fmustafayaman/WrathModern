#include "Platform/Mac/EpochRelay.h"
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// A byte-stream bridge, not a second game protocol implementation. SRP, world authentication,
// encryption, realm selection and the client's OSX/x86 identity stay in the existing client.
// All mutable transport state is confined to this serial queue. Neither the game/render thread
// nor the queue ever waits for a network operation. At most one 64 KiB WebSocket send and one
// <=1 MiB WebSocket receive are in flight per connection (TCP/WS backpressure in both directions).
namespace {

dispatch_queue_t Queue() {
    static dispatch_queue_t queue = dispatch_queue_create("org.wrathmodern.epoch-relay",
        dispatch_queue_attr_make_with_autorelease_frequency(DISPATCH_QUEUE_SERIAL, DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM));
    return queue;
}

void Log(const char* message, long code = 0) {
    // Only fixed diagnostic text and numbers. No headers, URLs with credentials, account names,
    // packet bytes, session keys, or NSError descriptions (which can contain request details).
    fprintf(stderr, "[EpochRelay] %s (%ld)\n", message, code);
    int fd = open("Logs/EpochRelay.log", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        dprintf(fd, "%s (%ld)\n", message, code);
        close(fd);
    }
}

bool ConfigureSocket(int fd) {
    int yes = 1;
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0;
}

int Listener(uint16_t& port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr = {};
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(addr);
    if (!ConfigureSocket(fd) || bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ||
        listen(fd, 16) || getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &size)) {
        close(fd);
        return -1;
    }
    port = ntohs(addr.sin_port);
    return fd;
}

}

@interface WMEpochStream : NSObject <NSURLSessionWebSocketDelegate> {
    int _socket;
    BOOL _closed;
    BOOL _opened;
    BOOL _readSuspended;
    BOOL _writeSuspended;
    BOOL _pingPending;
    BOOL _auth;
    dispatch_source_t _reader;
    dispatch_source_t _writer;
    dispatch_source_t _timer;
    NSURLSession* _session;
    NSURLSessionWebSocketTask* _task;
    NSData* _pendingWrite;
    size_t _writeOffset;
    void (^_onOpen)(void);
    void (^_onClose)(WMEpochStream*);
}
- (instancetype)initWithSocket:(int)fd auth:(BOOL)auth;
- (void)start:(NSURL*)url credential:(NSString*)credential timeout:(double)timeout
      onOpen:(void (^)(void))onOpen onClose:(void (^)(WMEpochStream*))onClose;
- (void)close:(const char*)reason code:(long)code;
@end

@implementation WMEpochStream

- (instancetype)initWithSocket:(int)fd auth:(BOOL)auth {
    if ((self = [super init])) {
        _socket = fd;
        _auth = auth;
        _readSuspended = YES;
        _writeSuspended = YES;
    }
    return self;
}

- (void)start:(NSURL*)url credential:(NSString*)credential timeout:(double)timeout
      onOpen:(void (^)(void))onOpen onClose:(void (^)(WMEpochStream*))onClose {
    _onOpen = [onOpen copy];
    _onClose = [onClose copy];
    __weak WMEpochStream* weakSelf = self;
    _reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, _socket, 0, Queue());
    _writer = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, _socket, 0, Queue());
    dispatch_source_set_event_handler(_reader, ^{ [weakSelf readSocket]; });
    dispatch_source_set_event_handler(_writer, ^{ [weakSelf writeSocket]; });
    // Both sources reference this descriptor. Close only after BOTH cancellation handlers ran;
    // otherwise a reused fd could still be registered with kqueue by the other source.
    dispatch_group_t cancelled = dispatch_group_create();
    dispatch_group_enter(cancelled);
    dispatch_group_enter(cancelled);
    dispatch_source_set_cancel_handler(_reader, ^{ dispatch_group_leave(cancelled); });
    dispatch_source_set_cancel_handler(_writer, ^{ dispatch_group_leave(cancelled); });
    int fd = _socket;
    dispatch_group_notify(cancelled, Queue(), ^{ close(fd); });

    NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.URLCache = nil;
    config.HTTPCookieStorage = nil;
    config.HTTPShouldSetCookies = NO;
    config.waitsForConnectivity = NO;
    config.timeoutIntervalForRequest = timeout;
    NSOperationQueue* callbacks = [[NSOperationQueue alloc] init];
    callbacks.maxConcurrentOperationCount = 1;
    callbacks.underlyingQueue = Queue();
    _session = [NSURLSession sessionWithConfiguration:config delegate:self delegateQueue:callbacks];
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
    [request setValue:[@"Bearer " stringByAppendingString:credential] forHTTPHeaderField:@"Authorization"];
    [request setValue:@"EpochTunnelTestLauncher/0.1 (+https://project-epoch.org)" forHTTPHeaderField:@"User-Agent"];
    _task = [_session webSocketTaskWithRequest:request];
    _task.maximumMessageSize = 1024 * 1024;
    [_task resume];

    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, Queue());
    dispatch_source_set_timer(_timer, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(timeout * NSEC_PER_SEC)),
                             30 * NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(_timer, ^{
        WMEpochStream* stream = weakSelf;
        if (!stream || stream->_closed) {
            return;
        }
        if (!stream->_opened) {
            [stream close:"WebSocket handshake timed out" code:0];
        } else if (stream->_pingPending) {
            [stream close:"WebSocket keepalive timed out" code:0];
        } else {
            stream->_pingPending = YES;
            [stream->_task sendPingWithPongReceiveHandler:^(NSError* error) {
                dispatch_async(Queue(), ^{
                    WMEpochStream* current = weakSelf;
                    if (!current || current->_closed) {
                        return;
                    }
                    current->_pingPending = NO;
                    if (error) {
                        [current close:"WebSocket keepalive failed" code:error.code];
                    }
                });
            }];
        }
    });
    dispatch_resume(_timer);
}

- (void)readSocket {
    if (_closed || !_opened) {
        return;
    }
    unsigned char buffer[65536];
    ssize_t size;
    do {
        size = recv(_socket, buffer, sizeof(buffer), 0);
    } while (size < 0 && errno == EINTR);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    if (size <= 0) {
        [self close:"TCP stream closed" code:size < 0 ? errno : 0];
        return;
    }
    dispatch_suspend(_reader);
    _readSuspended = YES;
    NSData* bytes = [NSData dataWithBytes:buffer length:static_cast<NSUInteger>(size)];
    NSURLSessionWebSocketMessage* message = [[NSURLSessionWebSocketMessage alloc] initWithData:bytes];
    __weak WMEpochStream* weakSelf = self;
    [_task sendMessage:message completionHandler:^(NSError* error) {
        dispatch_async(Queue(), ^{
            WMEpochStream* stream = weakSelf;
            if (!stream || stream->_closed) {
                return;
            }
            if (error) {
                [stream close:"WebSocket send failed" code:error.code];
            } else {
                stream->_readSuspended = NO;
                dispatch_resume(stream->_reader);
            }
        });
    }];
}

- (void)receive {
    if (_closed) {
        return;
    }
    __weak WMEpochStream* weakSelf = self;
    [_task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message, NSError* error) {
        dispatch_async(Queue(), ^{
            WMEpochStream* stream = weakSelf;
            if (!stream || stream->_closed) {
                return;
            }
            if (error) {
                [stream close:"WebSocket receive failed" code:error.code];
            } else if (message.type != NSURLSessionWebSocketMessageTypeData) {
                [stream close:"Relay sent a non-binary message" code:0];
            } else {
                stream->_pendingWrite = message.data;
                stream->_writeOffset = 0;
                [stream writeSocket];
            }
        });
    }];
}

- (void)writeSocket {
    if (_closed || !_pendingWrite) {
        return;
    }
    while (_writeOffset < _pendingWrite.length) {
        const unsigned char* data = static_cast<const unsigned char*>(_pendingWrite.bytes);
        ssize_t size = send(_socket, data + _writeOffset, _pendingWrite.length - _writeOffset, 0);
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (_writeSuspended) {
                _writeSuspended = NO;
                dispatch_resume(_writer);
            }
            return;
        }
        if (size <= 0) {
            [self close:"TCP write failed" code:size < 0 ? errno : 0];
            return;
        }
        _writeOffset += static_cast<size_t>(size);
    }
    if (!_writeSuspended) {
        dispatch_suspend(_writer);
        _writeSuspended = YES;
    }
    _pendingWrite = nil;
    // Only ask Foundation for the next WS message after TCP consumed this one.
    [self receive];
}

- (void)URLSession:(NSURLSession*)session webSocketTask:(NSURLSessionWebSocketTask*)task
 didOpenWithProtocol:(NSString*)protocol {
    if (_closed) {
        return;
    }
    _opened = YES;
    Log(_auth ? "Login WebSocket opened" : "World WebSocket opened");
    if (_onOpen) {
        _onOpen();
    }
    _readSuspended = NO;
    dispatch_resume(_reader);
    [self receive];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
 willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request
 completionHandler:(void (^)(NSURLRequest*))completionHandler {
    // A relay redirect must not take the shared credential to another origin.
    completionHandler(nil);
    [self close:"Relay HTTP redirect refused" code:response.statusCode];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    if (!_closed) {
        long status = [task.response isKindOfClass:[NSHTTPURLResponse class]] ?
            static_cast<NSHTTPURLResponse*>(task.response).statusCode : 0;
        Log("Relay HTTP status", status);
        [self close:"WebSocket task ended" code:error.code];
    }
}

- (void)URLSession:(NSURLSession*)session webSocketTask:(NSURLSessionWebSocketTask*)task
 didCloseWithCode:(NSURLSessionWebSocketCloseCode)closeCode reason:(NSData*)reason {
    [self close:"Relay closed WebSocket" code:closeCode];
}

- (void)close:(const char*)reason code:(long)code {
    if (_closed) {
        return;
    }
    _closed = YES;
    Log(reason, code);
    shutdown(_socket, SHUT_RDWR);
    _socket = -1;
    if (_readSuspended) {
        dispatch_resume(_reader);
    }
    if (_writeSuspended) {
        dispatch_resume(_writer);
    }
    dispatch_source_cancel(_reader);
    dispatch_source_cancel(_writer);
    _reader = nil;
    _writer = nil;
    dispatch_source_cancel(_timer);
    _timer = nil;
    [_task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
    [_session invalidateAndCancel];
    _task = nil;
    _session = nil;
    _pendingWrite = nil;
    _onOpen = nil;
    if (_onClose) {
        _onClose(self);
        _onClose = nil;
    }
}
@end

@interface WMEpochRelay : NSObject {
    dispatch_source_t _authListener;
    dispatch_source_t _worldListener;
    NSMutableSet<WMEpochStream*>* _streams;
    NSString* _credential;
    NSString* _baseURL;
    double _timeout;
@public
    uint16_t authPort;
    uint16_t worldPort;
    unsigned opened;
    unsigned closed;
}
- (BOOL)start:(NSString*)url credential:(NSString*)credential timeout:(double)timeout;
- (void)stop;
- (unsigned)active;
@end

@implementation WMEpochRelay
- (dispatch_source_t)watch:(int)fd auth:(BOOL)auth {
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, Queue());
    dispatch_source_set_cancel_handler(source, ^{ close(fd); });
    __weak WMEpochRelay* weakSelf = self;
    dispatch_source_set_event_handler(source, ^{
        WMEpochRelay* relay = weakSelf;
        if (!relay) {
            return;
        }
        for (unsigned i = 0; i < 16; ++i) {
            int client = accept(fd, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (relay->_streams.count >= 16 || !ConfigureSocket(client)) {
                close(client);
                continue;
            }
            int yes = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            NSString* path = auth ? [NSString stringWithFormat:@"/relay/auth?worldPort=%u", relay->worldPort] : @"/relay/world";
            NSURL* url = [NSURL URLWithString:[relay->_baseURL stringByAppendingString:path]];
            WMEpochStream* stream = [[WMEpochStream alloc] initWithSocket:client auth:auth];
            [relay->_streams addObject:stream];
            [stream start:url credential:relay->_credential timeout:relay->_timeout onOpen:^{
                WMEpochRelay* current = weakSelf;
                if (current) {
                    ++current->opened;
                }
            } onClose:^(WMEpochStream* ended) {
                WMEpochRelay* current = weakSelf;
                if (current) {
                    [current->_streams removeObject:ended];
                    ++current->closed;
                }
            }];
        }
    });
    dispatch_resume(source);
    return source;
}
- (BOOL)start:(NSString*)url credential:(NSString*)credential timeout:(double)timeout {
    _baseURL = [url copy];
    _credential = [credential copy];
    _timeout = timeout;
    _streams = [[NSMutableSet alloc] init];
    int auth = Listener(authPort);
    int world = Listener(worldPort);
    if (auth < 0 || world < 0) {
        if (auth >= 0) close(auth);
        if (world >= 0) close(world);
        authPort = worldPort = 0;
        Log("Cannot create loopback relay listeners", errno);
        return NO;
    }
    _authListener = [self watch:auth auth:YES];
    _worldListener = [self watch:world auth:NO];
    Log("Loopback login listener ready", authPort);
    Log("Loopback world listener ready", worldPort);
    return YES;
}
- (void)stop {
    if (_authListener) dispatch_source_cancel(_authListener);
    if (_worldListener) dispatch_source_cancel(_worldListener);
    _authListener = _worldListener = nil;
    for (WMEpochStream* stream in [_streams allObjects]) {
        [stream close:"Native relay stopped" code:0];
    }
    _credential = nil;
    authPort = worldPort = 0;
}
- (unsigned)active { return static_cast<unsigned>(_streams.count); }
@end

namespace {
WMEpochRelay* s_relay;
}

namespace EpochRelay {

uint16_t LoginPort() {
    __block uint16_t port = 0;
    dispatch_sync(Queue(), ^{
        @autoreleasepool {
            if (!s_relay) {
                std::string credential, error;
                if (!ReadCredential("EpochConnection.dll", credential, error)) {
                    Log(error.c_str());
                    return;
                }
                NSString* key = [[NSString alloc] initWithBytes:credential.data() length:credential.size() encoding:NSASCIIStringEncoding];
                ClearCredential(credential);
                WMEpochRelay* relay = [[WMEpochRelay alloc] init];
                // Fixed origin, OS certificate validation, no plaintext/direct fallback.
                if (![relay start:@"wss://game.project-epoch.org" credential:key timeout:10.0]) {
                    return;
                }
                s_relay = relay;
            }
            port = s_relay->authPort;
        }
    });
    return port;
}

uint16_t ProtocolBuild(uint32_t networkOrderAddress, uint16_t port) {
    if (networkOrderAddress != htonl(INADDR_LOOPBACK) || !port) {
        return 12340;
    }
    __block uint16_t build = 12340;
    dispatch_sync(Queue(), ^{
        if (s_relay && (port == s_relay->authPort || port == s_relay->worldPort)) {
            build = 12341;
            Log("Epoch protocol build selected", build);
        }
    });
    return build;
}

void Shutdown() {
    dispatch_sync(Queue(), ^{
        [s_relay stop];
        s_relay = nil;
    });
}

#if defined(WRATHMODERN_EPOCH_RELAY_TESTING)
bool StartForTesting(const char* loopbackURL, double handshakeTimeout) {
    NSString* base = loopbackURL ? [NSString stringWithUTF8String:loopbackURL] : nil;
    NSURLComponents* url = [NSURLComponents componentsWithString:base];
    if (!url || ![url.host isEqualToString:@"127.0.0.1"] ||
        !([url.scheme isEqualToString:@"ws"] || [url.scheme isEqualToString:@"wss"]) ||
        !url.port || url.user || url.password || url.query || url.fragment || url.path.length ||
        handshakeTimeout <= 0 || handshakeTimeout > 10) {
        return false;
    }
    __block BOOL result;
    dispatch_sync(Queue(), ^{
        [s_relay stop];
        s_relay = [[WMEpochRelay alloc] init];
        result = [s_relay start:base credential:@"test-relay-key" timeout:handshakeTimeout];
        if (!result) s_relay = nil;
    });
    return result;
}
TestState StateForTesting() {
    __block TestState result = {};
    dispatch_sync(Queue(), ^{
        if (s_relay) result = { s_relay->authPort, s_relay->worldPort, [s_relay active], s_relay->opened, s_relay->closed };
    });
    return result;
}
#endif

}
