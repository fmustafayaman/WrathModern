#include "WoW/Source/Ui/MetalFXMenu.h"

#if defined(__APPLE__)
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/CGxDeviceMetal/MetalFXSettings.h"

namespace {
int32_t Script_GetMetalFXStatus(lua_State* L) {
    GxMetalFXStatus status;
    GxMetalFXGetStatus(status);
    lua_pushboolean(L, status.supported);
    lua_pushinteger(L, GxMetalFXMode());
    lua_pushinteger(L, status.inputWidth);
    lua_pushinteger(L, status.inputHeight);
    lua_pushinteger(L, status.outputWidth);
    lua_pushinteger(L, status.outputHeight);
    lua_pushstring(L, status.message);
    return 7;
}

int32_t Script_ToggleMetalFXMenu(lua_State*) {
    GxMetalFXToggleMenu();
    return 0;
}

// Built-in FrameXML widgets, not an addon or a Cocoa window. Created lazily outside render traversal.
const char* s_toggleMenu = R"lua(
if not UIParent then return end
local f = WrathModernMetalFXFrame
if not f then
    f = CreateFrame("Frame", "WrathModernMetalFXFrame", UIParent)
    f:Hide()
    f:SetWidth(480)
    f:SetHeight(390)
    f:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
    f:SetFrameStrata("DIALOG")
    f:SetToplevel(true)
    f:EnableMouse(true)
    f:SetMovable(true)
    f:RegisterForDrag("LeftButton")
    f:SetScript("OnDragStart", function(self) self:StartMoving() end)
    f:SetScript("OnDragStop", function(self) self:StopMovingOrSizing() end)
    f:SetBackdrop({bgFile="Interface\\Tooltips\\UI-Tooltip-Background",
                   edgeFile="Interface\\Tooltips\\UI-Tooltip-Border",
                   tile=true, tileSize=16, edgeSize=16,
                   insets={left=4, right=4, top=4, bottom=4}})
    f:SetBackdropColor(0.035, 0.045, 0.065, 0.98)
    f:SetBackdropBorderColor(0.65, 0.55, 0.3, 1)
    local title = f:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    title:SetPoint("TOPLEFT", 22, -22)
    title:SetText("WrathModern — MetalFX Spatial")
    local close = CreateFrame("Button", nil, f, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", -4, -4)
    close:SetScript("OnClick", function() f:Hide() end)
    local subtitle = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    subtitle:SetPoint("TOPLEFT", 22, -53)
    subtitle:SetText("Only the 3D world is upscaled. No temporal upscaling or frame generation.")
    f.buttons = {}
    local labels = {"Off - native (100%)", "Quality - internal resolution 75%",
                    "Balanced - internal resolution 67%", "Performance - internal resolution 50%"}
    for i = 1, 4 do
        local mode = i - 1
        local button = CreateFrame("Button", nil, f, "UIPanelButtonTemplate")
        button:SetWidth(432)
        button:SetHeight(30)
        button:SetPoint("TOPLEFT", 24, -80 - (i - 1) * 36)
        button.label = labels[i]
        button:SetScript("OnClick", function()
            SetCVar("metalFX", mode)
            f:Refresh()
        end)
        f.buttons[i] = button
    end
    f.info = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    f.info:SetPoint("TOPLEFT", 24, -239)
    f.info:SetWidth(432)
    f.info:SetJustifyH("LEFT")
    f.info:SetJustifyV("TOP")
    local hint = f:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("BOTTOMLEFT", 24, 20)
    hint:SetWidth(432)
    hint:SetJustifyH("LEFT")
    hint:SetText("Control/Option + F8 / Esc: close · Drag the title bar to move.\nDynamic 60 FPS mode is not ready yet.")
    function f:Refresh()
        local supported, mode, iw, ih, ow, oh, status = WrathModern_GetMetalFXStatus()
        for i, button in ipairs(self.buttons) do
            if supported or i == 1 then button:Enable() else button:Disable() end
            button:SetText((mode == i - 1 and "|cff70ff80[Selected] " or "|cffffffff") .. button.label .. "|r")
        end
        self.info:SetText(string.format("%s\n3D world: %d x %d -> %d x %d\nInterface and text: native resolution", status, iw, ih, ow, oh))
    end
    f:SetScript("OnShow", function(self) self.elapsed = 0; self:Refresh() end)
    f:SetScript("OnUpdate", function(self, dt)
        self.elapsed = (self.elapsed or 0) + dt
        if self.elapsed >= 0.25 then self.elapsed = 0; self:Refresh() end
    end)
    UISpecialFrames = UISpecialFrames or {}
    table.insert(UISpecialFrames, "WrathModernMetalFXFrame")
end
if f:IsShown() then f:Hide() else
    if MouselookStop then MouselookStop() end
    f:Show()
    f:Raise()
end
)lua";
}

void MetalFXMenuInitialize() {
    FrameScript_RegisterFunction("WrathModern_GetMetalFXStatus", Script_GetMetalFXStatus);
    FrameScript_RegisterFunction("WrathModern_ToggleMetalFXMenu", Script_ToggleMetalFXMenu);
}

void MetalFXMenuUpdate() {
    if (GxMetalFXConsumeMenuToggle()) {
        FrameScript_Execute(s_toggleMenu, "WrathModern-MetalFX-Menu", nullptr);
    }
}
#endif
