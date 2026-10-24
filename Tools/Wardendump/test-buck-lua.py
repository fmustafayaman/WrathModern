#!/usr/bin/env python3
"""Offline 0x4B9 regression tests against the decoder used by the live client.

Build BuckSmoke first, then run:
    python3 Tools/Wardendump/test-buck-lua.py
No server connection or credentials are needed.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parent
SMOKE = Path(os.environ.get("BUCK_SMOKE", str(TOOLS / "BuckSmoke")))


def encode(chunks, seed):
    """Independent fixture writer; the key also advances over each NUL byte."""
    wire = bytearray([seed])
    key = seed
    for chunk in chunks:
        for byte in chunk + b"\0":
            wire.append(byte ^ key)
            key = (key + byte + 1) & 0xFF
    return bytes(wire)


class BuckLuaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SMOKE.is_file():
            raise RuntimeError("Build the BuckSmoke target before running these tests")

    def decode(self, wire):
        with tempfile.TemporaryDirectory(prefix="buck-lua-") as directory:
            payload = Path(directory) / "payload.bin"
            payload.write_bytes(wire)
            return subprocess.run(
                [str(SMOKE), "--lua", str(payload)],
                capture_output=True, text=True, check=False, timeout=10,
            )

    def assert_chunks(self, chunks, seed):
        result = self.decode(encode(chunks, seed))
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = "scripts (%u):\n" % len(chunks)
        for index, chunk in enumerate(chunks, 1):
            expected += "script %u (%u bytes):\n%s\n" % (
                index, len(chunk), chunk.decode("ascii"),
            )
        self.assertEqual(result.stdout, expected)

    def test_single_chunk(self):
        self.assert_chunks([b"SetUsesToken(false)"], 0x56)

    def test_key_continues_across_chunks_and_wraps(self):
        chunks = [b"first=1", b"second='xyz'", b"third=3"]
        for seed in (0, 1, 0x56, 0xFF):
            with self.subTest(seed=seed):
                self.assert_chunks(chunks, seed)

    def test_empty_chunks(self):
        self.assert_chunks([b"", b"middle=2", b""], 0xFF)

    def test_empty_script(self):
        self.assert_chunks([b""], 0)

    def test_malformed_packets_do_not_return_partial_scripts(self):
        complete_then_truncated = encode([b"valid=1", b"incomplete=2"], 0x56)[:-1]
        for wire in (b"", b"\x56", b"\x56\x01", complete_then_truncated):
            with self.subTest(wire=wire):
                result = self.decode(wire)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertIn("unterminated chunk", result.stderr)

    def test_captured_five_chunk_packet(self):
        wire = (TOOLS / "captures" / "lua-4b9.bin").read_bytes()
        self.assertEqual(len(wire), 1808)
        result = self.decode(wire)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("scripts (5):\n"))
        self.assertEqual(
            re.findall(r"^script (\d+) \((\d+) bytes\):$", result.stdout, re.MULTILINE),
            [("1", "799"), ("2", "404"), ("3", "126"), ("4", "19"), ("5", "454")],
        )
        for script in ("WMEXP=2", "CharacterSelectLogo:SetTexture(a)",
                       "SetUsesToken(false)", "switchRealmWM:SetScript"):
            self.assertIn(script, result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
