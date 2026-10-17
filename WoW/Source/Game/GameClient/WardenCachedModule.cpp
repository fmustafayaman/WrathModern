/*
 * WardenCachedModule and CWardenKey implementations. The methods are unchanged by the target
 * platform, so they live here once: the Win32 client file and the Win64 stub used to carry
 * their own copies, and the emulated macOS warden client needs them as well.
 */

#include "WoW/Source/Game/GameClient/WardenClient.h"
#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/Storm/H/storm.h"

// VERIFY: 29af4aa4 auto
// OFFSET: 0x007DA190
CDataStore& operator<<(CDataStore& msg, const CWardenKey& key) {
    for (uint32_t i = 0; i < sizeof(key.m_key); i++) {
        msg.Put(key.m_key[i]);
    }

    return msg;
}

// VERIFY: 43fb4be5 auto
// OFFSET: 0x007DA1C0
CDataStore& operator>>(CDataStore& msg, CWardenKey& key) {
    for (uint32_t i = 0; i < sizeof(key.m_key); i++) {
        msg.Get(key.m_key[i]);
    }

    return msg;
}

// VERIFY: 0dc2a3a3 auto
// OFFSET: 0x007DA1F0
WardenCachedModule::WardenCachedModule() {
    this->m_size = 0;
    this->m_data = nullptr;
}

// VERIFY: 6ea37899 manual
// OFFSET: 0x007DA200
WardenCachedModule::~WardenCachedModule() {
    if (this->m_data) {
        SMemFree(this->m_data, __FILE__, __LINE__, 0);
    }
}

// VERIFY: 7c74c6f8 auto
// OFFSET: 0x007DA220
void WardenCachedModule::Pack(CDataStore* msg) {
    msg->Put(this->m_data ? this->m_size : 0);

    for (uint32_t i = 0; i < this->m_size; i++) {
        msg->Put(this->m_data[i]);
    }
}

// VERIFY: 21e23518 manual
// OFFSET: 0x007DA260
void WardenCachedModule::Unpack(CDataStore* msg) {
    msg->Get(this->m_size);

    if (!this->m_size) {
        this->m_data = nullptr;
        return;
    }

    this->m_data = static_cast<uint8_t*>(SMemAlloc(this->m_size, __FILE__, __LINE__, 0));

    for (uint32_t i = 0; i < this->m_size; i++) {
        msg->Get(this->m_data[i]);
    }
}

// VERIFY: 2bf82dd5 auto
// OFFSET: 0x007DA5C0
WardenCachedModule& WardenCachedModule::operator=(const WardenCachedModule& source) {
    uint8_t* data = source.m_data;
    uint32_t size = source.m_size;

    if (!data) {
        this->m_size = 0;
        this->m_data = nullptr;
        return *this;
    }

    this->m_size = size;
    this->m_data = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));
    memcpy(this->m_data, data, size);
    return *this;
}
