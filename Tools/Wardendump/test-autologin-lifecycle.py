#!/usr/bin/env python3
"""Run the real autologin observer/timer bodies against offline service doubles.

No credentials, network or game process are used. Extracting the production functions
keeps this focused regression tied to the actual driver, not a second state machine.
AUTOLOGIN_SOURCE can select a pre-fix copy to demonstrate the failing regression.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("AUTOLOGIN_SOURCE", str(ROOT / "WoW/Source/Autologin.cpp")))

DOUBLES = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

struct WOWGUID { uint64_t m_guid = 1; };
struct C3Vector {
    float x, y, z;
    C3Vector(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct CGPlayer_C { C3Vector GetPosition() { return C3Vector(1, 2, 3); } };
struct CHARACTER_INFO { char m_name[32] = "OfflineTest"; WOWGUID m_guid; };
template<class T> struct List {
    std::vector<T> m_data = { T() };
    uint32_t Count() const { return static_cast<uint32_t>(m_data.size()); }
};
struct ClientConnection {
    bool m_connected = true;
    int32_t m_netState = 5;
    List<CHARACTER_INFO> m_characterList;
};
struct Login { bool IsLoggedOn() { return true; } };
static ClientConnection connection;
static Login login;
struct ClientServices {
    static inline Login* s_loginObj = &login;
    static inline bool m_selectRealmInfoValid = true;
    static ClientConnection* Connection() { return &connection; }
    static void ConnectToSelectedServer() { assert(false && "unexpected connect"); }
};
constexpr int32_t IDLE_NONE = 0, IDLE_ENTER_WORLD = 1, NS_INITIALIZED = 1;
static unsigned enters = 0, timers = 0;
struct CGlueMgr {
    static inline bool m_scandllOkayToLogIn = true, m_acceptedTOS = true, m_acceptedEULA = true;
    static inline bool m_suspended = true;
    static inline int32_t m_idleState = IDLE_NONE;
    static void EnterWorld() { enters++; }
    static void LoginServerLogin(const char*, char*) { assert(false && "unexpected logon"); }
};
struct CCharacterSelection { static inline List<CHARACTER_INFO> s_characterList; };
struct Gx { uint32_t m_frameCount = 1; };
static Gx gx;
static Gx* g_theGxDevicePtr = &gx;
static CGPlayer_C player;
static CGPlayer_C* active = &player;
static uint32_t now = 1000;
static uint32_t OsGetAsyncTimeMs() { return now; }
static CGPlayer_C* ClntObjMgrGetActivePlayerObj() { return active; }
static WOWGUID ClntObjMgrGetActivePlayer() { return WOWGUID(); }
static uint32_t ClntObjMgrGetMapID() { return active ? 1 : 0; }
static void EventSetTimer(uint32_t, int32_t (*)(const void*, void*), void*) { timers++; }
struct ProcessExit {};
[[noreturn]] static void TestExit(int32_t) { throw ProcessExit(); }
#define _exit TestExit
"""

SCENARIOS = r"""
static void Begin() {
    s_state = AUTOLOGIN_WAIT_WORLD;
    s_started = 100;
    s_loginSentAt = 200;
    s_loginAttempts = 1;
    s_characterListEpoch = 1;
    s_loginEpoch = 1;
    s_log = tmpfile();
    assert(s_log);
}
static void WorldReady() {
    assert(ObserveNativeWorld(nullptr, nullptr) == 1);
    assert(s_nativeWorldSince == now);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    Begin();
    std::string test(argv[1]);
    if (test == "native-ready-ends-driver") {
        WorldReady();
        assert(s_state == AUTOLOGIN_DONE);
        assert(AutologinTick(nullptr, nullptr) == 0);
        assert(timers == 0 && enters == 0);
    } else if (test == "no-reentry-after-normal-logout") {
        WorldReady();
        now += 1800001;
        active = nullptr;
        CGlueMgr::m_suspended = false;
        s_characterListEpoch++;
        assert(ObserveNativeWorld(nullptr, nullptr) == 1);
        assert(s_nativeWorldSince == 0);
        AutologinTick(nullptr, nullptr);
        assert(enters == 0 && timers == 0 && s_state == AUTOLOGIN_DONE);
    } else if (test == "no-exit-after-later-disconnect") {
        WorldReady();
        now += 1800001;
        active = nullptr;
        connection.m_connected = false;
        assert(ObserveNativeWorld(nullptr, nullptr) == 1);
        AutologinTick(nullptr, nullptr); // Must not kill an otherwise usable UI process.
        assert(s_state == AUTOLOGIN_DONE && timers == 0 && enters == 0);
    } else if (test == "observer-continues-across-manual-reentry") {
        WorldReady();
        now += 20000;
        active = nullptr;
        assert(ObserveNativeWorld(nullptr, nullptr) == 1);
        assert(s_nativeWorldSince == 0);
        now += 20000;
        active = &player;
        gx.m_frameCount += 1200;
        assert(ObserveNativeWorld(nullptr, nullptr) == 1);
        assert(s_nativeWorldSince == now && s_healthFrames == gx.m_frameCount);
        assert(s_state == AUTOLOGIN_DONE && enters == 0 && timers == 0);
    } else if (test == "missing-native-readiness-does-not-end-driver") {
        active = nullptr;
        ObserveNativeWorld(nullptr, nullptr);
        assert(s_state == AUTOLOGIN_WAIT_WORLD && s_nativeWorldSince == 0);
        active = &player;
        connection.m_connected = false;
        ObserveNativeWorld(nullptr, nullptr);
        assert(s_state == AUTOLOGIN_WAIT_WORLD && s_nativeWorldSince == 0);
        connection.m_connected = true;
        gx.m_frameCount = 0;
        ObserveNativeWorld(nullptr, nullptr);
        assert(s_state == AUTOLOGIN_WAIT_WORLD && s_nativeWorldSince == 0);
    } else if (test == "health-only-mode-does-not-start-authentication") {
        s_started = 0;
        s_state = AUTOLOGIN_WAIT;
        WorldReady();
        assert(s_state == AUTOLOGIN_WAIT && enters == 0 && timers == 0);
    } else if (test == "pre-entry-failure-remains-failure") {
        connection.m_connected = false;
        bool exited = false;
        try { AutologinTick(nullptr, nullptr); } catch (const ProcessExit&) { exited = true; }
        assert(exited && s_state == AUTOLOGIN_FAILED && enters == 0);
    } else if (test == "pre-entry-bounded-retry-remains-available") {
        s_state = AUTOLOGIN_MONITOR;
        now = 600000;
        s_characterListEpoch++;
        AutologinTick(nullptr, nullptr);
        assert(enters == 1 && timers == 1 && s_state == AUTOLOGIN_WAIT_WORLD);
        assert(s_loginAttempts == 2 && s_loginEpoch == s_characterListEpoch);
    } else {
        assert(false && "unknown offline scenario");
    }
    fclose(s_log);
    puts("PASS");
}
"""


class AutologinLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="autologin-lifecycle-")
        cls.work = Path(cls.directory.name)
        (cls.work / "Logs").mkdir()
        source = SOURCE.read_text()
        state = source.split("namespace {", 1)[1].split("bool ReadCredentials()", 1)[0]
        tick = source.split("int32_t AutologinTick(", 1)[1].split("}  // namespace", 1)[0]
        code = DOUBLES + state + "int32_t AutologinTick(" + tick + SCENARIOS
        cpp = cls.work / "test.cpp"
        cpp.write_text(code)
        cls.binary = cls.work / "test"
        subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer", str(cpp), "-o", str(cls.binary)],
                       capture_output=True, text=True, check=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def scenario(self, name):
        result = subprocess.run([str(self.binary), name], cwd=self.work,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "PASS")

    def test_native_ready_ends_authentication_driver(self):
        self.scenario("native-ready-ends-driver")

    def test_normal_logout_does_not_trigger_an_automatic_reentry(self):
        self.scenario("no-reentry-after-normal-logout")

    def test_later_disconnect_does_not_exit_the_application(self):
        self.scenario("no-exit-after-later-disconnect")

    def test_native_observer_continues_through_manual_reentry(self):
        self.scenario("observer-continues-across-manual-reentry")

    def test_native_player_connection_and_frames_are_all_required(self):
        self.scenario("missing-native-readiness-does-not-end-driver")

    def test_health_only_mode_does_not_drive_authentication(self):
        self.scenario("health-only-mode-does-not-start-authentication")

    def test_failed_initial_entry_is_not_relabeled_as_success(self):
        self.scenario("pre-entry-failure-remains-failure")

    def test_retry_before_any_native_world_entry_is_still_bounded(self):
        self.scenario("pre-entry-bounded-retry-remains-available")


if __name__ == "__main__":
    unittest.main(verbosity=2)
