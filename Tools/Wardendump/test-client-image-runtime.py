#!/usr/bin/env python3
"""Check shared PE startup-global derivation without logging in or executing a check.

Compiles the production header into a temporary harness. Expected bounds are independently
read from the actual client file; malformed PE tests are parser tests, not server answers.
"""

import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(os.environ.get("WARDEN_CLIENT_EXE", "/Volumes/Yaman SSD/Warmane/Wow.exe"))


class ClientImageRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="client-image-runtime-")
        cls.root = Path(cls.directory.name)
        cls.binary = cls.root / "runtime-test"
        source = cls.root / "runtime-test.cpp"
        source.write_text(r'''
#include "Engine/Source/Os/Mac/ClientImageRuntime.h"
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    uint32_t start = 0xFFFFFFFF, end = 0xFFFFFFFF;
    bool ok = ClientImageRuntime::TextBounds(data.data(), static_cast<uint32_t>(data.size()), start, end);
    printf("%u %08x %08x\n", ok ? 1 : 0, start, end);
    return 0;
}
''')
        subprocess.run(["c++", "-std=c++17", "-O2", "-I" + str(ROOT), str(source), "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True)
        cls.client = EXE.read_bytes()
        cls.nt = struct.unpack_from("<I", cls.client, 0x3C)[0]
        cls.sections = cls.nt + 0x18 + struct.unpack_from("<H", cls.client, cls.nt + 20)[0]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def bounds(self, data):
        path = self.root / "input.bin"
        path.write_bytes(data)
        result = subprocess.run([str(self.binary), str(path)], check=True, capture_output=True, text=True)
        ok, start, end = result.stdout.split()
        return bool(int(ok)), int(start, 16), int(end, 16)

    def test_actual_clean_client_matches_section_table(self):
        count = struct.unpack_from("<H", self.client, self.nt + 6)[0]
        base = struct.unpack_from("<I", self.client, self.nt + 0x34)[0]
        for index in range(count):
            offset = self.sections + index * 40
            if self.client[offset:offset + 8] == b".text\0\0\0":
                size, rva = struct.unpack_from("<II", self.client, offset + 8)
                self.assertEqual(self.bounds(self.client), (True, base + rva, base + rva + size))
                break
        else:
            self.fail("clean client has no .text section")

    def test_truncated_headers_and_section_table_are_rejected(self):
        for size in (0, 32, self.nt + 8, self.sections + 4):
            with self.subTest(size=size):
                self.assertEqual(self.bounds(self.client[:size]), (False, 0, 0))

    def test_invalid_pe_signature_and_optional_magic_are_rejected(self):
        for offset, replacement in ((self.nt, b"NOPE"), (self.nt + 0x18, b"\x0b\x02")):
            data = bytearray(self.client)
            data[offset:offset + len(replacement)] = replacement
            with self.subTest(offset=offset):
                self.assertEqual(self.bounds(data), (False, 0, 0))

    def test_section_name_comparison_is_exact_eight_bytes(self):
        data = bytearray(self.client)
        count = struct.unpack_from("<H", data, self.nt + 6)[0]
        for index in range(count):
            offset = self.sections + index * 40
            if data[offset:offset + 8] == b".text\0\0\0":
                data[offset + 5] = ord("X")
        self.assertEqual(self.bounds(data), (False, 0, 0))

    def test_overflow_and_empty_section_fail_without_seeding_globals(self):
        for size, rva in ((0, 0x1000), (0x100, 0xFFFFFFF0)):
            data = bytearray(self.client)
            count = struct.unpack_from("<H", data, self.nt + 6)[0]
            for index in range(count):
                offset = self.sections + index * 40
                if data[offset:offset + 8] == b".text\0\0\0":
                    struct.pack_into("<II", data, offset + 8, size, rva)
            with self.subTest(size=size, rva=rva):
                self.assertEqual(self.bounds(data), (False, 0, 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
