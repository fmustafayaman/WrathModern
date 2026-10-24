#!/usr/bin/env python3
"""Offline execution/ABI regressions for the real delivered Warmane framework.

Requires BuckSmoke and the clean game files (WARDEN_CLIENT_EXE overrides the path).
No login or network connection is made. Fixtures are delivered code, not canned replies.
"""

import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
CAPTURES = TOOLS / "captures"
SMOKE = Path(os.environ.get("BUCK_SMOKE", str(TOOLS / "BuckSmoke")))
EXE = Path(os.environ.get("WARDEN_CLIENT_EXE", "/Volumes/Yaman SSD/Warmane/Wow.exe"))
INSTALLS = [
    ("blob-2037-seg0-240.bin", "blob-2037-seg1-1789.bin"),
    ("blob-338-seg0-197.bin", "blob-338-seg1-133.bin"),
    ("blob-893-seg0-299.bin", "blob-893-seg1-350.bin", "blob-893-seg2-232.bin"),
    ("blob-502-seg0-178.bin", "blob-502-seg1-316.bin"),
]


def crypt(data, key):
    result = bytearray(data)
    for offset in range(0, len(result) - 3, 4):
        value = struct.unpack_from("<I", result, offset)[0] ^ key
        struct.pack_into("<I", result, offset, value)
        key = ((key >> 1) | (key << 31)) & 0xFFFFFFFF
    return bytes(result)


def packet(opcode, payload):
    return struct.pack("<H", opcode) + payload


def install_packets():
    result = [packet(0x2E9, (CAPTURES / "framework-2e9.bin").read_bytes())]
    for segments in INSTALLS:
        blob = b""
        for name in segments:
            code = (CAPTURES / name).read_bytes()
            blob += struct.pack("<I", len(code)) + code
        result.append(packet(0x4B8, blob))
    return result


def replies(output):
    return [bytes.fromhex(value) for value in re.findall(r"^sent \(\d+\): (.*)$", output, re.MULTILINE)]


def states(output):
    return [tuple(int(value, 16) for value in line.split())
            for line in re.findall(r"^framework state: (.*)$", output, re.MULTILINE)]


class FrameworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SMOKE.is_file() or not EXE.is_file():
            raise RuntimeError("Build BuckSmoke and set WARDEN_CLIENT_EXE to the clean Wow.exe")

    def run_stream(self, packets, actions=()):
        with tempfile.TemporaryDirectory(prefix="buck-framework-") as directory:
            files = []
            for index, payload in enumerate(packets):
                path = Path(directory) / ("%02d.packet" % index)
                path.write_bytes(payload)
                files.append(str(path))
            return subprocess.run([str(SMOKE), "--stream", str(EXE), *files, *actions],
                                  capture_output=True, text=True, check=False, timeout=90)

    def successful(self, packets, actions=()):
        result = self.run_stream(packets, actions)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_bootstrap_and_installers_choose_their_own_state_and_requests(self):
        output = self.successful(install_packets())
        sent = replies(output)
        state = states(output)[-1]
        self.assertEqual(len(sent), 3)
        self.assertEqual(sent[:2], [struct.pack("<II", 0x41D, 0), struct.pack("<II", 0x41C, 0)])
        self.assertEqual(sent[2], struct.pack("<II", 0x41B, state[1]))
        self.assertTrue(state[0])  # Actual FrameXML patch-list allocation.
        handlers = (state[1], state[3], state[7], state[8], state[9])
        self.assertEqual(len(set(handlers)), 5)
        self.assertTrue(all(address and address % 0x10000 == 0 for address in handlers))

    def test_native_content_and_real_framexml_hook(self):
        packets = install_packets() + [
            packet(0x41D, (CAPTURES / "payload-041d-live.bin").read_bytes()),
            packet(0x41C, (CAPTURES / "payload-041c-live.bin").read_bytes()),
        ]
        output = self.successful(packets, ["--initialize-game",
                                          "--patch-script=Interface\\FrameXML\\WorldStateFrame.lua"])
        state = states(output)[-1]
        self.assertEqual((state[2], state[4], state[6]), (3, 0, 200))
        self.assertTrue(state[5])  # String pool produced by the delivered parser, not an override.
        self.assertEqual(len(re.findall("applied delivered content to native database", output)), 8)
        self.assertIn("native content: map 37 directory 'PVPZone02', spell 300057", output)
        self.assertIn("WorldStateFrame.lua': changed=1", output)

    def test_reinstallation_reuses_token_and_reports_actual_versions(self):
        with tempfile.TemporaryDirectory(prefix="buck-reinstall-") as directory:
            # Actions must remain between packet groups, not be reordered after all messages.
            packets = install_packets() + [
                packet(0x41D, (CAPTURES / "payload-041d-live.bin").read_bytes()),
                packet(0x41C, (CAPTURES / "payload-041c-live.bin").read_bytes()),
            ]
            paths = []
            for index, data in enumerate(packets + install_packets()):
                path = Path(directory) / ("%02d.packet" % index)
                path.write_bytes(data)
                paths.append(str(path))
            command = [str(SMOKE), "--stream", str(EXE), *paths[:len(packets)],
                       "--initialize-game", *paths[len(packets):]]
            result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        sent = replies(result.stdout)
        self.assertEqual(len(sent), 6)
        self.assertEqual(sent[2], sent[5])
        self.assertEqual(sent[3:5], [struct.pack("<II", 0x41D, 200), struct.pack("<II", 0x41C, 3)])

    def test_same_machine_challenges_and_real_disk_capacity(self):
        setup = install_packets()
        token = states(self.successful(setup))[-1][1]
        first = crypt((CAPTURES / "client-40.bin").read_bytes(), 0x04D30000)
        nonce1 = struct.unpack_from("<I", first)[0]
        second = crypt((CAPTURES / "client-660.bin").read_bytes(), nonce1)
        nonce2 = struct.unpack_from("<I", second)[0]
        output = self.successful(setup + [packet(0x41B, crypt(first, token)),
                                          packet(0x41B, crypt(second, nonce1))])
        sent = replies(output)
        self.assertEqual([len(data) for data in sent], [8, 8, 8, 12, 112])
        decoded = sent[-1][:4] + crypt(sent[-1][4:], nonce2)
        self.assertEqual(struct.unpack_from("<II", decoded), (0x41B, 0xD0BE63D1))
        position = 8 + 8 + 4  # Opcode/magic, physical memory, processor count.
        for _ in range(4):
            position = decoded.index(b"\0", position) + 1
        position += 4  # InstallDate.
        position = decoded.index(b"\0", position) + 1  # CPUID brand.
        total = struct.unpack_from("<Q", decoded, position)[0]
        filesystem = os.statvfs(EXE)
        self.assertEqual(total, filesystem.f_blocks * (filesystem.f_frsize or filesystem.f_bsize))
        self.assertIn("GetDiskFreeSpaceExA: total=%u" % total, output)

    def test_module_scan_reports_the_mapped_guest_image_not_a_wine_fixture(self):
        setup = install_packets()
        token = states(self.successful(setup))[-1][1]
        encrypted = (CAPTURES / "challenge-192.bin").read_bytes()
        rotated_key = struct.unpack_from("<I", encrypted, 4)[0] ^ 184
        key = ((rotated_key << 1) | (rotated_key >> 31)) & 0xFFFFFFFF
        plain = crypt(encrypted, key)
        self.assertEqual(plain[8:11], b"\x55\x89\xe5")
        nonce = struct.unpack_from("<I", plain)[0]
        output = self.successful(setup + [packet(0x41B, crypt(plain, token))])
        reply = replies(output)[-1]
        decoded = reply[:4] + crypt(reply[4:], nonce)
        self.assertEqual(struct.unpack_from("<IIII", decoded),
                         (0x41B, 0xD0BE63D1, 0x00400000, 0x009FD000))
        end = decoded.index(b"\0", 16)
        expected = ("Z:" + str(EXE).replace("/", "\\")).encode()
        self.assertEqual(decoded[16:end], expected)
        self.assertFalse(any(decoded[end + 1:]))
        self.assertNotIn(b"windows", decoded.lower())

    def test_truncated_install_is_rejected_before_executing_code(self):
        packets = install_packets()
        result = self.run_stream([packets[0], packets[1][:-1]])
        self.assertEqual(result.returncode, 1)
        self.assertIn("truncated/empty 0x4B8 code segment", result.stdout)
        self.assertNotIn("sent (8):", result.stdout)

    def test_non_framework_packet_is_not_accepted_as_a_bootstrap(self):
        result = self.run_stream([packet(0x2E9, struct.pack("<II", 0, 1) + bytes(16))])
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("framework installed", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
