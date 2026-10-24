/*
 * Included by BuckCodeEmu.cpp: execution of the delivered framework/install segments.
 *
 * The 264-byte 0x2E9 capture is scattered by the original battleground handler's GUID/XY
 * writes. Reproduce those writes only inside the guest, then run the resulting x86 bootstrap.
 * Neither the framework addresses nor its request packets are seeded from a capture.
 */

void BuckCodeEmu::Impl::Fail(const char* reason) {
    if (!faulted) {
        faultReason = reason;
        Log("%s", reason);
    }

    faulted = true;
    uc_emu_stop(machine);
}

uint32_t BuckCodeEmu::Impl::TrackedAlloc(uint32_t size) {
    if (!size) {
        size = 1;
    }

    uint64_t capacity = size;

    // The content parser appends DBC records one at a time. Keep capacity separate from the
    // requested size, just like a heap allocator; realloc must never copy beyond the old block.
    if (size >= 0x10000) {
        capacity = (capacity + capacity / 2 + 0xFFFF) & ~uint64_t(0xFFFF);
    }

    if (capacity > HEAP_LIMIT - HEAP_BASE) {
        Fail("buck allocation exceeds the guest heap");
        return 0;
    }

    auto reusable = freeBlocks.lower_bound(static_cast<uint32_t>(capacity));

    if (reusable != freeBlocks.end()) {
        uint32_t block = reusable->second;
        allocations[block] = { size, reusable->first };
        freeBlocks.erase(reusable);
        return block;
    }

    uint32_t block = HeapAlloc(static_cast<uint32_t>(capacity));

    if (block) {
        allocations[block] = { size, static_cast<uint32_t>(capacity) };
    }

    return block;
}

bool BuckCodeEmu::Impl::TrackedFree(uint32_t block) {
    if (!block) return true;
    auto found = allocations.find(block);

    if (found == allocations.end()) {
        Fail("buck free called with an unknown allocation");
        return false;
    }

    freeBlocks.emplace(found->second.capacity, block);
    allocations.erase(found);
    return true;
}

uint32_t BuckCodeEmu::Impl::TrackedReAlloc(uint32_t block, uint32_t size) {
    if (!block) {
        return size ? TrackedAlloc(size) : 0;
    }

    auto old = allocations.find(block);

    if (old == allocations.end()) {
        Fail("buck realloc called with an unknown allocation");
        return 0;
    }

    if (!size) {
        TrackedFree(block);
        return 0;
    }

    if (size <= old->second.capacity) {
        old->second.size = size;
        return block;
    }

    uint32_t count = old->second.size < size ? old->second.size : size;
    uint32_t next = TrackedAlloc(size);

    if (!next) {
        return 0;
    }

    std::vector<uint8_t> bytes(count);

    if (!ReadMemory(block, bytes.data(), count) || !WriteMemory(next, bytes.data(), count)) {
        Fail("buck realloc could not copy the previous allocation");
        return 0;
    }

    TrackedFree(block);
    return next;
}

bool BuckCodeEmu::Impl::RunFunction(uint32_t address, const uint32_t* arguments, uint32_t count,
    uint32_t ecx, uint32_t& eax, uint32_t stopAt) {
    if (count > 32 || (count && !arguments)) {
        return false;
    }

    uint32_t frame[33] = { STOP_ADDRESS };

    if (count) {
        memcpy(frame + 1, arguments, count * sizeof(uint32_t));
    }

    uint32_t esp = STACK_TOP - 0x400;

    if (!WriteMemory(esp, frame, (count + 1) * sizeof(uint32_t))) {
        return false;
    }

    uint32_t zero = 0;
    uint32_t flags = 0x202;

    for (int reg : { UC_X86_REG_EBX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP }) {
        uc_reg_write(machine, reg, &zero);
    }

    uc_reg_write(machine, UC_X86_REG_EAX, &eax);
    uc_reg_write(machine, UC_X86_REG_ECX, &ecx);
    uc_reg_write(machine, UC_X86_REG_ESP, &esp);
    uc_reg_write(machine, UC_X86_REG_EFLAGS, &flags);
    faulted = false;
    faultReason.clear();
    extraStop = stopAt;
    uc_err error = uc_emu_start(machine, address, STOP_ADDRESS, 0, 50000000);
    extraStop = 0;
    uint32_t eip = 0;
    uc_reg_read(machine, UC_X86_REG_EIP, &eip);
    uc_reg_read(machine, UC_X86_REG_EAX, &eax);

    if (error != UC_ERR_OK || faulted || (eip != STOP_ADDRESS && (!stopAt || eip != stopAt))) {
        Log("delivered function %08x failed at %08x: %s%s%s", address, eip, uc_strerror(error),
            faultReason.empty() ? "" : "; ", faultReason.c_str());
        return false;
    }

    return true;
}

void BuckCodeEmu::Impl::BuildDispatchMessage(uint32_t opcode, const uint8_t* payload, uint32_t size) {
    StoreConstruct(MESSAGE_OBJECT);
    uint32_t buffer = HeapAlloc(size + 2);
    uint16_t id = static_cast<uint16_t>(opcode);
    WriteMemory(buffer, &id, sizeof(id));
    WriteMemory(buffer + 2, payload, size);
    WriteWord(MESSAGE_OBJECT + STORE_DATA, buffer);
    WriteWord(MESSAGE_OBJECT + STORE_ALLOC, size + 2);
    WriteWord(MESSAGE_OBJECT + STORE_SIZE, size + 2);
    WriteWord(MESSAGE_OBJECT + STORE_READ, 2);
}

bool BuckCodeEmu::InstallFramework(const uint8_t* payload, uint32_t size) {
    Impl& impl = *m_impl;

    if (!payload || size < 24 || (size - 8) % 16) {
        return false;
    }

    uint32_t players = 0;
    uint32_t flags = 0;
    memcpy(&players, payload, 4);
    memcpy(&flags, payload + 4, 4);
    uint32_t entries = (size - 8) / 16;

    if (players || flags <= entries || flags <= 2) {
        return false; // An ordinary battleground position packet, not this bootstrap.
    }

    if (!impl.Boot() || !impl.clientSize) {
        return false;
    }

    // Packet_MSG_BATTLEGROUND_PLAYER_POSITIONS (0x54B3F0): flagCarriers at 0xBEA170,
    // flagPositions at 0xBEA710, descending indexes. Validate every destination before writing.
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t index = static_cast<uint64_t>(flags) - 1 - i;
        uint64_t guid = 0x00BEA170ull + index * 8;
        uint64_t xy = 0x00BEA710ull + index * 8;

        if (guid < 0x00DD1000 || guid + 8 > 0x00DD1080 || xy < 0x00DD15A0 || xy + 8 > 0x00DD1620) {
            return false;
        }
    }

    if (impl.frameworkBase) {
        return true;
    }

    for (uint32_t i = 0; i < entries; i++) {
        uint32_t index = flags - 1 - i;
        impl.WriteMemory(0x00BEA170 + index * 8, payload + 8 + i * 16, 8);
        impl.WriteMemory(0x00BEA710 + index * 8, payload + 16 + i * 16, 8);
    }

    uc_ctl_remove_cache(impl.machine, 0x00DD1000, 0x00DD1620);
    impl.bootstrapActive = true;
    uint32_t result = 0x00DD1000;
    bool ran = impl.RunFunction(result, nullptr, 0, 0, result);
    impl.bootstrapActive = false;
    uint32_t base = 0;
    impl.ReadWord(FRAMEWORK_BASE_GLOBAL, base);
    uint32_t handler = 0;
    uint32_t param = 0;

    if (!ran || !base || !HandlerFor(FRAMEWORK_OPCODE, handler, param)
        || handler != base + FRAMEWORK_DISPATCHER_OFFSET || param != handler) {
        impl.Log("delivered framework bootstrap did not install its dispatcher");
        return false;
    }

    impl.frameworkBase = base;
    impl.Log("delivered framework installed at %08x; dispatcher %08x", base, handler);
    return true;
}

bool BuckCodeEmu::HandlerFor(uint32_t opcode, uint32_t& handler, uint32_t& param) const {
    auto entry = m_impl->handlers.find(opcode);

    if (entry == m_impl->handlers.end()) {
        return false;
    }

    handler = entry->second.handler;
    param = entry->second.param;
    return true;
}

bool BuckCodeEmu::Dispatch(uint32_t opcode, uint32_t time, const uint8_t* payload, uint32_t size,
    std::vector<std::vector<uint8_t>>& sent) {
    Impl& impl = *m_impl;
    sent.clear();
    uint32_t handler = 0;
    uint32_t param = 0;

    if ((!payload && size) || size > 0x100000 || !impl.Boot() || !HandlerFor(opcode, handler, param)) {
        return false;
    }

    if (opcode == FRAMEWORK_OPCODE) {
        uint32_t position = 0;
        if (!size) return false;

        while (position < size) {
            uint32_t length = 0;

            if (size - position < 4) {
                impl.Log("truncated 0x4B8 segment header");
                return false;
            }

            memcpy(&length, payload + position, 4);
            position += 4;

            if (length > size - position || (position == 4 && !length)) {
                impl.Log("truncated/empty 0x4B8 code segment");
                return false;
            }

            position += length;
        }
    }

    impl.BuildDispatchMessage(opcode, payload, size);
    impl.sentPackets.clear();
    impl.sentPacket.clear();
    impl.sentPacketValid = false;
    uint32_t args[] = { param, opcode, time, MESSAGE_OBJECT };
    uint32_t result = handler;

    if (!impl.RunFunction(handler, args, 4, 0, result)) {
        return false;
    }

    sent = impl.sentPackets;
    return true;
}

uint32_t BuckCodeEmu::AllocateGuest(uint32_t size) {
    return m_impl->Boot() ? m_impl->TrackedAlloc(size) : 0;
}

bool BuckCodeEmu::FreeGuest(uint32_t address) {
    return m_impl->Boot() && m_impl->TrackedFree(address);
}

bool BuckCodeEmu::WriteGuest(uint32_t address, const void* data, uint32_t size) {
    if ((!data && size) || !m_impl->Boot() || !m_impl->WriteMemory(address, data, size)) {
        return false;
    }

    if (size) {
        uc_ctl_remove_cache(m_impl->machine, address, static_cast<uint64_t>(address) + size);
    }

    return true;
}

bool BuckCodeEmu::ReadGuest(uint32_t address, void* data, uint32_t size) {
    return (data || !size) && m_impl->Boot() && m_impl->ReadMemory(address, data, size);
}

bool BuckCodeEmu::PatchScriptBuffer(void* buffer, uint32_t bytes, const char* chunkname) {
    if (!IsCallHooked(CALL_FRAMESCRIPT_EXECUTE_FILE)) return true;
    if (!buffer || !chunkname || bytes > 0x1000000) return false;
    uint32_t body = AllocateGuest(bytes + 1);
    uint32_t name = AllocateGuest(static_cast<uint32_t>(strlen(chunkname)) + 1);
    uint8_t terminator = 0;
    uint32_t result = 0;
    uint32_t args[] = { body, bytes, name, 0, 0 };
    std::vector<uint8_t> patched(bytes);
    bool ok = body && name
        && WriteGuest(body, buffer, bytes)
        && WriteGuest(body + bytes, &terminator, 1)
        && WriteGuest(name, chunkname, static_cast<uint32_t>(strlen(chunkname)) + 1)
        && RunHookedCall(CALL_FRAMESCRIPT_EXECUTE_FILE, args, 5, 0x00818E53, result)
        && ReadGuest(body, patched.data(), bytes);
    if (body) FreeGuest(body);
    if (name) FreeGuest(name);
    bool changed = ok && memcmp(buffer, patched.data(), bytes) != 0;
    if (ok) memcpy(buffer, patched.data(), bytes);
    m_impl->Log("delivered FrameXML hook: %s (%u bytes): %s, changed=%u", chunkname, bytes,
        ok ? "ok" : "failed", changed ? 1 : 0);
    return ok;
}

bool BuckCodeEmu::IsCallHooked(uint32_t site) {
    Impl& impl = *m_impl;
    uint8_t call[5];

    if (!impl.Boot() || !impl.ReadMemory(site, call, sizeof(call)) || call[0] != 0xE8) return false;
    int32_t displacement = 0;
    memcpy(&displacement, call + 1, 4);
    uint32_t target = site + 5 + displacement;
    return target < impl.clientBase || target >= impl.clientBase + impl.clientSize;
}

bool BuckCodeEmu::RunHookedCall(uint32_t site, const uint32_t* arguments, uint32_t count,
    uint32_t resume, uint32_t& result) {
    Impl& impl = *m_impl;
    uint8_t call[5];

    if (!impl.Boot() || !impl.ReadMemory(site, call, sizeof(call)) || call[0] != 0xE8) {
        return false;
    }

    int32_t displacement = 0;
    memcpy(&displacement, call + 1, 4);
    uint32_t target = site + 5 + displacement;

    if (target >= impl.clientBase && target < impl.clientBase + impl.clientSize) {
        return false;
    }

    if (site == CALL_CLIENT_INITIALIZE_GAME && !impl.PrepareContentDatabases()) {
        impl.Log("could not prepare native databases for delivered content");
        return false;
    }

    impl.Log("running delivered client hook at %08x -> %08x", site, target);
    result = target;

    if (!impl.RunFunction(target, arguments, count, 0, result, resume)) {
        return false;
    }

    return site != CALL_CLIENT_INITIALIZE_GAME || impl.CommitContentDatabases();
}
