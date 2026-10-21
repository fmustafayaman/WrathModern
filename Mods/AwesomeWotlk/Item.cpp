// GetItemInfoInstant of awesome_wotlk (Item.cpp), on the client's own item cache and DBC tables:
// itemID, itemType, itemSubType, itemEquipLoc, icon, classID, subclassID. The original read the same
// fields of the ClientDB record through Wow.exe addresses.

#include <cstdint>
#include "Mods/AwesomeWotlk/AwesomeWotlk.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/DBClient/DBCacheInstances.h"
#include "WoW/Source/Object/ItemStats.h"
#include "WoW/Source/Object/ObjectClient/Item_C.h"
#include "WoW/Source/UIUtil/Tooltip.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace awesome::item {

namespace {

int32_t GetItemInfoInstant(lua_State* L) {
    uint32_t itemId = 0;

    if (lua_isnumber(L, 1)) {
        double value = lua_tonumber(L, 1);

        if (value <= 0.0 || value != static_cast<double>(static_cast<uint32_t>(value))) {
            return 0;
        }

        itemId = static_cast<uint32_t>(value);
    } else if (lua_isstring(L, 1)) {
        itemId = static_cast<uint32_t>(GetItemIDByName(lua_tolstring(L, 1, nullptr)));
    } else {
        luaL_error(L, "Usage: GetItemInfoInstant(itemID | \"itemName\" | \"itemLink\")");
        return 0;
    }

    WOWGUID none;
    ItemStats_C* stats = itemId ? g_itemDBCache.GetRecord(static_cast<int32_t>(itemId), none, nullptr, nullptr, false) : nullptr;

    if (!stats) {
        // The item cache answers asynchronously: the first lookup of an item the client has not seen
        // sends its query and has no record yet, so return nothing and let the caller retry after
        // GET_ITEM_INFO_RECEIVED (the client's GetItemInfo behaves the same).
        return 0;
    }

    ItemClassRec* itemClass = g_itemClassDB.GetRecord(static_cast<int32_t>(stats->m_class));
    const ItemSubClassRec* subClass = nullptr;

    for (int32_t i = 0; i < g_itemSubClassDB.m_numRecords; i++) {
        const ItemSubClassRec* record = &g_itemSubClassDB.m_records[i];

        if (record->m_classID == static_cast<int32_t>(stats->m_class) && record->m_subClassID == static_cast<int32_t>(stats->m_subClass)) {
            subClass = record;
            break;
        }
    }

    lua_pushnumber(L, static_cast<double>(itemId));
    lua_pushstring(L, itemClass ? itemClass->m_className : "");
    lua_pushstring(L, subClass ? (*subClass->m_verboseName ? subClass->m_verboseName : subClass->m_displayName) : "");
    lua_pushstring(L, stats->m_inventoryType < 29 ? s_inventoryTypeTokens[stats->m_inventoryType] : "");
    lua_pushstring(L, CGItem_C::GetInventoryArt(static_cast<int32_t>(stats->m_displayId)));
    lua_pushnumber(L, static_cast<double>(stats->m_class));
    lua_pushnumber(L, static_cast<double>(stats->m_subClass));
    return 7;
}

}

void Register() {
    g_host->RegisterLuaFunction("GetItemInfoInstant", GetItemInfoInstant, WM_SCRIPT_ALL);
}

}
