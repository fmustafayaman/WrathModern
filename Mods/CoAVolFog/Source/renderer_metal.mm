// FogRenderer: Renderer::RenderPasses of the Windows CoAVolFog (src/renderer.cpp) on Metal. The constants are
// built as the Windows renderer builds them and uploaded under the same register numbers; the passes are
// the march into a low-resolution fp16 target and the composite over the frame.
//
// Not yet here (later slices): the temporal filter (the march stays at its midpoint samples until it
// exists), local lights, god rays, the scene-copy blends, the per-sample split and the depth probe.

#include "renderer_metal.h"

#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include "WMModAPI.h"
#include "engine.h"
#include "log.h"
#include "noise_volume.h"
#include "vf_fog_source.h"

namespace
{

constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr double kMaxNoiseStepSeconds = 0.25;
constexpr int kRegisterCount = 99;
constexpr float kHistoryMaxSeconds = 0.25f;
constexpr float kHistoryMaxMove = 30.0f;
constexpr float kHistoryMinForwardDot = 0.70710678f;

struct Float4
{
    float x, y, z, w;
};

enum class FogBlend
{
    GammaFixedFunction,
    LinearOverSceneCopy,
    LinearFixedFunction,
    GammaOverSceneCopy,
};

const char* FogBlendName(FogBlend blend)
{
    switch (blend)
    {
    case FogBlend::GammaFixedFunction:
        return "gamma, fixed-function blend";
    case FogBlend::LinearOverSceneCopy:
        return "linear light over a scene copy";
    case FogBlend::LinearFixedFunction:
        return "linear light, fixed-function blend";
    default:
        return "gamma over a scene copy";
    }
}

struct WorldDepthMapping
{
    float atInfinity;
    float perInverseViewDepth;
    float deepest;
};

WorldDepthMapping MapWorldDepth(const float* proj, const Viewport& vp)
{
    const bool usableRange = vp.MaxZ - vp.MinZ > kMinViewportDepthExtent && vp.MinZ >= 0.0f && vp.MaxZ <= 1.0f;
    const float worldMinZ = usableRange ? vp.MinZ : 0.0f;
    const float worldExtent = usableRange ? vp.MaxZ - vp.MinZ : 1.0f;
    const float worldMaxZ = worldMinZ + worldExtent;
    return {worldMinZ + worldExtent * (1.0f + proj[10]) * 0.5f, worldExtent * proj[14] * 0.5f,
            worldMaxZ >= kDeepestWorldDepthInFullRangeViewport ? kDeepestWorldDepthInFullRangeViewport
                                                               : worldMaxZ + kWorldDepthMargin};
}

bool WorldViewFromCameraRelative(const FrameInputs& in, float* viewToWorld, float* worldToView)
{
    if (!Invert4x4(in.cameraRelativeView, viewToWorld))
        return false;
    viewToWorld[12] = in.camPos[0];
    viewToWorld[13] = in.camPos[1];
    viewToWorld[14] = in.camPos[2];
    return Invert4x4(viewToWorld, worldToView);
}

void Normalize3(float* v)
{
    float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0.0f)
    {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

id<MTLTexture> MakeVolume(id<MTLDevice> device, uint32_t size, const std::vector<uint8_t>& texels)
{
    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType3D;
    descriptor.pixelFormat = MTLPixelFormatR8Unorm;
    descriptor.width = size;
    descriptor.height = size;
    descriptor.depth = size;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, size, size, size) mipmapLevel:0 slice:0 withBytes:texels.data()
               bytesPerRow:size bytesPerImage:size * size];
    return texture;
}

}

struct FogRenderer::Impl
{
    id<MTLDevice> device = nil;
    std::map<std::string, id<MTLLibrary>> libraries;
    std::map<std::string, id<MTLRenderPipelineState>> pipelines;
    id<MTLTexture> march = nil;
    id<MTLTexture> history[2] = {nil, nil};
    id<MTLTexture> historyDepth = nil;
    id<MTLTexture> densityNoise = nil;
    id<MTLTexture> authoredNoise = nil;
    id<MTLTexture> dummy2d = nil;
    bool failed = false;

    id<MTLLibrary> Library(int steps, bool lights, bool noise, bool depthMs)
    {
        std::string key = std::to_string(steps) + (lights ? "L" : "") + (noise ? "N" : "") + (depthMs ? "M" : "");
        auto found = libraries.find(key);
        if (found != libraries.end())
            return found->second;

        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.preprocessorMacros = @{
            @"STEPS" : @(steps),
            @"LOCAL_LIGHTS" : @(lights ? 1 : 0),
            @"SAMPLES_AUTHORED_NOISE" : @(noise ? 1 : 0),
            @"DEPTH_MS" : @(depthMs ? 1 : 0),
        };
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(kVfFogSource) options:options error:&error];
        if (!library)
            VF_LOG_ERROR("fog shaders %s failed to compile: %s", key.c_str(), error.localizedDescription.UTF8String);
        libraries[key] = library;
        return library;
    }

    id<MTLRenderPipelineState> Pipeline(id<MTLLibrary> library, NSString* fragment, MTLPixelFormat format,
                                        NSUInteger samples, bool blend, const std::string& key)
    {
        auto found = pipelines.find(key);
        if (found != pipelines.end())
            return found->second;

        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"vf_fullscreen"];
        descriptor.fragmentFunction = [library newFunctionWithName:fragment];
        descriptor.colorAttachments[0].pixelFormat = format;
        descriptor.rasterSampleCount = samples;

        if (blend)
        {
            // D3DRS_SRCBLEND ONE, D3DRS_DESTBLEND INVSRCALPHA, colour writes without alpha
            descriptor.colorAttachments[0].blendingEnabled = YES;
            descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
            descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        }

        NSError* error = nil;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline)
            VF_LOG_ERROR("pipeline %s failed: %s", key.c_str(), error.localizedDescription.UTF8String);
        pipelines[key] = pipeline;
        return pipeline;
    }

    id<MTLTexture> Target(MTLPixelFormat format, uint32_t width, uint32_t height)
    {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                              width:width height:height mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModePrivate;
        return [device newTextureWithDescriptor:descriptor];
    }

    // EnsureTargets: the march and the two histories in fp16, the history depth packed in 8-bit channels.
    // Returns whether they were made again (the history is then lost).
    bool EnsureTargets(uint32_t width, uint32_t height, bool& remade)
    {
        remade = false;
        if (march && march.width == width && march.height == height)
            return true;

        march = Target(MTLPixelFormatRGBA16Float, width, height);
        history[0] = Target(MTLPixelFormatRGBA16Float, width, height);
        history[1] = Target(MTLPixelFormatRGBA16Float, width, height);
        historyDepth = Target(MTLPixelFormatRGBA8Unorm, width, height);
        remade = true;
        VF_LOG_INFO("fog targets %ux%u: march, history fp16, history depth rgba8", width, height);
        return march && history[0] && history[1] && historyDepth;
    }

    void EnsureStatic()
    {
        if (!densityNoise)
            densityNoise = MakeVolume(device, kDensityNoiseSize, DensityNoiseVolume());
        if (!authoredNoise)
            authoredNoise = MakeVolume(device, kAuthoredNoiseSize, AuthoredNoiseVolume());
        if (!dummy2d)
        {
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                  width:1 height:1 mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;
            dummy2d = [device newTextureWithDescriptor:descriptor];
            const uint32_t black = 0;
            [dummy2d replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
        }
    }
};

void FogRenderer::Release()
{
    delete m_impl;
    m_impl = nullptr;
}

bool FogRenderer::Render(const WMModHost* host, const Config& cfg, const FogData& data, const SavedStockFog& stock)
{
    if (!m_impl)
    {
        m_impl = new Impl();
        m_impl->device = (__bridge id<MTLDevice>)host->device;
    }

    Impl& impl = *m_impl;

    if (impl.failed)
        return false;

    WMFrameTargets targets = {};
    if (!host->AcquireFrameTargets(&targets) || !targets.depth)
        return false;

    const WMWorldState* world = host->GetWorldState();
    FrameInputs in;
    if (!world || !engine::BuildFrameInputs(*world, targets.width, targets.height, in))
        return false;

    // UseClientFogRangeInsteadOfPushed
    if (stock.pushed)
    {
        in.fogStart = stock.start;
        in.fogEnd = stock.end;
    }

    if (in.inLiquid && !cfg.underwater)
        return false;

    id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)targets.commandBuffer;
    id<MTLTexture> color = (__bridge id<MTLTexture>)targets.color;
    id<MTLTexture> depth = (__bridge id<MTLTexture>)targets.depth;
    const bool depthMs = depth.textureType == MTLTextureType2DMultisample;
    const Viewport vp = in.viewport;

    const uint32_t scale = cfg.quality == 1 ? 4u : 2u;
    const uint32_t lowW = (vp.Width + scale - 1) / scale;
    const uint32_t lowH = (vp.Height + scale - 1) / scale;

    impl.EnsureStatic();
    bool remade = false;
    if (!impl.EnsureTargets(lowW, lowH, remade))
        return false;

    float viewToWorld[16];
    float worldToView[16];
    if (!WorldViewFromCameraRelative(in, viewToWorld, worldToView))
        return false;

    AuthoredFog authored = {};
    const bool hasAuthored = cfg.dataMode == 1 && data.Loaded() &&
                             data.Resolve(in.mapId, in.camPos, in.dayFraction, in.lightParams, authored);
    // DrawableFog: the authored noise texture always exists here
    const FogParams fog = BuildFogParams(in, cfg, hasAuthored ? &authored : nullptr);
    const bool samplesNoise = AnyLayerNoise(fog);
    const float* proj = in.glProjection;
    const WorldDepthMapping worldDepth = MapWorldDepth(proj, vp);

    // The history applies while nothing that moves the fog's samples changed
    const double now = world->time;
    const float dx = in.camPos[0] - m_prevCam[0];
    const float dy = in.camPos[1] - m_prevCam[1];
    const float dz = in.camPos[2] - m_prevCam[2];
    const float forwardDot = worldToView[2] * m_prevWorldToView[2] + worldToView[6] * m_prevWorldToView[6] +
                             worldToView[10] * m_prevWorldToView[10];
    const bool historyValid = m_historyValid && !remade && cfg.temporal > 0.0f && m_prevScale == scale &&
                              SameFogSettings(cfg, m_prevConfig) && in.mapId == m_prevMap &&
                              in.lightParams.screenEffectSlot == m_prevLightSlot && forwardDot > kHistoryMinForwardDot &&
                              std::memcmp(m_prevProj, proj, sizeof(m_prevProj)) == 0 && m_prevWidth == vp.Width &&
                              m_prevHeight == vp.Height && now - m_prevSeconds < kHistoryMaxSeconds &&
                              dx * dx + dy * dy + dz * dz < kHistoryMaxMove * kHistoryMaxMove;
    float reproj[16];
    float prevViewProj[16];
    Mul4x4(m_prevWorldToView, m_prevProj, prevViewProj);
    Mul4x4(viewToWorld, prevViewProj, reproj);

    float toLightInView[3];
    TransformDirection(in.toLight, in.cameraRelativeView, toLightInView);
    Normalize3(toLightInView);

    Float4 C[kRegisterCount] = {};
    C[0] = {static_cast<float>(vp.X), static_cast<float>(vp.Y), static_cast<float>(vp.Width), static_cast<float>(vp.Height)};
    C[1] = {static_cast<float>(scale), static_cast<float>(m_frame % 1024u), 1.0f / depth.width, 1.0f / depth.height};
    C[2] = {proj[0], proj[5], proj[8], proj[9]};
    C[3] = {worldDepth.atInfinity, worldDepth.perInverseViewDepth, fog.maxDistance, worldDepth.deepest};
    std::memcpy(&C[4], viewToWorld, sizeof(viewToWorld));
    C[8] = {static_cast<float>(lowW), static_cast<float>(lowH), 1.0f / lowW, 1.0f / lowH};

    float sunPx[2] = {};
    const bool sunInFront = toLightInView[2] > 0.05f;
    if (sunInFront)
    {
        float ndcX = toLightInView[0] / toLightInView[2] * proj[0] + proj[8];
        float ndcY = toLightInView[1] / toLightInView[2] * proj[5] + proj[9];
        sunPx[0] = vp.X + (ndcX * 0.5f + 0.5f) * vp.Width;
        sunPx[1] = vp.Y + (0.5f - ndcY * 0.5f) * vp.Height;
    }

    C[9] = {toLightInView[0], toLightInView[1], toLightInView[2], fog.lightAboveHorizon};
    C[11] = {cfg.temporal > 0.0f ? 1.0f : 0.0f, fog.maxDistance, fog.horizonStart, fog.farClip};
    static_assert(sizeof(FogLayer) == 6 * sizeof(Float4), "FogLayer is six shader registers");
    std::memcpy(&C[12], &fog.layers[0].start, sizeof(fog.layers));

    const double elapsed = m_noiseSeconds > 0.0 ? std::min(now - m_noiseSeconds, kMaxNoiseStepSeconds) : 0.0;
    m_noiseSeconds = now;
    m_noiseScroll.Advance(fog, in.camPos, std::max(elapsed, 0.0));
    LayerNoiseRegisters noise[kSceneLayers];
    m_noiseScroll.Registers(fog, noise);
    std::memcpy(&C[36], noise, sizeof(noise));

    // No local lights before slice 5
    C[53] = {0.0f, 0.0f, cfg.localLightPhase, 0.0f};

    const double windPeriod = static_cast<double>(kDensityNoiseSize) / std::max(cfg.noiseScale, 0.001f);
    C[78] = {cfg.noiseAmount, cfg.noiseScale, static_cast<float>(std::fmod(now * cfg.noiseWindSpeed, windPeriod)), 0.0f};

    const FogBlend blend = fog.linear ? FogBlend::LinearFixedFunction : FogBlend::GammaFixedFunction;
    const float blendMode = static_cast<float>(blend);
    if (blendMode != m_loggedBlend)
    {
        m_loggedBlend = blendMode;
        VF_LOG_INFO("fog blend: %s", FogBlendName(blend));
    }

    Float4 localLights[32] = {};
    const int steps = cfg.quality == 1 ? 16 : cfg.quality == 2 ? 24 : 32;
    id<MTLLibrary> library = impl.Library(steps, false, samplesNoise, depthMs);
    if (!library)
    {
        impl.failed = true;
        return false;
    }

    std::string variant = std::to_string(steps) + (samplesNoise ? "N" : "") + (depthMs ? "M" : "");
    id<MTLRenderPipelineState> marchPipeline =
        impl.Pipeline(library, @"vf_march", MTLPixelFormatRGBA16Float, 1, false, "march" + variant);
    id<MTLRenderPipelineState> temporalPipeline =
        impl.Pipeline(library, @"vf_temporal", MTLPixelFormatRGBA16Float, 1, false, "temporal" + variant);
    id<MTLRenderPipelineState> historyDepthPipeline =
        impl.Pipeline(library, @"vf_history_depth", MTLPixelFormatRGBA8Unorm, 1, false, "historydepth" + variant);
    id<MTLRenderPipelineState> compositePipeline =
        impl.Pipeline(library, @"vf_composite", color.pixelFormat, color.sampleCount, true,
                      "composite" + variant + std::to_string(color.pixelFormat) + "x" + std::to_string(color.sampleCount));
    if (!marchPipeline || !temporalPipeline || !historyDepthPipeline || !compositePipeline)
    {
        impl.failed = true;
        return false;
    }

    if (!m_loggedFrame)
    {
        m_loggedFrame = true;
        VF_LOG_INFO("fog: map %d, %s, %d layers%s, far clip %.1f, max distance %.0f, horizon %.0f, low res %ux%u (1/%u), %d steps",
                    in.mapId, hasAuthored ? "Classic data" : "derived from the day/night light",
                    hasAuthored ? authored.layerCount : kFogLayers, samplesNoise ? ", authored noise" : "", fog.farClip,
                    fog.maxDistance, fog.horizonStart, lowW, lowH, scale, steps);
    }

    const bool temporalFiltering = cfg.temporal > 0.0f;
    const int write = m_historyIndex ^ 1;
    id<MTLTexture> fogResult = temporalFiltering ? impl.history[write] : impl.march;

    auto bindFog = [&](id<MTLRenderCommandEncoder> encoder, Float4* constants) {
        [encoder setFragmentBytes:constants length:sizeof(Float4) * kRegisterCount atIndex:0];
        [encoder setFragmentBytes:localLights length:sizeof(localLights) atIndex:1];
        [encoder setFragmentTexture:depth atIndex:0];
        [encoder setFragmentTexture:fogResult atIndex:1];
        [encoder setFragmentTexture:impl.dummy2d atIndex:2];
        [encoder setFragmentTexture:impl.march atIndex:4];
        [encoder setFragmentTexture:impl.history[m_historyIndex] atIndex:5];
        [encoder setFragmentTexture:impl.historyDepth atIndex:6];
        [encoder setFragmentTexture:impl.densityNoise atIndex:9];
        [encoder setFragmentTexture:impl.authoredNoise atIndex:10];
    };

    auto drawInto = [&](id<MTLTexture> target, id<MTLRenderPipelineState> pipeline, Float4* constants, NSString* label) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        encoder.label = label;
        [encoder setRenderPipelineState:pipeline];
        bindFog(encoder, constants);
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    };

    drawInto(impl.march, marchPipeline, C, @"CoAVolFog march");

    // FilterWithHistory: the temporal pass replaces c9..c13 with the reprojection and its settings, then
    // the history depth of this frame is written for the next one
    const bool adaptiveLightingHistory = m_prevLocalLightCount > 0 || cfg.noiseAmount > 0.0f || samplesNoise;
    if (temporalFiltering)
    {
        Float4 T[kRegisterCount];
        std::memcpy(T, C, sizeof(T));
        std::memcpy(&T[9], reproj, sizeof(reproj));
        T[13] = {cfg.temporal, historyValid ? 1.0f : 0.0f, adaptiveLightingHistory ? 1.0f : 0.0f, 0.0f};
        drawInto(impl.history[write], temporalPipeline, T, @"CoAVolFog temporal");
        drawInto(impl.historyDepth, historyDepthPipeline, C, @"CoAVolFog history depth");
    }

    // Composite, with the march's fog (no local lights, so WithMeanNoise does not apply)
    C[96] = {hasAuthored ? cfg.classicExposure : cfg.exposure, 0.0f, static_cast<float>(cfg.debugView), blendMode};
    C[97] = {fog.rayColor[0], fog.rayColor[1], fog.rayColor[2], 0.0f};
    C[98] = {sunPx[0], sunPx[1], cfg.sunMarker && sunInFront ? 1.0f : 0.0f, 0.0f};
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        encoder.label = @"CoAVolFog composite";
        [encoder setRenderPipelineState:compositePipeline];
        bindFog(encoder, C);
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    }

    std::memcpy(m_prevWorldToView, worldToView, sizeof(m_prevWorldToView));
    std::memcpy(m_prevProj, proj, sizeof(m_prevProj));
    std::memcpy(m_prevCam, in.camPos, sizeof(m_prevCam));
    m_prevScale = scale;
    m_prevWidth = vp.Width;
    m_prevHeight = vp.Height;
    m_prevConfig = cfg;
    m_prevMap = in.mapId;
    m_prevLightSlot = in.lightParams.screenEffectSlot;
    m_prevLocalLightCount = 0;
    m_prevSeconds = now;
    m_historyValid = temporalFiltering;
    if (temporalFiltering)
        m_historyIndex = write;
    ++m_frame;
    return true;
}
