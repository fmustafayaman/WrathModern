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
#include "water_metal.h"
#include "water_data.h"
#include "lua.h"
#include "panel_source.h"

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
    WaterRenderer waterRenderer;
    std::string iniPath;
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
    // The end of the world render restores the stock fog even when the panel just turned the fog off
    if (hook == WM_HOOK_WORLD_END)
    {
        OnWorldEnd();
        return;
    }

    if (!s.config.enable)
    {
        s.renderedThisFrame = false;
        return;
    }

    switch (hook)
    {
    case WM_HOOK_WORLD_BEGIN:
        OnWorldBegin();
        break;
    case WM_HOOK_LIQUIDS_BEGIN:
        @autoreleasepool
        {
            s.waterRenderer.LiquidsBegin(s.host, s.config, s.water, s.renderedLastFrame, s.stockFogPushed);
        }
        break;
    case WM_HOOK_LIQUIDS_END:
        @autoreleasepool
        {
            s.waterRenderer.LiquidsEnd(s.host, s.fog);
        }
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

void OnLiquidDraw(const void* settings, int32_t begin, void* user)
{
    s.waterRenderer.LiquidDraw(settings, begin != 0);
}

// The settings panel's functions (panel.lua)

// CoAVolFog_Get(key) -> value, lo, hi, kind ("i", "f" or "b")
int LuaGet(lua_State* L)
{
    const char* key = lua_tolstring(L, 1, nullptr);
    char kind;
    float lo, hi, value;
    if (!key || !GetSetting(s.config, key, kind, lo, hi, value))
        return 0;
    const char kindText[2] = {kind, 0};
    lua_pushnumber(L, value);
    lua_pushnumber(L, lo);
    lua_pushnumber(L, hi);
    lua_pushstring(L, kindText);
    return 4;
}

// CoAVolFog_Set(key, value) -> the value taken (clamped to the setting's range)
int LuaSet(lua_State* L)
{
    const char* key = lua_tolstring(L, 1, nullptr);
    float applied;
    if (!key || !SetSetting(s.config, key, static_cast<float>(lua_tonumber(L, 2)), applied))
        return 0;
    LogSetLevel(s.config.logLevel);
    lua_pushnumber(L, applied);
    return 1;
}

int LuaSave(lua_State* L)
{
    bool saved = SaveConfig(s.iniPath, s.config);
    VF_LOG_INFO("settings %s %s", saved ? "saved to" : "could not be saved to", s.iniPath.c_str());
    lua_pushboolean(L, saved ? 1 : 0);
    return 1;
}

int LuaRevert(lua_State* L)
{
    LoadConfig(s.iniPath, s.config);
    LogSetLevel(s.config.logLevel);
    VF_LOG_INFO("settings reloaded from %s", s.iniPath.c_str());
    return 0;
}

void OnScriptContext(uint32_t context, int32_t loaded, void* user)
{
    if (context == WM_SCRIPT_GAME && loaded)
        s.host->ExecuteLua(kPanelSource, "CoAVolFog panel");
}

void Shutdown(void* user)
{
    if (s.stockFogPushed)
        engine::WriteStockFog(s.savedStockFog);
    s.renderer.Release();
    s.waterRenderer.Release();
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
    s.iniPath = iniPath;
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
    info->OnScriptContext = OnScriptContext;
    info->OnLiquidDraw = OnLiquidDraw;

    host->RegisterLuaFunction("CoAVolFog_Get", LuaGet, WM_SCRIPT_GAME);
    host->RegisterLuaFunction("CoAVolFog_Set", LuaSet, WM_SCRIPT_GAME);
    host->RegisterLuaFunction("CoAVolFog_Save", LuaSave, WM_SCRIPT_GAME);
    host->RegisterLuaFunction("CoAVolFog_Revert", LuaRevert, WM_SCRIPT_GAME);
    host->Log(s.config.enable ? "CoAVolFog: loaded" : "CoAVolFog: loaded, disabled by Enable=0");
    return 1;
}
