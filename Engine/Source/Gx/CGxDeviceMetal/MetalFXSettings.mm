#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Gx/CGxDeviceMetal/MetalFXSettings.h"

namespace {
GxMetalFXStatus s_status = {false, 0, 0, 0, 0, "MetalFX has not started yet."};
bool s_menuToggleRequested;

bool ValidateMode(CVar*, const char*, const char* value, void*) {
    return value && value[0] >= '0' && value[0] <= '3' && value[1] == '\0';
}
}

void GxMetalFXInitSettings() {
    CVar::Register("metalFX", "Optional MetalFX Spatial: 0 off, 1 quality, 2 balanced, 3 performance", 1,
                   "0", ValidateMode, 1, false, nullptr, false);
}

int32_t GxMetalFXMode() {
    // Test-only override does not change the archived preference.
    if (const char* override = getenv("WRATHMODERN_METALFX")) {
        return std::clamp(atoi(override), 0, 3);
    }
    CVar* var = CVar::LookupRegistered("metalFX");
    return var ? std::clamp(var->m_intValue, 0, 3) : 0;
}

void GxMetalFXSetMode(int32_t mode) {
    if (CVar* var = CVar::LookupRegistered("metalFX")) {
        var->Set(std::clamp(mode, 0, 3), true, false, false, true);
    }
}

float GxMetalFXScale(int32_t mode) {
    switch (mode) {
        case 1: return 0.75f;
        case 2: return 2.0f / 3.0f;
        case 3: return 0.5f;
        default: return 1.0f;
    }
}

void GxMetalFXUpdateStatus(bool supported, uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh, const char* status) {
    s_status.supported = supported;
    s_status.inputWidth = iw; s_status.inputHeight = ih;
    s_status.outputWidth = ow; s_status.outputHeight = oh;
    if (strcmp(s_status.message, status)) {
        snprintf(s_status.message, sizeof(s_status.message), "%s", status);
    }
}

void GxMetalFXGetStatus(GxMetalFXStatus& status) {
    status = s_status;
}

void GxMetalFXToggleMenu() {
    // Window events only enqueue the request. GameUI consumes it outside the render traversal.
    // A boolean, not a flip: two key presses before the next idle still open the panel once.
    s_menuToggleRequested = true;
}

bool GxMetalFXConsumeMenuToggle() {
    bool requested = s_menuToggleRequested;
    s_menuToggleRequested = false;
    return requested;
}

void GxMetalFXCloseMenu() {
    s_menuToggleRequested = false;
}
