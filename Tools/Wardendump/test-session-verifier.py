#!/usr/bin/env python3
"""Unit-test log-audit rejection gates with synthetic log text; never contacts a server.

These fixtures exercise the verifier only. They are not evidence that the game stayed online,
and no synthetic packet or check result is ever sent to a client or server.
"""

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("verify_session", TOOLS / "verify-session.py")
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class SessionVerifierTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="session-verifier-unit-")
        self.run = Path(self.directory.name)
        self.logs = self.run / "Logs"
        self.logs.mkdir()
        executable = self.run / "not-an-actual-game"
        executable.write_bytes(b"unit-test log verifier only")
        self.meta = {"game_directory": str(self.run), "executable": str(executable),
                     "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
        (self.run / "run.json").write_text(json.dumps(self.meta))
        (self.run / "client.pid").write_text("12345678\n")
        self.health = [
            "%u pid=12345678 connected=1 net=5 player=0000000000000001 map=1 readyMs=%u "
            "frames=%u frameDelta=600 maxIdleGapMs=20 x=%u y=0 z=0\n"
            % (100000 + age, age, 600 + age * 60 // 1000, age // 10000)
            for age in range(10000, 1810000, 10000)
        ]
        self.packets = (
            "110001 SMSG 041b size=4: 00 00 00 00\n"
            "110002 CMSG size=4: 1b 04 00 00\n"
            "120001 SMSG 02e6 size=1: 00\n"
            "120002 CMSG size=4: e7 02 00 00\n"
            "1900001 SMSG 0096 size=1: 00\n"
        )
        self.write()

    def tearDown(self):
        self.directory.cleanup()

    def write(self):
        (self.logs / "session-health.log").write_text("".join(self.health))
        (self.logs / "realm-packets.log").write_text(self.packets)

    def result(self):
        self.write()
        return VERIFY.audit(self.run)

    def test_control_fixture_exercises_the_30_minute_window(self):
        result = self.result()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["continuous_world_seconds"], 1800)

    def test_short_window_cannot_pass(self):
        self.health = self.health[:-1]
        self.assertFalse(self.result()["passed"])

    def test_disconnection_or_player_change_cannot_pass(self):
        for old, new in (("connected=1", "connected=0"), ("player=0000000000000001", "player=0000000000000002")):
            original = self.health[30]
            self.health[30] = original.replace(old, new)
            with self.subTest(field=old):
                self.assertFalse(self.result()["passed"])
            self.health[30] = original

    def test_reset_ready_timer_cannot_pass(self):
        self.health[30] = self.health[30].replace("readyMs=310000", "readyMs=10000")
        self.assertFalse(self.result()["passed"])

    def test_missing_sample_or_render_progress_cannot_pass(self):
        original = self.health.copy()
        del self.health[30:33]
        self.assertFalse(self.result()["passed"])
        self.health = original
        self.health[30] = self.health[30].replace("frameDelta=600", "frameDelta=0")
        self.assertFalse(self.result()["passed"])

    def test_server_warning_cannot_pass_even_when_connected(self):
        warning = b"Unauthorized software or modifications to your game client have been detected."
        self.packets += "1900002 SMSG 0096 size=%u: %s\n" % (len(warning), warning.hex(" "))
        result = self.result()
        self.assertFalse(result["passed"])
        self.assertTrue(any("unauthorized-software" in issue for issue in result["issues"]))

    def test_reconnect_dropped_send_and_emulator_fault_cannot_pass(self):
        for name, line in (("net-events.log", "140000 DISCONNECTED this=0x1234 state=5\n"),
                           ("net-events.log", "140000 SEND_DROPPED state=2 opcode=01dc\n"),
                           ("buck-data.log", "140000 SMSG 041b: delivered handler failed\n"),
                           ("warden.log", "WardenModule::Process: execution budget exhausted\n"),
                           ("warden.log", "import kernel32!Unknown was called but has no behavior\n")):
            path = self.logs / name
            path.write_text(line)
            with self.subTest(line=line):
                self.assertFalse(self.result()["passed"])
            path.unlink()

    def test_unanswered_challenge_or_stale_server_traffic_cannot_pass(self):
        original = self.packets
        self.packets = original.replace("110002 CMSG size=4: 1b 04 00 00\n", "")
        self.assertFalse(self.result()["passed"])
        self.packets = original.replace("1900001", "1300001")
        self.assertFalse(self.result()["passed"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
