// Translates every permutation of the .bls shader files given on the command line to Metal
// Shading Language and compiles the result with the system Metal compiler. Reports translation
// and compile failures; with -dump <dir> writes each failing source there.
//
//   shadercompile [-dump <dir>] <file.bls>...

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../../Engine/Source/Gx/CGxDeviceMetal/D3D9/ShaderTranslator.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace D3D9Metal;

static bool ReadFile(const char* path, std::vector<uint8_t>& data) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    data.resize(size);
    bool ok = fread(data.data(), 1, size, file) == static_cast<size_t>(size);
    fclose(file);
    return ok;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.preserveInvariance = YES;
        options.mathMode = MTLMathModeFast;

        const char* dumpDir = nullptr;
        int first = 1;
        if (argc > 2 && !strcmp(argv[1], "-dump")) {
            dumpDir = argv[2];
            first = 3;
        }

        std::set<std::string> compiled;
        uint32_t programs = 0, translateFailures = 0, compileFailures = 0, unique = 0, dumped = 0;

        for (int arg = first; arg < argc; arg++) {
            std::vector<uint8_t> data;
            if (!ReadFile(argv[arg], data) || data.size() < 12) continue;

            size_t offset = 12;
            uint32_t permutation = 0;
            while (offset + 16 <= data.size()) {
                uint32_t codeSize;
                memcpy(&codeSize, data.data() + offset + 12, 4);
                offset += 16;
                if (offset + codeSize > data.size()) break;

                if (codeSize) {
                    std::vector<uint32_t> tokens(codeSize / 4);
                    memcpy(tokens.data(), data.data() + offset, tokens.size() * 4);

                    ShaderProgram program;
                    std::string error;
                    std::vector<std::string> sources;

                    if (!ParseShader(tokens.data(), tokens.size(), program, error)) {
                        fprintf(stderr, "%s[%u]: parse: %s\n", argv[arg], permutation, error.c_str());
                        translateFailures++;
                    } else {
                        ShaderInfo info;
                        AnalyzeShader(program, info, error);
                        programs++;

                        if (program.m_stage == STAGE_VERTEX) {
                            VertexInputSignature inputs;
                            for (uint32_t i = 0; i < ATTRIB_COUNT; i++) {
                                if (info.m_attributeMask & (1u << i)) {
                                    inputs.m_class[i] = i == ATTRIB_BLENDINDICES ? ATTRIBCLASS_UINT : ATTRIBCLASS_FLOAT;
                                }
                            }
                            std::string source;
                            if (TranslateVertexShader(program, inputs, source, error)) sources.push_back(source);
                            else { fprintf(stderr, "%s[%u]: %s\n", argv[arg], permutation, error.c_str()); translateFailures++; }
                        } else {
                            // The declared kinds, plus every sampler as a depth texture where 2D
                            PixelShaderVariant variant;
                            variant.m_samplerKinds = info.m_declaredKinds;
                            PixelShaderVariant depth = variant;
                            for (uint32_t i = 0; i < 16; i++) {
                                if ((info.m_samplerMask & (1u << i)) && depth.Kind(i) == SAMPLERKIND_2D) {
                                    depth.m_samplerKinds |= SAMPLERKIND_DEPTH << (i * 2);
                                }
                            }
                            PixelShaderVariant sprite = variant;
                            sprite.m_pointSprite = 1;
                            PixelShaderVariant projected = variant;
                            projected.m_projected = 0x3 | (0x4 << 3) | (0x3 << 6) | (0x4 << 9);

                            for (const PixelShaderVariant& v : { variant, depth, sprite, projected }) {
                                std::string source;
                                if (TranslatePixelShader(program, v, source, error)) sources.push_back(source);
                                else { fprintf(stderr, "%s[%u]: %s\n", argv[arg], permutation, error.c_str()); translateFailures++; break; }
                            }
                        }
                    }

                    for (const std::string& source : sources) {
                        if (!compiled.insert(source).second) continue;
                        unique++;

                        NSError* nsError = nil;
                        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&nsError];
                        bool ok = library && [library newFunctionWithName:@(program.m_stage == STAGE_VERTEX ? kVertexFunctionName : kFragmentFunctionName)];
                        if (!ok) {
                            compileFailures++;
                            if (compileFailures <= 5) {
                                fprintf(stderr, "%s[%u]: compile:\n%s\n", argv[arg], permutation, nsError.localizedDescription.UTF8String);
                            }
                            if (dumpDir && dumped < 20) {
                                std::string path = std::string(dumpDir) + "/fail" + std::to_string(dumped++) + ".metal";
                                FILE* out = fopen(path.c_str(), "w");
                                if (out) { fputs(source.c_str(), out); fclose(out); }
                            }
                        }
                    }
                }

                offset += (codeSize + 3) & ~3u;
                permutation++;
            }
        }

        printf("%u programs, %u unique sources, %u translation failures, %u compile failures\n", programs, unique, translateFailures, compileFailures);
        return translateFailures || compileFailures ? 1 : 0;
    }
}
