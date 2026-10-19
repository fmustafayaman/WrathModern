#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#include "WoW/Source/Autologin.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/Glue/CGlueMgr.h"
#include "WoW/Source/WowServices/Login.h"
#include "WoW/Source/WowServices/RealmConnection.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"

/*
 * Optional unattended login for protocol testing. Enabled with WRATHMODERN_AUTOLOGIN=1; the
 * account, password and realm are read from a user-only file outside the repository (by default
 * $HOME/.wrathmodern-autologin, or WRATHMODERN_AUTOLOGIN_FILE):
 *
 *   accountname
 *   password
 *   Lordaeron        (optional)
 *
 * The password is never logged. The timer runs on the client's event context, so every call is on
 * the thread the glue and network already use.
 */

namespace {

enum AutologinState {
    AUTOLOGIN_WAIT,
    AUTOLOGIN_WAIT_LOGIN,
    AUTOLOGIN_WAIT_REALM,
    AUTOLOGIN_DONE,
    AUTOLOGIN_FAILED,
};

AutologinState s_state = AUTOLOGIN_WAIT;
char s_account[128];
char s_password[256];
char s_realm[128] = "Lordaeron";
uint32_t s_started;
uint32_t s_lastLog;
bool s_realmConnectTried;
FILE* s_log;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* format, ...) {
    if (!s_log) {
        s_log = fopen("Logs/autologin.log", "a");

        if (!s_log) {
            return;
        }
    }

    fprintf(s_log, "%u ", OsGetAsyncTimeMs());
    va_list args;
    va_start(args, format);
    vfprintf(s_log, format, args);
    va_end(args);
    fputc('\n', s_log);
    fflush(s_log);
}

bool ReadCredentials() {
    const char* path = getenv("WRATHMODERN_AUTOLOGIN_FILE");
    char defaultPath[1024];

    if (!path || !path[0]) {
        const char* home = getenv("HOME");

        if (!home || !home[0]) {
            return false;
        }

        snprintf(defaultPath, sizeof(defaultPath), "%s/.wrathmodern-autologin", home);
        path = defaultPath;
    }

    FILE* file = fopen(path, "r");

    if (!file) {
        Log("no credentials file at %s", path);
        return false;
    }

    char lines[3][256] = {};
    int32_t count = 0;

    while (count < 3 && fgets(lines[count], sizeof(lines[count]), file)) {
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
        Log("credentials file needs an account and a password line");
        return false;
    }

    strncpy(s_account, lines[0], sizeof(s_account) - 1);
    strncpy(s_password, lines[1], sizeof(s_password) - 1);

    if (count >= 3 && lines[2][0]) {
        strncpy(s_realm, lines[2], sizeof(s_realm) - 1);
    }

    return true;
}

int32_t AutologinTick(const void* data, void* param) {
    (void)data;
    (void)param;

    if (s_state == AUTOLOGIN_DONE || s_state == AUTOLOGIN_FAILED) {
        return 0;
    }

    ClientConnection* connection = ClientServices::Connection();
    uint32_t now = OsGetAsyncTimeMs();

    switch (s_state) {
        case AUTOLOGIN_WAIT: {
            if (now - s_started < 2000 || !connection) {
                break;
            }

            // Wait until the glue is ready to accept a login (the scan bypass sets this on
            // macOS); calling ClientServices::Logon directly skips the glue state the version
            // proof and realm list run in, and the login stalls at CHECKINGVERSIONS.
            if (!CGlueMgr::m_scandllOkayToLogIn || !CGlueMgr::m_acceptedTOS || !CGlueMgr::m_acceptedEULA
                || CGlueMgr::m_idleState != IDLE_NONE) {
                break;
            }

            char password[256];
            strncpy(password, s_password, sizeof(password) - 1);
            password[sizeof(password) - 1] = '\0';
            memset(s_password, 0, sizeof(s_password));

            Log("logon for account %s", s_account);
            CGlueMgr::LoginServerLogin(s_account, password);
            s_state = AUTOLOGIN_WAIT_LOGIN;
            break;
        }

        case AUTOLOGIN_WAIT_LOGIN: {
            if (!ClientServices::s_loginObj || !ClientServices::s_loginObj->IsLoggedOn()) {
                break;
            }

            Log("logged on; waiting for the realm list (realm %s)", s_realm);
            s_state = AUTOLOGIN_WAIT_REALM;
            break;
        }

        case AUTOLOGIN_WAIT_REALM: {
            if (connection && connection->m_characterList.Count() > 0) {
                CHARACTER_INFO& character = connection->m_characterList.m_data[0];
                Log("character list: %u entries; logging in %s", connection->m_characterList.Count(),
                    character.m_name);

                connection->CharacterLogin(character.m_guid, 0);
                s_state = AUTOLOGIN_DONE;
                break;
            }

            if (now - s_lastLog > 5000) {
                s_lastLog = now;
                Log("waiting for characters: state=%d connected=%u realmValid=%d chars=%u",
                    connection ? static_cast<int32_t>(connection->m_netState) : -1,
                    connection ? connection->m_connected : 0,
                    ClientServices::m_selectRealmInfoValid ? 1 : 0,
                    connection ? connection->m_characterList.Count() : 0);
            }

            // If the realm connection has not been started after the realm list arrived, start
            // it once; the glue normally does this from RealmEnumCallback.
            if (connection && ClientServices::m_selectRealmInfoValid && !connection->m_connected
                && connection->m_netState == NS_INITIALIZED && !s_realmConnectTried
                && now - s_started > 15000) {
                s_realmConnectTried = true;
                ClientServices::ConnectToSelectedServer();
                Log("starting the realm connection");
            }

            if (now - s_started > 60000) {
                Log("no character list after 60 seconds; giving up");
                s_state = AUTOLOGIN_FAILED;
            }

            break;
        }

        default:
            break;
    }

    if (s_state != AUTOLOGIN_DONE && s_state != AUTOLOGIN_FAILED) {
        EventSetTimer(500, AutologinTick, nullptr);
    }

    if (s_state == AUTOLOGIN_DONE) {
        Log("character login sent; leaving the client running");
    }

    return 0;
}

}  // namespace

void AutologinInitialize() {
    if (!getenv("WRATHMODERN_AUTOLOGIN")) {
        return;
    }

    if (!ReadCredentials()) {
        return;
    }

    s_started = OsGetAsyncTimeMs();
    Log("armed for account %s, realm %s", s_account, s_realm);
    EventSetTimer(1000, AutologinTick, nullptr);
}
