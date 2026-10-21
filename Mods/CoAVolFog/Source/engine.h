#pragma once

// The frame inputs of the Windows CoAVolFog's engine.h, filled on macOS from the client itself instead of
// from fixed addresses of Wow.exe (see engine_mac.mm). The members keep their names and meaning so the fog
// model and the renderer read them unchanged; D3DVIEWPORT9 becomes Viewport with the same fields.

#include "fog_data.h"
#include "engine_actors.h"
#include "engine_lights.h"
#include "water_types.h"

#include <cstdint>

struct WMWorldState;

struct Viewport
{
    uint32_t X;
    uint32_t Y;
    uint32_t Width;
    uint32_t Height;
    float MinZ;
    float MaxZ;
};

struct FrameInputs
{
    float cameraRelativeView[16];
    float glProjection[16];
    float camPos[3];
    float camTarget[3];
    Viewport viewport;
    float dayFraction;
    float toLight[3];
    bool lightIsMoon;
    uint32_t fogColor;
    uint32_t sunColor;
    uint32_t directColor;
    uint32_t ambientColor;
    float fogStart;
    float fogEnd;
    float zoneFogDistance;
    float farClip;
    float clientGlowAmount;
    bool inLiquid;
    int mapId;
    LightParamsSelection lightParams;
    LocalLightInputs localLights;
};

namespace engine
{
// The day/night fog groups (0xD38B90/94 and 0xD38BA4/A8: DNInfo::m_fog and m_fogInterior)
constexpr int kDayNightFogGroupCount = 2;
constexpr int kFrameInputsFogGroup = 1;

struct StockFog
{
    float start[kDayNightFogGroupCount];
    float end[kDayNightFogGroupCount];
};

StockFog ReadStockFog();
void WriteStockFog(const StockFog& fog);
// 0xCD8794, the liquid the camera is in
bool CameraInLiquid();

// The inputs of the frame being drawn, from the world state the client captured after the opaque pass and
// the frame's target size (the world viewport covers the whole target)
bool BuildFrameInputs(const WMWorldState& world, uint32_t width, uint32_t height, FrameInputs& out);
}
