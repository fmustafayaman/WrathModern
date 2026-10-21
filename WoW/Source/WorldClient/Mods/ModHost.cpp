#include "WoW/Source/WorldClient/Mods/ModHost.h"

#if defined(WRATHMODERN_MODS)

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"

namespace {

struct LoadedMod {
    void* library;
    std::string directory;
    WMModHost host;
    WMModInfo info;
};

bool s_initialized;
std::string s_gameDirectory;
std::vector<LoadedMod*> s_mods;
FILE* s_log;
WMWorldState s_worldState;
bool s_worldStateValid;

// Interface scripts. Lua functions and events of every mod, registered again in each new Lua state.
struct ModLuaFunction {
    std::string name;
    WMLuaFunction function;
    uint32_t contexts;
};

struct ModEvent {
    std::string name;
    uint32_t contexts;
    // Index in the current context's event table, or -1 when the event is not part of it
    int32_t index;
};

struct ModCVar {
    std::string name;
    WMCVarChanged changed;
    void* user;
};

std::vector<ModLuaFunction> s_luaFunctions;
std::vector<ModEvent> s_events;
std::vector<ModCVar*> s_cvars;
uint32_t s_scriptContext;

void LogLine(const char* format, ...) {
    if (!s_log) {
        std::string path = s_gameDirectory + "/Logs/Mods.log";
        s_log = fopen(path.c_str(), "w");

        if (!s_log) {
            return;
        }
    }

    va_list args;
    va_start(args, format);
    vfprintf(s_log, format, args);
    va_end(args);
    fputc('\n', s_log);
    fflush(s_log);
}

void HostLog(const char* message) {
    LogLine("%s", message ? message : "");
}

void ToColor(const CImVector& color, float* out) {
    out[0] = color.r / 255.0f;
    out[1] = color.g / 255.0f;
    out[2] = color.b / 255.0f;
}

void ToDirection(const C3Vector& body, const C3Vector& center, float* out) {
    float x = body.x - center.x;
    float y = body.y - center.y;
    float z = body.z - center.z;
    float length = std::sqrt(x * x + y * y + z * z);
    float scale = length > 0.0f ? 1.0f / length : 0.0f;
    out[0] = x * scale;
    out[1] = y * scale;
    out[2] = z * scale;
}

void CaptureWorldState() {
    WMWorldState& state = s_worldState;
    const DayNight::DNInfo& info = DayNight::g_dnInfo;

    std::memcpy(state.view, &CWorldScene::viewMatrix, sizeof(state.view));
    std::memcpy(state.projection, &CWorldScene::projMatrix, sizeof(state.projection));

    const C3Vector& camera = CWorldScene::s_activeWorldView;
    const C3Vector& target = CWorldScene::camTarget;
    state.cameraPosition[0] = camera.x;
    state.cameraPosition[1] = camera.y;
    state.cameraPosition[2] = camera.z;
    state.cameraTarget[0] = target.x;
    state.cameraTarget[1] = target.y;
    state.cameraTarget[2] = target.z;

    // A Direct3D projection: P[10] = f / (f - n), P[14] = -n f / (f - n)
    float p10 = state.projection[10];
    float p14 = state.projection[14];
    state.nearClip = p10 != 0.0f ? -p14 / p10 : 0.0f;
    state.farClip = p10 != 1.0f ? p14 / (1.0f - p10) : 0.0f;

    state.mapId = World::QueryMapId();
    state.cameraInLiquid = World::SceneCamLiquidStatus(nullptr) != 0;

    // The sun is the light between the client's dawn and dusk (DayNight::DNClouds::SetupBumpMap)
    state.dayFraction = info.m_dayProgression;
    state.moonIsLight = info.m_dayProgression < 0.2013889f || info.m_dayProgression > 0.9236111f;
    ToDirection(DayNight::g_planets.m_sun.m_position, info.m_cameraPos, state.sunDirection);
    ToDirection(DayNight::g_planets.m_moon.m_position, info.m_cameraPos, state.moonDirection);

    ToColor(info.m_bands.m_ambient, state.ambientColor);
    ToColor(info.m_bands.m_diffuse, state.directColor);
    ToColor(info.m_bands.m_sunCloudColors[0], state.sunColor);
    ToColor(info.m_fogInterior.m_color, state.fogColor);
    state.zoneFogEnd = info.m_bands.m_fogEnd;
    state.fogStart = info.m_fog.m_start;
    state.fogEnd = info.m_fog.m_end;
    state.stormBlend = info.m_stormBlend;
    state.interiorFogBlend = info.m_interiorFogBlend;

    state.time = OsGetAsyncTimeMs() / 1000.0;
    state.elapsed = info.m_deltaSec;
    s_worldStateValid = true;
}

const WMWorldState* HostGetWorldState() {
    if (!s_worldStateValid) {
        CaptureWorldState();
    }

    return &s_worldState;
}

int32_t HostAcquireFrameTargets(WMFrameTargets* targets) {
    if (!targets) {
        return 0;
    }

    GxMetalTargets metal = {};

    if (!GxMetalAcquireTargets(metal)) {
        return 0;
    }

    targets->commandBuffer = metal.commandBuffer;
    targets->color = metal.color;
    targets->depth = metal.depth;
    targets->width = metal.width;
    targets->height = metal.height;
    targets->sampleCount = metal.sampleCount;
    return 1;
}

void HostRegisterLuaFunction(const char* name, WMLuaFunction function, uint32_t contexts) {
    if (!name || !function || !(contexts & WM_SCRIPT_ALL)) {
        return;
    }

    s_luaFunctions.push_back({ name, function, contexts });

    // Available at once when the current Lua state is one of the contexts
    if (s_scriptContext & contexts) {
        FrameScript_RegisterFunction(name, reinterpret_cast<lua_CFunction>(function));
    }
}

int32_t HostRegisterEvent(const char* name, uint32_t contexts) {
    if (!name || !(contexts & WM_SCRIPT_ALL)) {
        return -1;
    }

    // Events join a context's table when the context is created; one registered later waits for the
    // next context (the interface is recreated on every login and /reload)
    s_events.push_back({ name, contexts, -1 });
    return static_cast<int32_t>(s_events.size() - 1);
}

void HostSignalEvent(int32_t event, const char* format, ...) {
    if (event < 0 || static_cast<size_t>(event) >= s_events.size() || s_events[event].index < 0) {
        return;
    }

    va_list args;
    va_start(args, format);
    vFrameScript_SignalEvent(static_cast<uint32_t>(s_events[event].index), format, args);
    va_end(args);
}

void HostExecuteLua(const char* source, const char* chunkName) {
    if (source && s_scriptContext) {
        FrameScript_Execute(source, chunkName ? chunkName : "mod", nullptr);
    }
}

lua_State* HostGetLuaState() {
    return s_scriptContext ? FrameScript_GetContext() : nullptr;
}

uint32_t HostGetScriptContext() {
    return s_scriptContext;
}

bool ModCVarCallback(CVar* var, const char* oldValue, const char* newValue, void* arg) {
    ModCVar* cvar = static_cast<ModCVar*>(arg);
    return !cvar->changed || cvar->changed(cvar->name.c_str(), oldValue, newValue, cvar->user) != 0;
}

int32_t HostRegisterCVar(const char* name, const char* help, const char* defaultValue, WMCVarChanged changed, void* user) {
    if (!name || !defaultValue) {
        return 0;
    }

    auto* cvar = new ModCVar { name, changed, user };
    CVar* var = CVar::Register(name, help ? help : "", 0, defaultValue, ModCVarCallback, CATEGORY_GAME, false, cvar, false);

    if (!var) {
        delete cvar;
        return 0;
    }

    s_cvars.push_back(cvar);
    return 1;
}

const char* HostGetCVar(const char* name) {
    CVar* var = name ? CVar::Lookup(name) : nullptr;
    return var ? var->m_stringValue.m_str : nullptr;
}

int32_t HostSetCVar(const char* name, const char* value) {
    CVar* var = name && value ? CVar::Lookup(name) : nullptr;

    if (!var) {
        return 0;
    }

    var->Set(value, true, false, false, true);
    return 1;
}

void LoadMod(const std::string& path, const std::string& name) {
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

    if (!library) {
        LogLine("%s: not loaded: %s", name.c_str(), dlerror());
        return;
    }

    auto load = reinterpret_cast<WMModLoadFunction>(dlsym(library, WM_MOD_ENTRY_POINT));

    if (!load) {
        LogLine("%s: no %s entry point", name.c_str(), WM_MOD_ENTRY_POINT);
        dlclose(library);
        return;
    }

    // The client accepts Lua functions and console variable callbacks only from code it loaded itself
    Dl_info image;

    if (dladdr(reinterpret_cast<const void*>(load), &image) && image.dli_fbase) {
        OsAllowFunctionPointerImage(image.dli_fbase);
    }

    auto* mod = new LoadedMod();
    mod->library = library;
    mod->directory = s_gameDirectory + "/Mods/" + name;

    WMModHost& host = mod->host;
    host.version = WM_MOD_API_VERSION;
    host.device = GxMetalDevice();
    host.gameDirectory = s_gameDirectory.c_str();
    host.modDirectory = mod->directory.c_str();
    host.Log = HostLog;
    host.GetWorldState = HostGetWorldState;
    host.AcquireFrameTargets = HostAcquireFrameTargets;
    host.size = sizeof(WMModHost);
    host.RegisterLuaFunction = HostRegisterLuaFunction;
    host.RegisterEvent = HostRegisterEvent;
    host.SignalEvent = HostSignalEvent;
    host.ExecuteLua = HostExecuteLua;
    host.GetLuaState = HostGetLuaState;
    host.GetScriptContext = HostGetScriptContext;
    host.RegisterCVar = HostRegisterCVar;
    host.GetCVar = HostGetCVar;
    host.SetCVar = HostSetCVar;

    mod->info = WMModInfo();

    if (!load(&mod->host, &mod->info) || mod->info.version < 1 || mod->info.version > WM_MOD_API_VERSION) {
        LogLine("%s: declined to load (interface version %u, the client's is %u)", name.c_str(), mod->info.version, WM_MOD_API_VERSION);
        delete mod;
        dlclose(library);
        return;
    }

    // A version 1 mod has no members after Shutdown
    if (mod->info.version < 2) {
        mod->info.OnScriptContext = nullptr;
        mod->info.OnCameraTarget = nullptr;
    }

    LogLine("%s: loaded as \"%s\" (interface version %u)", name.c_str(), mod->info.name ? mod->info.name : name.c_str(), mod->info.version);
    s_mods.push_back(mod);
}

void Initialize() {
    s_initialized = true;

    // The client runs in its game directory (SetPaths)
    char directory[1024];

    if (!getcwd(directory, sizeof(directory))) {
        return;
    }

    s_gameDirectory = directory;
    std::string modsDirectory = s_gameDirectory + "/Mods";
    DIR* dir = opendir(modsDirectory.c_str());

    if (!dir) {
        return;
    }

    if (!GxMetalDevice()) {
        closedir(dir);
        LogLine("Mods need the Metal graphics device; none loaded");
        return;
    }

    std::vector<std::string> names;

    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;

        // ExFAT stores AppleDouble sidecars as ._Name.dylib. Those are not Mach-O images.
        if (name.size() > 6 && name[0] != '.' && name.compare(name.size() - 6, 6, ".dylib") == 0) {
            names.push_back(name);
        }
    }

    closedir(dir);
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        LoadMod(modsDirectory + "/" + name, name.substr(0, name.size() - 6));
    }
}

}

void ModsRunHook(WMHook hook) {
    if (!s_initialized) {
        Initialize();
    }

    if (s_mods.empty()) {
        return;
    }

    // The first hook of a frame captures the world state again
    if (hook == WM_HOOK_AFTER_OPAQUE) {
        s_worldStateValid = false;
    }

    for (LoadedMod* mod : s_mods) {
        if (mod->info.OnHook) {
            mod->info.OnHook(hook, mod->info.user);
        }
    }
}

void ModsInitialize() {
    if (!s_initialized) {
        Initialize();
    }
}

void ModsRegisterScriptFunctions(uint32_t context) {
    ModsInitialize();
    s_scriptContext = context;

    for (const ModLuaFunction& function : s_luaFunctions) {
        if (function.contexts & context) {
            FrameScript_RegisterFunction(function.name.c_str(), reinterpret_cast<lua_CFunction>(function.function));
        }
    }
}

void ModsCreateEvents(uint32_t context) {
    std::vector<const char*> names;

    for (ModEvent& event : s_events) {
        if (event.contexts & context) {
            names.push_back(event.name.c_str());
        } else {
            event.index = -1;
        }
    }

    if (!names.empty()) {
        uint32_t index = FrameScript_AppendEvents(names.data(), static_cast<uint32_t>(names.size()));

        for (ModEvent& event : s_events) {
            if (event.contexts & context) {
                event.index = static_cast<int32_t>(index++);
            }
        }
    }

    for (LoadedMod* mod : s_mods) {
        if (mod->info.OnScriptContext) {
            mod->info.OnScriptContext(context, 0, mod->info.user);
        }
    }
}

void ModsScriptsLoaded(uint32_t context) {
    for (LoadedMod* mod : s_mods) {
        if (mod->info.OnScriptContext) {
            mod->info.OnScriptContext(context, 1, mod->info.user);
        }
    }
}

void ModsCameraTarget(const float* cameraPosition, float* targetPosition) {
    bool any = false;

    for (LoadedMod* mod : s_mods) {
        any = any || mod->info.OnCameraTarget;
    }

    if (!any) {
        return;
    }

    WMCameraTarget camera = {};
    std::memcpy(camera.cameraPosition, cameraPosition, sizeof(camera.cameraPosition));
    std::memcpy(camera.targetPosition, targetPosition, sizeof(camera.targetPosition));

    if (CGPlayer_C* player = ClntObjMgrGetActivePlayerObj()) {
        C3Vector position = player->GetPosition();
        camera.playerPosition[0] = position.x;
        camera.playerPosition[1] = position.y;
        camera.playerPosition[2] = position.z;
        camera.playerFacing = player->GetFacing();
        camera.playerValid = 1;
    }

    for (LoadedMod* mod : s_mods) {
        if (mod->info.OnCameraTarget) {
            mod->info.OnCameraTarget(&camera, mod->info.user);
        }
    }

    std::memcpy(targetPosition, camera.targetPosition, sizeof(camera.targetPosition));
}

void ModsShutdown() {
    for (LoadedMod* mod : s_mods) {
        if (mod->info.Shutdown) {
            mod->info.Shutdown(mod->info.user);
        }
    }

    s_mods.clear();

    if (s_log) {
        fclose(s_log);
        s_log = nullptr;
    }
}

#endif
