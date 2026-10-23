#include "WoW/Source/Autotest.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Frame/CSimpleTop.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Scrn/Scrn.h"
#include "WoW/Source/Glue/CGlueMgr.h"
#include "WoW/Source/Glue/CharacterSelection.h"
#include "WoW/Source/LoadingScreen.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/UIUtil/InputControl.h"
#include "WoW/Source/WowServices/Login.h"
#include "WoW/Source/WowServices/RealmConnection.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"

// See Autotest.h. Every step runs on the client's event context (EventSetTimer), the thread the
// glue and the network already use, so the glue is driven exactly as by a player's clicks.

namespace {

enum Phase {
    PHASE_GLUE,
    PHASE_WAIT_LOGIN,
    PHASE_WAIT_CHARACTERS,
    PHASE_WAIT_WORLD,
    PHASE_WORLD,
    PHASE_DONE,
};

struct Shot {
    std::string m_label;
    bool m_world;
    uint32_t m_delayMs;
    bool m_taken;
};

Phase s_phase = PHASE_GLUE;
bool s_login;
bool s_exitAfterShots;
char s_account[128];
char s_password[256];
std::string s_character;
std::string s_realm;
std::vector<Shot> s_shots;
uint32_t s_started;
uint32_t s_charactersSeenAt;
uint32_t s_enteredAt;
uint32_t s_worldLoadedAt;
uint32_t s_lastLog;
// The label of the screenshot the client is capturing; empty while none is pending
std::string s_pendingShot;
FILE* s_log;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* format, ...) {
    if (!s_log) {
        s_log = fopen("Logs/autotest.log", "w");

        if (!s_log) {
            return;
        }
    }

    fprintf(s_log, "%8.3f ", (OsGetAsyncTimeMs() - s_started) / 1000.0);
    va_list args;
    va_start(args, format);
    vfprintf(s_log, format, args);
    va_end(args);
    fputc('\n', s_log);
    fflush(s_log);
}

void SetCVar(const char* name, const char* value) {
    CVar* var = CVar::Lookup(name);

    if (!var) {
        Log("cvar %s does not exist", name);
        return;
    }

    // Set the value without marking it for saving: Config.wtf keeps the player's settings
    var->Set(value, true, false, false, false);
}

bool ReadCredentials() {
    const char* path = getenv("WRATHMODERN_AUTOLOGIN_FILE");
    std::string defaultPath;

    if (!path || !path[0]) {
        const char* home = getenv("HOME");

        if (!home) {
            return false;
        }

        defaultPath = std::string(home) + "/.wrathmodern-autologin";
        path = defaultPath.c_str();
    }

    FILE* file = fopen(path, "r");

    if (!file) {
        Log("no credentials file at %s", path);
        return false;
    }

    char lines[2][256] = {};
    int32_t count = 0;

    while (count < 2 && fgets(lines[count], sizeof(lines[count]), file)) {
        size_t length = strlen(lines[count]);

        while (length && (lines[count][length - 1] == '\n' || lines[count][length - 1] == '\r')) {
            lines[count][--length] = '\0';
        }

        if (length) {
            count++;
        }
    }

    fclose(file);

    if (count < 2) {
        Log("the credentials file needs an account line and a password line");
        return false;
    }

    strncpy(s_account, lines[0], sizeof(s_account) - 1);
    strncpy(s_password, lines[1], sizeof(s_password) - 1);
    memset(lines, 0, sizeof(lines));
    return true;
}

void ParseShots(const char* spec) {
    std::string list = spec;
    size_t start = 0;

    while (start < list.size()) {
        size_t end = list.find(',', start);

        if (end == std::string::npos) {
            end = list.size();
        }

        std::string item = list.substr(start, end - start);
        size_t at = item.find('@');

        if (at != std::string::npos) {
            Shot shot;
            std::string phase = item.substr(0, at);
            shot.m_world = phase == "world";
            shot.m_delayMs = static_cast<uint32_t>(atof(item.c_str() + at + 1) * 1000.0);
            shot.m_taken = false;

            // Several shots of a phase get distinct labels: glue, glue-2, ...
            uint32_t index = 1;

            for (const Shot& other : s_shots) {
                index += other.m_world == shot.m_world;
            }

            shot.m_label = index == 1 ? phase : phase + "-" + std::to_string(index);
            s_shots.push_back(shot);
        }

        start = end + 1;
    }
}

// The client names screenshots by time stamp; give the newest one the shot's label
void OnScreenshot(int32_t success) {
    std::string label = s_pendingShot;
    s_pendingShot.clear();

    if (!success) {
        Log("screenshot %s failed", label.c_str());
        return;
    }

    DIR* dir = opendir("Screenshots");
    std::string newest;
    time_t newestTime = 0;

    while (dir) {
        dirent* entry = readdir(dir);

        if (!entry) {
            break;
        }

        std::string name = entry->d_name;

        if (name.compare(0, 12, "WoWScrnShot_") != 0) {
            continue;
        }

        struct stat info;
        std::string path = "Screenshots/" + name;

        if (stat(path.c_str(), &info) == 0 && info.st_mtime >= newestTime) {
            newestTime = info.st_mtime;
            newest = path;
        }
    }

    if (dir) {
        closedir(dir);
    }

    if (newest.empty()) {
        Log("screenshot %s: no file written", label.c_str());
        return;
    }

    std::string target = "Screenshots/autotest-" + label + ".tga";
    rename(newest.c_str(), target.c_str());
    Log("screenshot %s written to %s", label.c_str(), target.c_str());
}

// Takes the next due screenshot; returns true while shots remain
bool TakeShots(uint32_t now) {
    bool remaining = false;

    for (Shot& shot : s_shots) {
        if (shot.m_taken) {
            continue;
        }

        uint32_t base = shot.m_world ? s_worldLoadedAt : s_started;
        bool due = (!shot.m_world || s_worldLoadedAt) && now - base >= shot.m_delayMs;

        if (due && s_pendingShot.empty()) {
            shot.m_taken = true;
            s_pendingShot = shot.m_label;
            ScrnSetScreenshotFormat("tga");
            // As the Screenshot() script function does: the image writer needs the client stamp
            ScrnScreenshot(OnScreenshot, reinterpret_cast<uint8_t*>(ClientServices::GetClientStamp()), 88);
            Log("capturing %s", shot.m_label.c_str());
        }

        remaining = remaining || !shot.m_taken;
    }

    return remaining || !s_pendingShot.empty();
}

int32_t Tick(const void* data, void* param) {
    (void)data;
    (void)param;

    uint32_t now = OsGetAsyncTimeMs();
    bool shotsRemain = TakeShots(now);
    ClientConnection* connection = ClientServices::Connection();

    switch (s_phase) {
        case PHASE_GLUE: {
            bool glueShotsPending = false;

            for (const Shot& shot : s_shots) {
                glueShotsPending = glueShotsPending || (!shot.m_world && !shot.m_taken);
            }

            if (!s_login || glueShotsPending || !s_pendingShot.empty() || !connection) {
                break;
            }

            // Wait until the glue accepts a login, as the Login button does
            if (!CGlueMgr::m_scandllOkayToLogIn || !CGlueMgr::m_acceptedTOS || !CGlueMgr::m_acceptedEULA
                || CGlueMgr::m_idleState != IDLE_NONE) {
                break;
            }

            if (!s_realm.empty()) {
                SetCVar("realmName", s_realm.c_str());
            }

            char password[256];
            memcpy(password, s_password, sizeof(password));
            memset(s_password, 0, sizeof(s_password));

            Log("logging in as %s", s_account);
            CGlueMgr::LoginServerLogin(s_account, password);
            memset(password, 0, sizeof(password));
            s_phase = PHASE_WAIT_LOGIN;
            break;
        }

        case PHASE_WAIT_LOGIN: {
            if (ClientServices::s_loginObj && ClientServices::s_loginObj->IsLoggedOn()) {
                Log("logged in; waiting for the character list");
                s_phase = PHASE_WAIT_CHARACTERS;
            } else if (now - s_started > 90000) {
                Log("login did not complete");
                s_phase = PHASE_DONE;
            }

            break;
        }

        case PHASE_WAIT_CHARACTERS: {
            if (!connection || connection->m_characterList.Count() == 0 || CCharacterSelection::s_characterList.Count() == 0) {
                if (now - s_lastLog > 5000) {
                    s_lastLog = now;
                    Log("waiting for characters (connected %u, realm info %d)",
                        connection ? connection->m_connected : 0, ClientServices::m_selectRealmInfoValid ? 1 : 0);
                }

                break;
            }

            // A player presses Enter World some time after the list appears; the glue finishes
            // loading the character model and its callbacks in between
            if (!s_charactersSeenAt) {
                s_charactersSeenAt = now;
                break;
            }

            if (now - s_charactersSeenAt < 4000) {
                break;
            }

            uint32_t index = 0;

            for (uint32_t i = 0; i < connection->m_characterList.Count(); i++) {
                if (!s_character.empty() && !strcasecmp(connection->m_characterList.m_data[i].m_name, s_character.c_str())) {
                    index = i;
                }
            }

            CCharacterSelection::s_selectionIndex = static_cast<int32_t>(index);
            // A click on Enter World is an input event; without one the game UI's idle check
            // (CGGameUI::OnIdle: 30 minutes since CSimpleTop::s_eventTime) logs out at once
            CSimpleTop::s_eventTime = now;
            Log("entering the world as %s", connection->m_characterList.m_data[index].m_name);
            if (const char* noAddons = getenv("WRATHMODERN_AUTOTEST_NO_ADDONS")) {
                if (!strcmp(noAddons, "1")) {
                    Log("disabling Lua addons for this test (native mods remain enabled)");
                    FrameScript_Execute("DisableAllAddOns()", "autotest", nullptr);
                }
            }
            CGlueMgr::EnterWorld();
            s_enteredAt = now;
            s_phase = PHASE_WAIT_WORLD;
            break;
        }

        case PHASE_WAIT_WORLD: {
            if (ClntObjMgrGetActivePlayer() && !LoadingScreenDrawing()) {
                s_worldLoadedAt = now;
                Log("in the world after %.1f s", (now - s_enteredAt) / 1000.0);
                s_phase = PHASE_WORLD;

                // WRATHMODERN_AUTOTEST_LUA runs once in the world. A Lua error's message is written to
                // Logs/FrameXML.log, so a shell script can check a mod's Lua API without a screenshot.
                if (const char* script = getenv("WRATHMODERN_AUTOTEST_LUA")) {
                    Log("running the autotest Lua snippet");
                    FrameScript_Execute(script, "autotest", nullptr);
                    Log("the autotest Lua snippet returned");
                }
            } else if (now - s_enteredAt > 180000) {
                Log("the world did not load");
                s_phase = PHASE_DONE;
            }

            break;
        }

        default:
            break;
    }

    // WRATHMODERN_AUTOTEST_WALK="start,duration" (seconds in the world) holds the forward movement key in that
    // window through the client's own control bits: MoveForwardStart is protected, so the Lua snippet cannot walk
    static int32_t s_walk = -1;
    static float s_walkStart = 0.0f;
    static float s_walkDuration = 0.0f;

    if (s_walk < 0) {
        const char* walk = getenv("WRATHMODERN_AUTOTEST_WALK");
        s_walk = walk && sscanf(walk, "%f,%f", &s_walkStart, &s_walkDuration) == 2 ? 0 : 3;
    }

    if (s_walk < 3 && s_phase == PHASE_WORLD && s_worldLoadedAt && CGInputControl::s_inputControl) {
        float inWorld = (now - s_worldLoadedAt) / 1000.0f;

        if (s_walk == 0 && inWorld >= s_walkStart) {
            Log("walking forward");
            CGInputControl::s_inputControl->ToggleControlBit(0x10, 1, CSimpleTop::s_eventTime, 0);
            s_walk = 1;
        } else if (s_walk == 1 && inWorld >= s_walkStart + s_walkDuration) {
            Log("stopping");
            CGInputControl::s_inputControl->ToggleControlBit(0x10, 0, CSimpleTop::s_eventTime, 0);
            s_walk = 3;
        }
    }

    if (s_exitAfterShots && !s_shots.empty() && !shotsRemain && (s_phase == PHASE_WORLD || s_phase == PHASE_DONE || !s_login)) {
        Log("all screenshots taken; exiting");
        fflush(s_log);
        _exit(0);
    }

    if (s_phase == PHASE_DONE && s_exitAfterShots) {
        Log("exiting after a failed step");
        fflush(s_log);
        _exit(1);
    }

    EventSetTimer(250, Tick, nullptr);
    return 0;
}

}

void AutotestInitialize() {
    const char* realmList = getenv("WRATHMODERN_REALMLIST");
    const char* realm = getenv("WRATHMODERN_REALM");
    const char* login = getenv("WRATHMODERN_AUTOLOGIN");
    const char* character = getenv("WRATHMODERN_CHARACTER");
    const char* shots = getenv("WRATHMODERN_SHOTS");
    const char* exitAfter = getenv("WRATHMODERN_EXIT_AFTER_SHOTS");

    if (!realmList && !realm && !login && !shots) {
        return;
    }

    s_started = OsGetAsyncTimeMs();

    if (realmList && realmList[0]) {
        SetCVar("realmList", realmList);
        Log("realm list %s", realmList);
    }

    if (realm && realm[0]) {
        s_realm = realm;
        SetCVar("realmName", realm);
    }

    if (character) {
        s_character = character;
    }

    if (shots) {
        ParseShots(shots);
    }

    s_exitAfterShots = exitAfter && exitAfter[0] == '1';
    s_login = login && login[0] == '1' && ReadCredentials();
    Log("armed: login %d, %zu screenshot(s)", s_login ? 1 : 0, s_shots.size());
    EventSetTimer(250, Tick, nullptr);
}
