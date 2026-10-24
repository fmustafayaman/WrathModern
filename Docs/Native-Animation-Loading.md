# Native macOS external-animation allocation

## Observed failure

The Epoch client accepted `/sit` and `/dance`, selected animations 96/97/98/69,
and opened the corresponding `Character\Dwarf\Male\DwarfMale*.anim` assets.
However, `CM2Shared::LoadLowPrioritySequence` returned null because
`OsAllocNear(modelData, fileSize)` returned null. The model retained its standing
sequence and had zero loaded external-animation buffers. This was captured
while the user reproduced the problem, not inferred from an idle snapshot.

The problem is therefore not established to be missing MPQs, a bad sit opcode,
or an animation-choice problem. A user also observed it with other Epoch races.
The working WowRe installation is intentionally left unchanged for this fix.
The underlying allocator is generic macOS code; this does not establish that
other installations or datasets can never encounter the same failure.

## Cause and correction

M2 uses signed 32-bit relative offsets and requires nearby animation/vertex
buffers. The original macOS allocator searched only for unoccupied Mach VM
regions within 1.5 GiB of the model. On this machine, malloc reserves large VM
ranges around model allocations: Mach reports these as occupied even when the
heap can supply nearby storage. A standalone 1.6 MiB model-sized malloc
allocation reproduced the failure without launching a game or using a network.

`Engine/Source/Os/Mac/OsMemory.cpp` retains the original VM search, then tries a
zero-filled heap allocation. The entire returned block must still fit the same
near-address window; an out-of-range candidate is freed and rejected. The
allocation registry distinguishes VM and heap ownership so release uses the
correct allocator. Null/zero/overflowing requests are rejected, and VM region
query send rights are released. The fallback preserves the >=16-byte alignment
required by the M2 buffers; it does not require or execute a Windows DLL.

No game animation priorities, stand-state prediction, game assets, relay
credentials, server packets, or account state are changed by this fix.

## Regression tests

```sh
cd ~/Projects/WrathModern-Epoch
python3 Tools/MacDev/test-near-memory.py --sanitize
```

The runner compiles the complete production allocator twice:

- Native Mach/malloc: observed animation sizes, zero fill/alignment/range checks,
  invalid sizes, ownership, 400 concurrent allocation/free pairs, and real
  model-sized allocation. **415 allocations passed.**
- Simulated reserved VM range and successful VM path: heap/VM release dispatch,
  exact VM release size, 400 concurrent pairs, and all **824** returned region
  send rights released. **408 allocations passed with ASan/UBSan.**
  LeakSanitizer is not supported by this platform and is disabled explicitly.

The saved pre-fix production source fails the positive-allocation regression;
the modified source passes. This tests the native allocation failure and the
correction, but does not replace an in-game test after installing the new build.

## Remaining validation / separate map issue

The corrected ARM64 Epoch bundle has been rebuilt, installed, and code-signature
verified at `/Volumes/Yaman SSD/Epoch/WrathModern-Epoch.app`. The original WowRe
executable and both worktrees' pre-existing modifications are unchanged. A prior
Epoch bundle is retained under `Epoch/.wrathmodern-backups/` with the prefix
`WrathModern-Epoch-before-near-memory-20261023-233234.app`.

Post-install visual validation is pending: verify `/sit`, `/dance`, food/drink
loops, standing up and movement cancellation in Epoch. Check more than one race
if available.

The separate world-map report was resolved by the user: the server does not
provide the expected quest POI data/feature, and enabling `questPOI` made the
party member appear. A scoped native response for three accepted quests had
confirmed zero location records. No map code change, Questie-patch transfer or
rebuild was performed for that report. This allocation fix is not a map fix.
