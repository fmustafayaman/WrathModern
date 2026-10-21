// CoAVolFog for the native Metal client: volumetric fog and modern water, ported from the Windows
// CoAVolFog (Raz0r1337/coa-vfog, GPL-3.0). The Windows build wraps Direct3D 9 and patches call sites of
// Wow.exe; here the client's mod interface hands over the same points of the world render (see WOWRE.md,
// "Faz 5") and the frame's Metal targets.
//
// Slice 2: the fog march and composite over the world, in place of the client's linear fog.

#import <Metal/Metal.h>
#include <string>
#include "WMModAPI.h"
#include "config.h"
#include "engine.h"
#include "fog_data.h"
#include "log.h"
#include "renderer_metal.h"
#include "water_data.h"

namespace
{

// hooks.cpp of the Windows build: the stock fog moves out of range while the fog draws in its place
constexpr float kOutOfRangeStockFogStart = 50000.0f;

struct State
{
    const WMModHost* host = nullptr;
    Config config;
    FogData fog;
    WaterData water;
    FogRenderer renderer;
    engine::StockFog savedStockFog = {};
    bool stockFogPushed = false;
    bool renderedThisFrame = false;
    bool renderedLastFrame = false;
};

State s;

// FogDrawsInPlaceOfStockFog: only while the fog drew last frame, so a failure keeps the client's fog
void OnWorldBegin()
{
    s.renderedThisFrame = false;

    if (!s.renderedLastFrame || s.config.stockFog != 1 || engine::CameraInLiquid())
        return;

    s.savedStockFog = engine::ReadStockFog();
    engine::StockFog outOfRange;
    for (int group = 0; group < engine::kDayNightFogGroupCount; ++group)
    {
        outOfRange.start[group] = kOutOfRangeStockFogStart;
        outOfRange.end[group] = kOutOfRangeStockFogStart * 2.0f;
    }
    engine::WriteStockFog(outOfRange);
    s.stockFogPushed = true;
}

void OnWorldEnd()
{
    if (s.stockFogPushed)
    {
        engine::WriteStockFog(s.savedStockFog);
        s.stockFogPushed = false;
    }

    s.renderedLastFrame = s.renderedThisFrame;
}

void OnHook(WMHook hook, void* user)
{
    if (!s.config.enable)
        return;

    switch (hook)
    {
    case WM_HOOK_WORLD_BEGIN:
        OnWorldBegin();
        break;
    case WM_HOOK_WORLD_END:
        OnWorldEnd();
        break;
    case WM_HOOK_BEFORE_SCREEN_EFFECTS:
        @autoreleasepool
        {
            SavedStockFog stock;
            stock.pushed = s.stockFogPushed;
            stock.start = s.savedStockFog.start[engine::kFrameInputsFogGroup];
            stock.end = s.savedStockFog.end[engine::kFrameInputsFogGroup];
            s.renderedThisFrame = s.renderer.Render(s.host, s.config, s.fog, stock);
        }
        break;
    default:
        break;
    }
}

void Shutdown(void* user)
{
    if (s.stockFogPushed)
        engine::WriteStockFog(s.savedStockFog);
    s.renderer.Release();
}

}

extern "C" __attribute__((visibility("default"))) int32_t WMModLoad(const WMModHost* host, WMModInfo* info)
{
    if (host->version != WM_MOD_API_VERSION || !host->device || !host->gameDirectory)
        return 0;

    s.host = host;

    std::string game = host->gameDirectory;
    LogOpen((game + "/Logs/CoAVolFog.log").c_str());

    std::string iniPath = game + "/Mods/CoAVolFog.ini";
    bool read = LoadConfig(iniPath, s.config);
    LogSetLevel(s.config.logLevel);
    VF_LOG_INFO("CoAVolFog for Metal, slice 2; settings %s %s", read ? "from" : "defaulted, no", iniPath.c_str());
    VF_LOG_DEBUG("%s", DescribeConfig(s.config).c_str());

    s.fog.Load(game + "/Mods/fogdata.bin");
    s.water.Load(game + "/Mods/waterdata.bin");

    info->version = WM_MOD_API_VERSION;
    info->name = "CoAVolFog";
    info->OnHook = OnHook;
    info->Shutdown = Shutdown;
    host->Log(s.config.enable ? "CoAVolFog: loaded" : "CoAVolFog: loaded, disabled by Enable=0");
    return 1;
}
