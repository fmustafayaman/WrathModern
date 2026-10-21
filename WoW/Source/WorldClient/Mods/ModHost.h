#ifndef WOW_SOURCE_WORLDCLIENT_MODS_MODHOST_H
#define WOW_SOURCE_WORLDCLIENT_MODS_MODHOST_H

// Loads the mods in the game's Mods folder and calls them at the hooks of the world render (see
// WMModAPI.h). Without a Mods folder, or on a platform without mod support, the hooks do nothing.

#include <cstdint>
#include "WoW/Source/WorldClient/Mods/WMModAPI.h"

#if defined(WRATHMODERN_MODS)
// Loads the mods; also done by the first of the calls below
void ModsInitialize();
void ModsRunHook(WMHook hook);
// The interface's Lua state for context (WM_SCRIPT_GLUE or WM_SCRIPT_GAME) is being built: after the
// client registered its functions, after it created its events, and once its files have loaded
void ModsRegisterScriptFunctions(uint32_t context);
void ModsCreateEvents(uint32_t context);
void ModsScriptsLoaded(uint32_t context);
// The camera's orbit target (CGCamera::CalcTargetCamera); mods may move it
void ModsCameraTarget(const float* cameraPosition, float* targetPosition);
void ModsCameraCreated(float* fieldOfView);
void ModsWorld(int32_t entered);
void ModsShutdown();
#define WRATHMODERN_MOD_HOOK(hook) ModsRunHook(hook)
#define WRATHMODERN_MOD_SCRIPT_FUNCTIONS(context) ModsRegisterScriptFunctions(context)
#define WRATHMODERN_MOD_SCRIPT_EVENTS(context) ModsCreateEvents(context)
#define WRATHMODERN_MOD_SCRIPTS_LOADED(context) ModsScriptsLoaded(context)
#define WRATHMODERN_MOD_CAMERA_TARGET(camera, target) ModsCameraTarget(camera, target)
#define WRATHMODERN_MOD_CAMERA_CREATED(fov) ModsCameraCreated(fov)
#define WRATHMODERN_MOD_WORLD(entered) ModsWorld(entered)
#else
#define WRATHMODERN_MOD_HOOK(hook) ((void)0)
#define WRATHMODERN_MOD_SCRIPT_FUNCTIONS(context) ((void)0)
#define WRATHMODERN_MOD_SCRIPT_EVENTS(context) ((void)0)
#define WRATHMODERN_MOD_SCRIPTS_LOADED(context) ((void)0)
#define WRATHMODERN_MOD_CAMERA_TARGET(camera, target) ((void)0)
#define WRATHMODERN_MOD_CAMERA_CREATED(fov) ((void)0)
#define WRATHMODERN_MOD_WORLD(entered) ((void)0)
#endif

#endif
