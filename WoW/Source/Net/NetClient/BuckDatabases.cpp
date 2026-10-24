#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

#include "WoW/Source/Net/NetClient/BuckDatabases.h"
#include "WoW/Source/DB/DBClient/DBClient.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <type_traits>
#include <vector>

namespace {

struct RecordField {
    uint32_t guestOffset;
    size_t nativeOffset;
    size_t nativeSize;
    bool string;
};

#define BUCK_RECORD_FIELD(T, offset, member) \
    { offset, offsetof(T, member), sizeof(((T*)nullptr)->member), \
        std::is_pointer<decltype(((T*)nullptr)->member)>::value }

#include "WoW/Source/Net/NetClient/BuckDatabaseLayouts.inl"

#undef BUCK_RECORD_FIELD

// Custom record strings must outlive all records referring to them, including UI reloads.
// DBC record destructors do not free individual strings; the original pool remains owned by DB.
std::map<std::string, std::string> s_contentStrings;

bool ReadString(const BuckDatabase& source, uint32_t offset, const char*& value) {
    if (offset == 0xFFFFFFFF) {
        value = nullptr;
        return true;
    }

    if (offset >= source.strings.size()) {
        return false;
    }

    const char* start = reinterpret_cast<const char*>(source.strings.data() + offset);
    const void* end = memchr(start, 0, source.strings.size() - offset);

    if (!end) {
        return false;
    }

    std::string text(start, static_cast<const char*>(end) - start);
    auto entry = s_contentStrings.emplace(text, text);
    value = entry.first->second.c_str();
    return true;
}

template <class T, class DB, size_t N>
bool ReadNative(DB& native, uint32_t stride, const RecordField (&fields)[N], BuckDatabase& out) {
    if (!native.m_loaded) {
        native.Load(__FILE__, __LINE__);
    }

    if (!native.m_loaded || native.m_numRecords < 0) {
        return false;
    }

    out = BuckDatabase();
    out.rle = std::is_same<DB, WowClientDB_RLE<T>>::value;
    out.recordSize = stride;
    out.numRecords = native.m_numRecords;
    out.minID = native.m_minID;
    out.maxID = native.m_maxID;
    out.records.resize(static_cast<size_t>(stride) * out.numRecords);
    out.strings.push_back(0);
    std::map<const char*, uint32_t> strings;

    for (const RecordField& field : fields) {
        size_t width = field.string ? 4 : field.nativeSize;

        if (field.guestOffset + width > stride || field.nativeOffset + field.nativeSize > sizeof(T)) {
            return false;
        }

        if (field.string) {
            out.stringFields.push_back(field.guestOffset);
        }
    }

    for (int32_t i = 0; i < out.numRecords; i++) {
        T record;

        if (!native.GetRecordByIndex(i, static_cast<void*>(&record))) {
            return false;
        }

        out.ids.push_back(record.m_id);
        const uint8_t* source = reinterpret_cast<const uint8_t*>(&record);
        uint8_t* row = out.records.data() + static_cast<size_t>(i) * stride;

        for (const RecordField& field : fields) {
            if (!field.string) {
                memcpy(row + field.guestOffset, source + field.nativeOffset, field.nativeSize);
                continue;
            }

            const char* value = nullptr;
            memcpy(&value, source + field.nativeOffset, sizeof(value));
            uint32_t offset = 0xFFFFFFFF;

            if (value) {
                auto entry = strings.find(value);

                if (entry == strings.end()) {
                    offset = static_cast<uint32_t>(out.strings.size());
                    size_t length = strlen(value) + 1;
                    out.strings.insert(out.strings.end(), value, value + length);
                    strings.emplace(value, offset);
                } else {
                    offset = entry->second;
                }
            }

            memcpy(row + field.guestOffset, &offset, 4);
        }
    }

    return true;
}

template <class T>
bool ReplaceRows(WowClientDB<T>& native, const std::vector<T>& rows, int32_t minID, int32_t maxID) {
    T* records = static_cast<T*>(SMemAlloc(rows.size() * sizeof(T), __FILE__, __LINE__, 0));
    T** index = new (__FILE__, __LINE__) T*[static_cast<size_t>(maxID - minID) + 1]();
    for (size_t i = 0; i < rows.size(); i++) {
        new (records + i) T(rows[i]);
        index[records[i].m_id - minID] = records + i;
    }

    SMemFree(native.m_records, __FILE__, __LINE__, 0);
    delete[] native.m_recordsById;
    native.m_records = records;
    native.m_recordsById = index;
    native.m_numRecords = static_cast<int32_t>(rows.size());
    native.m_minID = minID;
    native.m_maxID = maxID;
    return true;
}

template <class T>
bool ReplaceRows(WowClientDB_RLE<T>& native, const std::vector<T>& rows, int32_t minID, int32_t maxID) {
    uint32_t bytes = 0;

    for (const T& row : rows) {
        bytes += g_clientDbPerformCompress
            ? CompressRow(reinterpret_cast<const uint8_t*>(&row), sizeof(T), nullptr) : sizeof(T);
    }

    uint8_t* data = static_cast<uint8_t*>(SMemAlloc(bytes, __FILE__, __LINE__, 0));
    size_t range = static_cast<size_t>(maxID - minID) + 1;
    T** index = static_cast<T**>(SMemAlloc((rows.size() + range) * sizeof(T*), __FILE__, __LINE__, 0));
    T** byID = index + rows.size();
    memset(byID, 0, range * sizeof(T*));
    uint8_t* destination = data;

    for (size_t i = 0; i < rows.size(); i++) {
        index[i] = reinterpret_cast<T*>(destination);
        byID[rows[i].m_id - minID] = index[i];

        if (g_clientDbPerformCompress) {
            destination += CompressRow(reinterpret_cast<const uint8_t*>(&rows[i]), sizeof(T), destination);
        } else {
            memcpy(destination, &rows[i], sizeof(T));
            destination += sizeof(T);
        }
    }

    SMemFree(native.m_data, __FILE__, __LINE__, 0);
    SMemFree(native.m_records, __FILE__, __LINE__, 0);
    native.m_data = reinterpret_cast<T*>(data);
    native.m_records = index;
    native.m_recordsById = byID;
    native.m_numRecords = static_cast<int32_t>(rows.size());
    native.m_minID = minID;
    native.m_maxID = maxID;
    return true;
}

template <class T, class DB, size_t N>
bool CommitNative(DB& native, uint32_t stride, const RecordField (&fields)[N], const BuckDatabase& changed) {
    if (!native.m_loaded || changed.recordSize != stride || changed.ids.empty()
        || changed.records.size() != changed.ids.size() * stride) {
        return false;
    }

    std::vector<T> rows(native.m_numRecords);
    std::map<int32_t, size_t> byID;

    for (int32_t i = 0; i < native.m_numRecords; i++) {
        if (!native.GetRecordByIndex(i, static_cast<void*>(&rows[i]))) {
            return false;
        }

        byID[rows[i].m_id] = i;
    }

    for (size_t i = 0; i < changed.ids.size(); i++) {
        T record;
        memset(static_cast<void*>(&record), 0, sizeof(record));
        uint8_t* destination = reinterpret_cast<uint8_t*>(&record);
        const uint8_t* source = changed.records.data() + i * stride;

        for (const RecordField& field : fields) {
            if (field.string) {
                uint32_t offset = 0;
                memcpy(&offset, source + field.guestOffset, 4);
                const char* value = nullptr;

                if (!ReadString(changed, offset, value)) {
                    return false;
                }

                memcpy(destination + field.nativeOffset, &value, sizeof(value));
            } else {
                memcpy(destination + field.nativeOffset, source + field.guestOffset, field.nativeSize);
            }
        }

        if (record.m_id != changed.ids[i] || record.m_id < 0 || record.m_id > 10000000) {
            return false;
        }

        auto found = byID.find(record.m_id);

        if (found == byID.end()) {
            byID[record.m_id] = rows.size();
            rows.push_back(record);
        } else {
            rows[found->second] = record;
        }
    }

    int32_t minID = rows.front().m_id;
    int32_t maxID = minID;

    for (const T& row : rows) {
        minID = std::min(minID, row.m_id);
        maxID = std::max(maxID, row.m_id);
    }

    return ReplaceRows(native, rows, minID, maxID);
}

} // namespace

bool BuckReadNativeDatabase(uint32_t address, BuckDatabase& database) {
    switch (address) {
#define READ_DB(address, size, T, variable) \
        case address: return ReadNative<T>(variable, size, s_##T##Fields, database);
        BUCK_DATABASE_BINDINGS(READ_DB)
#undef READ_DB
        default: return false;
    }
}

bool BuckCommitNativeDatabase(uint32_t address, const BuckDatabase& database) {
    switch (address) {
#define COMMIT_DB(address, size, T, variable) \
        case address: return CommitNative<T>(variable, size, s_##T##Fields, database);
        BUCK_DATABASE_BINDINGS(COMMIT_DB)
#undef COMMIT_DB
        default: return false;
    }
}

#endif
