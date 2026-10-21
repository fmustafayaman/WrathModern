#include <cstdio>
#include "WMModAPI.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace {

const WMModHost* s_host;
int32_t s_event = -1;

void Log(const char* format, const char* a = "", int b = 0) {
    char line[256];
    snprintf(line, sizeof(line), format, a, b);
    s_host->Log(line);
}

// ApiSmokePing(n) returns n + 1 and raises API_SMOKE_EVENT with n
int Ping(lua_State* L) {
    int n = static_cast<int>(luaL_checknumber(L, 1));
    Log("ApiSmoke: ping %s%d", "", n);
    s_host->SignalEvent(s_event, "%d", n);
    lua_pushnumber(L, n + 1);
    return 1;
}

// ApiSmokeLog(text) writes text to Mods.log
int LogFromLua(lua_State* L) {
    Log("ApiSmoke: lua says %s", luaL_checkstring(L, 1));
    return 0;
}

int32_t CVarChanged(const char* name, const char* oldValue, const char* newValue, void* user) {
    Log("ApiSmoke: cvar changed to %s", newValue);
    return 1;
}

void OnScriptContext(uint32_t context, int32_t loaded, void* user) {
    Log(loaded ? "ApiSmoke: context %s loaded" : "ApiSmoke: context %s created", context == WM_SCRIPT_GLUE ? "glue" : "game");

    if (!loaded) {
        return;
    }

    s_host->ExecuteLua(
        "local f = CreateFrame('Frame')\n"
        "f:RegisterEvent('API_SMOKE_EVENT')\n"
        "f:SetScript('OnEvent', function(self, event, n) ApiSmokeLog('event ' .. event .. ' ' .. tostring(n)) end)\n"
        "ApiSmokeLog('ping returned ' .. ApiSmokePing(41))\n"
        "SetCVar('apiSmokeValue', '7')\n"
        "ApiSmokeLog('cvar is ' .. GetCVar('apiSmokeValue'))\n",
        "ApiSmoke");
}

}

extern "C" __attribute__((visibility("default"))) int32_t WMModLoad(const WMModHost* host, WMModInfo* info) {
    s_host = host;

    if (host->version < 2 || !WM_HOST_HAS(host, SetCVar)) {
        host->Log("ApiSmoke: the client's mod interface is older than version 2");
        return 0;
    }

    host->RegisterLuaFunction("ApiSmokePing", Ping, WM_SCRIPT_ALL);
    host->RegisterLuaFunction("ApiSmokeLog", LogFromLua, WM_SCRIPT_ALL);
    s_event = host->RegisterEvent("API_SMOKE_EVENT", WM_SCRIPT_ALL);
    host->RegisterCVar("apiSmokeValue", "ApiSmoke test variable", "1", CVarChanged, nullptr);

    info->version = WM_MOD_API_VERSION;
    info->name = "ApiSmoke";
    info->OnScriptContext = OnScriptContext;
    return 1;
}
