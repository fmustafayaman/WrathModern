// Regression for the native Metal no-alpha-test specialization. Uses the real translators
// and the system Metal compiler; runs offscreen, without launching or modifying the game.
// Build/run:
// clang++ -std=c++17 -fobjc-arc -I. Tools/MacDev/test-opaque-shaders.mm \
//   Engine/Source/Gx/CGxDeviceMetal/{MetalFixedFunction.cpp,Shader/ShaderTranslator.cpp,Shader/ShaderBytecode.cpp} \
//   -framework Metal -framework Foundation -o /tmp/test-opaque-shaders && /tmp/test-opaque-shaders
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "Engine/Source/Gx/CGxDeviceMetal/MetalFixedFunction.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace GxMetal;

static void Require(bool ok, const char* message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static Instruction Move(uint32_t dest, uint32_t index, uint32_t constant) {
    Instruction instruction;
    instruction.m_opcode = OP_MOV;
    instruction.m_hasDest = true;
    instruction.m_dest.m_type = dest;
    instruction.m_dest.m_index = index;
    SourceOperand source;
    source.m_type = REG_CONST;
    source.m_index = constant;
    instruction.m_sources.push_back(source);
    return instruction;
}

static std::string Translated(bool noAlphaTest, bool alphaOnly, bool texkill) {
    ShaderProgram program;
    program.m_stage = STAGE_PIXEL;
    program.m_major = 3;
    if (texkill) {
        program.m_instructions.push_back(Move(REG_TEMP, 0, 1));
        Instruction kill;
        kill.m_opcode = OP_TEXKILL;
        kill.m_hasDest = true;
        kill.m_dest.m_type = REG_TEMP;
        program.m_instructions.push_back(kill);
    }
    program.m_instructions.push_back(Move(REG_COLOROUT, 0, 0));
    PixelShaderVariant variant;
    variant.m_noAlphaTest = noAlphaTest;
    variant.m_alphaOnly = alphaOnly;
    std::string source, error;
    Require(TranslatePixelShader(program, variant, source, error), error.c_str());
    const size_t discard = source.find("discard_fragment()");
    Require((discard != std::string::npos) == (!noAlphaTest || texkill), "translated discard policy");
    return source;
}

static std::string FixedSource(bool noAlphaTest, bool specular) {
    FixedPixelKey key;
    key.m_stages[0].m_colorOp = StageOp_PassThrough;
    key.m_stages[0].m_alphaOp = StageOp_PassThrough;
    key.m_stages[1].m_colorOp = StageOp_Disable;
    key.m_specularEnable = specular;
    key.m_noAlphaTest = noAlphaTest;
    std::string source, error;
    Require(GenerateFixedPixelShader(key, source, error), error.c_str());
    Require((source.find("discard_fragment()") == std::string::npos) == noAlphaTest, "fixed discard policy");
    return source;
}

struct alignas(16) VertexUniform {
    float colour[4];
    float z;
    float padding[3] = {};
};

struct Harness {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue;
    id<MTLFunction> vertex;
    id<MTLTexture> colour;
    id<MTLTexture> depth;
    id<MTLDepthStencilState> depthState;
    MTLCompileOptions* options = [MTLCompileOptions new];

    Harness() {
        Require(device != nil, "Metal device");
        queue = [device newCommandQueue];
        options.preserveInvariance = YES;
        options.mathMode = MTLMathModeFast;
        NSString* source = @R"MSL(#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position, invariant]]; float4 colour [[user(d3d_color0)]];
           float4 specular [[user(d3d_color1)]]; float fog [[user(d3d_fog)]]; };
struct U { float4 colour; float z; };
vertex V vertex_main(uint id [[vertex_id]], constant U& u [[buffer(20)]]) {
    float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return {float4(p[id], u.z, 1), u.colour, float4(0.1, 0.2, 0.3, 0), 0.4};
})MSL";
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        Require(library != nil, error.localizedDescription.UTF8String);
        vertex = [library newFunctionWithName:@"vertex_main"];
        MTLTextureDescriptor* target = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:8 height:8 mipmapped:NO];
        target.storageMode = MTLStorageModeShared;
        target.usage = MTLTextureUsageRenderTarget;
        colour = [device newTextureWithDescriptor:target];
        target.pixelFormat = MTLPixelFormatDepth32Float;
        target.storageMode = MTLStorageModePrivate;
        depth = [device newTextureWithDescriptor:target];
        MTLDepthStencilDescriptor* state = [MTLDepthStencilDescriptor new];
        state.depthCompareFunction = MTLCompareFunctionLess;
        state.depthWriteEnabled = YES;
        depthState = [device newDepthStencilStateWithDescriptor:state];
        Require(colour && depth && depthState, "offscreen targets");
    }

    id<MTLRenderPipelineState> Pipeline(const std::string& source) {
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(source.c_str()) options:options error:&error];
        Require(library != nil, error.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = [library newFunctionWithName:@(kFragmentFunctionName)];
        descriptor.colorAttachments[0].pixelFormat = colour.pixelFormat;
        descriptor.depthAttachmentPixelFormat = depth.pixelFormat;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        Require(pipeline != nil, error.localizedDescription.UTF8String);
        return pipeline;
    }

    std::array<float, 4> Draw(id<MTLRenderPipelineState> pipeline, float alpha, uint32_t function,
                               uint32_t reference, bool fog, bool kill) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = colour;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(-9, -9, -9, -9);
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 1;
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setDepthStencilState:depthState];
        VertexUniform uniform = {{0.25f, 0.5f, 0.75f, alpha}, 0.5f};
        [encoder setVertexBytes:&uniform length:sizeof(uniform) atIndex:20];
        float constants[2][4] = {{0.25f, 0.5f, 0.75f, alpha}, {kill ? -1.0f : 1.0f, 1, 1, 1}};
        [encoder setFragmentBytes:constants length:sizeof(constants) atIndex:PS_BUFFER_FLOAT_CONSTANTS];
        PixelStateBlock state = {};
        state.m_flags[0] = fog;
        state.m_flags[1] = function;
        state.m_flags[2] = reference;
        state.m_fogColor[0] = 0.7f; state.m_fogColor[1] = 0.3f; state.m_fogColor[2] = 0.1f;
        [encoder setFragmentBytes:&state length:sizeof(state) atIndex:PS_BUFFER_STATE];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        Require(commands.status == MTLCommandBufferStatusCompleted, commands.error.localizedDescription.UTF8String);
        std::array<float, 4> pixel;
        [colour getBytes:pixel.data() bytesPerRow:sizeof(pixel) fromRegion:MTLRegionMake2D(3, 3, 1, 1) mipmapLevel:0];
        return pixel;
    }
};

int main() {
    @autoreleasepool {
        Harness harness;
        size_t comparisons = 0, alphaTests = 0;
        const float alphas[] = {-2, -0.01f, 0, 0.49f / 255, 0.51f / 255, 0.499f, 0.5f, 0.501f, 1, 2};
        PixelShaderVariant a, b;
        b.m_noAlphaTest = 1;
        Require(!(a == b), "shader cache must distinguish no-alpha-test variants");
        FixedPixelKey x, y;
        y.m_noAlphaTest = 1;
        Require(!(x == y), "fixed shader cache must distinguish no-alpha-test variants");
        for (int family = 0; family < 3; ++family) {
            // Two translated families (normal and depth/alpha-only) and the fixed-function one.
            for (int extra = 0; extra < 2; ++extra) {
                auto source = [&](bool optimized) {
                    return family < 2 ? Translated(optimized, family == 1, extra) : FixedSource(optimized, extra);
                };
                id<MTLRenderPipelineState> reference = harness.Pipeline(source(false));
                id<MTLRenderPipelineState> optimized = harness.Pipeline(source(true));
                for (float alpha : alphas) for (uint32_t ref : {0u, 1u, 127u, 128u, 255u}) {
                    for (bool fog : {false, true}) for (bool kill : {false, true}) {
                        auto before = harness.Draw(reference, alpha, SHADERCOMPARE_ALWAYS, ref, fog, kill);
                        auto after = harness.Draw(optimized, alpha, SHADERCOMPARE_ALWAYS, ref, fog, kill);
                        Require(before == after, "disabled alpha test: specialization must be bit-identical");
                        const bool discarded = family < 2 && extra && kill;
                        Require((after[0] == -9) == discarded, "explicit TEXKILL must remain active");
                        ++comparisons;
                    }
                    auto tested = harness.Draw(reference, alpha, SHADERCOMPARE_GREATEREQUAL, ref, false, false);
                    Require((tested[0] != -9) == (std::nearbyint(alpha * 255) >= ref), "enabled alpha test coverage/threshold");
                    ++alphaTests;
                }
            }
        }
        printf("PASS: %zu bit-identical disabled-alpha/TEXKILL/fog comparisons, %zu enabled-alpha threshold cases\n", comparisons, alphaTests);
    }
}
