// Window and clipboard functions of awesome_wotlk (Misc.cpp): IsWindowFocused, FocusWindow and
// CopyToClipboard on AppKit. FlashWindow is the client's own (WRATHMODERN_CLIENT_PATCHES).

#import <AppKit/AppKit.h>
#include "Mods/AwesomeWotlk/AwesomeWotlk.h"
#include "Engine/Source/Os/OsClipboard.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace awesome::window {

namespace {

int IsWindowFocused(lua_State* L) {
    if ([NSApp isActive] && [NSApp keyWindow]) {
        lua_pushnumber(L, 1);
        return 1;
    }

    return 0;
}

int FocusWindow(lua_State* L) {
    [NSApp activateIgnoringOtherApps:YES];

    NSWindow* window = [NSApp mainWindow] ?: [[NSApp windows] firstObject];
    [window makeKeyAndOrderFront:nil];
    return 0;
}

int CopyToClipboard(lua_State* L) {
    // The client's clipboard takes UTF-8 and stores it as Unicode text
    OsClipboardPutString(luaL_checkstring(L, 1));
    return 0;
}

}

void Register() {
    g_host->RegisterLuaFunction("IsWindowFocused", IsWindowFocused, WM_SCRIPT_ALL);
    g_host->RegisterLuaFunction("FocusWindow", FocusWindow, WM_SCRIPT_ALL);
    g_host->RegisterLuaFunction("CopyToClipboard", CopyToClipboard, WM_SCRIPT_ALL);
}

}
