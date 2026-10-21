// Unit, inventory and spell functions of awesome_wotlk (UnitAPI.cpp, Inventory.cpp, Spell.cpp), on the
// client's own objects: the original read the same descriptor fields through Wow.exe addresses.

#include <algorithm>
#include <cstdio>
#include "Mods/AwesomeWotlk/AwesomeWotlk.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/UnitCombatLog_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/ScriptEvents.h"

namespace awesome::unit_api {

namespace {

// UNIT_FIELD_FLAGS bits
constexpr uint32_t kSilenced = 0x00002000;
constexpr uint32_t kPacified = 0x00020000;
constexpr uint32_t kStunned = 0x00040000;
constexpr uint32_t kDisarmed = 0x00200000;
constexpr uint32_t kConfused = 0x00400000;
constexpr uint32_t kFleeing = 0x00800000;

CGUnit_C* UnitByGuid(WOWGUID guid) {
    return guid ? static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(guid, TYPEMASK_UNIT, __FILE__, __LINE__)) : nullptr;
}

// A unit token, or a GUID written as 0x... (object_mgr::string2Guid)
WOWGUID GuidFromString(const char* text) {
    if (!text) {
        return WOWGUID();
    }

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return StringToGuid(text);
    }

    return GetGuidByKeyword(text);
}

CGUnit_C* CheckUnit(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: %s(unitID)", "Unit");
    }

    return UnitByGuid(GetGuidByKeyword(lua_tostring(L, 1)));
}

int UnitHasFlag(lua_State* L, uint32_t flag) {
    CGUnit_C* unit = CheckUnit(L);

    if (unit && (unit->m_unit->m_unitFieldFlags & flag)) {
        lua_pushnumber(L, 1);
        return 1;
    }

    return 0;
}

int UnitIsControlled(lua_State* L) {
    return UnitHasFlag(L, kFleeing | kConfused | kStunned | kPacified);
}

int UnitIsDisarmed(lua_State* L) {
    return UnitHasFlag(L, kDisarmed);
}

int UnitIsSilenced(lua_State* L) {
    return UnitHasFlag(L, kSilenced);
}

int UnitOccupations(lua_State* L) {
    CGUnit_C* unit = CheckUnit(L);

    if (!unit) {
        return 0;
    }

    lua_pushnumber(L, unit->m_unit->m_unitNpcFlags);
    return 1;
}

int UnitOwner(lua_State* L) {
    CGUnit_C* unit = CheckUnit(L);

    if (!unit) {
        return 0;
    }

    WOWGUID ownerGuid = unit->m_unit->m_unitFieldSummonedby ? unit->m_unit->m_unitFieldSummonedby : unit->m_unit->m_unitFieldCreatedby;
    CGUnit_C* owner = UnitByGuid(ownerGuid);

    if (!owner) {
        return 0;
    }

    const char* name = owner->GetObjectName();
    lua_pushstring(L, name ? name : "UNKNOWN");

    char text[24];
    snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(ownerGuid.m_guid));
    lua_pushstring(L, text);
    return 2;
}

bool PushTokenIfMatches(lua_State* L, const char* token, WOWGUID guid) {
    if (GetGuidByKeyword(token) == guid) {
        lua_pushstring(L, token);
        return true;
    }

    return false;
}

bool PushIndexedToken(lua_State* L, const char* base, int first, int last, WOWGUID guid) {
    char token[32];

    for (int i = first; i <= last; i++) {
        snprintf(token, sizeof(token), "%s%d", base, i);

        if (PushTokenIfMatches(L, token, guid)) {
            return true;
        }
    }

    return false;
}

int UnitTokenFromGUID(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: %s(GUID)", "UnitTokenFromGUID");
    }

    WOWGUID guid = StringToGuid(lua_tostring(L, 1));

    if (!guid || !UnitByGuid(guid)) {
        return 0;
    }

    for (const char* token : { "player", "vehicle", "pet", "target", "focus", "mouseover" }) {
        if (PushTokenIfMatches(L, token, guid)) {
            return 1;
        }
    }

    if (PushIndexedToken(L, "party", 1, 4, guid) || PushIndexedToken(L, "partypet", 1, 4, guid)
        || PushIndexedToken(L, "raid", 1, 40, guid) || PushIndexedToken(L, "raidpet", 1, 40, guid)
        || PushIndexedToken(L, "arena", 1, 5, guid) || PushIndexedToken(L, "arenapet", 1, 5, guid)
        || PushIndexedToken(L, "boss", 1, 5, guid)) {
        return 1;
    }

    // nameplateN tokens join with the nameplate module
    return 0;
}

int GetInventoryItemTransmog(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isnumber(L, 2)) {
        luaL_error(L, "Usage: %s(unitID, slotID)", "GetInventoryItemTransmog");
    }

    lua_Number raw = lua_tonumber(L, 2);
    WOWGUID guid = GuidFromString(lua_tostring(L, 1));

    if (raw != static_cast<lua_Number>(static_cast<int>(raw)) || !guid) {
        luaL_error(L, "Usage: %s(unitID, slotID)", "GetInventoryItemTransmog");
    }

    auto* player = static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(guid, TYPEMASK_PLAYER, __FILE__, __LINE__));
    int index = static_cast<int>(raw) - 1;

    if (!player || !player->m_player || index < 0 || index >= 19) {
        return 0;
    }

    const VisibleItem_C& item = player->m_player->m_playerVisibleItems[index];
    lua_pushnumber(L, item.m_itemId);
    // The descriptor's enchantment field as one 32-bit value, as the original reads it
    lua_pushnumber(L, item.m_enchantment[0] | (static_cast<uint32_t>(item.m_enchantment[1]) << 16));
    return 2;
}

int GetSpellBaseCooldown(lua_State* L) {
    if (!lua_isnumber(L, 1)) {
        luaL_error(L, "Usage: %s(spellID)", "GetSpellBaseCooldown");
    }

    int32_t spellId = static_cast<int32_t>(lua_tonumber(L, 1));
    // Spell.dbc is run-length compressed: records are copied out
    SpellRec record;

    if (!g_spellDB.GetRecord(spellId, &record)) {
        return 0;
    }

    const SpellRec* spell = &record;

    int32_t cooldown = spell->m_recoveryTime ? spell->m_recoveryTime : spell->m_categoryRecoveryTime;
    int32_t global = spell->m_startRecoveryTime;

    // A spell that only triggers others takes their cooldowns
    if (!cooldown) {
        for (int32_t triggered : spell->m_effectTriggerSpell) {
            if (!triggered || triggered == spellId) {
                continue;
            }

            SpellRec triggeredRecord;

            if (g_spellDB.GetRecord(triggered, &triggeredRecord)) {
                cooldown = std::max(cooldown, triggeredRecord.m_recoveryTime ? triggeredRecord.m_recoveryTime : triggeredRecord.m_categoryRecoveryTime);
                global = std::max(global, triggeredRecord.m_startRecoveryTime);
            }
        }
    }

    lua_pushnumber(L, cooldown);
    lua_pushnumber(L, global);
    return 2;
}

}

void Register() {
    g_host->RegisterLuaFunction("UnitIsControlled", UnitIsControlled, WM_SCRIPT_GAME);
    g_host->RegisterLuaFunction("UnitIsDisarmed", UnitIsDisarmed, WM_SCRIPT_GAME);
    g_host->RegisterLuaFunction("UnitIsSilenced", UnitIsSilenced, WM_SCRIPT_GAME);
    g_host->RegisterLuaFunction("UnitOccupations", UnitOccupations, WM_SCRIPT_GAME);
    g_host->RegisterLuaFunction("UnitOwner", UnitOwner, WM_SCRIPT_GAME);
    g_host->RegisterLuaFunction("UnitTokenFromGUID", UnitTokenFromGUID, WM_SCRIPT_GAME);
    g_host->RegisterLuaFunction("GetInventoryItemTransmog", GetInventoryItemTransmog, WM_SCRIPT_GAME);
    g_host->RegisterLuaFunction("GetSpellBaseCooldown", GetSpellBaseCooldown, WM_SCRIPT_GAME);
}

}
