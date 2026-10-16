// Generates fixed-function shaders for random state combinations and compiles them with the
// system Metal compiler, to check the generator covers every state it accepts.
//   ffcompile [count]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../../Engine/Source/Gx/CGxDeviceMetal/D3D9/FixedFunction.h"
#include "../../Engine/Source/Gx/CGxDeviceMetal/D3D9/D3D9Types.h"
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace D3D9Metal;

int main(int argc, char** argv) {
    @autoreleasepool {
        int count = argc > 1 ? atoi(argv[1]) : 300;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.preserveInvariance = YES;
        options.mathMode = MTLMathModeFast;
        std::mt19937 rng(1234);
        auto r = [&](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
        int failures = 0;

        auto compile = [&](const std::string& source, const char* function, const char* what) {
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:@(source.c_str()) options:options error:&error];
            if (!library || ![library newFunctionWithName:@(function)]) {
                if (failures++ < 3) fprintf(stderr, "%s:\n%s\n%s\n", what, source.c_str(), error.localizedDescription.UTF8String);
            }
        };

        static const uint8_t ops[] = { D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X, D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT, D3DTOP_ADDSMOOTH, D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDTEXTUREALPHA, D3DTOP_BLENDFACTORALPHA, D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_BLENDCURRENTALPHA, D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR, D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP };
        static const uint8_t args[] = { D3DTA_DIFFUSE, D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTA_SPECULAR, D3DTA_TEMP };

        for (int i = 0; i < count; i++) {
            FixedVertexKey vk;
            for (uint32_t a = 0; a < ATTRIB_COUNT; a++) vk.m_attributes[a] = r(3) ? 0 : 1 + r(3);
            vk.m_attributes[ATTRIB_POSITION] = ATTRIBCLASS_FLOAT;
            for (uint32_t t = 0; t < 8; t++) vk.m_texcoordComponents[t] = vk.m_attributes[ATTRIB_TEXCOORD0 + t] ? 1 + r(4) : 0;
            vk.m_lighting = r(2);
            for (uint32_t l = 0; l < 4; l++) vk.m_lightTypes[l] = r(4);
            vk.m_diffuseSource = r(3); vk.m_ambientSource = r(3); vk.m_specularSource = r(3); vk.m_emissiveSource = r(3);
            vk.m_localViewer = r(2); vk.m_normalizeNormals = r(2);
            for (uint32_t t = 0; t < 8; t++) { vk.m_texcoordIndex[t] = r(8); vk.m_texgen[t] = r(5); vk.m_transformCount[t] = r(5); }
            vk.m_fogMode = r(5); vk.m_rangeFog = r(2); vk.m_pointScale = r(2);
            std::string source, error;
            if (!GenerateFixedVertexShader(vk, source, error)) { failures++; fprintf(stderr, "vs: %s\n", error.c_str()); }
            else compile(source, kVertexFunctionName, "vs");

            FixedPixelKey pk;
            uint32_t stages = 1 + r(8);
            for (uint32_t s = 0; s < 8; s++) {
                FixedPixelStage& st = pk.m_stages[s];
                if (s >= stages) { st.m_colorOp = D3DTOP_DISABLE; st.m_alphaOp = D3DTOP_DISABLE; continue; }
                st.m_colorOp = ops[r(sizeof(ops))]; st.m_alphaOp = r(4) ? ops[r(sizeof(ops))] : D3DTOP_DISABLE;
                for (uint32_t a = 0; a < 3; a++) { st.m_colorArg[a] = args[r(sizeof(args))] | (r(4) ? 0 : D3DTA_COMPLEMENT) | (r(4) ? 0 : D3DTA_ALPHAREPLICATE); st.m_alphaArg[a] = args[r(sizeof(args))]; }
                st.m_resultTemp = r(5) == 0; st.m_samplerKind = r(4); st.m_textureBound = r(4) != 0; st.m_projected = r(3) ? 0 : 1 + r(4);
            }
            pk.m_specularEnable = r(2); pk.m_pointSprite = r(4) == 0;
            if (!GenerateFixedPixelShader(pk, source, error)) { failures++; fprintf(stderr, "ps: %s\n", error.c_str()); }
            else compile(source, kFragmentFunctionName, "ps");
        }
        printf("%d vertex + %d pixel keys, %d failures\n", count, count, failures);
        return failures != 0;
    }
}
