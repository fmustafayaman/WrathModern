/* Included by BuckCodeEmu.cpp: real native DBC rows around the delivered 0x41D parser. */

bool BuckCodeEmu::Impl::MirrorDatabase(uint32_t address, uint32_t recordSize) {
    ContentDatabase& pending = contentDatabases[address];

    if (mirroredDatabases.count(address)) {
        return pending.stride == recordSize;
    }

    BuckDatabase database;

    if (!services->ClientDatabase(address, database) || database.recordSize != recordSize
        || database.numRecords < 0 || database.ids.size() != static_cast<size_t>(database.numRecords)
        || database.records.size() != static_cast<size_t>(database.numRecords) * recordSize) {
        Log("no native database/layout bridge for %08x (stride %u)", address, recordSize);
        return false;
    }

    int64_t range = static_cast<int64_t>(database.maxID) - database.minID + 1;

    if (range <= 0 || range > 10000000 || database.records.size() > 0x4000000
        || database.strings.empty() || database.strings.size() > 0x2000000) {
        Log("database %08x has unsupported bounds", address);
        return false;
    }

    uint32_t strings = TrackedAlloc(static_cast<uint32_t>(database.strings.size()));
    uint32_t records = TrackedAlloc(static_cast<uint32_t>(database.records.size()));
    uint32_t indexCount = static_cast<uint32_t>(range) + (database.rle ? database.numRecords : 0);
    uint32_t index = TrackedAlloc(indexCount * 4);

    if (!strings || !records || !index || !WriteMemory(strings, database.strings.data(),
            static_cast<uint32_t>(database.strings.size()))) {
        return false;
    }

    for (int32_t i = 0; i < database.numRecords; i++) {
        for (uint32_t field : database.stringFields) {
            if (field > recordSize - 4) {
                return false;
            }

            uint8_t* slot = database.records.data() + static_cast<size_t>(i) * recordSize + field;
            uint32_t offset = 0;
            memcpy(&offset, slot, 4);

            if (offset != 0xFFFFFFFF && offset >= database.strings.size()) {
                return false;
            }

            uint32_t pointer = offset == 0xFFFFFFFF ? 0 : strings + offset;
            memcpy(slot, &pointer, 4);
        }
    }

    if (!WriteMemory(records, database.records.data(), static_cast<uint32_t>(database.records.size()))) {
        return false;
    }

    std::vector<uint32_t> pointers(indexCount, 0);
    uint32_t byID = database.rle ? database.numRecords : 0;

    for (int32_t i = 0; i < database.numRecords; i++) {
        int64_t id = static_cast<int64_t>(database.ids[i]) - database.minID;

        if (id < 0 || id >= range) {
            return false;
        }

        uint32_t pointer = records + i * recordSize;
        pointers[byID + id] = pointer;

        if (database.rle) {
            pointers[i] = pointer;
        }
    }

    WriteMemory(index, pointers.data(), indexCount * 4);
    WriteWord(address + DB_LOADED, 1);
    WriteWord(address + DB_NUM_RECORDS, database.numRecords);
    WriteWord(address + DB_MIN_ID, database.minID);
    WriteWord(address + DB_MAX_ID, database.maxID);
    WriteWord(address + 0x14, strings);
    WriteWord(address + DB_RLE_DATA, database.rle ? records : 0);
    WriteWord(address + DB_RECORDS, database.rle ? index : records);
    WriteWord(address + DB_RECORDS_BY_ID, index + byID * 4);
    pending.stride = recordSize;
    pending.stringFields = database.stringFields;
    mirroredDatabases.insert(address);
    Log("mirrored native database %08x: %u rows, stride %u, IDs %d..%d, %u string fields", address,
        database.numRecords, recordSize, database.minID, database.maxID,
        static_cast<uint32_t>(database.stringFields.size()));
    return true;
}

bool BuckCodeEmu::Impl::PrepareContentDatabases() {
    for (auto& entry : contentDatabases) {
        entry.second.ids.clear();
    }

    uint32_t block = 0;
    uint32_t size = 0;
    ReadWord(FRAMEWORK_CONTENT, block);

    if (!block) {
        return true;
    }

    if (!ReadWord(block, size) || size < 4 || size > 0x1000000) {
        return false;
    }

    std::vector<uint8_t> content(size);

    if (!ReadMemory(block + 4, content.data(), size)) {
        return false;
    }

    uint32_t position = 0;
    auto word = [&](uint32_t& out) {
        if (position > size - 4) return false;
        memcpy(&out, content.data() + position, 4);
        position += 4;
        return true;
    };
    auto skip = [&](uint64_t count) {
        if (count > size - position) return false;
        position += static_cast<uint32_t>(count);
        return true;
    };
    uint32_t version = 0;
    uint32_t previous = 0;
    word(version);
    ReadWord(0x00DD0FF0, previous);

    // A content-version change also resets/reloads the listed databases via their vtables.
    // That path needs a native reload bridge; do not pretend it succeeded on unknown layouts.
    if (previous && previous != version) {
        Log("content version changed %u -> %u: database reload bridge required", previous, version);
        return false;
    }

    while (position < size) {
        uint32_t opcode = 0;

        if (!word(opcode)) return false;

        if (opcode == CONTENT_STRINGS) {
            uint32_t length = 0;
            if (!word(length) || !skip(length)) return false;
        } else if (opcode == CONTENT_RESET) {
            uint32_t count = 0;
            if (!word(count) || !skip(static_cast<uint64_t>(count) * 4)) return false;
        } else if (opcode == CONTENT_UPSERT) {
            uint32_t database = 0;
            uint32_t stride = 0;
            uint32_t count = 0;
            uint32_t fields = 0;

            if (!word(database) || !word(stride) || !word(count) || !word(fields)
                || stride < 4 || stride > 4096 || fields > stride / 4) return false;

            std::vector<uint32_t> strings(fields);

            for (uint32_t& field : strings) {
                if (!word(field) || field > stride - 4) return false;
            }

            if (static_cast<uint64_t>(count) * stride > size - position
                || !MirrorDatabase(database, stride)) return false;
            ContentDatabase& pending = contentDatabases[database];

            if (strings != pending.stringFields) {
                Log("content database %08x: delivered/native string layouts differ", database);
                return false;
            }

            for (uint32_t i = 0; i < count; i++) {
                uint32_t id = 0;
                if (!word(id) || !skip(stride - 4)) return false;
                if (std::find(pending.ids.begin(), pending.ids.end(), id) == pending.ids.end()) {
                    pending.ids.push_back(id);
                }
            }
        } else {
            Log("content opcode %u requires a native database bridge", opcode);
            return false;
        }
    }

    return true;
}

bool BuckCodeEmu::Impl::CommitContentDatabases() {
    for (const auto& entry : contentDatabases) {
        const ContentDatabase& pending = entry.second;

        if (pending.ids.empty()) continue;
        BuckDatabase changed;
        changed.recordSize = pending.stride;
        changed.numRecords = static_cast<int32_t>(pending.ids.size());
        changed.stringFields = pending.stringFields;
        changed.records.resize(pending.ids.size() * pending.stride);
        changed.strings.push_back(0);
        uint32_t minID = 0;
        uint32_t maxID = 0;
        uint32_t index = 0;
        ReadWord(entry.first + DB_MIN_ID, minID);
        ReadWord(entry.first + DB_MAX_ID, maxID);
        ReadWord(entry.first + DB_RECORDS_BY_ID, index);

        for (size_t i = 0; i < pending.ids.size(); i++) {
            uint32_t id = pending.ids[i];
            uint32_t record = 0;

            if (id < minID || id > maxID || !ReadWord(index + (id - minID) * 4, record) || !record) {
                Log("delivered parser produced no row %u in database %08x", id, entry.first);
                return false;
            }

            uint8_t* row = changed.records.data() + i * pending.stride;

            if (!ReadMemory(record, row, pending.stride)) return false;
            changed.ids.push_back(static_cast<int32_t>(id));

            for (uint32_t field : pending.stringFields) {
                uint32_t pointer = 0;
                memcpy(&pointer, row + field, 4);
                uint32_t offset = 0xFFFFFFFF;

                if (pointer) {
                    std::string text = ReadGuestString(pointer, 0x10000);
                    if (text.size() == 0x10000) return false;
                    offset = static_cast<uint32_t>(changed.strings.size());
                    changed.strings.insert(changed.strings.end(), text.begin(), text.end());
                    changed.strings.push_back(0);
                }

                memcpy(row + field, &offset, 4);
            }
        }

        if (!services->CommitClientDatabase(entry.first, changed)) {
            Log("native database %08x rejected %u changed rows", entry.first, changed.numRecords);
            return false;
        }

        Log("applied delivered content to native database %08x: %u rows", entry.first, changed.numRecords);
    }

    return true;
}
