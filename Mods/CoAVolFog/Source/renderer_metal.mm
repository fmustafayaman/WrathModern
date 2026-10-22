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
#include <limits>
#include <map>
#include <string>
#include "WMModAPI.h"
#include "engine.h"
#include "gpu_profile.h"
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
constexpr uint32_t kRayScale = 4;
constexpr float kMinGodRayStrength = 0.005f;
constexpr float kRayFalloff = 8.0f;
constexpr float kRayThreshold = 0.12f;
constexpr float kRayStep = 0.075f;
constexpr float kRayDecay = 0.9f;
constexpr int kRayTaps = 8;
constexpr float kHistoryMaxMove = 30.0f;
constexpr float kHistoryMinForwardDot = 0.70710678f;
constexpr double kDesignHeight = 1080.0;

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

// forever_look.cpp: the Classic lights' share of the blend above kMinimumClassicCoverage, as a 0..1 weight
float ForeverLookWeight(float coverage)
{
    if (!(coverage > kMinimumClassicCoverage))
        return 0.0f;
    return std::min((coverage - kMinimumClassicCoverage) / (1.0f - kMinimumClassicCoverage), 1.0f);
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
    id<MTLTexture> sceneCopy = nil;
    id<MTLTexture> rays[2] = {nil, nil};
    id<MTLTexture> gradingCopy = nil;
    id<MTLTexture> densityNoise = nil;
    id<MTLTexture> authoredNoise = nil;
    id<MTLTexture> dummy2d = nil;
    // vf_resolve_copy for the frame's format, set each frame before the copies
    id<MTLRenderPipelineState> resolvePipeline = nil;
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
        const double started = [[NSProcessInfo processInfo] systemUptime];
        id<MTLLibrary> library = [device newLibraryWithSource:@(kVfFogSource) options:options error:&error];
        if (!library)
            VF_LOG_ERROR("fog shaders %s failed to compile: %s", key.c_str(), error.localizedDescription.UTF8String);
        else
            VF_LOG_INFO("fog shaders %s compiled in %.0f ms", key.c_str(), ([[NSProcessInfo processInfo] systemUptime] - started) * 1000.0);
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

    // The resolved world after the fog, for the colour grading
    bool CopyGraded(id<MTLCommandBuffer> commands, id<MTLTexture> color)
    {
        id<MTLTexture> saved = sceneCopy;
        sceneCopy = gradingCopy;
        bool copied = CopyScene(commands, color);
        gradingCopy = sceneCopy;
        sceneCopy = saved;
        return copied;
    }

    // CopyWorldViewport: the world before the fog, resolved when multisampled, at the target's size
    bool CopyScene(id<MTLCommandBuffer> commands, id<MTLTexture> color)
    {
        if (!sceneCopy || sceneCopy.width != color.width || sceneCopy.height != color.height ||
            sceneCopy.pixelFormat != color.pixelFormat)
        {
            sceneCopy = Target(color.pixelFormat, static_cast<uint32_t>(color.width), static_cast<uint32_t>(color.height));
            if (!sceneCopy)
                return false;
        }

        if (color.sampleCount > 1)
        {
            // The samples averaged by a pass that reads the frame as a texture (vf_resolve_copy)
            id<MTLRenderPipelineState> pipeline = resolvePipeline;
            if (!pipeline)
                return false;
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = sceneCopy;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = ProfiledRenderEncoder(commands, pass, "CoAVolFog scene copy");
            [encoder setRenderPipelineState:pipeline];
            [encoder setFragmentTexture:color atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        }
        else
        {
            id<MTLBlitCommandEncoder> blit = ProfiledBlitEncoder(commands, "CoAVolFog scene copy");
            [blit copyFromTexture:color toTexture:sceneCopy];
            [blit endEncoding];
        }
        return true;
    }

    bool EnsureRays(uint32_t width, uint32_t height)
    {
        if (rays[0] && rays[0].width == width && rays[0].height == height)
            return true;
        rays[0] = Target(MTLPixelFormatRGBA8Unorm, width, height);
        rays[1] = Target(MTLPixelFormatRGBA8Unorm, width, height);
        return rays[0] && rays[1];
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

    // Renderer::RenderPasses marches at 1/4 (quality 1) or 1/2 of the viewport, sizes chosen for a 1080-line
    // display. The march keeps that density on taller outputs: the scale grows with the viewport's height so the
    // low-resolution target stays near the original's at 1080 lines (1440 lines: 1/3, 2160: 1/4)
    const uint32_t baseScale = cfg.quality == 1 ? 4u : 2u;
    // Ceiling, not rounding: with a round, a shorter window could land on a smaller scale and so march *more*
    // pixels (1912 lines: 1/4, 1664: 1/3), making a smaller window the more expensive one
    const uint32_t scale = std::max(baseScale, static_cast<uint32_t>(std::ceil(baseScale * vp.Height / kDesignHeight)));
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

    // CaptureLocalLightInputs and the stabiliser step (engine.cpp BuildFrameInputsUnsafe)
    if (cfg.localLights)
    {
        LocalLightCamera lightCamera;
        std::memcpy(lightCamera.position, in.camPos, sizeof(lightCamera.position));
        std::memcpy(lightCamera.cameraRelativeView, in.cameraRelativeView, sizeof(lightCamera.cameraRelativeView));
        std::memcpy(lightCamera.glProjection, in.glProjection, sizeof(lightCamera.glProjection));
        engine::CaptureLocalPointLights(lightCamera, LocalLightUpload(cfg), in.localLights);
        m_stabiliser.Step(lightCamera, in.mapId, m_frame, world->time, in.localLights);
    }

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
    float sunScreenFade = 0.0f;
    if (sunInFront)
    {
        float ndcX = toLightInView[0] / toLightInView[2] * proj[0] + proj[8];
        float ndcY = toLightInView[1] / toLightInView[2] * proj[5] + proj[9];
        sunPx[0] = vp.X + (ndcX * 0.5f + 0.5f) * vp.Width;
        sunPx[1] = vp.Y + (0.5f - ndcY * 0.5f) * vp.Height;
        sunScreenFade = std::clamp(1.6f - std::max(std::fabs(ndcX), std::fabs(ndcY)), 0.0f, 1.0f);
    }
    const float rayStrength = cfg.godRays * fog.lightVisibility * sunScreenFade;
    const bool rays = rayStrength > kMinGodRayStrength;

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

    // The uploaded point lights: view-space position and cutoff radius, colour, attenuation (32 float4)
    uint32_t pointLightCount = cfg.localLights ? std::min(in.localLights.pointLightCount, kMaxLocalPointLights) : 0;
    Float4 localLights[32] = {};
    for (uint32_t i = 0; i < pointLightCount; ++i)
    {
        const LocalPointLight& light = in.localLights.pointLights[i];
        float relative[3] = {light.position[0] - in.camPos[0], light.position[1] - in.camPos[1],
                             light.position[2] - in.camPos[2]};
        float position[3];
        TransformDirection(relative, in.cameraRelativeView, position);
        localLights[i * 3] = {position[0], position[1], position[2], light.cutoff};
        localLights[i * 3 + 1] = {light.uploadedColor[0], light.uploadedColor[1], light.uploadedColor[2], 0.0f};
        localLights[i * 3 + 2] = {light.attenuation[0], light.attenuation[1], light.attenuation[2], 0.0f};
    }
    float marchLightLimit = 0.0f;
    for (uint32_t i = 0; i < pointLightCount; ++i)
    {
        const Float4& positionRadius = localLights[i * 3];
        const double x = positionRadius.x;
        const double y = positionRadius.y;
        const double z = positionRadius.z;
        const float limit = static_cast<float>(std::sqrt(x * x + y * y + z * z) + positionRadius.w + 0.001);
        marchLightLimit = std::max(marchLightLimit, std::nextafter(limit, std::numeric_limits<float>::infinity()));
    }
    const bool marchesLocalLights = pointLightCount > 0;
    if (pointLightCount != m_prevLocalLightCount && (pointLightCount == 0 || m_prevLocalLightCount == 0))
        VF_LOG_INFO("local lights: %u uploaded (%u candidates, %u disabled, %u not point lights)", pointLightCount,
                    in.localLights.candidateCount, in.localLights.skippedDisabled, in.localLights.skippedNonPoint);
    C[53] = {static_cast<float>(pointLightCount), marchLightLimit, cfg.localLightPhase, 0.0f};

    const double windPeriod = static_cast<double>(kDensityNoiseSize) / std::max(cfg.noiseScale, 0.001f);
    C[78] = {cfg.noiseAmount, cfg.noiseScale, static_cast<float>(std::fmod(now * cfg.noiseWindSpeed, windPeriod)), 0.0f};

    const int steps = cfg.quality == 1 ? 16 : cfg.quality == 2 ? 24 : 32;
    id<MTLLibrary> library = impl.Library(steps, marchesLocalLights, samplesNoise, depthMs);
    if (!library)
    {
        impl.failed = true;
        return false;
    }

    // The copies of a multisampled frame resolve in a pass of their own (vf_resolve_copy)
    impl.resolvePipeline = color.sampleCount > 1
        ? impl.Pipeline(library, @"vf_resolve_copy", color.pixelFormat, 1, false, "resolve" + std::to_string(color.pixelFormat))
        : nil;

    // The blend: over a copy of the scene in linear light, or in gamma when god rays need the copy
    const bool wantsCopy = fog.linear || rays;
    const bool copied = wantsCopy && impl.CopyScene(commands, color);
    FogBlend blend = FogBlend::GammaFixedFunction;
    if (fog.linear)
        blend = copied ? FogBlend::LinearOverSceneCopy : FogBlend::LinearFixedFunction;
    else if (rays && copied)
        blend = FogBlend::GammaOverSceneCopy;
    const bool sceneBlend = blend == FogBlend::LinearOverSceneCopy || blend == FogBlend::GammaOverSceneCopy;
    const float blendMode = static_cast<float>(blend);
    if (blendMode != m_loggedBlend)
    {
        m_loggedBlend = blendMode;
        VF_LOG_INFO("fog blend: %s", FogBlendName(blend));
    }

    std::string variant = std::to_string(steps) + (marchesLocalLights ? "L" : "") + (samplesNoise ? "N" : "") + (depthMs ? "M" : "");
    id<MTLRenderPipelineState> marchPipeline =
        impl.Pipeline(library, @"vf_march", MTLPixelFormatRGBA16Float, 1, false, "march" + variant);
    id<MTLRenderPipelineState> temporalPipeline =
        impl.Pipeline(library, @"vf_temporal", MTLPixelFormatRGBA16Float, 1, false, "temporal" + variant);
    id<MTLRenderPipelineState> historyDepthPipeline =
        impl.Pipeline(library, @"vf_history_depth", MTLPixelFormatRGBA8Unorm, 1, false, "historydepth" + variant);
    id<MTLRenderPipelineState> compositePipeline =
        impl.Pipeline(library, @"vf_composite", color.pixelFormat, color.sampleCount, !sceneBlend,
                      "composite" + variant + std::to_string(color.pixelFormat) + "x" + std::to_string(color.sampleCount) +
                          (sceneBlend ? "copy" : "blend"));
    id<MTLRenderPipelineState> gradePipeline =
        impl.Pipeline(library, @"vf_grade", color.pixelFormat, color.sampleCount, false,
                      "grade" + variant + std::to_string(color.pixelFormat) + "x" + std::to_string(color.sampleCount));
    id<MTLRenderPipelineState> downsamplePipeline =
        impl.Pipeline(library, @"vf_downsample", MTLPixelFormatRGBA8Unorm, 1, false, "downsample" + variant);
    id<MTLRenderPipelineState> rayMaskPipeline =
        impl.Pipeline(library, @"vf_ray_mask", MTLPixelFormatRGBA8Unorm, 1, false, "raymask" + variant);
    id<MTLRenderPipelineState> rayBlurPipeline =
        impl.Pipeline(library, @"vf_ray_blur", MTLPixelFormatRGBA8Unorm, 1, false, "rayblur" + variant);
    if (!marchPipeline || !temporalPipeline || !historyDepthPipeline || !compositePipeline || !downsamplePipeline ||
        !rayMaskPipeline || !rayBlurPipeline || !gradePipeline)
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
        [encoder setFragmentTexture:impl.rays[1] ? impl.rays[1] : impl.dummy2d atIndex:2];
        [encoder setFragmentTexture:impl.sceneCopy && sceneBlend ? impl.sceneCopy : impl.dummy2d atIndex:3];
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
        id<MTLRenderCommandEncoder> encoder = ProfiledRenderEncoder(commands, pass, label.UTF8String);
        [encoder setRenderPipelineState:pipeline];
        bindFog(encoder, constants);
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    };

    drawInto(impl.march, marchPipeline, C, @"CoAVolFog march");

    // FilterWithHistory: the temporal pass replaces c9..c13 with the reprojection and its settings, then
    // the history depth of this frame is written for the next one
    const bool adaptiveLightingHistory =
        pointLightCount > 0 || m_prevLocalLightCount > 0 || cfg.noiseAmount > 0.0f || samplesNoise;
    if (temporalFiltering)
    {
        Float4 T[kRegisterCount];
        std::memcpy(T, C, sizeof(T));
        std::memcpy(&T[9], reproj, sizeof(reproj));
        T[13] = {cfg.temporal, historyValid ? 1.0f : 0.0f, adaptiveLightingHistory ? 1.0f : 0.0f, 0.0f};
        drawInto(impl.history[write], temporalPipeline, T, @"CoAVolFog temporal");
        drawInto(impl.historyDepth, historyDepthPipeline, C, @"CoAVolFog history depth");
    }

    // DrawGodRayMask: the bright sky around the sun, then two radial blurs toward it
    const bool raysDrawn = rays && copied && impl.EnsureRays(std::max(1u, vp.Width / kRayScale), std::max(1u, vp.Height / kRayScale));
    if (raysDrawn != m_loggedRays)
    {
        m_loggedRays = raysDrawn;
        VF_LOG_INFO("god rays %s (strength %.2f, sun at %.0f, %.0f)", raysDrawn ? "drawn" : "stopped", rayStrength, sunPx[0], sunPx[1]);
    }
    if (raysDrawn)
    {
        const uint32_t rayW = static_cast<uint32_t>(impl.rays[0].width);
        const uint32_t rayH = static_cast<uint32_t>(impl.rays[0].height);
        {
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = impl.rays[0];
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = ProfiledRenderEncoder(commands, pass, "CoAVolFog god ray scene");
            [encoder setRenderPipelineState:downsamplePipeline];
            [encoder setFragmentTexture:impl.sceneCopy atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        }
        Float4 M[kRegisterCount];
        std::memcpy(M, C, sizeof(M));
        M[9] = {toLightInView[0], toLightInView[1], toLightInView[2], kRayFalloff};
        M[10] = {0.0f, kRayThreshold, 1.0f / rayW, 1.0f / rayH};
        {
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = impl.rays[1];
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = ProfiledRenderEncoder(commands, pass, "CoAVolFog god ray mask");
            [encoder setRenderPipelineState:rayMaskPipeline];
            bindFog(encoder, M);
            [encoder setFragmentTexture:impl.rays[0] atIndex:7];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        }
        float norm = 0.0f;
        for (int k = 0; k < kRayTaps; ++k)
            norm += std::pow(kRayDecay, static_cast<float>(k));
        const float sunUv[2] = {(sunPx[0] - vp.X) / vp.Width, (sunPx[1] - vp.Y) / vp.Height};
        float step = kRayStep;
        for (int blurPass = 0; blurPass < 2; ++blurPass)
        {
            Float4 B[11] = {};
            B[9] = {sunUv[0], sunUv[1], step, 1.0f / norm};
            B[10] = {static_cast<float>(rayW), static_cast<float>(rayH), 1.0f / rayW, 1.0f / rayH};
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = impl.rays[blurPass == 0 ? 0 : 1];
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = ProfiledRenderEncoder(commands, pass, "CoAVolFog god ray blur");
            [encoder setRenderPipelineState:rayBlurPipeline];
            [encoder setFragmentBytes:B length:sizeof(B) atIndex:0];
            [encoder setFragmentTexture:impl.rays[blurPass == 0 ? 1 : 0] atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            step /= kRayTaps;
        }
    }

    // Composite: with local lights its full-resolution march takes the noise at its mean
    const FogParams compositeMarchFog = marchesLocalLights ? WithMeanNoise(fog) : fog;
    std::memcpy(&C[12], &compositeMarchFog.layers[0].start, sizeof(compositeMarchFog.layers));
    C[96] = {hasAuthored ? cfg.classicExposure : cfg.exposure, raysDrawn && sceneBlend ? rayStrength : 0.0f,
             static_cast<float>(cfg.debugView), blendMode};
    C[97] = {fog.rayColor[0], fog.rayColor[1], fog.rayColor[2], 0.0f};
    C[98] = {sunPx[0], sunPx[1], cfg.sunMarker && sunInFront ? 1.0f : 0.0f,
             cfg.glowCompensation && sceneBlend ? in.clientGlowAmount : 0.0f};
    // PrepareGrading and Grade (forever_look.cpp, grading_renderer.cpp): the Classic light's curve over the
    // fogged world, by ColorGrading times the Classic lights' share of the blend; never under the ghost effect
    // or in liquid (this pass does not run in liquid). Over a scene copy the composite writes every pixel, so
    // it grades in the same pass; after a fixed-function blend a copy of the fogged frame is graded.
    const float gradingStrength = hasAuthored ? cfg.colorGrading * ForeverLookWeight(authored.coverage) : 0.0f;
    const bool wantsGrading = gradingStrength > 0.0f && authored.hasGradingCurve && !engine::GhostEffectRuns();
    const bool gradesInComposite = wantsGrading && sceneBlend;
    C[95] = {gradesInComposite ? gradingStrength : 0.0f, 0.0f, 0.0f, 0.0f};
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color;
        // Over the scene copy every pixel is written: the frame's samples need not be loaded
        pass.colorAttachments[0].loadAction = sceneBlend ? MTLLoadActionDontCare : MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = ProfiledRenderEncoder(commands, pass, "CoAVolFog composite");
        [encoder setRenderPipelineState:compositePipeline];
        bindFog(encoder, C);
        static const float kNoCurve[32] = {};
        [encoder setFragmentBytes:gradesInComposite ? authored.gradingCurve : kNoCurve length:sizeof(kNoCurve) atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    }

    const bool grading = gradesInComposite || (wantsGrading && impl.CopyGraded(commands, color));
    if (grading != m_loggedGrading)
    {
        m_loggedGrading = grading;
        VF_LOG_INFO("colour grading %s (strength %.2f)", grading ? "drawn" : "idle", gradingStrength);
    }
    if (grading && !gradesInComposite)
    {
        Float4 G[2] = {{static_cast<float>(vp.X), static_cast<float>(vp.Y), 1.0f / vp.Width, 1.0f / vp.Height},
                       {gradingStrength, 0.0f, 0.0f, 0.0f}};
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = ProfiledRenderEncoder(commands, pass, "CoAVolFog colour grading");
        [encoder setRenderPipelineState:gradePipeline];
        [encoder setFragmentBytes:G length:sizeof(G) atIndex:0];
        [encoder setFragmentBytes:authored.gradingCurve length:sizeof(authored.gradingCurve) atIndex:1];
        [encoder setFragmentTexture:impl.gradingCopy atIndex:0];
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
    m_prevLocalLightCount = pointLightCount;
    m_prevSeconds = now;
    m_historyValid = temporalFiltering;
    if (temporalFiltering)
        m_historyIndex = write;
    ++m_frame;
    return true;
}
