// CoAVolFog for the native Metal client: volumetric fog and modern water, ported from the Windows
// CoAVolFog (Raz0r1337/coa-vfog, GPL-3.0). The Windows build wraps Direct3D 9 and patches call sites of
// Wow.exe; here the client's mod interface hands over the same points of the world render (see WOWRE.md,
// "Faz 5") and the frame's Metal targets.
//
// Slice 1: settings, data and the frame inputs. DebugView=3 draws the linear depth the passes will read.

#import <Metal/Metal.h>
#include <string>
#include "WMModAPI.h"
#include "config.h"
#include "fog_data.h"
#include "log.h"
#include "water_data.h"

namespace
{

const char* const kShaders = R"MSL(#include <metal_stdlib>
using namespace metal;

struct Varyings {
    float4 position [[position]];
};

// One triangle covering the target
vertex Varyings vf_fullscreen(uint vid [[vertex_id]]) {
    float2 corner = float2((vid << 1) & 2, vid & 2);
    Varyings out;
    out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

struct DepthParams {
    float p10;          // projection[10]
    float p14;          // projection[14]
    float minZ;         // world viewport depth range
    float maxZ;
    float farClip;
};

// The depth buffer holds minZ + d (maxZ - minZ); the projection's d = p10 + p14 / z
static float4 debug_depth(float stored, constant DepthParams& p) {
    if (stored >= 1.0) {
        return float4(0.05, 0.05, 0.25, 1.0);   // nothing drawn
    }
    if (stored > p.maxZ + 1e-5) {
        return float4(0.15, 0.25, 0.6, 1.0);    // sky and distant terrain, beyond the world range
    }

    float d = (stored - p.minZ) / max(p.maxZ - p.minZ, 1e-6);
    float viewZ = p.p14 / (d - p.p10);
    float g = 1.0 - saturate(viewZ / max(p.farClip, 1.0));
    return float4(g, g, g, 1.0);
}

fragment float4 vf_debug_depth(Varyings in [[stage_in]], depth2d<float> depth [[texture(0)]], constant DepthParams& p [[buffer(0)]]) {
    return debug_depth(depth.read(uint2(in.position.xy)), p);
}

fragment float4 vf_debug_depth_ms(Varyings in [[stage_in]], depth2d_ms<float> depth [[texture(0)]], constant DepthParams& p [[buffer(0)]]) {
    return debug_depth(depth.read(uint2(in.position.xy), 0), p);
}
)MSL";

struct DepthParams
{
    float p10;
    float p14;
    float minZ;
    float maxZ;
    float farClip;
};

struct State
{
    const WMModHost* host = nullptr;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = nil;
    id<MTLRenderPipelineState> debugDepth = nil;
    MTLPixelFormat debugFormat = MTLPixelFormatInvalid;
    NSUInteger debugSamples = 0;
    Config config;
    FogData fog;
    WaterData water;
    bool loggedFrame = false;
};

State s;

bool MakeLibrary()
{
    if (s.library)
        return true;

    NSError* error = nil;
    s.library = [s.device newLibraryWithSource:@(kShaders) options:nil error:&error];

    if (!s.library)
        VF_LOG_ERROR("shaders failed to compile: %s", error.localizedDescription.UTF8String);

    return s.library != nil;
}

id<MTLRenderPipelineState> DebugDepthPipeline(MTLPixelFormat format, NSUInteger samples)
{
    if (s.debugDepth && s.debugFormat == format && s.debugSamples == samples)
        return s.debugDepth;

    if (!MakeLibrary())
        return nil;

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [s.library newFunctionWithName:@"vf_fullscreen"];
    descriptor.fragmentFunction = [s.library newFunctionWithName:samples > 1 ? @"vf_debug_depth_ms" : @"vf_debug_depth"];
    descriptor.colorAttachments[0].pixelFormat = format;
    descriptor.rasterSampleCount = samples;

    NSError* error = nil;
    s.debugDepth = [s.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    s.debugFormat = format;
    s.debugSamples = samples;

    if (!s.debugDepth)
        VF_LOG_ERROR("debug depth pipeline failed: %s", error.localizedDescription.UTF8String);

    return s.debugDepth;
}

void DrawDebugDepth()
{
    WMFrameTargets targets = {};

    if (!s.host->AcquireFrameTargets(&targets) || !targets.depth)
        return;

    id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)targets.commandBuffer;
    id<MTLTexture> color = (__bridge id<MTLTexture>)targets.color;
    id<MTLTexture> depth = (__bridge id<MTLTexture>)targets.depth;
    id<MTLRenderPipelineState> pipeline = DebugDepthPipeline(color.pixelFormat, color.sampleCount);

    if (!pipeline)
        return;

    const WMWorldState* world = s.host->GetWorldState();
    DepthParams params = {world->projection[10], world->projection[14], world->viewportMinZ, world->viewportMaxZ, world->farClip};

    if (!s.loggedFrame)
    {
        s.loggedFrame = true;
        VF_LOG_INFO("frame %ux%u, %u sample(s); map %d; near %.3f far %.1f; viewport depth [%.4f, %.4f]; zone fog %.1f; day %.3f",
                    targets.width, targets.height, targets.sampleCount, world->mapId, world->nearClip, world->farClip,
                    world->viewportMinZ, world->viewportMaxZ, world->zoneFogEnd, world->dayFraction);
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    encoder.label = @"CoAVolFog debug depth";
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:depth atIndex:0];
    [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

void OnHook(WMHook hook, void* user)
{
    if (!s.config.enable)
        return;

    if (hook == WM_HOOK_BEFORE_SCREEN_EFFECTS && s.config.debugView == 3)
    {
        @autoreleasepool
        {
            DrawDebugDepth();
        }
    }
}

void Shutdown(void* user)
{
    s.debugDepth = nil;
    s.library = nil;
    s.device = nil;
}

}

extern "C" __attribute__((visibility("default"))) int32_t WMModLoad(const WMModHost* host, WMModInfo* info)
{
    if (host->version != WM_MOD_API_VERSION || !host->device || !host->gameDirectory)
        return 0;

    s.host = host;
    s.device = (__bridge id<MTLDevice>)host->device;

    std::string game = host->gameDirectory;
    LogOpen((game + "/Logs/CoAVolFog.log").c_str());

    std::string iniPath = game + "/Mods/CoAVolFog.ini";
    bool read = LoadConfig(iniPath, s.config);
    LogSetLevel(s.config.logLevel);
    VF_LOG_INFO("CoAVolFog for Metal, slice 1; settings %s %s", read ? "from" : "defaulted, no", iniPath.c_str());
    VF_LOG_DEBUG("%s", DescribeConfig(s.config).c_str());

    s.fog.Load(game + "/Mods/fogdata.bin");
    s.water.Load(game + "/Mods/waterdata.bin");

    info->version = WM_MOD_API_VERSION;
    info->name = "CoAVolFog";
    info->OnHook = OnHook;
    info->Shutdown = Shutdown;
    host->Log(s.config.enable ? "CoAVolFog: loaded" : "CoAVolFog: loaded, disabled by Enable=0");
    return 1;
}
