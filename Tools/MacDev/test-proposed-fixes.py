#!/usr/bin/env python3
"""Sanitizer regressions for the PDF fixes. Run from any directory on macOS.

Compiles the CURRENT source function bodies against small model fixtures and the real
Storm hash-table templates. Cursor association is stubbed: tests never detach the user's mouse.
This is not a substitute for the in-game/Metal-validation smoke test.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def definition(path, signature):
    text = (ROOT / path).read_text()
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


model = "Engine/Source/Model2/M2Shared.cpp"
window = "Engine/Source/Os/Mac/GameWindow.mm"
input_path = "Engine/Source/Os/Mac/Input.mm"
portrait = (ROOT / "WoW/Source/UIUtil/PortraitButton.cpp").read_text()
creation = re.search(r"portrait = (s_unitPortraits\.[^;]+);", portrait).group(0)
source = r'''
#import <AppKit/AppKit.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Engine/Source/Storm/H/stpl.h"

// Only the allocator is substituted; cache lookup/link/key logic is the production template.
void* SMemAlloc(size_t n, const char*, int32_t, uint32_t) { return calloc(1, n); }
int32_t SMemFree(void* p, const char*, int32_t, uint32_t) { free(p); return 1; }
void* SMemReAlloc(void* p, size_t n, const char*, int32_t, uint32_t) { return realloc(p, n); }
int32_t SErrDisplayError(uint32_t, const char*, int32_t, const char*, int32_t, uint32_t, uint32_t) {
    abort();
}

struct M2Batch {
    uint16_t m_shader = 0;
    uint16_t m_materialIndex = 0;
    uint16_t m_textureCoordComboIndex = 0;
    uint16_t m_textureCount = 0;
};
struct M2Material { uint16_t m_blendMode = 0; };
template<class T> struct Array {
    uint32_t m_count;
    std::vector<T> values;
    T& operator[](size_t i) { return values.at(i); }
};
struct Skin { Array<M2Batch> m_batches{1, std::vector<M2Batch>(1)}; };
struct Data {
    uint32_t m_flags = 8, m_numSkinProfiles = 1;
    M2Material m_materials[1];
    uint16_t m_textureCoordCombos[32] = {}, m_textureCombinerCombos[32] = {};
};
static const uint32_t s_skinProfileBoneCounts[] = {256, 64, 53, 21};
struct CM2Shared {
    Data* m_data;
    Skin* m_skinData;
    const char* m_fileName = "regression-fixture";
    void SubstituteSimpleShaders();
    uint32_t GetSkinProfileIndex(uint32_t maxBones);
};
'''
source += definition(model, "void CM2Shared::SubstituteSimpleShaders()") + "\n"
source += definition(model, "uint32_t CM2Shared::GetSkinProfileIndex(uint32_t maxBones)") + "\n"
source += r'''
static bool s_cursorDetached, s_gameCursorVisible = true;
static NSCursor* s_gameCursor;
static int refreshes, associations;
static bool associated = true;
void OsMacRefreshGameCursor() { ++refreshes; }
static void TestAssociate(bool value) { associated = value; ++associations; }
#define CGAssociateMouseAndMouseCursorPosition TestAssociate
'''
source += definition(input_path, "bool OsMacInputCursorDetached()") + "\n"
source += definition(input_path, "static void DetachCursor(bool detach)") + "\n"
source += definition(window, "static NSCursor* CurrentCursor()") + "\n"
source += r'''
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);
'''
source += definition("Engine/Source/Event/EvtSched.cpp", "class ScopedAutoreleasePool") + ";\n"
source += r'''
static int destroyed;
@interface PoolProbe : NSObject
@end
@implementation PoolProbe
- (void)dealloc { ++destroyed; [super dealloc]; }
@end
struct UNITPORTRAIT : TSHashObject<UNITPORTRAIT, HASHKEY_DWORD> {};
static TSHashTable<UNITPORTRAIT, HASHKEY_DWORD> s_unitPortraits;
static UNITPORTRAIT* Portrait(uint32_t displayId) {
    auto* portrait = s_unitPortraits.Ptr(displayId, HASHKEY_DWORD{displayId});
    if (!portrait) {
'''
source += creation + "\n    }\n    return portrait;\n}\n"
source += r'''
int main() {
    @autoreleasepool {
        Data data;
        Skin skin;
        CM2Shared model{&data, &skin};
        unsigned shaderCases = 0;
        // All 1/2-stage outputs remain unchanged; 3/4 stages equal the two-stage output.
        for (unsigned seed = 0; seed < 40; ++seed) {
            for (unsigned i = 0; i < 32; ++i) {
                data.m_textureCombinerCombos[i] = (seed + i * 3) % 8;
                data.m_textureCoordCombos[i] = (seed + i) % 5;
            }
            data.m_materials[0].m_blendMode = seed % 8;
            for (unsigned flags : {0u, 8u}) {
                data.m_flags = flags;
                for (unsigned count : {0u, 1u, 2u, 3u, 4u, 255u}) {
                    auto& b = skin.m_batches[0];
                    b = {}; b.m_shader = seed % 8; b.m_textureCount = count;
                    unsigned expected = 0;
                    if (flags & 8) {
                        unsigned stages = count < 2 ? count : 2;
                        for (unsigned j = 0; j < stages; ++j) {
                            unsigned op = data.m_textureCombinerCombos[b.m_shader + j];
                            if (!j && !data.m_materials[0].m_blendMode) op = 0;
                            if (data.m_textureCoordCombos[j] > 2) op |= 8;
                            expected |= op << (j ? 0 : 4);
                        }
                        if (stages && data.m_textureCoordCombos[stages - 1] == 1) expected |= 0x4000;
                    } else {
                        unsigned op = data.m_materials[0].m_blendMode != 0;
                        if (data.m_textureCoordCombos[0] > 2) op |= 8;
                        expected = op << 4;
                        if (data.m_textureCoordCombos[0] == 1) expected |= 0x4000;
                    }
                    model.SubstituteSimpleShaders();
                    assert(b.m_shader == expected && b.m_textureCount == count);
                    ++shaderCases;
                    b.m_shader = 0x8012;
                    model.SubstituteSimpleShaders();
                    assert(b.m_shader == 0x8012);
                }
            }
        }
        printf("PASS shader: %u cases + explicit-shader preservation (ASan/UBSan)\n", shaderCases);
        for (unsigned count = 1; count <= 8; ++count) {
            data.m_numSkinProfiles = count;
            for (unsigned budget : {256u, 64u, 53u, 21u, 10u}) {
                unsigned expected = 0xFFFFFFFF;
                for (unsigned i = 0; i < count; ++i) {
                    unsigned index = count <= 4 ? 4 - count + i : (i < 3 ? i : 3);
                    if (s_skinProfileBoneCounts[index] <= budget) { expected = i; break; }
                }
                assert(model.GetSkinProfileIndex(budget) == expected);
            }
        }
        data.m_numSkinProfiles = 0;
        assert(model.GetSkinProfileIndex(256) == 0xFFFFFFFF);
        puts("PASS skin profiles: 40 selections vs PDF rule + zero-profile rejection");
        for (unsigned id : {1u, 17u, 33u, 65535u}) {
            auto* first = Portrait(id);
            for (unsigned repeat = 0; repeat < 1000; ++repeat) assert(Portrait(id) == first);
        }
        puts("PASS portrait: 4000 repeated lookups reuse original nodes (real TSHashTable)");
        auto* visible = [NSCursor crosshairCursor]; s_gameCursor = visible;
        for (bool show : {false, true}) {
            for (bool detach : {false, true}) {
                s_gameCursorVisible = show; DetachCursor(detach);
                assert((CurrentCursor() == visible) == (show && !detach));
                assert(associated == !detach);
                int old = refreshes; DetachCursor(detach); assert(refreshes == old);
            }
        }
        DetachCursor(false); s_gameCursorVisible = true; s_gameCursor = nullptr;
        assert(CurrentCursor() == [NSCursor arrowCursor]);
        assert(refreshes == associations);
        puts("PASS cursor: visible/detached truth table, idempotence, reattach and arrow fallback");
        for (int frame = 0; frame < 100; ++frame) {
            { ScopedAutoreleasePool pool;
              for (int i = 0; i < 100; ++i) [[[PoolProbe alloc] init] autorelease]; }
            assert(destroyed == (frame + 1) * 100);
        }
        puts("PASS autorelease: 10000 objects destroyed at iteration boundaries");
    }
}
'''
# The production entry point must actually instantiate the tested RAII helper.
assert "ScopedAutoreleasePool pool;" in definition(
    "Engine/Source/Event/EvtSched.cpp", "int32_t SchedulerThreadProcProcess(")
assert not re.search(r"\[NSCursor (hide|unhide)\]", (ROOT / input_path).read_text())
with tempfile.TemporaryDirectory(prefix="wowre-pdf-tests-") as tmp:
    src, exe = Path(tmp) / "test.mm", Path(tmp) / "test"
    src.write_text(source)
    subprocess.run(["clang++", "-std=c++17", "-fno-objc-arc", "-g", "-O1",
                    "-D__stdcall=", "-Draw_name=name",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(ROOT), str(src), "-framework", "AppKit", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
