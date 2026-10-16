#include <cstdint>
#include <new>
#include "Engine/Source/SteelseriesMouse/HidManager.h"
#include "Engine/Source/Storm/H/storm.h"

// The SteelSeries World of Warcraft mouse on macOS. Its extra buttons arrive through the system mouse
// driver as numbered buttons, which the input layer reports like the Windows driver's extra buttons.
// The vendor feature reports for its lighting have no macOS path, so the manager finds no devices,
// as the Windows one does without the mouse attached.

class MacHidManager : public HidManager {
    public:
    ~MacHidManager() override {
    }

    bool EnumerateDevices() override {
        return false;
    }

    void SetWindow(void* window) override {
    }

    uint32_t GetDeviceCount() override {
        return 0;
    }

    bool WriteWait(const void* data, uint32_t size) override {
        return false;
    }

    bool Write(const void* data, uint32_t size) override {
        return false;
    }

    bool SetFeature(void* data, uint32_t size) override {
        return false;
    }
};

HidManager::~HidManager() {
}

HidManager* HidManager::Create() {
    return new (SMemAlloc(sizeof(MacHidManager), __FILE__, __LINE__, 0)) MacHidManager();
}
