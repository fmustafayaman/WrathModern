// engine::BuildFrameInputs on macOS: the inputs the Windows build reads from fixed addresses of Wow.exe
// (README "Engine inputs"), read here from the client's own state through the engine SDK. Each member
// names the address it replaces.

#include "engine.h"

#include <cmath>
#include <cstring>
#include "WMModAPI.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldScene.h"

namespace
{

// CImVector is stored B, G, R, A: its 32 bits are the A8R8G8B8 value the Windows build reads
uint32_t Argb(const CImVector& color)
{
    uint32_t value;
    std::memcpy(&value, &color, sizeof(value));
    return value;
}

// The Windows build's camera interior test (engine_lights.cpp, ReadCameraInterior): the camera stands in
// a map object group that is loaded and lacks the exterior fog flags (0x48)
constexpr uint32_t kWmoExteriorFogFlags = 0x48;

bool CameraInInterior()
{
    CMapObjDef* instance = CWorldScene::s_viewerMapObjDef;
    const TSGrowableArray<uint32_t>& groups = CWorldScene::s_viewerMapObjGroups;

    if (!instance || !groups.m_count || !instance->m_owner || !instance->m_owner->m_isGroupLoaded)
        return false;

    CMapObj* root = instance->m_owner;

    for (uint32_t i = 0; i < groups.m_count; i++)
    {
        uint32_t id = groups.m_data[i];

        if (id >= static_cast<uint32_t>(root->m_mapObjGroupCount) || id >= 512)
            continue;

        CMapObjGroup* group = root->m_mapObjGroupArray[id];

        if (group && (group->m_unkLoadedFlag & 1) && !(static_cast<uint32_t>(group->m_flags) & kWmoExteriorFogFlags))
            return true;
    }

    return false;
}

}

namespace engine
{

StockFog ReadStockFog()
{
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    return {{info.m_fog.m_start, info.m_fogInterior.m_start}, {info.m_fog.m_end, info.m_fogInterior.m_end}};
}

void WriteStockFog(const StockFog& fog)
{
    DayNight::DNInfo& info = DayNight::g_dnInfo;
    info.m_fog.m_start = fog.start[0];
    info.m_fog.m_end = fog.end[0];
    info.m_fogInterior.m_start = fog.start[1];
    info.m_fogInterior.m_end = fog.end[1];
}

bool CameraInLiquid()
{
    return s_liquidTypeId != 0;
}

bool BuildFrameInputs(const WMWorldState& world, uint32_t width, uint32_t height, FrameInputs& out)
{
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    FrameInputs in = {};

    // Gx +0x1B00 view: the camera-relative view is the view without its translation
    std::memcpy(in.cameraRelativeView, world.view, sizeof(in.cameraRelativeView));
    in.cameraRelativeView[12] = 0.0f;
    in.cameraRelativeView[13] = 0.0f;
    in.cameraRelativeView[14] = 0.0f;

    // Gx +0xF88: the engine's OpenGL projection (depth -1..1); the client hands mods the Direct3D one
    // (depth 0..1), which differs only in the depth row: d3d = (gl + 1) / 2
    std::memcpy(in.glProjection, world.projection, sizeof(in.glProjection));
    in.glProjection[2] = 2.0f * world.projection[2] - world.projection[3];
    in.glProjection[6] = 2.0f * world.projection[6] - world.projection[7];
    in.glProjection[10] = 2.0f * world.projection[10] - world.projection[11];
    in.glProjection[14] = 2.0f * world.projection[14] - world.projection[15];

    // 0xCD8F5C, 0xCD8F68
    std::memcpy(in.camPos, world.cameraPosition, sizeof(in.camPos));
    std::memcpy(in.camTarget, world.cameraTarget, sizeof(in.camTarget));

    // Gx +0xF80: the world viewport covers the target
    in.viewport = {0, 0, width, height, world.viewportMinZ, world.viewportMaxZ};

    // 0xD38B04, and the visible sun or moon (0xD38E28, 0xD38E48 from the sky centre 0xD38B18)
    in.dayFraction = info.m_dayProgression;
    in.lightIsMoon = world.moonIsLight != 0;
    const float* toLight = in.lightIsMoon ? world.moonDirection : world.sunDirection;
    std::memcpy(in.toLight, toLight, sizeof(in.toLight));

    // Fog group 1 (0xD38BA0..0xD38BA8, kFrameInputsFogGroup), light colours 0xD38BD4/D8/F8
    in.fogColor = Argb(info.m_fogInterior.m_color);
    in.fogStart = info.m_fogInterior.m_start;
    in.fogEnd = info.m_fogInterior.m_end;
    in.ambientColor = Argb(info.m_bands.m_ambient);
    in.directColor = Argb(info.m_bands.m_diffuse);
    in.sunColor = Argb(info.m_bands.m_sunCloudColors[0]);

    // 0xD38C1C (light float band 0), 0xD38C2C (glow)
    in.zoneFogDistance = info.m_bands.m_fogEnd;
    in.clientGlowAmount = info.m_bands.m_glow;

    // From the projection
    in.farClip = world.farClip;
    in.inLiquid = world.cameraInLiquid != 0;
    in.mapId = world.mapId;

    // 0xD38B9C, and the camera's map object (0xCD87A4, 0xCDB0D8, 0xCDB0DC)
    in.localLights.cameraInterior = CameraInInterior();
    in.localLights.interiorBlend = in.localLights.cameraInterior ? std::fmin(std::fmax(info.m_interiorFogBlend, 0.0f), 1.0f) : 0.0f;

    // 0xD38B88 storm weight, 0xD38B58 light params slot forced by a screen effect (-1 = none)
    // (ReadLightParamsSelection: the storm clamped to 0..1, a slot only within the eight params slots)
    in.lightParams.stormBlend = std::isfinite(info.m_stormBlend) ? std::fmin(std::fmax(info.m_stormBlend, 0.0f), 1.0f) : 0.0f;
    int32_t slot = static_cast<int32_t>(info.m_overrideLightParamsId);
    in.lightParams.screenEffectSlot = slot >= 0 && slot < FogData::kLightParamsSlots ? slot : kNoScreenEffectLightSlot;

    out = in;
    return std::isfinite(in.camPos[0]) && width > 0 && height > 0;
}

}
