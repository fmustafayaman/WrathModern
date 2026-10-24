#!/usr/bin/env python3
"""Test the current macOS near-memory implementation without launching the client.

    python3 Tools/MacDev/test-near-memory.py --sanitize

The native test uses real Mach VM and malloc. A second build simulates malloc's
reserved VM cage and successful VM allocations, with optional ASan/UBSan. It
checks the complete production source, not a copied allocator implementation.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--source", type=Path,
                    default=ROOT / "Engine/Source/Os/Mac/OsMemory.cpp",
                    help="alternate allocator source, e.g. a saved pre-fix version")
args = parser.parse_args()

common = ["clang++", "-std=c++17", "-O1", "-g", "-pthread",
          "-DWOWDECOMP_ARCH_64=1", "-I", str(ROOT),
          str(ROOT / "Tools/MacDev/test-near-memory.cpp"), str(args.source)]
with tempfile.TemporaryDirectory(prefix="wowre-near-memory-") as directory:
    tmp = Path(directory)
    native = tmp / "native"
    subprocess.run([*common, "-o", str(native)], check=True)
    subprocess.run([str(native)], check=True)

    simulated = tmp / "simulated"
    flags = ["-DWRATHMODERN_TEST_NEAR_VM=1",
             "-Dmach_vm_region=TestNearVmRegion",
             "-Dmach_vm_allocate=TestNearVmAllocate",
             "-Dmach_vm_deallocate=TestNearVmDeallocate",
             "-Dmach_port_deallocate=TestNearPortDeallocate"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run([*common, *flags, "-o", str(simulated)], check=True)
    subprocess.run([str(simulated)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
print("PASS near-memory regression suite" + (" (ASan/UBSan)" if args.sanitize else ""))
