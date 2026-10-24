#!/usr/bin/env python3
"""Offline tests of persistent observation/archiving only; never touches a game process."""

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("watch_session", Path(__file__).with_name("watch-session.py"))
WATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WATCH)


class SessionWatcherTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="session-watcher-test-")
        self.root = Path(self.directory.name)
        self.run = self.root / "private-run"
        self.run.mkdir()
        self.game = self.root / "offline-game-logs"
        (self.game / "Logs").mkdir(parents=True)
        (self.run / "before").mkdir()
        # A previous successful window must survive every later observer decision.
        (self.run / "Logs").mkdir()
        (self.run / "Logs/frozen-proof").write_bytes(b"unchanged evidence")
        self.meta = {"game_directory": str(self.game), "start_wall_time": time.time() - 4000}
        (self.run / "run.json").write_text(json.dumps(self.meta))
        (self.run / "client.pid").write_text("12345\n")
        self.good = {"passed": True, "issues": [], "continuous_world_seconds": 1900,
                     "health_samples": 190}
        self.bad = {"passed": False, "issues": ["connection/player changed at 2000000"],
                    "continuous_world_seconds": 1900, "health_samples": 190}

    def tearDown(self):
        self.directory.cleanup()

    def test_short_run_is_measuring_but_never_relabelled_as_passed(self):
        result = {"passed": False, "issues": ["world duration 10 seconds is below 1800"]}
        self.assertEqual(WATCH.classify(result, 1000), "measuring")
        self.assertFalse(result["passed"])

    def test_warmup_does_not_hide_disconnects_or_render_stalls(self):
        result = {"passed": False, "issues": ["no native world-ready samples"]}
        self.assertEqual(WATCH.classify(result, 10), "warming-up")
        self.assertEqual(WATCH.classify(result, 121), "problem")
        for issue in ("disconnect, reconnect or dropped sends occurred during the run",
                      "main event-loop stall exceeded 2 seconds at 1000",
                      "emulator/bridge failure found in the live logs"):
            result["issues"] = [issue]
            self.assertEqual(WATCH.classify(result, 10), "problem")
        result = {"passed": False, "health_samples": 2,
                  "issues": ["render rate fell below 15 FPS over a health interval"]}
        self.assertEqual(WATCH.classify(result, 10), "problem")

    def test_failure_keeps_observing_without_repeated_identical_archives(self):
        state = {}
        with patch.object(WATCH.VERIFY, "audit", return_value=self.bad):
            first = WATCH.observe_once(self.run, self.meta, state)
            second = WATCH.observe_once(self.run, self.meta, state)
        self.assertEqual(first["status"], "problem")
        self.assertEqual(second["status"], "problem")
        self.assertIn("archive", first)
        self.assertNotIn("archive", second)
        self.assertEqual(len(list((self.run / "events").iterdir())), 1)
        self.assertEqual(len((self.run / "progress.jsonl").read_text().splitlines()), 2)
        self.assertFalse(json.loads((self.run / "latest-audit.json").read_text())["passed"])
        self.assertEqual((self.run / "Logs/frozen-proof").read_bytes(), b"unchanged evidence")

    def test_new_problem_is_archived_separately_and_timestamp_noise_is_not(self):
        first = WATCH.problem_signature(self.bad)
        later = dict(self.bad, issues=["connection/player changed at 3000000"])
        self.assertEqual(first, WATCH.problem_signature(later))
        state = {}
        with patch.object(WATCH.VERIFY, "audit", return_value=self.bad):
            WATCH.observe_once(self.run, self.meta, state)
        later["issues"].append("emulator/bridge failure found in the live logs")
        with patch.object(WATCH.VERIFY, "audit", return_value=later):
            WATCH.observe_once(self.run, self.meta, state)
        self.assertEqual(len(list((self.run / "events").iterdir())), 2)

    def test_checkpoint_requires_both_live_and_frozen_audits(self):
        state = {}
        with patch.object(WATCH.VERIFY, "audit", side_effect=[self.good, self.bad]):
            first = WATCH.observe_once(self.run, self.meta, state)
        self.assertFalse(first["snapshot_passed"])
        self.assertNotIn("last_checkpoint_seconds", state)
        with patch.object(WATCH.VERIFY, "audit", return_value=self.good):
            second = WATCH.observe_once(self.run, self.meta, state)
        self.assertTrue(second["snapshot_passed"])
        self.assertEqual(state["last_checkpoint_seconds"], 1900)
        self.assertNotEqual(first["archive"], second["archive"])
        self.assertEqual((self.run / "Logs/frozen-proof").read_bytes(), b"unchanged evidence")

    def test_log_prefix_subtraction_respects_truncation_and_hashes_all_evidence(self):
        (self.run / "before/kept.log").write_bytes(b"old\n")
        (self.run / "before/truncated.log").write_bytes(b"previous longer bytes\n")
        (self.game / "Logs/kept.log").write_bytes(b"old\nnew\n")
        (self.game / "Logs/truncated.log").write_bytes(b"fresh\n")
        (self.game / "Logs/warden-session-key.bin").write_bytes(b"synthetic unit-test key")
        out = self.run / "checkpoints/test"
        with patch.object(WATCH.VERIFY, "audit", return_value=self.good):
            WATCH.snapshot(self.run, out, self.meta, self.good)
        self.assertEqual((out / "Logs/kept.log").read_bytes(), b"new\n")
        self.assertEqual((out / "Logs/truncated.log").read_bytes(), b"fresh\n")
        manifest = json.loads((out / "sha256.json").read_text())
        self.assertIn("live-audit.json", manifest)
        self.assertIn("Logs/warden-session-key.bin", manifest)
        self.assertTrue(all(hashlib.sha256((out / name).read_bytes()).hexdigest() == digest
                            for name, digest in manifest.items()))
        with self.assertRaises(FileExistsError):
            WATCH.snapshot(self.run, out, self.meta)

    def test_snapshot_error_is_explicit_and_retried_without_stopping_observation(self):
        state = {}
        with patch.object(WATCH.VERIFY, "audit", return_value=self.bad), \
                patch.object(WATCH, "snapshot", side_effect=OSError("test disk full")):
            result = WATCH.observe_once(self.run, self.meta, state)
        self.assertEqual(result["status"], "problem")
        self.assertIn("test disk full", result["archive_error"])
        self.assertNotEqual(state.get("last_problem_signature"), WATCH.problem_signature(self.bad))
        with patch.object(WATCH.VERIFY, "audit", return_value=self.bad):
            next_result = WATCH.observe_once(self.run, self.meta, state)
        self.assertIn("archive", next_result)
        self.assertEqual(next_result["status"], "problem")


if __name__ == "__main__":
    unittest.main(verbosity=2)
