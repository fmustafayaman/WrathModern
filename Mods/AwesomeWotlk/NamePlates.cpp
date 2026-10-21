// Nameplate options of awesome_wotlk (NamePlates.cpp): nameplateDistance and nameplatePlacement.
//
// The original rewrites two immediates inside Wow.exe: the 0.6666667f that GetNamePlateScreenPos
// adds to the unit's name position (the operand at 0x00715739, replaced by a pointer to the CVar) and
// the 1681.0f (41^2) range caps of the nameplate check and of tab targeting. The port writes the
// client's g_namePlateHeightOffset / g_namePlateMaxDistanceSq / g_tabTargetMaxDistanceSq instead
// (WoW/Source/Object/ObjectClient/NamePlateTuning.h), so the same numbers apply without code patches.
//
// The rest of the original's layout manager (stacking, clamping, hitboxes, occlusion, mouse modes)
// is not ported; the client's own SmartScreenRectPositionFrame layout stays in charge of overlap.

#include <cstring>
#include "Mods/AwesomeWotlk/AwesomeWotlk.h"
#include "WoW/Source/Object/ObjectClient/NamePlateTuning.h"

namespace awesome::nameplates {

namespace {

void ApplyDistance(float distance) {
    g_namePlateMaxDistanceSq = distance * distance;
    g_tabTargetMaxDistanceSq = distance * distance;
}

void ApplyPlacement(float placement) {
    g_namePlateHeightOffset = placement;
}

}

void Apply() {
    float distance = cvars::Float("nameplateDistance");
    ApplyDistance(distance);
    ApplyPlacement(cvars::Float("nameplatePlacement"));
    Log("nameplates: %.0f yd, %.2f yd above the name position", distance, g_namePlateHeightOffset);
}

void OnCVarChanged(const char* name, float value) {
    if (!strcmp(name, "nameplateDistance")) {
        ApplyDistance(value);
    } else if (!strcmp(name, "nameplatePlacement")) {
        ApplyPlacement(value);
    }
}

void Register() {
    Apply();
}

}
