// Interaction options of awesome_wotlk (Misc.cpp): QueueInteract, /interact and the highlight of the
// object a key press would use.
//
// The original scans the visible objects every 100 ms, keeps the nearest unit that can be looted,
// skinned or assisted (or that has NPC flags) or the nearest game object in range, highlights it and
// uses it on request by calling the client's own right-click handler. This is the same scan on the
// client's objects; the per-frame tick comes from WMModInfo::OnFrame (the client calls it from
// CGWorldFrame::OnWorldRender).
//
// Game objects use the original's subtype allowlist and CanUse/CanUseNow checks. Decorative
// objects (for example Dalaran lampposts) must not become interaction targets or gain a proximity
// highlight merely because they are in front of the player.

#include <cmath>
#include <cstring>
#include "Mods/AwesomeWotlk/AwesomeWotlk.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/GameObject_C.h"
#include "WoW/Source/Object/ObjectClient/UnitCombatLog_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"
#include "WoW/Source/Ui/ScriptEvents.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace awesome::interaction {

namespace {

constexpr float kScanDistanceSq = 400.0f;         // 20 yards, the original's scan radius
constexpr float kUnitInteractDistanceSq = 25.0f;  // 5 yards for a unit (its own interact distance)
constexpr float kScanInterval = 0.1f;             // the original scans every 100 ms
constexpr uint32_t kUnitDynamicLootable = 0x0001;
constexpr uint32_t kUnitFlagSkinnable = 0x04000000;
constexpr int32_t kHighlightInteract = 2;         // the client uses 0 (mouseover) and 1 (target)

WOWGUID s_candidate;
WOWGUID s_highlighted;
WOWGUID s_request;
float s_scanIn = 0.0f;

CGObject_C* Object(WOWGUID guid, TypeMask mask) {
    return guid.m_guid ? ClntObjMgrObjectPtr(guid, mask, __FILE__, __LINE__) : nullptr;
}

float DistanceSq(const C3Vector& a, const C3Vector& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool IsInteractableUnit(CGUnit_C* unit, CGUnit_C* player) {
    if (!unit || unit == player) {
        return false;
    }

    uint32_t dynamicFlags = unit->m_unit->m_unitDynamicFlags;
    uint32_t flags = unit->m_unit->m_unitFieldFlags;
    uint32_t npcFlags = unit->m_unit->m_unitNpcFlags;
    bool lootable = (dynamicFlags & kUnitDynamicLootable) != 0;
    bool skinnable = (flags & kUnitFlagSkinnable) != 0;
    bool assistable = player->CanAssist(unit, true);
    return lootable || skinnable || (assistable && npcFlags != 0);
}

bool IsInteractableGameObject(CGGameObject_C* object) {
    if (!object || !object->m_gameObject || !object->m_typeData) {
        return false;
    }

    // awesome_wotlk Misc.cpp: kValidGameobjectTypes (3.3.5 GAMEOBJECT_BYTES_1 subtype).
    switch (object->m_gameObject->m_typeID) {
        case 0:  // Door
        case 1:  // Button
        case 2:  // Questgiver
        case 3:  // Chest
        case 4:  // Binder
        case 7:  // Chair
        case 8:  // SpellFocus
        case 10: // Goober
        case 17: // FishingNode
        case 19: // Mailbox
        case 23: // MeetingStone
        case 34: // GuildBank
            return object->CanUse() && object->CanUseNow();
        default:
            return false;
    }
}

struct Scan {
    CGUnit_C* m_player;
    C3Vector m_from;
    float m_cosLimit;   // 1.0: the whole circle, the original's interactionMode 0
    float m_bestDistanceSq;
    WOWGUID m_best;
};

// Returns false to stop the enumeration once nothing closer can be found
int32_t ScanObject(WOWGUID guid, void* param) {
    Scan* scan = static_cast<Scan*>(param);

    if (guid.m_guid == scan->m_player->m_obj->m_guid.m_guid) {
        return 1;
    }

    CGUnit_C* unit = static_cast<CGUnit_C*>(Object(guid, TYPEMASK_UNIT));
    CGObject_C* object = unit ? static_cast<CGObject_C*>(unit) : Object(guid, TYPEMASK_GAMEOBJECT);

    if (!object) {
        return 1;
    }

    C3Vector position = object->GetPosition();
    float distanceSq = DistanceSq(position, scan->m_from);
    float limit = unit ? kUnitInteractDistanceSq : kScanDistanceSq;

    if (distanceSq > limit || distanceSq >= scan->m_bestDistanceSq) {
        return 1;
    }

    if (unit) {
        if (!IsInteractableUnit(unit, scan->m_player)) {
            return 1;
        }
    } else if (!IsInteractableGameObject(static_cast<CGGameObject_C*>(object))) {
        return 1;
    }

    if (scan->m_cosLimit < 1.0f) {
        float dx = position.x - scan->m_from.x;
        float dy = position.y - scan->m_from.y;
        float length = std::sqrt(dx * dx + dy * dy);

        if (length < 0.0001f) {
            return 1;
        }

        float facing = scan->m_player->GetFacing();

        if ((dx / length) * std::cos(facing) + (dy / length) * std::sin(facing) < scan->m_cosLimit) {
            return 1;
        }
    }

    scan->m_bestDistanceSq = distanceSq;
    scan->m_best = guid;
    return 1;
}

void SetHighlight(WOWGUID guid) {
    if (guid.m_guid == s_highlighted.m_guid) {
        return;
    }

    if (CGObject_C* old = Object(s_highlighted, TYPEMASK_OBJECT)) {
        old->HideHighlightType(kHighlightInteract);
    }

    s_highlighted = guid;

    if (CGObject_C* now = Object(s_highlighted, TYPEMASK_OBJECT)) {
        now->ShowHighlightType(kHighlightInteract);
    }
}

void ScanForCandidate() {
    CGUnit_C* player = static_cast<CGUnit_C*>(Object(WOWGUID(ClntObjMgrGetActivePlayer()), TYPEMASK_PLAYER));

    if (!player) {
        s_candidate = WOWGUID();
        SetHighlight(WOWGUID());
        return;
    }

    Scan scan;
    scan.m_player = player;
    scan.m_from = player->GetPosition();
    scan.m_bestDistanceSq = kScanDistanceSq;
    scan.m_best = WOWGUID();

    if (cvars::Int("interactionMode") == 1) {
        float half = cvars::Float("interactionAngle") * 0.5f * 0.017453292f;
        scan.m_cosLimit = std::cos(half);
    } else {
        scan.m_cosLimit = 1.0f;
    }

    ClntObjMgrEnumVisibleObjects(ScanObject, &scan);
    s_candidate = scan.m_best;

    SetHighlight(cvars::Int("interactionHighlight") ? s_candidate : WOWGUID());
}

int32_t QueueInteract(lua_State* L) {
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        WOWGUID guid = GetGuidByKeyword(lua_tolstring(L, 1, nullptr));

        if (!guid.m_guid) {
            guid = StringToGuid(lua_tolstring(L, 1, nullptr));
        }

        // A token can name an object the client does not have; only queue what it can right click
        if (guid.m_guid && Object(guid, TYPEMASK_OBJECT)) {
            s_request = guid;
            lua_pushboolean(L, 1);
            return 1;
        }

        lua_pushboolean(L, 0);
        return 1;
    }

    if (s_candidate.m_guid) {
        s_request = s_candidate;
    }

    lua_pushboolean(L, s_candidate.m_guid != 0);
    return 1;
}

}

void OnFrame(float elapsed, void* user) {
    if (!ClntObjMgrGetActivePlayer()) {
        s_candidate = WOWGUID();
        s_request = WOWGUID();
        SetHighlight(WOWGUID());
        return;
    }

    if (s_request.m_guid) {
        WOWGUID request = s_request;
        s_request = WOWGUID();
        CGGameUI::OnSpriteRightClick(request);
    }

    s_scanIn -= elapsed;

    if (s_scanIn <= 0.0f) {
        s_scanIn = kScanInterval;
        ScanForCandidate();
    }
}

void OnWorld(int32_t entered, void* user) {
    s_candidate = WOWGUID();
    s_highlighted = WOWGUID();
    s_request = WOWGUID();
    s_scanIn = 0.0f;

    if (entered) {
        // /interact [unit]: the addon binds a key to QueueInteract itself
        g_host->ExecuteLua("SLASH_INTERACT1 = '/interact'\n"
                           "SlashCmdList['INTERACT'] = function(msg) if msg and msg ~= '' then QueueInteract(msg) else QueueInteract() end end\n",
                           "awesome");
    }
}

void Register() {
    g_host->RegisterLuaFunction("QueueInteract", QueueInteract, WM_SCRIPT_GAME);
}

}
