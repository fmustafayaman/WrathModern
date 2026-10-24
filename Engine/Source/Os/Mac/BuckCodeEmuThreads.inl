/*
 * Included by BuckCodeEmu.cpp: cooperative threads inside the same x86 machine.
 *
 * The captured 288-byte challenge opens the dispatch thread, creates a worker, erases its
 * own prefix, then waits. The worker calls GetThreadContext(CONTEXT_DEBUG_REGISTERS) and
 * patches the waiting thread's continuation. Running the worker inline in CreateThread, or
 * returning a zeroed CONTEXT without running it, is not the delivered program's behavior.
 *
 * Contexts below are Unicorn CPU contexts, not captured Windows answers. Each worker has
 * its own stack and initial CPU state; shared memory and the instruction cache stay shared.
 * uc_emu_start is never nested in a hook: blocking waits yield to this outer scheduler.
 */

bool BuckCodeEmu::Impl::BootThreads() {
    if (uc_context_alloc(machine, &initialThreadContext) != UC_ERR_OK
        || uc_context_save(machine, initialThreadContext) != UC_ERR_OK) {
        return false;
    }

    GuestThread& main = threads[1];
    return uc_context_alloc(machine, &main.context) == UC_ERR_OK
        && uc_context_save(machine, main.context) == UC_ERR_OK;
}

void BuckCodeEmu::Impl::DestroyThreads() {
    for (auto& entry : threads) {
        if (entry.second.context) uc_context_free(entry.second.context);
    }

    threads.clear();
    threadHandles.clear();
    if (initialThreadContext) uc_context_free(initialThreadContext);
    initialThreadContext = nullptr;
}

uint32_t BuckCodeEmu::Impl::OpenGuestThread(uint32_t access, uint32_t id) {
    if (!threads.count(id)) return 0;

    if (threadHandles.size() >= 1024) {
        Fail("guest thread handle limit exceeded");
        return 0;
    }

    uint32_t handle = nextThreadHandle;
    nextThreadHandle += 4;
    threadHandles.emplace(handle, ThreadHandle{ id, access });
    return handle;
}

uint32_t BuckCodeEmu::Impl::CreateGuestThread(const uint32_t* arguments) {
    // Security descriptors / suspended starts are not implemented; never pretend to have
    // created a thread with semantics this scheduler cannot execute.
    if (arguments[0] || arguments[4]) {
        Fail("CreateThread security attributes/creation flags are not supported");
        return 0;
    }

    if (threads.size() >= 64 || arguments[1] > 0x1000000) {
        Fail("CreateThread exceeds the guest thread/stack limit");
        return 0;
    }

    uint32_t start = arguments[2];

    if (start < MACHINE_BASE || start >= MACHINE_BASE + MACHINE_SIZE) {
        Fail("CreateThread entry is outside guest memory");
        return 0;
    }

    uint32_t size = arguments[1] ? (arguments[1] + 0xFFF) & ~0xFFFu : STACK_SIZE;
    uint32_t stack = TrackedAlloc(size);
    if (!stack) return 0;
    uint32_t id = nextThreadId++;
    GuestThread& thread = threads[id];
    thread.stack = stack;
    thread.stackSize = size;
    thread.start = start;
    thread.parameter = arguments[3];

    if (arguments[5] && !WriteWord(arguments[5], id)) {
        Fail("CreateThread thread-id output is outside guest memory");
        return 0;
    }

    uint32_t handle = OpenGuestThread(0x001FFFFF, id);
    Log("created delivered guest thread %u at %08x (handle %08x)", id, start, handle);
    return handle;
}

bool BuckCodeEmu::Impl::ReadGuestThreadContext(uint32_t handle, uint32_t buffer) {
    auto found = threadHandles.find(handle);
    uint32_t flags = 0;

    if (found == threadHandles.end() || !(found->second.access & 0x8)
        || !ReadWord(buffer, flags)) {
        return false;
    }

    if (flags != 0x00010010) {
        Fail("GetThreadContext requested an unsupported CONTEXT group");
        return false;
    }

    auto target = threads.find(found->second.id);

    if (target == threads.end() || target->second.finished) return false;

    if (target->first == currentThreadId || !target->second.context) {
        Fail("GetThreadContext target has no stopped guest CPU context");
        return false;
    }

    // Win32 CONTEXT_i386 starts with ContextFlags then Dr0/Dr1/Dr2/Dr3/Dr6/Dr7. Preserve
    // every unrequested byte. Read the saved CPU, including nonzero breakpoints/status;
    // a canned all-zero result would suppress the very check the server asked to execute.
    const int registers[] = { UC_X86_REG_DR0, UC_X86_REG_DR1, UC_X86_REG_DR2,
        UC_X86_REG_DR3, UC_X86_REG_DR6, UC_X86_REG_DR7 };
    uint32_t debug[6] = {};

    for (uint32_t i = 0; i < 6; i++) {
        uint64_t value = 0;

        if (uc_context_reg_read(target->second.context, registers[i], &value) != UC_ERR_OK) {
            Fail("GetThreadContext could not read the guest debug registers");
            return false;
        }

        debug[i] = static_cast<uint32_t>(value);
    }

    if (!WriteMemory(buffer + 4, debug, sizeof(debug))) {
        Fail("GetThreadContext output is outside guest memory");
        return false;
    }

    Log("GetThreadContext guest %u: flags %08x, DR0..3 %08x %08x %08x %08x, DR6 %08x DR7 %08x",
        target->first, flags, debug[0], debug[1], debug[2], debug[3], debug[4], debug[5]);
    return true;
}

bool BuckCodeEmu::Impl::WaitForGuestThread(uint32_t handle, uint32_t timeout, uint32_t& result) {
    auto found = threadHandles.find(handle);

    if (found == threadHandles.end() || !(found->second.access & 0x00100000)) {
        result = 0xFFFFFFFF; // WAIT_FAILED: not a synchronizable thread handle.
        return false;
    }

    GuestThread& target = threads.at(found->second.id);

    if (target.finished) {
        result = 0; // WAIT_OBJECT_0, from actual execution reaching the thread return sentinel.
        return false;
    }

    if (timeout && timeout != 0xFFFFFFFF) {
        Fail("finite WaitForSingleObject timeouts are not supported by the guest scheduler");
        result = 0xFFFFFFFF;
        return false;
    }

    result = 0x102; // A zero-time poll yields, but still reports WAIT_TIMEOUT.
    if (timeout) threads.at(currentThreadId).waitFor = found->second.id;
    return true;
}

void BuckCodeEmu::Impl::ReapThreads() {
    for (auto it = threads.begin(); it != threads.end();) {
        uint32_t id = it->first;
        bool referenced = id == 1 || id == currentThreadId || !it->second.finished;

        for (const auto& handle : threadHandles) referenced |= handle.second.id == id;
        for (const auto& thread : threads) referenced |= thread.second.waitFor == id;

        if (referenced) {
            ++it;
        } else {
            if (it->second.context) uc_context_free(it->second.context);
            if (it->second.stack) TrackedFree(it->second.stack);
            it = threads.erase(it);
        }
    }
}

uc_err BuckCodeEmu::Impl::RunGuest(uint32_t address, uint32_t stopAt) {
    currentThreadId = 1;
    threads.at(1).waitFor = 0;
    extraStop = stopAt;
    const auto deadline = ThreadClock::now() + std::chrono::seconds(5);
    uc_err error = UC_ERR_OK;

    for (uint32_t switches = 0; switches < 4096 && !faulted; switches++) {
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - ThreadClock::now()).count();

        if (remaining <= 0) {
            Fail("delivered guest execution exceeded its five-second wall-time bound");
            break;
        }

        threadYielded = false;
        error = uc_emu_start(machine, address, STOP_ADDRESS, static_cast<uint64_t>(remaining), 50000000);
        uint32_t eip = 0;
        uc_reg_read(machine, UC_X86_REG_EIP, &eip);
        GuestThread& current = threads.at(currentThreadId);

        if (error != UC_ERR_OK || faulted) break;

        if (uc_context_save(machine, current.context) != UC_ERR_OK) {
            Fail("could not save a delivered guest thread context");
            break;
        }

        if (eip == STOP_ADDRESS || (currentThreadId == 1 && stopAt && eip == stopAt)) {
            if (currentThreadId == 1) {
                // Main entry completed, not process/thread termination. Keep its CPU state
                // across challenges; worker debug registers must not leak back into it.
                extraStop = 0;
                ReapThreads();
                return UC_ERR_OK;
            }

            current.finished = true;
            uc_reg_read(machine, UC_X86_REG_EAX, &current.exitCode);
            Log("delivered guest thread %u exited with %08x", currentThreadId, current.exitCode);
        } else if (!threadYielded) {
            Fail("delivered guest code did not reach a return or scheduling boundary");
            break;
        }

        for (auto& entry : threads) {
            GuestThread& waiter = entry.second;
            if (!waiter.waitFor) continue;
            if (threads.at(waiter.waitFor).finished) {
                uint32_t result = 0;

                if (uc_context_reg_write(waiter.context, UC_X86_REG_EAX, &result) != UC_ERR_OK) {
                    Fail("could not wake a delivered guest thread");
                    break;
                }

                waiter.waitFor = 0;
            }
        }

        if (faulted) break;

        // Round robin at API scheduling boundaries; a poll must not starve the worker.
        auto next = threads.upper_bound(currentThreadId);
        auto ready = [&](const GuestThread& thread) { return !thread.finished && !thread.waitFor; };

        for (size_t searched = 0; searched < threads.size(); searched++) {
            if (next == threads.end()) next = threads.begin();
            if (ready(next->second)) break;
            ++next;
        }

        if (next == threads.end() || !ready(next->second)) {
            Fail("delivered guest threads are deadlocked or require an unsupported wait object");
            break;
        }

        currentThreadId = next->first;
        GuestThread& thread = next->second;

        if (!thread.context) {
            uint32_t frame[] = { STOP_ADDRESS, thread.parameter };
            uint32_t esp = thread.stack + thread.stackSize - 0x20;
            uint32_t flags = 0x202;

            if (uc_context_restore(machine, initialThreadContext) != UC_ERR_OK
                || !WriteMemory(esp, frame, sizeof(frame))) {
                Fail("could not initialize a delivered guest thread");
                break;
            }

            uc_reg_write(machine, UC_X86_REG_ESP, &esp);
            uc_reg_write(machine, UC_X86_REG_EFLAGS, &flags);
            uc_reg_write(machine, UC_X86_REG_EIP, &thread.start);

            if (uc_context_alloc(machine, &thread.context) != UC_ERR_OK
                || uc_context_save(machine, thread.context) != UC_ERR_OK) {
                Fail("could not allocate a delivered guest thread context");
                break;
            }
        } else if (uc_context_restore(machine, thread.context) != UC_ERR_OK) {
            Fail("could not restore a delivered guest thread context");
            break;
        }

        uc_reg_read(machine, UC_X86_REG_EIP, &address);
        ReapThreads();
    }

    if (!faulted && error == UC_ERR_OK) Fail("delivered guest scheduling budget exhausted");
    // A failed worker must not leave its registers selected for the next diagnostic call.
    if (currentThreadId != 1) uc_context_restore(machine, threads.at(1).context);
    currentThreadId = 1;
    extraStop = 0;
    return error;
}
