// Awesome WotLK for WrathModern. Shared declarations of the mod's modules.

#ifndef MODS_AWESOMEWOTLK_AWESOMEWOTLK_H
#define MODS_AWESOMEWOTLK_AWESOMEWOTLK_H

#include "WMModAPI.h"

namespace awesome {

// The version of awesome_wotlk whose behaviour this port follows; the AwesomeCVar addon compares it with
// its own MOD_VERSION through the AwesomeWotlk global
constexpr int kVersion = 38;

extern const WMModHost* g_host;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

namespace cvars {
void Register();
float Float(const char* name);
int Int(const char* name);
}

namespace unit_api {
void Register();
}

namespace window {
void Register();
}

namespace camera {
void OnCameraCreated(float* fieldOfView, void* user);
void OnWorld(int32_t entered, void* user);
void OnCVarChanged(const char* name, float value);
}

}

#endif
