// WaterRenderer: water_renderer.cpp of the Windows CoAVolFog on Metal. The constants are built as the Windows
// renderer builds them (BuildCommonConstants, FillClassConstants, BuildReflectionFog) and uploaded under the
// same register numbers; the passes follow WaterRenderer::Begin and ShadeTaggedWater.

#include "water_metal.h"

#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "Engine/Source/Gx/Gx.h"
#include "WMModAPI.h"
#include "engine.h"
#include "fog_data.h"
#include "fog_model.h"
#include "log.h"
#include "vw_water_source.h"
#include "water_classify.h"
#include "water_data.h"
#include "water_fft_metal.h"
#include "water_spectrum.h"

namespace
{
constexpr int kWaterQualityLevels = 3;
constexpr int kWaterSkyBands = 5;
constexpr int kWaterShadedMaskSlots = 5;
constexpr int kCommonConstants = 9;
constexpr int kShoreFoamSlot = static_cast<int>(WaterMaskSlot::ShoreFoam);
constexpr int kDepthFoamSlot = static_cast<int>(WaterMaskSlot::DepthFoam);
constexpr float kRippleEdgeRampPerExtent = 1.0f / 16.0f;
constexpr float kRippleFadeYards = 4.0f;
constexpr float kWaterMaxViewDepth = 65536.0f;
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr uint32_t kMinWorldViewportSide = 16;
constexpr float kDisplayGamma = 2.2f;
constexpr float kSunVisibilityHalfWidth = 0.1f;
constexpr float kWaterF0 = 0.02f;
constexpr int kSchlickExponent = 5;
constexpr float kMinClarity = 0.01f;
constexpr float kShoreDistancePerDepth = 4.0f;
constexpr float kMinStockFogRange = 1e-3f;
constexpr double kMaxFoamStepSeconds = 0.1;
constexpr int kLowQuality = 1;
constexpr int kSkyReflectionsOnlyVariant = 0;
constexpr float kReflectionFogActive = 1.0f;
constexpr int kFftResolutionLow = 128;
constexpr int kFftResolution = 256;
constexpr int kFftReferenceResolution = 256;
constexpr float kWindDirection[2] = {0.8f, 0.6f};
constexpr int kSkyTop = 0;
constexpr int kSkyMiddle = 1;
constexpr int kSkyUpperBand = 2;
constexpr int kSkyLowerBand = 3;
constexpr int kSkyHorizon = 4;
constexpr int kSkyColourOfBand[kWaterSkyBands] = {kSkyTop, kSkyMiddle, kSkyUpperBand, kSkyLowerBand, kSkyHorizon};
constexpr int kCloseWaterColour = 0;
constexpr int kFarWaterColour = 1;
constexpr int kSsrSteps[kWaterQualityLevels] = {0, 16, 32};

// The texture slots of vw_water (the sampler stages of water_renderer.cpp)
constexpr int kSceneColourStage = 0;
constexpr int kSceneDepthStage = 1;
constexpr int kWaterDepthStage = 2;
constexpr int kFirstSurfaceStage = 3;
constexpr int kFirstFoamStateStage = 7;
constexpr int kWaveFoamMaskStage = 11;
constexpr int kRippleStage = 12;
constexpr int kShoreFoamMaskStage = 14;
constexpr int kDepthFoamMaskStage = 15;

struct Float4
{
    float x, y, z, w;
};

struct ReflectionFogLayer
{
    Float4 curve;
    Float4 height;
    Float4 emissive;
    Float4 diffuse;
    Float4 scattering;
};

struct ReflectionFog
{
    ReflectionFogLayer layers[kFogLayers];
    Float4 range;
};

// c9..c67 (vw_constants.hlsli)
struct ShadingConstants
{
    Float4 light;
    Float4 sunColour;
    Float4 ambient;
    Float4 isotropicLight;
    Float4 sky[kWaterSkyBands];
    Float4 stockFogColour;
    Float4 stockFog;
    Float4 absorption;
    Float4 scatteringIntensities;
    Float4 scatteringTop;
    Float4 scatteringBottom;
    Float4 depthFadeFoam;
    Float4 shoreFoam;
    Float4 waveFoam;
    Float4 waveFoamScaling;
    Float4 surfaceResponse;
    Float4 inverseTileSizes;
    Float4 waveControl;
    Float4 foamScroll;
    Float4 depthFoamScroll;
    Float4 depthDecode;
    Float4 maskTints[kWaterShadedMaskSlots * 2];
    ReflectionFog reflectionFog;
    Float4 rippleWindow;
    Float4 rippleShape;
    Float4 rippleFade;
};

static_assert(sizeof(ShadingConstants) == (68 - kCommonConstants) * sizeof(Float4), "the shading constants are c9..c67");

struct PassConstants
{
    Float4 common[kCommonConstants];
    ShadingConstants shading;
};

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

bool BuildCommonConstants(const FrameInputs& in, uint32_t depthWidth, uint32_t depthHeight, Float4 (&common)[kCommonConstants])
{
    float viewToWorld[16];
    if (!Invert4x4(in.cameraRelativeView, viewToWorld))
        return false;
    viewToWorld[12] = in.camPos[0];
    viewToWorld[13] = in.camPos[1];
    viewToWorld[14] = in.camPos[2];
    const Viewport& vp = in.viewport;
    const float* proj = in.glProjection;
    const WorldDepthMapping depth = MapWorldDepth(proj, vp);
    const float width = static_cast<float>(vp.Width);
    const float height = static_cast<float>(vp.Height);
    common[0] = {static_cast<float>(vp.X), static_cast<float>(vp.Y), width, height};
    common[1] = {1.0f, 0.0f, 1.0f / depthWidth, 1.0f / depthHeight};
    common[2] = {proj[0], proj[5], proj[8], proj[9]};
    common[3] = {depth.atInfinity, depth.perInverseViewDepth, kWaterMaxViewDepth, depth.deepest};
    std::memcpy(&common[4], viewToWorld, sizeof(viewToWorld));
    common[8] = {width, height, 1.0f / width, 1.0f / height};
    return true;
}

float Saturate(float x)
{
    return std::clamp(x, 0.0f, 1.0f);
}

float SmoothStep(float edge0, float edge1, float x)
{
    const float t = Saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

float FresnelSchlick(float cosine)
{
    return kWaterF0 + (1.0f - kWaterF0) * std::pow(1.0f - Saturate(cosine), static_cast<float>(kSchlickExponent));
}

void LinearColour(uint32_t argb, float* rgb)
{
    UnpackColor(argb, rgb);
    for (int c = 0; c < 3; ++c)
        rgb[c] = std::pow(rgb[c], kDisplayGamma);
}

void ClampedTint(const float* linearTint, float* rgb)
{
    for (int c = 0; c < 3; ++c)
        rgb[c] = Saturate(linearTint[c]);
}

float ScrollOffset(double seconds, int axis, float multiplier)
{
    const double travelled = seconds * kWindDirection[axis] * multiplier;
    return static_cast<float>(travelled - std::floor(travelled));
}

uint32_t MipSide(uint32_t size, uint32_t level)
{
    const uint32_t side = size >> level;
    return side ? side : 1;
}

int QualityIndex(const Config& cfg)
{
    return std::clamp(cfg.waterQuality, 1, kWaterQualityLevels) - 1;
}

int ShadingVariant(const Config& cfg)
{
    return cfg.waterReflections > 0.0f ? QualityIndex(cfg) : kSkyReflectionsOnlyVariant;
}

struct FoamMaskTexture
{
    int32_t sources[kWaveFoamMaskSlots];
    bool packed;
    bool present[kWaveFoamMaskSlots];
    id<MTLTexture> texture;
};
}

struct WaterRenderer::Impl
{
    id<MTLDevice> device = nil;
    std::map<int, id<MTLLibrary>> libraries;
    std::map<std::string, id<MTLRenderPipelineState>> pipelines;
    id<MTLDepthStencilState> classStencil = nil;
    id<MTLDepthStencilState> noDepth = nil;
    std::unique_ptr<WaterFft> fft;
    id<MTLTexture> sceneColour = nil;
    id<MTLTexture> sceneDepth = nil;
    id<MTLTexture> waterDepth = nil;
    id<MTLTexture> flat = nil;
    std::vector<FoamMaskTexture> foamMasks;
    uint32_t maskRevision = 0;
    bool masksPlanned = false;
    bool masksUploaded = false;
    bool failed = false;

    // The frame being drawn (Begin to End)
    bool armed = false;
    bool tagged = false;
    bool depthForced = false;
    FrameInputs in = {};
    WaterInputs water = {};
    Config cfg;
    const WaterData* data = nullptr;
    PassConstants constants = {};
    ReflectionFog reflectionFog = {};
    unsigned draws[kWaterClassCount] = {};
    uint32_t waveTiles = 0;
    WaterFftSettings waveSettings = {};
    bool wavesSimulated = false;
    double lastSeconds = -1.0;
    double loggedLightSeconds = -1.0;
    Float4 rippleWindow = {};
    Float4 rippleShape = {};
    Float4 rippleFade = {};

    // Logging (once per change, as the Windows build)
    const char* skip = "";
    const char* loggedSkip = nullptr;
    const char* loggedWaveState = nullptr;
    bool loggedFirstShade = false;
    uint32_t loggedCopyW = 0;
    uint32_t loggedCopyH = 0;

    bool Skip(const char* reason)
    {
        skip = reason;
        return false;
    }

    id<MTLLibrary> Library(int ssrSteps)
    {
        auto found = libraries.find(ssrSteps);
        if (found != libraries.end())
            return found->second;
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.preprocessorMacros = @{@"SSR_STEPS" : @(ssrSteps)};
        // The wave simulation is checked against a double-precision reference: no fast math
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(kVwWaterSource) options:options error:&error];
        if (!library)
            VF_LOG_ERROR("water shaders (SSR %d) failed to compile: %s", ssrSteps, error.localizedDescription.UTF8String);
        libraries[ssrSteps] = library;
        return library;
    }

    id<MTLRenderPipelineState> Pipeline(id<MTLLibrary> library, NSString* fragment, MTLPixelFormat colour, NSUInteger samples,
                                        MTLPixelFormat depthStencil, bool rgbOnly, const std::string& key)
    {
        auto found = pipelines.find(key);
        if (found != pipelines.end())
            return found->second;
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"vw_fullscreen"];
        descriptor.fragmentFunction = [library newFunctionWithName:fragment];
        descriptor.colorAttachments[0].pixelFormat = colour;
        if (rgbOnly)
            descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        descriptor.depthAttachmentPixelFormat = depthStencil;
        descriptor.stencilAttachmentPixelFormat = depthStencil == MTLPixelFormatDepth32Float_Stencil8 ? depthStencil
                                                                                                    : MTLPixelFormatInvalid;
        descriptor.rasterSampleCount = samples;
        NSError* error = nil;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline)
            VF_LOG_ERROR("water pipeline %s failed: %s", key.c_str(), error.localizedDescription.UTF8String);
        pipelines[key] = pipeline;
        return pipeline;
    }

    id<MTLTexture> Target(MTLPixelFormat format, uint32_t width, uint32_t height)
    {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width
                                                                                             height:height mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModePrivate;
        return [device newTextureWithDescriptor:descriptor];
    }

    void EnsureStatic()
    {
        if (!flat)
        {
            // m_flat: one black texel, no waves, no foam and no masks
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                  width:1 height:1 mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;
            flat = [device newTextureWithDescriptor:descriptor];
            const uint32_t black = 0;
            [flat replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
        }
        if (!classStencil)
        {
            MTLStencilDescriptor* stencil = [MTLStencilDescriptor new];
            stencil.stencilCompareFunction = MTLCompareFunctionEqual;
            stencil.readMask = 0xFF;
            stencil.writeMask = 0;
            MTLDepthStencilDescriptor* state = [MTLDepthStencilDescriptor new];
            state.depthCompareFunction = MTLCompareFunctionAlways;
            state.depthWriteEnabled = NO;
            state.frontFaceStencil = stencil;
            state.backFaceStencil = stencil;
            classStencil = [device newDepthStencilStateWithDescriptor:state];
            MTLDepthStencilDescriptor* none = [MTLDepthStencilDescriptor new];
            none.depthCompareFunction = MTLCompareFunctionAlways;
            none.depthWriteEnabled = NO;
            noDepth = [device newDepthStencilStateWithDescriptor:none];
        }
    }

    // EnsureCopies: the world's colour in the target's format, its linear depth and the water's in R32F
    bool EnsureCopies(MTLPixelFormat colourFormat, uint32_t w, uint32_t h)
    {
        if (sceneColour && sceneColour.width == w && sceneColour.height == h && sceneColour.pixelFormat == colourFormat)
            return true;
        sceneColour = Target(colourFormat, w, h);
        sceneDepth = Target(MTLPixelFormatR32Float, w, h);
        waterDepth = Target(MTLPixelFormatR32Float, w, h);
        if (!sceneColour || !sceneDepth || !waterDepth)
        {
            sceneColour = sceneDepth = waterDepth = nil;
            VF_LOG_ERROR("water scene copies could not be created (%ux%u); water keeps the client's shading", w, h);
            return Skip("water copy creation failed");
        }
        if (w != loggedCopyW || h != loggedCopyH)
            VF_LOG_INFO("water copies: %ux%u, depth r32f", w, h);
        loggedCopyW = w;
        loggedCopyH = h;
        return true;
    }

    // PlanFoamMasks and EnsureMasks: the packed wave foam masks of each preset and its single shore and depth masks
    void EnsureMasks()
    {
        if (masksPlanned && maskRevision == data->Revision() && masksUploaded)
            return;
        if (!masksPlanned || maskRevision != data->Revision())
        {
            foamMasks.clear();
            auto plan = [this](const int32_t (&sources)[kWaveFoamMaskSlots], bool packed) {
                for (const FoamMaskTexture& mask : foamMasks)
                    if (mask.packed == packed && std::equal(std::begin(sources), std::end(sources), mask.sources))
                        return;
                FoamMaskTexture mask = {{sources[0], sources[1], sources[2]}, packed, {}, nil};
                foamMasks.push_back(mask);
            };
            for (const WaterPreset& preset : data->Presets())
            {
                const int32_t wave[kWaveFoamMaskSlots] = {preset.masks[static_cast<int>(WaterMaskSlot::HighFoam)],
                                                          preset.masks[static_cast<int>(WaterMaskSlot::MidFoam)],
                                                          preset.masks[static_cast<int>(WaterMaskSlot::LowFoam)]};
                if (std::any_of(std::begin(wave), std::end(wave), [](int32_t index) { return index >= 0; }))
                    plan(wave, true);
                for (int slot : {kShoreFoamSlot, kDepthFoamSlot})
                {
                    const int32_t single[kWaveFoamMaskSlots] = {preset.masks[slot], kWaterNoIndex, kWaterNoIndex};
                    if (single[0] >= 0)
                        plan(single, false);
                }
            }
            masksPlanned = true;
            maskRevision = data->Revision();
        }
        int uploaded = 0;
        for (FoamMaskTexture& mask : foamMasks)
        {
            if (!mask.texture && mask.packed)
            {
                const WaterPackedMask packed = PackWaveFoamMasks(data->Masks(), mask.sources);
                // A8R8G8B8 texels: BGRA8 in memory
                mask.texture = MaskTexture(MTLPixelFormatBGRA8Unorm, packed.size, packed.levels.size(), 4,
                                           [&](size_t level) { return static_cast<const void*>(packed.levels[level].data()); });
                std::copy(std::begin(packed.present), std::end(packed.present), mask.present);
            }
            else if (!mask.texture)
            {
                const WaterMaskLevels& levels = data->Masks()[mask.sources[0]];
                // L8: R8, read through .r
                mask.texture = MaskTexture(MTLPixelFormatR8Unorm, levels.info.size, levels.levels.size(), 1,
                                           [&](size_t level) { return static_cast<const void*>(levels.levels[level].data()); });
            }
            uploaded += mask.texture ? 1 : 0;
        }
        masksUploaded = uploaded == static_cast<int>(foamMasks.size());
        VF_LOG_INFO("water foam masks uploaded: %d of %d", uploaded, static_cast<int>(foamMasks.size()));
    }

    template <typename LevelBytes>
    id<MTLTexture> MaskTexture(MTLPixelFormat format, uint32_t size, size_t levels, uint32_t texelBytes, LevelBytes bytes)
    {
        if (size == 0 || levels == 0)
            return nil;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:size
                                                                                             height:size mipmapped:levels > 1];
        descriptor.mipmapLevelCount = levels;
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
        for (size_t level = 0; texture && level < levels; ++level)
        {
            const uint32_t side = MipSide(size, static_cast<uint32_t>(level));
            [texture replaceRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:level withBytes:bytes(level)
                       bytesPerRow:side * texelBytes];
        }
        return texture;
    }

    const FoamMaskTexture* SingleMask(int32_t index) const
    {
        for (const FoamMaskTexture& mask : foamMasks)
            if (!mask.packed && mask.sources[0] == index && mask.texture)
                return &mask;
        return nullptr;
    }

    const FoamMaskTexture* WaveFoamMasks(const WaterPreset& preset) const
    {
        for (const FoamMaskTexture& mask : foamMasks)
            if (mask.packed && mask.texture && std::equal(mask.sources, mask.sources + kWaveFoamMaskSlots, preset.masks))
                return &mask;
        return nullptr;
    }

    id<MTLTexture> SingleMaskTexture(int32_t index) const
    {
        const FoamMaskTexture* mask = SingleMask(index);
        return mask ? mask->texture : nil;
    }

    bool MaskPresent(const WaterPreset& preset, int slot) const
    {
        if (slot >= kWaveFoamMaskSlots)
            return SingleMaskTexture(preset.masks[slot]) != nil;
        const FoamMaskTexture* wave = WaveFoamMasks(preset);
        return wave && wave->present[slot];
    }

    // CopySceneColour: the world before the liquids, resolved when multisampled
    void CopySceneColour(id<MTLCommandBuffer> commands, id<MTLTexture> color)
    {
        if (color.sampleCount > 1)
        {
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = color;
            pass.colorAttachments[0].resolveTexture = sceneColour;
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            encoder.label = @"CoAVolFog water scene copy";
            [encoder endEncoding];
            return;
        }
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        blit.label = @"CoAVolFog water scene copy";
        [blit copyFromTexture:color toTexture:sceneColour];
        [blit endEncoding];
    }

    // CopyLinearDepth: the view depth of every pixel of the world viewport (sample 0 when multisampled)
    bool CopyLinearDepth(id<MTLCommandBuffer> commands, id<MTLTexture> depth, id<MTLTexture> copy, NSString* label)
    {
        id<MTLLibrary> library = Library(0);
        const bool multisampled = depth.textureType == MTLTextureType2DMultisample;
        id<MTLRenderPipelineState> pipeline = library ? Pipeline(library, multisampled ? @"vw_depth_ms" : @"vw_depth",
                                                                 MTLPixelFormatR32Float, 1, MTLPixelFormatInvalid, false,
                                                                 multisampled ? "depth_ms" : "depth")
                                                      : nil;
        if (!pipeline)
            return false;
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = copy;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        encoder.label = label;
        [encoder setRenderPipelineState:pipeline];
        [encoder setFragmentBytes:constants.common length:sizeof(constants.common) atIndex:0];
        [encoder setFragmentTexture:depth atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return true;
    }

    // ClearWaterStencil: no pixel of the world is tagged before the liquids
    void ClearWaterStencil(id<MTLCommandBuffer> commands, id<MTLTexture> depth)
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.texture = depth;
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.clearStencil = 0;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.renderTargetWidth = depth.width;
        pass.renderTargetHeight = depth.height;
        pass.defaultRasterSampleCount = depth.sampleCount;
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        encoder.label = @"CoAVolFog water stencil clear";
        [encoder endEncoding];
    }

    bool AnyClassDrawn() const
    {
        for (int index = 1; index < kWaterClassCount; ++index)
            if (draws[index])
                return true;
        return false;
    }

    uint32_t DrawnTileMask() const
    {
        uint32_t mask = 0;
        for (int index = 1; index < kWaterClassCount; ++index)
        {
            const WaterPreset* preset = draws[index] ? data->Preset(static_cast<WaterClass>(index)) : nullptr;
            if (!preset)
                continue;
            for (int32_t tile : preset->tiles)
                if (tile >= 0 && tile < kWaterMaxTiles && static_cast<size_t>(tile) < data->Tiles().size())
                    mask |= 1u << tile;
        }
        return mask;
    }

    // PrepareWaves and SimulateWaves
    bool SimulateWaves(id<MTLCommandBuffer> commands, id<MTLLibrary> library, double seconds)
    {
        waveTiles = DrawnTileMask();
        waveSettings = {};
        waveSettings.resolution = cfg.waterQuality == kLowQuality ? kFftResolutionLow : kFftResolution;
        waveSettings.referenceResolution = kFftReferenceResolution;
        waveSettings.windSpeed = cfg.waterWind;
        waveSettings.windDirection[0] = kWindDirection[0];
        waveSettings.windDirection[1] = kWindDirection[1];
        waveSettings.amplitudeScale = cfg.waterWaves * cfg.waterWaves;
        const bool attempted = waveTiles != 0 && cfg.waterWaves > 0.0f;
        if (!attempted)
            return false;
        if (!fft)
            fft = std::make_unique<WaterFft>(device, library);
        const double step = lastSeconds < 0.0 ? 0.0 : std::clamp(seconds - lastSeconds, 0.0, kMaxFoamStepSeconds);
        const bool simulated = fft->Simulate(commands, waveSettings, data->Tiles(), waveTiles, seconds, static_cast<float>(step));
        // LogWaveState
        const char* state = simulated ? "" : fft->LastFailure();
        if (!loggedWaveState || std::strcmp(state, loggedWaveState) != 0)
        {
            if (simulated)
                VF_LOG_INFO("water waves simulated (tiles 0x%02X, %d texels, %u dispatches)", waveTiles,
                            waveSettings.resolution, fft->Dispatches());
            else
                VF_LOG_INFO("water waves unavailable, shading flat water: %s", state);
            loggedWaveState = state;
        }
        return simulated;
    }

    // FillRippleConstants without ripples (slice 8): an empty window, no gain
    void FillRippleConstants()
    {
        const float extent = 1.0f;
        rippleWindow = {0.0f, 0.0f, 1.0f / extent, 0.0f};
        rippleShape = {0.0f, 0.0f, extent, 0.0f};
        rippleFade = {extent * (0.5f - kRippleEdgeRampPerExtent), 1.0f / kRippleFadeYards, 1.0f, 0.0f};
    }

    void BuildReflectionFog(const FogData& fogData)
    {
        reflectionFog = {};
        if (water.stockFogApplies)
            return;
        AuthoredFog authored = {};
        const bool hasAuthored = cfg.dataMode == 1 && fogData.Loaded() &&
                                 fogData.Resolve(in.mapId, in.camPos, in.dayFraction, in.lightParams, authored);
        const FogParams fog = WithMeanNoise(BuildFogParams(in, cfg, hasAuthored ? &authored : nullptr));
        const float exposure = fog.authored ? cfg.classicExposure : cfg.exposure;
        auto linearScattered = [&fog, exposure](const float* rgb) {
            float out[3];
            for (int c = 0; c < 3; ++c)
                out[c] = (fog.linear ? rgb[c] : std::pow(std::max(rgb[c], 0.0f), kDisplayGamma)) * exposure;
            return Float4{out[0], out[1], out[2], 0.0f};
        };
        for (int i = 0; i < kFogLayers; ++i)
        {
            const FogLayer& layer = fog.layers[i];
            const float shadow = layer.shadowed * (1.0f - fog.lightAboveHorizon);
            float emissive[3];
            float diffuse[3];
            for (int c = 0; c < 3; ++c)
            {
                emissive[c] = layer.emissive[c] + (layer.shadowEmissive[c] - layer.emissive[c]) * shadow;
                diffuse[c] = layer.diffuse[c] * (1.0f - shadow);
            }
            const float density = layer.density * (1.0f + (layer.shadowDensity - 1.0f) * shadow);
            ReflectionFogLayer& out = reflectionFog.layers[i];
            out.curve = {layer.start, density, layer.strength, layer.exponent};
            out.height = {layer.upperHeight, layer.upperFalloff, layer.lowerHeight, layer.lowerFalloff};
            out.emissive = linearScattered(emissive);
            out.diffuse = linearScattered(diffuse);
            out.scattering = {layer.g, layer.isotropic, layer.endDistance, layer.skyFalloff};
        }
        reflectionFog.range = {fog.maxDistance, fog.maxDistance, 0.0f, kReflectionFogActive};
    }

    void FillClassConstants(ShadingConstants& c, const WaterPreset& preset, WaterClass waterClass, double seconds) const
    {
        c = {};
        const bool noSun = waterClass == WaterClass::Interior;
        float toLight[3] = {in.toLight[0], in.toLight[1], in.toLight[2]};
        const float length = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
        for (float& axis : toLight)
            axis = length > 1e-6f ? axis / length : 0.0f;
        const float sunVisibility = noSun ? 0.0f : SmoothStep(-kSunVisibilityHalfWidth, kSunVisibilityHalfWidth, toLight[2]);
        const float sunTransmission = noSun ? 1.0f : 1.0f - FresnelSchlick(Saturate(toLight[2]));
        float sun[3];
        LinearColour(in.directColor, sun);
        for (float& channel : sun)
            channel *= in.lightIsMoon ? kMoonLightScale : 1.0f;
        float sky[kSkyColorCount][3];
        float ambient[3] = {};
        for (int i = 0; i < kSkyColorCount; ++i)
        {
            LinearColour(water.skyColors[i], sky[i]);
            for (int channel = 0; channel < 3; ++channel)
                ambient[channel] += sky[i][channel] / kSkyColorCount;
        }
        if (noSun)
            LinearColour(in.ambientColor, ambient);

        c.light = {toLight[0], toLight[1], toLight[2], sunVisibility};
        c.sunColour = {sun[0], sun[1], sun[2], sunTransmission};
        c.ambient = {ambient[0], ambient[1], ambient[2], 0.0f};
        c.isotropicLight = noSun ? Float4{1.0f, 1.0f, 1.0f, 0.0f}
                                 : Float4{ambient[0] + sunVisibility * sun[0], ambient[1] + sunVisibility * sun[1],
                                          ambient[2] + sunVisibility * sun[2], 0.0f};
        for (int band = 0; band < kWaterSkyBands; ++band)
        {
            const float* colour = sky[kSkyColourOfBand[band]];
            c.sky[band] = noSun ? Float4{} : Float4{colour[0], colour[1], colour[2], 0.0f};
        }
        if (water.stockFogApplies)
        {
            float fog[3];
            UnpackColor(in.fogColor, fog);
            c.stockFogColour = {fog[0], fog[1], fog[2], 1.0f};
            c.stockFog = {in.fogEnd, 1.0f / std::max(in.fogEnd - in.fogStart, kMinStockFogRange), 0.0f, 0.0f};
        }

        const float absorptionScale = preset.absorption[3] / std::max(cfg.waterClarity, kMinClarity);
        c.absorption = {preset.absorption[0] * absorptionScale, preset.absorption[1] * absorptionScale,
                        preset.absorption[2] * absorptionScale, 0.0f};
        c.scatteringIntensities = {preset.scatteringIntensities[0], preset.scatteringIntensities[1],
                                   preset.scatteringIntensities[2], preset.scatteringIntensities[3]};
        const uint32_t* zone = waterClass == WaterClass::Ocean ? water.oceanColors : water.riverColors;
        float closeWater[3];
        float farWater[3];
        LinearColour(zone[kCloseWaterColour], closeWater);
        LinearColour(zone[kFarWaterColour], farWater);
        const float zoneWeight = Saturate(cfg.waterZoneColors);
        auto towardZone = [zoneWeight](const float* authored, const float* zoneColour, float w) {
            return Float4{authored[0] + (zoneColour[0] - authored[0]) * zoneWeight,
                          authored[1] + (zoneColour[1] - authored[1]) * zoneWeight,
                          authored[2] + (zoneColour[2] - authored[2]) * zoneWeight, w};
        };
        c.scatteringTop = towardZone(preset.scatteringTop, closeWater, preset.scatteringTop[3]);
        c.scatteringBottom = towardZone(preset.scatteringBottom, farWater, preset.scatteringBottom[3]);

        const float foam = std::max(cfg.waterFoam, 0.0f);
        c.depthFadeFoam = {std::max(preset.depthFadeFoam[0], 0.0f) * foam, preset.depthFadeFoam[1], preset.depthFadeFoam[2], 0.0f};
        c.shoreFoam = {std::max(preset.shoreFoam[0], 0.0f) * foam, preset.shoreFoam[1], preset.shoreFoam[2], kShoreDistancePerDepth};
        c.waveFoam = {std::max(preset.waveFoam[0], 0.0f) * foam, 0.0f, 0.0f, 0.0f};
        c.waveFoamScaling = {preset.waveFoamScaling[0], preset.waveFoamScaling[1], preset.waveFoamScaling[2], 0.0f};
        c.surfaceResponse = {preset.roughness[0], preset.roughness[1], preset.roughness[2] * cfg.waterReflections,
                             cfg.waterSpecular};

        float inverse[kWaterPresetTiles] = {};
        int tileCount = 0;
        for (int k = 0; k < kWaterPresetTiles; ++k)
        {
            const int32_t tile = preset.tiles[k];
            if (tile < 0 || static_cast<size_t>(tile) >= data->Tiles().size() || !(data->Tiles()[tile].size > 0.0f))
                continue;
            inverse[k] = 1.0f / data->Tiles()[tile].size;
            ++tileCount;
        }
        c.inverseTileSizes = {inverse[0], inverse[1], inverse[2], inverse[3]};
        c.waveControl = {1.0f / static_cast<float>(std::max(tileCount, 1)), 0.0f, static_cast<float>(waterClass),
                         static_cast<float>(cfg.waterDebugView)};
        c.foamScroll = {ScrollOffset(seconds, 0, preset.waveFoam[1]), ScrollOffset(seconds, 1, preset.waveFoam[1]),
                        ScrollOffset(seconds, 0, preset.shoreFoam[3]), ScrollOffset(seconds, 1, preset.shoreFoam[3])};
        c.depthFoamScroll = {ScrollOffset(seconds, 0, preset.depthFadeFoam[3]), ScrollOffset(seconds, 1, preset.depthFadeFoam[3]),
                             0.0f, 0.0f};
        if (!noSun)
            c.reflectionFog = reflectionFog;
        c.depthDecode = {1.0f, 0.0f, 0.0f, 0.0f};
        c.rippleWindow = rippleWindow;
        c.rippleShape = rippleShape;
        c.rippleFade = rippleFade;
        for (int slot = 0; slot < kWaterShadedMaskSlots; ++slot)
        {
            const int32_t mask = preset.masks[slot];
            if (!MaskPresent(preset, slot))
                continue;
            const WaterMask& info = data->Masks()[mask].info;
            float low[3];
            float high[3];
            ClampedTint(info.tintLow, low);
            ClampedTint(info.tintHigh, high);
            c.maskTints[slot * 2] = {low[0], low[1], low[2], 1.0f};
            c.maskTints[slot * 2 + 1] = {high[0], high[1], high[2], 0.0f};
        }
    }

    void BindClassTextures(id<MTLRenderCommandEncoder> encoder, const WaterPreset& preset)
    {
        for (int k = 0; k < kWaterPresetTiles; ++k)
        {
            const int32_t tile = preset.tiles[k];
            id<MTLTexture> surface = wavesSimulated && tile >= 0 ? fft->Surface(tile) : nil;
            id<MTLTexture> foam = wavesSimulated && tile >= 0 ? fft->Foam(tile) : nil;
            [encoder setFragmentTexture:surface ? surface : flat atIndex:kFirstSurfaceStage + k];
            [encoder setFragmentTexture:foam ? foam : flat atIndex:kFirstFoamStateStage + k];
        }
        const FoamMaskTexture* wave = WaveFoamMasks(preset);
        id<MTLTexture> shore = SingleMaskTexture(preset.masks[kShoreFoamSlot]);
        id<MTLTexture> depth = SingleMaskTexture(preset.masks[kDepthFoamSlot]);
        [encoder setFragmentTexture:wave ? wave->texture : flat atIndex:kWaveFoamMaskStage];
        [encoder setFragmentTexture:shore ? shore : flat atIndex:kShoreFoamMaskStage];
        [encoder setFragmentTexture:depth ? depth : flat atIndex:kDepthFoamMaskStage];
    }
};

void WaterRenderer::Release()
{
    if (m_impl && m_impl->depthForced)
        GxMetalForceDepthWrite(false);
    delete m_impl;
    m_impl = nullptr;
}

// WaterRenderer::Begin with OnLiquidSurfaceBegin (the forced depth writes)
void WaterRenderer::LiquidsBegin(const WMModHost* host, const Config& cfg, const WaterData& data, bool fogDrewLastFrame,
                                 bool stockFogPushed)
{
    if (!m_impl)
    {
        m_impl = new Impl();
        m_impl->device = (__bridge id<MTLDevice>)host->device;
    }
    Impl& impl = *m_impl;
    impl.armed = false;
    impl.tagged = false;
    impl.skip = "";
    std::fill(std::begin(impl.draws), std::end(impl.draws), 0u);

    auto arm = [&]() -> bool {
        if (!cfg.water)
            return impl.Skip("water disabled");
        if (impl.failed)
            return impl.Skip("water shaders unavailable");
        if (!data.Loaded())
            return impl.Skip("no water data");

        WMFrameTargets targets = {};
        if (!host->AcquireFrameTargets(&targets) || !targets.depth)
            return impl.Skip("fog depth unavailable");
        const WMWorldState* world = host->GetWorldState();
        FrameInputs in;
        if (!world || !engine::BuildFrameInputs(*world, targets.width, targets.height, in))
            return impl.Skip("invalid frame inputs");
        if (in.inLiquid)
            return impl.Skip("camera under water");

        id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)targets.commandBuffer;
        id<MTLTexture> color = (__bridge id<MTLTexture>)targets.color;
        id<MTLTexture> depth = (__bridge id<MTLTexture>)targets.depth;
        if (depth.pixelFormat != MTLPixelFormatDepth32Float_Stencil8)
            return impl.Skip("no stencil plane for the water classes");
        const Viewport& vp = in.viewport;
        if (vp.Width < kMinWorldViewportSide || vp.Height < kMinWorldViewportSide || vp.X + vp.Width > depth.width ||
            vp.Y + vp.Height > depth.height || color.width != depth.width || color.height != depth.height ||
            color.sampleCount != depth.sampleCount)
            return impl.Skip("world viewport outside the render target");
        if (vp.X != 0 || vp.Y != 0 || vp.Width != color.width || vp.Height != color.height)
            return impl.Skip("world viewport is not the whole target");

        impl.EnsureStatic();
        if (!impl.EnsureCopies(color.pixelFormat, vp.Width, vp.Height))
            return false;
        if (!BuildCommonConstants(in, static_cast<uint32_t>(depth.width), static_cast<uint32_t>(depth.height),
                                  impl.constants.common))
            return impl.Skip("view matrix not invertible");

        impl.data = &data;
        impl.EnsureMasks();
        impl.CopySceneColour(commands, color);
        if (!impl.CopyLinearDepth(commands, depth, impl.sceneDepth, @"CoAVolFog water scene depth"))
        {
            impl.failed = true;
            return impl.Skip("water shaders unavailable");
        }
        impl.ClearWaterStencil(commands, depth);

        impl.in = in;
        engine::BuildWaterInputs(impl.water);
        impl.water.stockFogApplies = !stockFogPushed;
        impl.cfg = cfg;
        return true;
    };

    impl.armed = arm();
    if (!impl.armed && impl.skip != impl.loggedSkip)
    {
        VF_LOG_INFO("water: %s", *impl.skip ? impl.skip : "armed");
        impl.loggedSkip = impl.skip;
    }
    else if (impl.armed && impl.loggedSkip)
    {
        VF_LOG_INFO("water: armed");
        impl.loggedSkip = nullptr;
    }

    // Water surfaces write depth while modern water is drawn, and with LiquidDepth while the fog drew last frame
    impl.depthForced = impl.armed || (cfg.liquidDepth && fogDrewLastFrame);
    if (impl.depthForced)
        GxMetalForceDepthWrite(true);
}

// WaterRenderer::Tag and Untag
void WaterRenderer::LiquidDraw(const void* settings, bool begin)
{
    if (!m_impl || !m_impl->armed)
        return;
    Impl& impl = *m_impl;
    if (begin)
    {
        WaterClass waterClass = engine::ClassifyWaterSettings(settings);
        const int index = static_cast<int>(waterClass);
        if (index <= 0 || index >= kWaterClassCount)
            return;
        GxMetalSetStencilTag(static_cast<uint32_t>(index));
        impl.tagged = true;
        ++impl.draws[index];
    }
    else if (impl.tagged)
    {
        GxMetalSetStencilTag(0);
        impl.tagged = false;
    }
}

// WaterRenderer::End and ShadeTaggedWater
void WaterRenderer::LiquidsEnd(const WMModHost* host, const FogData& fog)
{
    if (!m_impl)
        return;
    Impl& impl = *m_impl;
    GxMetalSetStencilTag(0);
    impl.tagged = false;
    if (impl.depthForced)
    {
        GxMetalForceDepthWrite(false);
        impl.depthForced = false;
    }
    if (!impl.armed)
        return;
    impl.armed = false;
    impl.wavesSimulated = false;
    if (!impl.AnyClassDrawn())
        return;

    WMFrameTargets targets = {};
    if (!host->AcquireFrameTargets(&targets) || !targets.depth)
        return;
    id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)targets.commandBuffer;
    id<MTLTexture> color = (__bridge id<MTLTexture>)targets.color;
    id<MTLTexture> depth = (__bridge id<MTLTexture>)targets.depth;
    if (depth.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 || color.width != impl.sceneColour.width ||
        color.height != impl.sceneColour.height)
        return;

    const int variant = ShadingVariant(impl.cfg);
    id<MTLLibrary> library = impl.Library(kSsrSteps[variant]);
    id<MTLRenderPipelineState> shade =
        library ? impl.Pipeline(library, @"vw_water", color.pixelFormat, color.sampleCount, depth.pixelFormat, true,
                                "shade" + std::to_string(variant) + "_" + std::to_string(color.pixelFormat) + "x" +
                                    std::to_string(color.sampleCount))
                : nil;
    if (!shade)
    {
        impl.failed = true;
        return;
    }

    const double seconds = host->GetWorldState() ? host->GetWorldState()->time : 0.0;
    impl.BuildReflectionFog(fog);
    impl.wavesSimulated = impl.SimulateWaves(commands, library, seconds);
    impl.FillRippleConstants();
    if (!impl.CopyLinearDepth(commands, depth, impl.waterDepth, @"CoAVolFog water depth"))
        return;

    // ShadeClasses
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.loadAction = MTLLoadActionLoad;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    encoder.label = @"CoAVolFog water";
    [encoder setRenderPipelineState:shade];
    [encoder setDepthStencilState:impl.classStencil];
    [encoder setFragmentTexture:impl.sceneColour atIndex:kSceneColourStage];
    [encoder setFragmentTexture:impl.sceneDepth atIndex:kSceneDepthStage];
    [encoder setFragmentTexture:impl.waterDepth atIndex:kWaterDepthStage];
    [encoder setFragmentTexture:impl.flat atIndex:kRippleStage];

    int shadedClasses = 0;
    for (int index = 1; index < kWaterClassCount; ++index)
    {
        const WaterClass waterClass = static_cast<WaterClass>(index);
        const WaterPreset* preset = impl.draws[index] ? impl.data->Preset(waterClass) : nullptr;
        if (!preset)
            continue;
        impl.FillClassConstants(impl.constants.shading, *preset, waterClass, seconds);
        [encoder setFragmentBytes:&impl.constants length:sizeof(impl.constants) atIndex:0];
        impl.BindClassTextures(encoder, *preset);
        [encoder setStencilReferenceValue:static_cast<uint32_t>(index)];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        ++shadedClasses;
    }
    [encoder endEncoding];
    impl.lastSeconds = seconds;

    if (LogEnabled(LogLevel::Debug) && (impl.loggedLightSeconds < 0.0 || seconds - impl.loggedLightSeconds > 10.0))
    {
        impl.loggedLightSeconds = seconds;
        VF_LOG_DEBUG("water light: day %.3f, to light %.2f %.2f %.2f%s, direct %08X, ambient %08X, sky %08X %08X, "
                     "river %08X %08X, ocean %08X %08X, stock fog %s",
                     impl.in.dayFraction, impl.in.toLight[0], impl.in.toLight[1], impl.in.toLight[2],
                     impl.in.lightIsMoon ? " (moon)" : "", impl.in.directColor, impl.in.ambientColor, impl.water.skyColors[0],
                     impl.water.skyColors[4], impl.water.riverColors[0], impl.water.riverColors[1], impl.water.oceanColors[0],
                     impl.water.oceanColors[1], impl.water.stockFogApplies ? "applies" : "pushed");
    }
    if (shadedClasses > 0 && !impl.loggedFirstShade)
    {
        impl.loggedFirstShade = true;
        VF_LOG_INFO("water shaded: %d class%s, waves %s, quality %d", shadedClasses, shadedClasses == 1 ? "" : "es",
                    impl.wavesSimulated ? "simulated" : "flat", QualityIndex(impl.cfg) + 1);
    }
}
