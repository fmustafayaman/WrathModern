// Offscreen regression for the no-user-clip vertex variants. Compiles both the translated
// bytecode path and the fixed-function path with the system Metal compiler, checks that only the
// no-clip source omits [[clip_distance]], and renders a full-screen triangle through the fixed
// pipeline with the clip mask off to verify identical output. Does not launch or modify the game.
//
// clang++ -std=c++17 -fobjc-arc -I. Tools/MacDev/test-clip-shaders.mm \
//   Engine/Source/Gx/CGxDeviceMetal/{MetalFixedFunction.cpp,Shader/ShaderTranslator.cpp,Shader/ShaderBytecode.cpp} \
//   -framework Metal -framework Foundation -o /tmp/test-clip-shaders && /tmp/test-clip-shaders

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

static Instruction Move(uint32_t destType, uint32_t destIndex, uint32_t sourceType, uint32_t sourceIndex) {
    Instruction instruction;
    instruction.m_opcode = OP_MOV;
    instruction.m_hasDest = true;
    instruction.m_dest.m_type = destType;
    instruction.m_dest.m_index = destIndex;
    SourceOperand source;
    source.m_type = sourceType;
    source.m_index = sourceIndex;
    instruction.m_sources.push_back(source);
    return instruction;
}

static ShaderProgram VertexProgram() {
    ShaderProgram program;
    program.m_stage = STAGE_VERTEX;
    program.m_major = 3;
    Instruction positionInput;
    positionInput.m_opcode = OP_DCL;
    positionInput.m_hasDest = true;
    positionInput.m_dest.m_type = REG_INPUT;
    positionInput.m_dest.m_index = 0;
    positionInput.m_dest.m_writeMask = 0xF;
    positionInput.m_usage = USAGE_POSITION;
    program.m_instructions.push_back(positionInput);
    Instruction positionOutput;
    positionOutput.m_opcode = OP_DCL;
    positionOutput.m_hasDest = true;
    positionOutput.m_dest.m_type = REG_OUTPUT;
    positionOutput.m_dest.m_index = 0;
    positionOutput.m_dest.m_writeMask = 0xF;
    positionOutput.m_usage = USAGE_POSITION;
    program.m_instructions.push_back(positionOutput);
    program.m_instructions.push_back(Move(REG_OUTPUT, 0, REG_INPUT, 0));
    return program;
}

static std::string Translated(bool noClip, bool& compiled) {
    ShaderProgram program = VertexProgram();
    VertexInputSignature inputs;
    inputs.m_class[ATTRIB_POSITION] = ATTRIBCLASS_FLOAT;
    inputs.m_noClip = noClip;
    std::string source, error;
    Require(TranslateVertexShader(program, inputs, source, error), error.c_str());
    Require((source.find("vertex D3DVaryingsNoClip") != std::string::npos) == noClip, "translated no-clip output type");
    Require((source.find("vertex D3DVaryings ") != std::string::npos) == !noClip, "translated regular output type");
    Require((source.find("out.clipDistance") == std::string::npos) == noClip, "translated clip-distance writes");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSError* nsError = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.preserveInvariance = YES;
    id<MTLLibrary> library = [device newLibraryWithSource:@(source.c_str()) options:options error:&nsError];
    compiled = library && [library newFunctionWithName:@(kVertexFunctionName)] != nil;
    Require(compiled, nsError.localizedDescription.UTF8String);
    return source;
}

static std::string FixedSource(bool noClip) {
    FixedVertexKey key;
    key.m_attributes[ATTRIB_POSITION] = ATTRIBCLASS_FLOAT;
    key.m_noClip = noClip ? 1 : 0;
    std::string source, error;
    Require(GenerateFixedVertexShader(key, source, error), error.c_str());
    Require((source.find("vertex D3DVaryingsNoClip") != std::string::npos) == noClip, "fixed no-clip output type");
    Require((source.find("vertex D3DVaryings ") != std::string::npos) == !noClip, "fixed regular output type");
    Require((source.find("out.clipDistance") == std::string::npos) == noClip, "fixed clip-distance writes");
    return source;
}

static std::string FixedFragment() {
    FixedPixelKey key;
    key.m_stages[0].m_colorOp = StageOp_PassThrough;
    key.m_stages[0].m_alphaOp = StageOp_PassThrough;
    key.m_stages[1].m_colorOp = StageOp_Disable;
    key.m_noAlphaTest = 1;
    std::string source, error;
    Require(GenerateFixedPixelShader(key, source, error), error.c_str());
    return source;
}

struct Harness {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue;
    id<MTLTexture> colour;
    id<MTLTexture> depth;
    MTLCompileOptions* options = [MTLCompileOptions new];

    Harness() {
        Require(device != nil, "Metal device");
        queue = [device newCommandQueue];
        options.preserveInvariance = YES;
        options.mathMode = MTLMathModeFast;
        MTLTextureDescriptor* target = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:8 height:8 mipmapped:NO];
        target.storageMode = MTLStorageModeShared;
        target.usage = MTLTextureUsageRenderTarget;
        colour = [device newTextureWithDescriptor:target];
        target.pixelFormat = MTLPixelFormatDepth32Float;
        target.storageMode = MTLStorageModePrivate;
        depth = [device newTextureWithDescriptor:target];
        Require(colour && depth, "offscreen targets");
    }

    id<MTLRenderPipelineState> Pipeline(const std::string& vertex, const std::string& fragment) {
        NSError* error = nil;
        id<MTLLibrary> vertexLibrary = [device newLibraryWithSource:@(vertex.c_str()) options:options error:&error];
        Require(vertexLibrary != nil, error.localizedDescription.UTF8String);
        id<MTLLibrary> fragmentLibrary = [device newLibraryWithSource:@(fragment.c_str()) options:options error:&error];
        Require(fragmentLibrary != nil, error.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [vertexLibrary newFunctionWithName:@(kVertexFunctionName)];
        descriptor.fragmentFunction = [fragmentLibrary newFunctionWithName:@(kFragmentFunctionName)];
        MTLVertexDescriptor* layout = [MTLVertexDescriptor vertexDescriptor];
        layout.attributes[ATTRIB_POSITION].format = MTLVertexFormatFloat4;
        layout.attributes[ATTRIB_POSITION].offset = 0;
        layout.attributes[ATTRIB_POSITION].bufferIndex = 0;
        layout.layouts[0].stride = sizeof(float) * 4;
        descriptor.vertexDescriptor = layout;
        descriptor.colorAttachments[0].pixelFormat = colour.pixelFormat;
        descriptor.depthAttachmentPixelFormat = depth.pixelFormat;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        Require(pipeline != nil, error.localizedDescription.UTF8String);
        return pipeline;
    }

    std::array<float, 4> Draw(id<MTLRenderPipelineState> pipeline, bool clipMask) {
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
        FixedVertexStateBlock ff = {};
        for (int i = 0; i < 16; ++i) { ff.m_world[i] = ff.m_worldView[i] = ff.m_normal[i] = ff.m_projection[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
        ff.m_pointScale[0] = 1.0f; ff.m_pointScale[1] = 1.0f; ff.m_pointScale[2] = 1.0f; ff.m_pointScale[3] = 1.0f;
        [encoder setVertexBytes:&ff length:sizeof(ff) atIndex:VS_BUFFER_FIXED_FUNCTION];
        VertexStateBlock state = {};
        state.m_flags[0] = clipMask ? 1u : 0u;
        state.m_pointSize[0] = 1.0f; state.m_pointSize[1] = 1.0f; state.m_pointSize[2] = 1.0f;
        [encoder setVertexBytes:&state length:sizeof(state) atIndex:VS_BUFFER_STATE];
        const float positions[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
        [encoder setVertexBytes:positions length:sizeof(positions) atIndex:0];
        PixelStateBlock pixel = {};
        pixel.m_flags[1] = SHADERCOMPARE_ALWAYS;
        [encoder setFragmentBytes:&pixel length:sizeof(pixel) atIndex:PS_BUFFER_STATE];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        Require(commands.status == MTLCommandBufferStatusCompleted, commands.error.localizedDescription.UTF8String);
        std::array<float, 4> value;
        [colour getBytes:value.data() bytesPerRow:sizeof(value) fromRegion:MTLRegionMake2D(3, 3, 1, 1) mipmapLevel:0];
        return value;
    }
};

int main() {
    @autoreleasepool {
        VertexInputSignature a, b;
        b.m_noClip = 1;
        Require(!(a == b), "vertex shader cache must distinguish no-clip variants");
        FixedVertexKey x, y;
        y.m_noClip = 1;
        Require(!(x == y), "fixed vertex cache must distinguish no-clip variants");
        bool compiled = false;
        Translated(false, compiled);
        Translated(true, compiled);
        FixedSource(false);
        FixedSource(true);
        Harness harness;
        id<MTLRenderPipelineState> reference = harness.Pipeline(FixedSource(false), FixedFragment());
        id<MTLRenderPipelineState> optimized = harness.Pipeline(FixedSource(true), FixedFragment());
        auto referencePixel = harness.Draw(reference, false);
        auto optimizedPixel = harness.Draw(optimized, false);
        Require(referencePixel == optimizedPixel, "clip-disabled fixed output must be identical with no user planes");
        auto c = harness.Draw(reference, true);
        printf("PASS: translated/fixed sources compile; no-clip field omitted; fixed output identical; clip-on sample %.2f\n", c[0]);
    }
}
