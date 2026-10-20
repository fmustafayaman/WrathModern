#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <unistd.h>

#include "WoW/Source/Autologin.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/Glue/CGlueMgr.h"
#include "WoW/Source/Glue/CharacterSelection.h"
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
    AUTOLOGIN_WAIT_WORLD,
    AUTOLOGIN_MONITOR,
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

// The first login is rejected when the emulated module scan does not match the official client;
// the server then re-sends SMSG_CHAR_ENUM. A new character list after a login attempt therefore
// means "try again" (the second attempt is not challenged and enters the world).
uint32_t s_characterListEpoch;
uint32_t s_loginEpoch;
uint32_t s_loginAttempts;
uint32_t s_loginSentAt;
uint32_t s_worldVerifiedAt;
WOWGUID s_loginGuid;
bool s_worldVerified;

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
                Log("character list: %u entries; entering world as %s", connection->m_characterList.Count(),
                    character.m_name);

                // Go through the glue, exactly like the Enter World button: it draws the loading
                // screen, calls ClientInitializeGame (which installs the SMSG_LOGIN_VERIFY_WORLD
                // handler) and only then sends CMSG_PLAYER_LOGIN. Calling CharacterLogin directly
                // sends the login before the game handlers exist, so the 0236 is missed and the
                // client sits on the loading screen while the server streams the world.
                if (CCharacterSelection::s_characterList.Count() == 0) {
                    Log("character selection list is empty; waiting for the UI callback");
                    break;
                }

                s_worldVerified = false;
                s_worldVerifiedAt = 0;
                CGlueMgr::EnterWorld();
                s_loginGuid = character.m_guid;
                s_loginAttempts = 1;
                s_loginSentAt = now;
                s_loginEpoch = s_characterListEpoch;
                s_state = AUTOLOGIN_WAIT_WORLD;
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

        case AUTOLOGIN_WAIT_WORLD: {
            // SMSG_LOGIN_VERIFY_WORLD alone is not proof that the client left the glue: the
            // server can accept the player login and then send the character list back (the
            // first login after the module scan is rejected). In that case the verifier is
            // followed by a new SMSG_CHAR_ENUM, so wait for it and press Enter World again.
            if (s_worldVerified) {
                if (!s_worldVerifiedAt) {
                    s_worldVerifiedAt = now;
                }

                if (now - s_worldVerifiedAt > 8000) {
                    // Leave the client at the character screen when the server sends the list
                    // back, but keep a slow watchdog: a fresh Enter World after half an hour
                    // catches the session once the server-side ghost login has expired. The
                    // 30 s tick keeps the world loader's event loop quiet between attempts.
                    Log("world entry accepted after %u character login attempt(s); monitoring", s_loginAttempts);
                    s_state = AUTOLOGIN_MONITOR;
                    EventSetTimer(30000, AutologinTick, nullptr);
                    return 0;
                }

                break;
            }

            if (!connection || !connection->m_connected) {
                Log("realm connection lost before world entry");
                s_state = AUTOLOGIN_FAILED;
                break;
            }

            // While the glue is drawing the loading screen or suspended in the world, a login is
            // already in flight; calling EnterWorld again would reset that state. Wait for the
            // 0236 or for the server to send us back to character selection.
            bool glueBusy = CGlueMgr::m_idleState == IDLE_ENTER_WORLD || CGlueMgr::m_suspended;

            // Do not hammer Enter World on every SMSG_CHAR_ENUM the server sends back. The
            // server's session teardown needs time; the monitor retries after half an hour.
            (void)glueBusy;

            if (now - s_loginSentAt > 60000) {
                Log("world entry did not complete; giving up");
                s_state = AUTOLOGIN_FAILED;
            }

            break;
        }

        case AUTOLOGIN_MONITOR: {
            // The server can accept the player login and log the character out again a few
            // seconds later (the first login after the module scan is often rejected). The
            // client then returns to the character selection screen and requests the list
            // again; treat a new list as "press Enter World once more".
            if (!connection || !connection->m_connected) {
                Log("connection lost while monitoring; waiting for a restart");
                s_state = AUTOLOGIN_FAILED;
                break;
            }

            // The server answers the first world entry with the character list again a few
            // seconds later (the module-scan gate). Pressing Enter World again immediately is
            // rejected as well, and a tight retry loop only keeps the account hot. Leave the
            // client at the character screen and let a human click anytime; the automatic
            // retry below waits half an hour so the server side has settled.
            if (s_characterListEpoch != s_loginEpoch && now - s_loginSentAt > 1800000 && s_loginAttempts < 6) {
                Log("world entry after a long backoff; pressing Enter World again (attempt %u)",
                    s_loginAttempts + 1);
                s_worldVerified = false;
                s_worldVerifiedAt = 0;
                CGlueMgr::EnterWorld();
                s_loginAttempts++;
                s_loginSentAt = now;
                s_loginEpoch = s_characterListEpoch;
                s_state = AUTOLOGIN_WAIT_WORLD;
            }

            break;
        }

        default:
            break;
    }

    if (s_state != AUTOLOGIN_DONE && s_state != AUTOLOGIN_FAILED) {
        // The monitor only watches for a new character list; a slow tick is enough and keeps
        // the world loader's event loop quiet.
        EventSetTimer(s_state == AUTOLOGIN_MONITOR ? 30000 : 500, AutologinTick, nullptr);
    }

    // A process stuck at character selection (or with a world socket but no character) cannot
    // fix itself. Exit so the supervisor starts a fresh client with a fresh world handshake.
    if (s_state == AUTOLOGIN_FAILED) {
        Log("giving up this process; the supervisor will restart it");
        fflush(s_log);
        _exit(0);
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

void AutologinNotifyCharacterList() {
    s_characterListEpoch++;
}

void AutologinNotifyWorldVerified() {
    if (!s_started) {
        return;
    }

    if (!s_worldVerified) {
        Log("SMSG_LOGIN_VERIFY_WORLD received");
    }

    s_worldVerified = true;
    s_worldVerifiedAt = OsGetAsyncTimeMs();
}
