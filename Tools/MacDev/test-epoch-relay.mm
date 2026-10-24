// Standalone harness: does not link or launch the game. Built by test-epoch-relay.py.
#include "Platform/Mac/EpochRelay.h"
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static void Require(bool result, const char* message) {
    if (!result) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}
static void Ready() {
    auto s = EpochRelay::StateForTesting();
    Require(EpochRelay::ProtocolBuild(htonl(INADDR_LOOPBACK), s.authPort) == 12341, "Epoch login protocol build");
    Require(EpochRelay::ProtocolBuild(htonl(INADDR_LOOPBACK), s.worldPort) == 12341, "Epoch world protocol build");
    Require(EpochRelay::ProtocolBuild(htonl(INADDR_LOOPBACK), 1) == 12340, "other local servers keep 12340");
    Require(EpochRelay::ProtocolBuild(htonl(0x7f000002), s.authPort) == 12340, "other addresses keep 12340");
    Require(EpochRelay::ProtocolBuild(htonl(0x08080808), s.worldPort) == 12340, "other servers keep 12340");
    printf("READY %u %u\n", s.authPort, s.worldPort);
    fflush(stdout);
}
static int Connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address = {};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(fd >= 0 && !connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), "loopback connect");
    return fd;
}
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--self-test")) {
            Require(EpochRelay::IsLoginHost("game.project-epoch.org", 3724), "Epoch host");
            Require(EpochRelay::IsLoginHost("GAME.PROJECT-EPOCH.ORG.", 3724), "case / trailing dot");
            Require(!EpochRelay::IsLoginHost(nullptr, 3724), "null host");
            Require(!EpochRelay::IsLoginHost("", 3724), "empty host");
            Require(!EpochRelay::IsLoginHost("game.project-epoch.org.evil", 3724), "host suffix");
            Require(!EpochRelay::IsLoginHost("other.project-epoch.org", 3724), "other subdomain");
            Require(!EpochRelay::IsLoginHost("game.project-epoch.org", 443), "other port");
            Require(!EpochRelay::IsLoginHost("127.0.0.1", 3724), "other servers unchanged");
            Require(EpochRelay::ProtocolBuild(htonl(INADDR_LOOPBACK), 3724) == 12340, "inactive relay has stock build");
            Require(!EpochRelay::StartForTesting("ws://example.com:1234", 1), "test origin is loopback only");
            Require(!EpochRelay::StartForTesting("ws://u:p@127.0.0.1:1234", 1), "no user info");
            Require(!EpochRelay::StartForTesting("ws://127.0.0.1:1234?key=bad", 1), "no URL credentials");
            Require(!EpochRelay::StartForTesting("ws://127.0.0.1:1234", 0), "positive timeout");
            puts("PASS host routing and test-origin validation");
            return 0;
        }
        if (argc == 3 && !strcmp(argv[1], "--credential")) {
            std::string key, error;
            if (!EpochRelay::ReadCredential(argv[2], key, error)) {
                Require(key.empty(), "failed import did not retain key");
                fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            Require(key.size() == 64 && error.empty(), "credential import");
            EpochRelay::ClearCredential(key);
            puts("PASS credential revision and format verified (not printed)");
            return 0;
        }
        if (argc == 4 && !strcmp(argv[1], "--challenge-probe")) {
            // Bounded unauthenticated metadata probe: account identifier from stdin, NO password
            // or SRP proof. Prints only the server's three-byte initial response header.
            std::string account;
            std::getline(std::cin, account);
            Require(!account.empty() && account.size() <= 64, "account identifier length");
            for (char& c : account) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            unsigned build = strtoul(argv[2], nullptr, 10);
            uint32_t os = static_cast<uint32_t>(strtoul(argv[3], nullptr, 16));
            Require((build == 12340 || build == 12341) && (os == 0x4f5358 || os == 0x57696e), "reviewed profiles only");
            unsigned char packet[128] = {};
            size_t n = 0;
            auto put = [&](uint32_t value, unsigned bytes) {
                for (unsigned i = 0; i < bytes; ++i) packet[n++] = static_cast<unsigned char>(value >> (8 * i));
            };
            put(0, 1); put(8, 1); put(30 + account.size(), 2);
            put(0x576f57, 4); put(3, 1); put(3, 1); put(5, 1); put(build, 2);
            put(0x783836, 4); put(os, 4); put(0x656e5553, 4); put(0, 4); put(0, 4);
            put(account.size(), 1);
            for (char c : account) put(static_cast<unsigned char>(c), 1);
            Require(n == 34 + account.size(), "challenge packet size");
            uint16_t port = EpochRelay::LoginPort();
            Require(port != 0, "production configuration");
            int fd = Connect(port);
            timeval timeout = { 8, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            Require(send(fd, packet, n, 0) == static_cast<ssize_t>(n), "challenge send");
            unsigned char response[3];
            size_t received = 0;
            while (received < sizeof(response)) {
                ssize_t count = recv(fd, response + received, sizeof(response) - received, 0);
                Require(count > 0, "initial challenge response");
                received += static_cast<size_t>(count);
            }
            printf("CHALLENGE build=%u os=0x%06x opcode=%u protocol=%u result=%u (NO proof/login sent)\n",
                   build, os, response[0], response[1], response[2]);
            close(fd);
            EpochRelay::Shutdown();
            return 0;
        }
        if (argc == 2 && !strcmp(argv[1], "--probe")) {
            uint16_t port = EpochRelay::LoginPort();
            Require(port != 0, "production configuration");
            auto s = EpochRelay::StateForTesting();
            int auth = Connect(s.authPort), world = Connect(s.worldPort);
            bool opened = false;
            for (unsigned i = 0; i < 240; ++i) {
                s = EpochRelay::StateForTesting();
                if (s.opened == 2) { opened = true; break; }
                if (s.closed) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            close(auth);
            close(world);
            EpochRelay::Shutdown();
            Require(opened, "production TLS / WebSocket upgrade");
            puts("PASS login + world TLS/WebSocket upgrades; NO game login attempted");
            return 0;
        }
        if (argc == 4 && !strcmp(argv[1], "--serve")) {
            double timeout = atof(argv[3]);
            Require(EpochRelay::StartForTesting(argv[2], timeout), "test listeners");
            Ready();
            std::string command;
            while (std::getline(std::cin, command)) {
                if (command == "stats") {
                    auto s = EpochRelay::StateForTesting();
                    printf("STATS %u %u %u\n", s.active, s.opened, s.closed);
                } else if (command == "stop") {
                    auto old = EpochRelay::StateForTesting();
                    EpochRelay::Shutdown();
                    Require(EpochRelay::ProtocolBuild(htonl(INADDR_LOOPBACK), old.authPort) == 12340, "login profile cleared on stop");
                    Require(EpochRelay::ProtocolBuild(htonl(INADDR_LOOPBACK), old.worldPort) == 12340, "world profile cleared on stop");
                    puts("STOPPED");
                } else if (command == "start") {
                    Require(EpochRelay::StartForTesting(argv[2], timeout), "restart listeners");
                    Ready();
                } else {
                    Require(false, "unknown harness command");
                }
                fflush(stdout);
            }
            EpochRelay::Shutdown();
            return 0;
        }
        fprintf(stderr, "usage: --self-test | --credential <dll> | --serve <loopback-ws-url> <timeout> | --probe\n");
        return 2;
    }
}
