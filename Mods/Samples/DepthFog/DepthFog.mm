// Sample mod: tints the world toward the zone's fog colour by view depth, at the hook before the screen
// effects. It exercises the mod interface end to end: loading, hooks, the world state and drawing into
// the frame's targets with Metal while reading its depth buffer.

#import <Metal/Metal.h>
#include <cstdarg>
#include <cstdio>
#include "WMModAPI.h"

namespace {

const char* const kShaders = R"MSL(#include <metal_stdlib>
using namespace metal;

struct Varyings {
    float4 position [[position]];
};

// One triangle covering the target
vertex Varyings fog_vertex(uint vid [[vertex_id]]) {
    float2 corner = float2((vid << 1) & 2, vid & 2);
    Varyings out;
    out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

struct FogParams {
    float4 color;    // rgb, a = strength
    float p10;       // projection[10]
    float p14;       // projection[14]
    float start;
    float end;
};

fragment float4 fog_fragment(Varyings in [[stage_in]], depth2d<float> depth [[texture(0)]], constant FogParams& params [[buffer(0)]]) {
    float d = depth.read(uint2(in.position.xy));

    // Leave the sky, drawn at the far plane, to the client
    if (d >= 0.99999) {
        discard_fragment();
    }

    float viewDepth = params.p14 / (d - params.p10);
    float amount = saturate((viewDepth - params.start) / max(params.end - params.start, 1.0)) * params.color.a;
    return float4(params.color.rgb, amount);
}
)MSL";

struct FogParams {
    float color[4];
    float p10;
    float p14;
    float start;
    float end;
};

struct State {
    const WMModHost* host;
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipeline;
    MTLPixelFormat pipelineFormat;
    bool warnedMultisample;
    bool loggedFirstFrame;
};

State s_state;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    s_state.host->Log(line);
}

bool MakePipeline(MTLPixelFormat format) {
    NSError* error = nil;
    id<MTLLibrary> library = [s_state.device newLibraryWithSource:@(kShaders) options:nil error:&error];

    if (!library) {
        Log("DepthFog: shaders failed: %s", error.localizedDescription.UTF8String);
        return false;
    }

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"fog_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"fog_fragment"];
    descriptor.colorAttachments[0].pixelFormat = format;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;

    s_state.pipeline = [s_state.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    s_state.pipelineFormat = format;

    if (!s_state.pipeline) {
        Log("DepthFog: pipeline failed: %s", error.localizedDescription.UTF8String);
        return false;
    }

    return true;
}

void Draw() {
    WMFrameTargets targets = {};

    if (!s_state.host->AcquireFrameTargets(&targets) || !targets.depth) {
        return;
    }

    if (targets.sampleCount > 1) {
        if (!s_state.warnedMultisample) {
            s_state.warnedMultisample = true;
            Log("DepthFog: the world is multisampled (%u samples); this sample draws only without multisampling", targets.sampleCount);
        }
        return;
    }

    id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)targets.commandBuffer;
    id<MTLTexture> color = (__bridge id<MTLTexture>)targets.color;
    id<MTLTexture> depth = (__bridge id<MTLTexture>)targets.depth;

    if ((!s_state.pipeline || s_state.pipelineFormat != color.pixelFormat) && !MakePipeline(color.pixelFormat)) {
        return;
    }

    const WMWorldState* world = s_state.host->GetWorldState();
    FogParams params = {};
    params.color[0] = world->fogColor[0];
    params.color[1] = world->fogColor[1];
    params.color[2] = world->fogColor[2];
    params.color[3] = 0.6f;
    params.p10 = world->projection[10];
    params.p14 = world->projection[14];
    // A haze from a fifth of the zone's fog distance to its end
    params.end = world->zoneFogEnd > 1.0f ? world->zoneFogEnd : world->farClip;
    params.start = params.end * 0.2f;

    if (!s_state.loggedFirstFrame) {
        s_state.loggedFirstFrame = true;
        Log("DepthFog: first frame %ux%u, map %d, near %.2f far %.1f, zone fog %.1f, fog colour %.2f %.2f %.2f, day %.3f, sun %.2f %.2f %.2f%s",
            targets.width, targets.height, world->mapId, world->nearClip, world->farClip, world->zoneFogEnd,
            world->fogColor[0], world->fogColor[1], world->fogColor[2], world->dayFraction,
            world->sunDirection[0], world->sunDirection[1], world->sunDirection[2], world->moonIsLight ? " (moon)" : "");
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    encoder.label = @"DepthFog";
    [encoder setRenderPipelineState:s_state.pipeline];
    [encoder setFragmentTexture:depth atIndex:0];
    [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

void OnHook(WMHook hook, void* user) {
    if (hook == WM_HOOK_BEFORE_SCREEN_EFFECTS) {
        @autoreleasepool {
            Draw();
        }
    }
}

void Shutdown(void* user) {
    s_state.pipeline = nil;
    s_state.device = nil;
}

}

extern "C" __attribute__((visibility("default"))) int32_t WMModLoad(const WMModHost* host, WMModInfo* info) {
    if (host->version != WM_MOD_API_VERSION || !host->device) {
        return 0;
    }

    s_state.host = host;
    s_state.device = (__bridge id<MTLDevice>)host->device;

    info->version = WM_MOD_API_VERSION;
    info->name = "Depth fog sample";
    info->OnHook = OnHook;
    info->Shutdown = Shutdown;
    return 1;
}
