#include <cstdint>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Platform/Mac/Sync.h"

// Event is an opaque handle; on macOS it holds an auto-reset MacSync event, as CreateEventA(nullptr,
// FALSE, FALSE, nullptr) does on Windows.

static MacSync::Waitable* ToWaitable(Blizzard::Lock::Event event) {
    return static_cast<MacSync::Waitable*>(event);
}

void Blizzard::Lock::EventCreate(Event& event) {
    event = MacSync::EventCreate(false, false);
}

void Blizzard::Lock::EventDestroy(Event& event) {
    MacSync::EventDestroy(ToWaitable(event));
}

void Blizzard::Lock::EventSet(Event& event) {
    MacSync::EventSet(ToWaitable(event));
}

void Blizzard::Lock::EventWaitForever(Event& event) {
    MacSync::Wait(ToWaitable(event), MacSync::kInfinite);
}

Blizzard::Lock::Event* Blizzard::Lock::EventWait(uint32_t timeout, Event& event) {
    return MacSync::Wait(ToWaitable(event), timeout) ? &event : nullptr;
}

Blizzard::Lock::EventObject::EventObject() {
    this->m_event = MacSync::EventCreate(false, false);
}

Blizzard::Lock::EventObject::~EventObject() {
    EventDestroy(this->m_event);
}
