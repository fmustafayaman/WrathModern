#include "Screen.h"
#include <cstdint>
#include <cstdio>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

// VERIFY: a937de73 auto
// OFFSET: 0x0050F7C0
float Screen::GetAspectRatio() {
    CVar* widescreen = CVar::Lookup("widescreen");
    CVar* resolution = CVar::Lookup("gxResolution");
    int32_t width = 4;
    int32_t height = 3;
    char separator;

    if (widescreen && widescreen->m_intValue && resolution) {
        sscanf(resolution->m_stringValue.m_str, "%d%c%d", &width, &separator, &height);

#if defined(__APPLE__)
        // Fullscreen on macOS renders the resolution extended to the display's aspect ratio
        // (CGxDeviceMetalNative::ICreateWindow), so the image the client draws into decides it
        if (g_theGxDevicePtr) {
            const CRect& window = g_theGxDevicePtr->DeviceDefWindow();

            if (window.maxX - window.minX >= 1.0f && window.maxY - window.minY >= 1.0f) {
                return (window.maxX - window.minX) / (window.maxY - window.minY);
            }
        }
#endif
    }

    return static_cast<float>(width) / height;
}
