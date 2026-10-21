/*
 * Lux Shoulder Cam for WrathModern.
 *
 * A port of LuxShoulderCam.dll (Stormhand-dev/Lux-Shoulder-Cam, MIT; the repository ships only the
 * binary, so its behaviour was recovered by disassembly, see Docs/Mods/LuxShoulderCam.md). The DLL
 * patched CGCamera::CalcTargetCamera at 0x006070CB to shift the camera's orbit target sideways and up,
 * and ran its direction update once per frame from a Direct3D BeginScene detour. Here the client calls
 * OnCameraTarget at the same point, and the per-frame update runs first in that call, from the values
 * captured the frame before, exactly as the DLL's tick did between two camera updates.
 *
 * Lua: LSC(command, argument) -> number, the DLL's protocol, so the unmodified addon works:
 *   1 ping (1), 2 patch status (1 = active), 3/10 height, 4 player facing (-999 without a player),
 *   11 set height, 12 horizontal, 13 set horizontal (clamped to [-1, 1]), 14 reset.
 *   -88888: rejected because the camera is in first person; -99999: bad arguments.
 * Settings: LuxShoulderCam.cfg in the game directory, the DLL's file and format.
 */

#include <cmath>
#include <cstdio>
#include <string>
#include "WMModAPI.h"

extern "C" {
#include "lua.h"
}

namespace {

const float kLocked = -88888.0f;
const float kError = -99999.0f;

const WMModHost* s_host;
std::string s_configPath;

// Settings
float s_height;
float s_horizontal;

// The previous camera update: its camera eye and unshifted orbit target
float s_capturedCameraX;
float s_capturedCameraY;
float s_capturedTargetX;
float s_capturedTargetY;

// Unit vector to the left of the view direction; the DLL starts with (0, 1)
float s_directionX = 0.0f;
float s_directionY = 1.0f;

// Offsets in effect; zero while the camera is in first person
float s_appliedHeight;
float s_appliedHorizontal;
bool s_locked;

// The player as of the last camera update, for LSC(4)
bool s_playerValid;
float s_playerFacing;

void LoadConfig() {
    FILE* file = fopen(s_configPath.c_str(), "r");

    if (!file) {
        return;
    }

    char line[128];

    while (fgets(line, sizeof(line), file)) {
        float value;

        if (sscanf(line, "height=%f", &value) == 1) {
            s_height = value;
        } else if (sscanf(line, "horizontal=%f", &value) == 1) {
            s_horizontal = value;
        }
    }

    fclose(file);
}

void SaveConfig() {
    FILE* file = fopen(s_configPath.c_str(), "w");

    if (file) {
        fprintf(file, "height=%f\nhorizontal=%f\n", s_height, s_horizontal);
        fclose(file);
    }
}

// The DLL's per-frame update, from the values the previous camera update captured
void UpdateDirection(const WMCameraTarget* camera) {
    if ((s_capturedCameraX != 0.0f || s_capturedCameraY != 0.0f) && camera->playerValid) {
        float dx = camera->playerPosition[0] - s_capturedCameraX;
        float dy = camera->playerPosition[1] - s_capturedCameraY;
        float distance = std::sqrt(dx * dx + dy * dy);

        float tx = s_capturedTargetX - s_capturedCameraX;
        float ty = s_capturedTargetY - s_capturedCameraY;
        float targetDistance = std::sqrt(tx * tx + ty * ty);

        // The eye within 0.7 yards of the target is first person: no offset there
        s_locked = targetDistance < 0.7f;

        // Away from the player, the forward direction turned a quarter left (x north, y west, z up)
        if (!s_locked && 2.0f * std::fabs(s_horizontal) + 1.0f < distance) {
            s_directionY = dx / distance;
            s_directionX = -dy / distance;
        }
    } else if (camera->playerValid) {
        s_directionY = std::cos(camera->playerFacing);
        s_directionX = -std::sin(camera->playerFacing);
    }

    s_appliedHeight = s_locked ? 0.0f : s_height;
    s_appliedHorizontal = s_locked ? 0.0f : s_horizontal;
}

void OnCameraTarget(WMCameraTarget* camera, void* user) {
    UpdateDirection(camera);

    s_capturedCameraX = camera->cameraPosition[0];
    s_capturedCameraY = camera->cameraPosition[1];
    s_capturedTargetX = camera->targetPosition[0];
    s_capturedTargetY = camera->targetPosition[1];
    s_playerValid = camera->playerValid != 0;
    s_playerFacing = camera->playerFacing;

    camera->targetPosition[2] += s_appliedHeight;
    camera->targetPosition[1] += s_appliedHorizontal * s_directionY;
    camera->targetPosition[0] += s_appliedHorizontal * s_directionX;
}

int LSC(lua_State* L) {
    if (!lua_isnumber(L, 1)) {
        lua_pushnumber(L, kError);
        return 1;
    }

    int command = static_cast<int>(lua_tonumber(L, 1));
    float argument = static_cast<float>(lua_tonumber(L, 2));
    float result = 0.0f;

    switch (command) {
        case 1:
            result = 1.0f;
            break;

        case 2:
            result = 1.0f;
            break;

        case 3:
        case 10:
            result = s_height;
            break;

        case 4:
            result = s_playerValid ? s_playerFacing : -999.0f;
            break;

        case 11:
            if (s_locked) {
                result = kLocked;
            } else {
                s_height = argument;
                SaveConfig();
                result = argument;
            }
            break;

        case 12:
            result = s_horizontal;
            break;

        case 13:
            if (s_locked) {
                result = kLocked;
            } else {
                s_horizontal = argument < -1.0f ? -1.0f : argument > 1.0f ? 1.0f : argument;
                SaveConfig();
                result = s_horizontal;
            }
            break;

        case 14:
            s_height = 0.0f;
            s_horizontal = 0.0f;
            SaveConfig();
            result = 0.0f;
            break;

        default:
            break;
    }

    lua_pushnumber(L, result);
    return 1;
}

void OnScriptContext(uint32_t context, int32_t loaded, void* user) {
    // The DLL announced itself in chat once per interface
    if (context == WM_SCRIPT_GAME && loaded) {
        s_host->ExecuteLua(
            "if DEFAULT_CHAT_FRAME then DEFAULT_CHAT_FRAME:AddMessage("
            "'|cFF00FF00[LuxShoulderCam]|r |cFF00FF00Active  --  /lsc to open panel|r') end",
            "LuxShoulderCam");
    }
}

}

extern "C" __attribute__((visibility("default"))) int32_t WMModLoad(const WMModHost* host, WMModInfo* info) {
    s_host = host;

    if (host->version < 2 || !WM_HOST_HAS(host, SetCVar)) {
        host->Log("LuxShoulderCam: needs mod interface version 2");
        return 0;
    }

    s_configPath = std::string(host->gameDirectory) + "/LuxShoulderCam.cfg";
    LoadConfig();
    host->RegisterLuaFunction("LSC", LSC, WM_SCRIPT_GAME);

    char line[128];
    snprintf(line, sizeof(line), "LuxShoulderCam: height %.2f, horizontal %.2f", s_height, s_horizontal);
    host->Log(line);

    info->version = WM_MOD_API_VERSION;
    info->name = "Lux Shoulder Cam";
    info->OnScriptContext = OnScriptContext;
    info->OnCameraTarget = OnCameraTarget;
    return 1;
}
