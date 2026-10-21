#include <cstdarg>
#include <cstdio>
#include "Mods/AwesomeWotlk/AwesomeWotlk.h"

extern "C" {
#include "lua.h"
}

namespace awesome {

const WMModHost* g_host;

void Log(const char* format, ...) {
    char line[512];
    int prefix = snprintf(line, sizeof(line), "AwesomeWotlk: ");
    va_list args;
    va_start(args, format);
    vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
    va_end(args);
    g_host->Log(line);
}

namespace {

// The world callbacks of the modules
void OnWorld(int32_t entered, void* user) {
    camera::OnWorld(entered, user);
    interaction::OnWorld(entered, user);
}

void OnFrame(float elapsed, void* user) {
    interaction::OnFrame(elapsed, user);
}

void OnScriptContext(uint32_t context, int32_t loaded, void* user) {
    // luaOpenAwesomeWotlk: the version global exists in every new Lua state before any interface file runs
    if (!loaded) {
        lua_State* L = g_host->GetLuaState();
        lua_pushnumber(L, kVersion);
        lua_setfield(L, LUA_GLOBALSINDEX, "AwesomeWotlk");
    }
}

}

}

extern "C" __attribute__((visibility("default"))) int32_t WMModLoad(const WMModHost* host, WMModInfo* info) {
    awesome::g_host = host;

    if (host->version < 2 || !WM_HOST_HAS(host, SetCVar)) {
        host->Log("AwesomeWotlk: needs mod interface version 2");
        return 0;
    }

    awesome::cvars::Register();
    awesome::unit_api::Register();
    awesome::item::Register();
    awesome::nameplates::Register();
    awesome::interaction::Register();
    awesome::window::Register();

    info->version = WM_MOD_API_VERSION;
    info->name = "Awesome WotLK";
    info->OnScriptContext = awesome::OnScriptContext;
    info->OnCameraCreated = awesome::camera::OnCameraCreated;
    info->OnWorld = awesome::OnWorld;
    info->OnFrame = awesome::OnFrame;
    return 1;
}
