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

constexpr int kSkyColorCount = 6;
constexpr int kWaterColorPair = 2;

// The water's inputs of the Windows build's engine.h
struct WaterInputs
{
    uint32_t skyColors[kSkyColorCount];
    uint32_t riverColors[kWaterColorPair];
    uint32_t oceanColors[kWaterColorPair];
    bool stockFogApplies;
    WaterContactFrame contacts;
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
// ClassifyWaterSettings: the class of a liquid draw from its Liquid::CMaterialSettings (the settings bank
// 0xD43B1C gives the LiquidType id, whose row gives the material and sound bank; texture 0 its folder)
WaterClass ClassifyWaterSettings(const void* settings);

// BuildWaterInputs: the sky and water colours of the light bands (0xD38BE0.., 0xD38C0C.. and 0xD38C14..:
// DNInfo::m_bands.m_skyColors, m_waterColors[0..1] for the ocean and [2..3] for rivers and lakes)
void BuildWaterInputs(WaterInputs& out);

// The current screen effect (0xD45780) is the death effect (0xB74368)
bool GhostEffectRuns();

// CaptureLocalLightInputs on macOS: the point lights of the world's M2 scene (CM2Scene::m_lightGrid, the
// Windows build's [0xCD754C] + 0x24) offered to lights through SelectLocalPointLightInView, as the Windows
// capture walks its table. The interior members of lights are left as BuildFrameInputs set them.
void CaptureLocalPointLights(const LocalLightCamera& camera, const PointLightUpload& upload, LocalLightInputs& lights);

// The inputs of the frame being drawn, from the world state the client captured after the opaque pass and
// the frame's target size (the world viewport covers the whole target)
bool BuildFrameInputs(const WMWorldState& world, uint32_t width, uint32_t height, FrameInputs& out);
}
