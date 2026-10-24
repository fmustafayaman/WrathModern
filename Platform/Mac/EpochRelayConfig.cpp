#include "Platform/Mac/EpochRelay.h"
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace EpochRelay {

void ClearCredential(std::string& credential) {
    volatile char* bytes = credential.data();
    for (size_t i = 0; i < credential.size(); ++i) bytes[i] = 0;
    credential.clear();
}

bool IsLoginHost(const char* host, uint16_t port) {
    return host && port == 3724 &&
        (!strcasecmp(host, "game.project-epoch.org") || !strcasecmp(host, "game.project-epoch.org."));
}

bool ReadCredential(const char* dllPath, std::string& credential, std::string& error) {
    ClearCredential(credential);
    error.clear();
    // Bind the decoded field to the EXACT reviewed artifact. Unknown updates fail closed;
    // do not guess a token by scanning arbitrary strings or embed the key in our binary.
    // SHA-256 9af04f7afd21b0bc93860d66e6ccdc96ccf1b039ea31871119a3deaa271352c3.
    constexpr std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> expected = {
        0x9a, 0xf0, 0x4f, 0x7a, 0xfd, 0x21, 0xb0, 0xbc, 0x93, 0x86, 0x0d, 0x66, 0xe6, 0xcc, 0xdc, 0x96,
        0xcc, 0xf1, 0xb0, 0x39, 0xea, 0x31, 0x87, 0x11, 0x19, 0xa3, 0xde, 0xaa, 0x27, 0x13, 0x52, 0xc3
    };
    constexpr size_t imageSize = 160768;
    constexpr size_t keyOffset = 0x1a660;
    constexpr size_t keySize = 64;

    int fd = dllPath ? open(dllPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    if (fd < 0) {
        error = "Cannot open EpochConnection.dll next to the app";
        return false;
    }
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size != imageSize) {
        close(fd);
        error = "Unsupported EpochConnection.dll revision (file size/type)";
        return false;
    }
    struct Snapshot {
        std::array<unsigned char, imageSize> bytes;
        ~Snapshot() {
            volatile unsigned char* data = bytes.data();
            for (size_t i = 0; i < bytes.size(); ++i) data[i] = 0;
        }
    } snapshot;
    auto& image = snapshot.bytes;
    size_t size = 0;
    while (size < image.size()) {
        ssize_t count = read(fd, image.data() + size, image.size() - size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        size += static_cast<size_t>(count);
    }
    close(fd);
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest;
    CC_SHA256(image.data(), static_cast<CC_LONG>(size), digest.data());
    if (size != image.size() || digest != expected) {
        error = "Unsupported or changed EpochConnection.dll (SHA-256 mismatch)";
        return false;
    }
    if (image[keyOffset + keySize] != 0) {
        error = "Invalid Epoch relay configuration";
        return false;
    }
    for (size_t i = 0; i < keySize; ++i) {
        unsigned char c = image[keyOffset + i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            error = "Invalid Epoch relay configuration";
            return false;
        }
    }
    credential.assign(reinterpret_cast<const char*>(image.data() + keyOffset), keySize);
    // Snapshot is erased on every return. The key remains only in memory for HTTPS requests.
    return true;
}

}
