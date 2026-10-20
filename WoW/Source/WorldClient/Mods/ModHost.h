#ifndef WOW_SOURCE_WORLDCLIENT_MODS_MODHOST_H
#define WOW_SOURCE_WORLDCLIENT_MODS_MODHOST_H

// Loads the mods in the game's Mods folder and calls them at the hooks of the world render (see
// WMModAPI.h). Without a Mods folder, or on a platform without mod support, the hooks do nothing.

#include <cstdint>
#include "WoW/Source/WorldClient/Mods/WMModAPI.h"

#if defined(WRATHMODERN_MODS)
void ModsRunHook(WMHook hook);
void ModsShutdown();
#define WRATHMODERN_MOD_HOOK(hook) ModsRunHook(hook)
#else
#define WRATHMODERN_MOD_HOOK(hook) ((void)0)
#endif

#endif
