-- CoAVolFog settings panel (the Windows build's Alt+F7 overlay, as an interface frame). Opens with ALT-F7
-- (Option+F7) or /coavolfog. Changes apply at once; Save writes CoAVolFog.ini, Revert reloads it.
-- The mod provides CoAVolFog_Get(key) -> value, lo, hi, kind; CoAVolFog_Set(key, value) -> value;
-- CoAVolFog_Save() -> ok; CoAVolFog_Revert().

if CoAVolFogPanel then return end

local SETTINGS = {
    {"General"},
    {"Enable", "Enabled"},
    {"Quality", "Quality (1 low - 3 high)"},
    {"Temporal", "Temporal filter"},
    {"StockFog", "Replace the stock fog"},
    {"DebugView", "Debug view (1 radiance, 2 transmittance, 3 depth)"},
    {"Fog"},
    {"Density", "Density"},
    {"Haze", "Distance haze"},
    {"GroundFog", "Ground mist"},
    {"FarFog", "Distance fog"},
    {"MaxDistance", "Maximum distance (yards)"},
    {"DataMode", "Classic fog data"},
    {"ClassicNoise", "Classic fog banks"},
    {"NoiseAmount", "Density variation"},
    {"NoiseScale", "Variation scale"},
    {"NoiseWindSpeed", "Wind speed"},
    {"Light and colour"},
    {"ColorSpace", "Blend in linear light"},
    {"SunScatter", "Sun scattering"},
    {"Ambient", "Ambient light"},
    {"Exposure", "Exposure"},
    {"ClassicExposure", "Classic exposure"},
    {"Local lights"},
    {"LocalLights", "Lamps and fires light the fog"},
    {"LocalLightIntensity", "Light intensity"},
    {"LocalLightPhase", "Forward scattering"},
    {"Interiors"},
    {"InteriorAware", "Interior aware"},
    {"InteriorDensity", "Interior density"},
    {"InteriorFarFog", "Interior distance fog"},
}

local ROW = 38
local WIDTH = 420

local panel = CreateFrame("Frame", "CoAVolFogPanel", UIParent)
panel:SetWidth(WIDTH + 40)
panel:SetHeight(560)
panel:SetPoint("CENTER")
panel:SetFrameStrata("DIALOG")
panel:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = {left = 11, right = 12, top = 12, bottom = 11},
})
panel:SetMovable(true)
panel:EnableMouse(true)
panel:RegisterForDrag("LeftButton")
panel:SetScript("OnDragStart", panel.StartMoving)
panel:SetScript("OnDragStop", panel.StopMovingOrSizing)
panel:Hide()
tinsert(UISpecialFrames, "CoAVolFogPanel")

local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
title:SetPoint("TOP", 0, -18)
title:SetText("CoAVolFog")

local hint = panel:CreateFontString(nil, "ARTWORK", "GameFontDisableSmall")
hint:SetPoint("TOP", title, "BOTTOM", 0, -4)
hint:SetText("Changes apply at once · Option+F7 or Esc closes")

local scroll = CreateFrame("ScrollFrame", "CoAVolFogPanelScroll", panel, "UIPanelScrollFrameTemplate")
scroll:SetPoint("TOPLEFT", 20, -56)
scroll:SetPoint("BOTTOMRIGHT", -38, 50)

local content = CreateFrame("Frame", nil, scroll)
content:SetWidth(WIDTH - 20)
scroll:SetScrollChild(content)

local controls = {}

local function Format(kind, value)
    if kind == "f" then
        return string.format(value >= 100 and "%.0f" or "%.2f", value)
    end
    return tostring(math.floor(value + 0.5))
end

local function Refresh()
    for _, control in ipairs(controls) do
        local value = CoAVolFog_Get(control.key)
        control.updating = true
        if control.kind == "b" then
            control.widget:SetChecked(value and value >= 0.5)
        else
            control.widget:SetValue(value or control.lo)
            control.valueText:SetText(Format(control.kind, value or control.lo))
        end
        control.updating = false
    end
end

local y = -4
for index, entry in ipairs(SETTINGS) do
    local key, label = entry[1], entry[2]
    if not label then
        local header = content:CreateFontString(nil, "ARTWORK", "GameFontNormal")
        header:SetPoint("TOPLEFT", 4, y - 6)
        header:SetText(key)
        y = y - 26
    else
        local value, lo, hi, kind = CoAVolFog_Get(key)
        if value then
            local name = "CoAVolFogPanelControl" .. index
            local control = {key = key, kind = kind, lo = lo, hi = hi}
            if kind == "b" or (kind == "i" and lo == 0 and hi == 1) then
                control.kind = "b"
                local check = CreateFrame("CheckButton", name, content, "UICheckButtonTemplate")
                check:SetPoint("TOPLEFT", 8, y)
                _G[name .. "Text"]:SetText(label)
                check:SetScript("OnClick", function(self)
                    CoAVolFog_Set(key, self:GetChecked() and 1 or 0)
                end)
                control.widget = check
                y = y - 30
            else
                local slider = CreateFrame("Slider", name, content, "OptionsSliderTemplate")
                slider:SetPoint("TOPLEFT", 14, y - 14)
                slider:SetWidth(WIDTH - 110)
                slider:SetMinMaxValues(lo, hi)
                slider:SetValueStep(kind == "f" and math.max((hi - lo) / 200, 0.001) or 1)
                _G[name .. "Text"]:SetText(label)
                _G[name .. "Low"]:SetText(Format(kind, lo))
                _G[name .. "High"]:SetText(Format(kind, hi))
                local valueText = content:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
                valueText:SetPoint("LEFT", slider, "RIGHT", 10, 0)
                control.valueText = valueText
                slider:SetScript("OnValueChanged", function(self, v)
                    if control.updating then return end
                    local applied = CoAVolFog_Set(key, v)
                    valueText:SetText(Format(kind, applied or v))
                end)
                control.widget = slider
                y = y - ROW - 8
            end
            tinsert(controls, control)
        end
    end
end
content:SetHeight(-y + 8)

local function Button(text, anchor, x, onClick)
    local button = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
    button:SetWidth(110)
    button:SetHeight(22)
    button:SetPoint(anchor, x, 18)
    button:SetText(text)
    button:SetScript("OnClick", onClick)
    return button
end

local status = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
status:SetPoint("BOTTOM", 0, 42)

Button("Save", "BOTTOMLEFT", 20, function()
    status:SetText(CoAVolFog_Save() and "Saved to CoAVolFog.ini" or "Could not save")
end)
Button("Revert", "BOTTOM", 0, function()
    CoAVolFog_Revert()
    Refresh()
    status:SetText("Reloaded from CoAVolFog.ini")
end)
Button("Close", "BOTTOMRIGHT", -20, function() panel:Hide() end)

panel:SetScript("OnShow", function()
    status:SetText("")
    Refresh()
end)

local toggle = CreateFrame("Button", "CoAVolFogPanelToggle", UIParent)
toggle:SetScript("OnClick", function()
    if panel:IsShown() then panel:Hide() else panel:Show() end
end)
SetOverrideBindingClick(toggle, true, "ALT-F7", "CoAVolFogPanelToggle")

SLASH_COAVOLFOG1 = "/coavolfog"
SlashCmdList["COAVOLFOG"] = function() toggle:Click() end
