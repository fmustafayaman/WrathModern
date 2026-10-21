// The console variables of awesome_wotlk (docs/api_reference.md, Docs/Mods/AwesomeWotlk.md section 3.3), with
// their defaults and ranges. Values outside the range are clamped, as the original's CVar thunk does. They are
// saved in Config.wtf like the client's own; the AwesomeCVar addon reads and writes them with GetCVar/SetCVar.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Mods/AwesomeWotlk/AwesomeWotlk.h"

namespace awesome::cvars {

namespace {

struct Definition {
    const char* name;
    const char* help;
    const char* defaultValue;
    float min;
    float max;
    bool integer;
};

const Definition kCVars[] = {
    // Nameplates
    { "nameplateDistance", "Nameplate visibility distance in yards", "41", 41, 200, false },
    { "nameplatePlacement", "Nameplate height offset", "0", -1, 2, false },
    { "nameplateMouseMode", "Nameplate mouse hit mode", "0", 0, 8, true },
    { "nameplateBandX", "Nameplate stacking band width", "0.7", 0.1f, 1, false },
    { "nameplateBandY", "Nameplate stacking band height", "1", 0.1f, 1.5f, false },
    { "nameplateHitboxAnchor", "Nameplate hitbox anchor", "1", 0, 2, true },
    { "nameplateHitboxWidthE", "Enemy nameplate hitbox width", "1", 0, 1, false },
    { "nameplateHitboxHeightE", "Enemy nameplate hitbox height", "1", 0, 1, false },
    { "nameplateHitboxWidthF", "Friendly nameplate hitbox width", "1", 0, 1, false },
    { "nameplateHitboxHeightF", "Friendly nameplate hitbox height", "1", 0, 1, false },
    { "nameplateRaiseSpeed", "Nameplate stacking raise speed", "100", 1, 250, false },
    { "nameplateLowerSpeed", "Nameplate stacking lower speed", "100", 1, 250, false },
    { "nameplatePullSpeed", "Nameplate stacking pull speed", "50", 1, 250, false },
    { "nameplateRaiseDistance", "Nameplate stacking raise distance", "8", 1, 20, false },
    { "nameplatePullDistance", "Nameplate stacking pull distance", "0.25", 0, 0.75f, false },
    { "nameplateOcclusionAlpha", "Alpha of nameplates behind terrain", "1", -1, 1, false },
    { "nameplateOcclusionMode", "Nameplate occlusion test", "0", 0, 1, true },
    { "nameplateNonTargetAlpha", "Alpha of non-target nameplates", "0.5", 0, 1, false },
    { "nameplateAlphaSpeed", "Nameplate alpha transition speed", "0.25", 0.01f, 1, false },
    { "nameplateInertia", "Nameplate movement inertia", "1", 0, 20, false },
    { "nameplateHysteresisDecay", "Nameplate stacking hysteresis decay", "1", 0.25f, 30, false },
    { "nameplateClampMode", "Clamp nameplates to the screen edges", "0", 0, 4, true },
    { "nameplateClampModeVOffset", "Nameplate clamp vertical offset", "0.01", 0, 0.05f, false },
    { "nameplateClampModeHOffset", "Nameplate clamp horizontal offset", "0.01", 0, 0.1f, false },
    { "nameplateClampModeFilter", "Which nameplates are clamped", "0", 0, 3, true },
    { "nameplateMouseFreeze", "Freeze nameplates under the mouse", "0", 0, 2, true },
    { "nameplateMouseFreezeGrace", "Nameplate freeze grace time", "0.15", 0, 1, false },
    { "nameplateMouseFreezeTime", "Nameplate freeze time", "0.3", 0, 2, false },
    { "nameplateStacking", "Nameplate stacking mode", "0", -3, 3, true },
    // Camera
    { "cameraFov", "Camera field of view", "100", 90, 150, false },
    { "cameraIndirectVisibility", "Fade models between the camera and the player", "0", 0, 1, true },
    { "cameraIndirectAlpha", "Alpha of faded models", "0.6", 0.6f, 1, false },
    { "showPlayer", "Draw the player character", "1", 0, 1, true },
    // Interaction
    { "interactionMode", "Interaction target selection", "1", 0, 1, true },
    { "interactionAngle", "Interaction cone angle in degrees", "60", 15, 160, true },
    { "interactionHighlight", "Highlight the interaction target", "1", 0, 1, true },
    // Interface
    { "uiHalfPixelFix", "Interface half-pixel fix", "1", 0, 1, true },
    { "uiTextureSampling", "Interface texture sampling", "1", 0, 2, true },
    { "uiPixelSnap", "Snap interface quads to pixels", "1", 0, 1, true },
    // Fonts
    { "MSDFMode", "Signed distance field fonts (restart)", "1", 0, 2, true },
    { "MSDFOutlinePass", "Draw font outlines in their own pass", "1", 0, 1, true },
    // Misc
    { "objectHighlightMode", "Object highlight mode", "0", 0, 2, true },
    { "portraitResolution", "Portrait render resolution", "64", 64, 2048, true },
    { "chatLogSessionKey", "Name chat logs by session", "1", 0, 1, true },
    { "combatLogSessionKey", "Name combat logs by session", "1", 0, 1, true },
    // Text to speech
    { "ttsVoice", "Text to speech voice", "1", 0, 1000, true },
    { "ttsSpeed", "Text to speech rate", "0", -10, 10, true },
    { "ttsVolume", "Text to speech volume", "100", 0, 100, true },
};

// The modules that act on a variable as it changes
void Changed(const char* name, float value) {
    camera::OnCVarChanged(name, value);
    nameplates::OnCVarChanged(name, value);
}

int32_t OnChanged(const char* name, const char* oldValue, const char* newValue, void* user) {
    const Definition* definition = static_cast<const Definition*>(user);
    char* end = nullptr;
    float value = std::strtof(newValue ? newValue : "", &end);

    if (!newValue || end == newValue) {
        return 0;
    }

    float clamped = std::fmin(std::fmax(value, definition->min), definition->max);

    if (definition->integer) {
        clamped = std::round(clamped);
    }

    if (clamped == value) {
        // The client stores the value only after this returns: modules get it here
        Changed(name, value);
        return 1;
    }

    // Store the clamped value instead; the client calls this again with it
    char text[32];
    snprintf(text, sizeof(text), definition->integer ? "%.0f" : "%g", clamped);
    g_host->SetCVar(name, text);
    return 0;
}

}

void Register() {
    for (const Definition& definition : kCVars) {
        if (!g_host->RegisterCVar(definition.name, definition.help, definition.defaultValue, OnChanged, const_cast<Definition*>(&definition))) {
            Log("could not register %s", definition.name);
        }
    }
}

float Float(const char* name) {
    const char* value = g_host->GetCVar(name);
    return value ? std::strtof(value, nullptr) : 0.0f;
}

int Int(const char* name) {
    return static_cast<int>(std::lround(Float(name)));
}

}
