// Optional spatial upscaling. Settings are archived in Config.wtf; UI remains at native resolution.
#ifndef ENGINE_SOURCE_GX_CGXDEVICEMETAL_METALFXSETTINGS_H
#define ENGINE_SOURCE_GX_CGXDEVICEMETAL_METALFXSETTINGS_H

#include <cstdint>

void GxMetalFXInitSettings();
int32_t GxMetalFXMode(); // 0 off, 1 quality (75%), 2 balanced (2/3), 3 performance (50%)
void GxMetalFXSetMode(int32_t mode);
float GxMetalFXScale(int32_t mode);
void GxMetalFXUpdateStatus(bool supported, uint32_t inputWidth, uint32_t inputHeight,
                          uint32_t outputWidth, uint32_t outputHeight, const char* status);
struct GxMetalFXStatus {
    bool supported;
    uint32_t inputWidth, inputHeight, outputWidth, outputHeight;
    char message[256];
};
void GxMetalFXGetStatus(GxMetalFXStatus& status);
void GxMetalFXToggleMenu();
bool GxMetalFXConsumeMenuToggle();
void GxMetalFXCloseMenu();

#endif
