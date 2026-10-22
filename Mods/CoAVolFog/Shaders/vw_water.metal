// The water of the Windows CoAVolFog on Metal: the wave simulation (shaders/vw_fft_common.hlsli,
// vw_fft_evolve.hlsl, vw_fft_butterfly.hlsl, vw_fft_surface.hlsl, vw_fft_mip.hlsl) as compute kernels, the
// linear depth copy (vw_linear_depth.hlsl) and the shading (vw_constants.hlsli, vw_surface.hlsli,
// vw_optics.hlsli, vw_reflection.hlsli, vw_water.hlsl), ported function by function.
//
// The HLSL wave passes draw a full-screen triangle per pass into an atlas of tiles and sample their sources
// with point filtering; here each pass is a dispatch over one tile and reads texels directly. The butterfly
// passes of the HLSL run one or two stages each (FUSED_STAGES); here one threadgroup transforms a whole line
// through every stage in threadgroup memory, with the same table and the same arithmetic per stage.
// Constants keep the HLSL register numbers in a float4 buffer C. Variant: SSR_STEPS (0, 16, 32).

#include <metal_stdlib>
using namespace metal;

#ifndef SSR_STEPS
#define SSR_STEPS 0
#endif

// ---- vw_fft_common.hlsli ----

constant float kWaterTwoPi = 6.28318548;

static float2 ComplexMultiply(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

static float2 Conjugate(float2 value)
{
    return float2(value.x, -value.y);
}

static float2 TimesMinusI(float2 value)
{
    return float2(value.y, -value.x);
}

// ---- vw_fft_evolve.hlsl ----

constant float kSmallestWaveNumberSquared = 1.0e-6;
constant float kFlatWaveNumber = 1.0e-16;
constant float kSmallestAlongWind = 1.0e-30;

struct Evolve
{
    constant float4* C;
    texture2d<float, access::read> noise; // sNoise

    float Resolution() const { return C[0].x; }
    float HalfResolution() const { return C[0].w; }
    float WaveNumberPerTexel() const { return C[1].x; }
    float Amplitude() const { return C[1].y; }
    float Alignment() const { return C[1].z; }
    float InversePeakLengthSquared() const { return C[1].w; }
    float2 WindDirection() const { return C[2].xy; }
    float CoarseLoopFraction() const { return C[3].x; }
    float FineLoopFraction() const { return C[3].y; }
    float LoopFrequenciesPerRootRadius() const { return C[3].z; }

    float2 WaveNumber(float2 texel) const { return (texel - HalfResolution()) * WaveNumberPerTexel(); }

    float2 SpectrumAmplitudes(float2 waveNumber) const
    {
        float squared = dot(waveNumber, waveNumber);
        float clamped = max(squared, kSmallestWaveNumberSquared);
        float isotropic = Amplitude() / (clamped * clamped) * exp(-InversePeakLengthSquared() / clamped);
        float alongWind = dot(waveNumber, WindDirection()) * rsqrt(clamped);
        float directional = exp2(Alignment() * log2(max(abs(alongWind), kSmallestAlongWind)));
        float2 sides = float2(alongWind > 0, alongWind < 0);
        return squared < kSmallestWaveNumberSquared ? float2(0) : sqrt(0.5 * isotropic * directional) * sides;
    }

    float2 LoopRotation(float rootRadius) const
    {
        float loopFrequency = floor(rootRadius * LoopFrequenciesPerRootRadius() + 0.5);
        float cycles = fract(fract(loopFrequency * CoarseLoopFraction()) + loopFrequency * FineLoopFraction());
        float cosine;
        float sine = sincos(cycles * kWaterTwoPi, cosine);
        return float2(cosine, sine);
    }

    static float2 ForeverEvolvedMode(float3 h0, float2 rotation)
    {
        return ComplexMultiply(h0.xy, rotation) + ComplexMultiply(float2(h0.z, -h0.y), Conjugate(rotation));
    }

    float2 EvolvedHeight(float2 texel, thread float2& direction) const
    {
        float2 waveNumber = WaveNumber(texel);
        direction = waveNumber / max(length(waveNumber), kFlatWaveNumber);
        float4 n = noise.read(uint2(texel));
        return ForeverEvolvedMode(n.xyz * SpectrumAmplitudes(waveNumber).xxy, LoopRotation(n.w));
    }

    static float2 FoldedHermitian(float2 atTexel, float2 atMirror)
    {
        return 0.5 * (atTexel + Conjugate(atMirror));
    }

    float4 At(float2 texel) const
    {
        float2 mirror = (Resolution() - texel) * float2(texel > 0);
        float2 direction;
        float2 mirrorDirection;
        float2 height = EvolvedHeight(texel, direction);
        float2 mirrorHeight = EvolvedHeight(mirror, mirrorDirection);
        float2 foldedHeight = FoldedHermitian(height, mirrorHeight);
        float2 foldedX = FoldedHermitian(TimesMinusI(height) * direction.x, TimesMinusI(mirrorHeight) * mirrorDirection.x);
        float2 foldedY = FoldedHermitian(TimesMinusI(height) * direction.y, TimesMinusI(mirrorHeight) * mirrorDirection.y);
        return float4(foldedHeight.x - foldedX.y, foldedHeight.y + foldedX.x, foldedY);
    }
};

kernel void vw_fft_evolve(constant float4* C [[buffer(0)]], texture2d<float, access::read> noise [[texture(0)]],
                          texture2d<float, access::write> spectrum [[texture(1)]], uint2 texel [[thread_position_in_grid]])
{
    Evolve evolve = {C, noise};
    if (texel.x >= uint(evolve.Resolution()) || texel.y >= uint(evolve.Resolution()))
        return;
    spectrum.write(evolve.At(float2(texel)), texel);
}

// ---- vw_fft_butterfly.hlsl ----

// WaterButterflies: the twiddle of a row of a stage and the two rows it combines
struct Butterfly
{
    float2 twiddle;
    uint first;
    uint second;
};

static float4 Combine(Butterfly butterfly, float4 first, float4 second)
{
    return float4(first.xy + ComplexMultiply(butterfly.twiddle, second.xy),
                  first.zw + ComplexMultiply(butterfly.twiddle, second.zw));
}

// C[0]: resolution, stages, inverse reference area, assemble (the last column pass writes displacement)
static void TransformLine(constant float4* C, constant Butterfly* butterflies, threadgroup float4* line,
                          threadgroup float4* scratch, uint index)
{
    const uint resolution = uint(C[0].x);
    const uint stages = uint(C[0].y);
    for (uint stage = 0; stage < stages; ++stage)
    {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        Butterfly butterfly = butterflies[stage * resolution + index];
        scratch[index] = Combine(butterfly, line[butterfly.first], line[butterfly.second]);
        threadgroup_barrier(mem_flags::mem_threadgroup);
        line[index] = scratch[index];
    }
}

// The ASSEMBLE output of the last column pass
static float4 AssembledDisplacement(float2 pixel, float4 transformed, float inverseReferenceArea)
{
    float scale = (1 - 4 * fract(0.5 * (pixel.x + pixel.y))) * inverseReferenceArea;
    return float4(transformed.y, transformed.z, -transformed.x, transformed.w) * scale;
}

kernel void vw_fft_rows(constant float4* C [[buffer(0)]], constant Butterfly* butterflies [[buffer(1)]],
                        texture2d<float, access::read> source [[texture(0)]],
                        texture2d<float, access::write> target [[texture(1)]],
                        threadgroup float4* line [[threadgroup(0)]], threadgroup float4* scratch [[threadgroup(1)]],
                        uint index [[thread_position_in_threadgroup]], uint row [[threadgroup_position_in_grid]])
{
    line[index] = source.read(uint2(index, row));
    TransformLine(C, butterflies, line, scratch, index);
    target.write(line[index], uint2(index, row));
}

kernel void vw_fft_columns(constant float4* C [[buffer(0)]], constant Butterfly* butterflies [[buffer(1)]],
                           texture2d<float, access::read> source [[texture(0)]],
                           texture2d<float, access::write> target [[texture(1)]],
                           threadgroup float4* line [[threadgroup(0)]], threadgroup float4* scratch [[threadgroup(1)]],
                           uint index [[thread_position_in_threadgroup]], uint column [[threadgroup_position_in_grid]])
{
    line[index] = source.read(uint2(column, index));
    TransformLine(C, butterflies, line, scratch, index);
    float4 transformed = line[index];
    if (C[0].w > 0.5)
        transformed = AssembledDisplacement(float2(column, index), transformed, C[0].z);
    target.write(transformed, uint2(column, index));
}

// ---- vw_fft_surface.hlsl ----

constant float kSmallestNormalHalf = 6.10351563e-5;
constant float kCoarseHalfStepsPerOctave = 512;
constant float kLargestResidualFraction = 0.99951171875;

struct WaveGradients
{
    float3 alongX;
    float3 alongY;
    float2 wide;
};

struct Surface
{
    constant float4* C;
    texture2d<float, access::read> displacement; // sDisplacement
    texture2d<float, access::read> previousFoam; // sPreviousFoam

    float Resolution() const { return C[0].x; }
    float InverseResolution() const { return C[0].y; }
    float HalfTexelsPerUnit() const { return C[1].x; }
    float WideRadius() const { return C[1].y; }
    float InverseWideBaseline() const { return C[1].z; }
    float DeltaSeconds() const { return C[1].w; }

    float3 DisplacementAt(float2 texel, float2 offset) const
    {
        float2 wrapped = texel + offset;
        wrapped -= Resolution() * floor(wrapped * InverseResolution());
        return displacement.read(uint2(wrapped)).xyz;
    }

    float3 CentralDifference(float2 texel, float2 step) const
    {
        return (DisplacementAt(texel, step) - DisplacementAt(texel, -step)) * HalfTexelsPerUnit();
    }

    WaveGradients GradientsAt(float2 texel) const
    {
        WaveGradients gradients;
        gradients.alongX = CentralDifference(texel, float2(1, 0));
        gradients.alongY = CentralDifference(texel, float2(0, 1));
        float2 wideAhead = float2(DisplacementAt(texel, float2(WideRadius(), 0)).z,
                                  DisplacementAt(texel, float2(0, WideRadius())).z);
        float2 wideBehind = float2(DisplacementAt(texel, float2(-WideRadius(), 0)).z,
                                   DisplacementAt(texel, float2(0, -WideRadius())).z);
        gradients.wide = (wideAhead - wideBehind) * InverseWideBaseline();
        return gradients;
    }

    static float4 SurfaceMoments(WaveGradients gradients)
    {
        float2 slope = float2(gradients.alongX.z, gradients.alongY.z);
        return float4(slope, dot(slope, slope), dot(gradients.wide, gradients.wide));
    }

    static float Jacobian(WaveGradients gradients)
    {
        return (1 + gradients.alongX.x) * (1 + gradients.alongY.y) - gradients.alongY.x * gradients.alongX.y;
    }

    float FoamStep(float previous, float jacobian, float3 biasRateDecay) const
    {
        float injection = max(biasRateDecay.x + 1 - jacobian, 0.0);
        float rate = biasRateDecay.y * injection * (1 - previous) - biasRateDecay.z * previous;
        return saturate(previous + DeltaSeconds() * rate);
    }

    static float4 SplitForHalfStorage(float2 state)
    {
        float2 step = exp2(floor(log2(max(state, kSmallestNormalHalf)))) / kCoarseHalfStepsPerOctave;
        float2 coarse = floor(state / step) * step;
        return float4(coarse, min(state - coarse, step * kLargestResidualFraction));
    }

    float4 NextFoam(float2 texel, float jacobian) const
    {
        float4 stored = previousFoam.read(uint2(texel));
        float2 previous = saturate(stored.xy + stored.zw);
        return SplitForHalfStorage(float2(FoamStep(previous.x, jacobian, C[2].xyz), FoamStep(previous.y, jacobian, C[3].xyz)));
    }
};

// SURFACE && FOAM
kernel void vw_fft_surface(constant float4* C [[buffer(0)]], texture2d<float, access::read> displacement [[texture(0)]],
                           texture2d<float, access::read> previousFoam [[texture(1)]],
                           texture2d<float, access::write> surface [[texture(2)]],
                           texture2d<float, access::write> nextFoam [[texture(3)]], uint2 texel [[thread_position_in_grid]])
{
    Surface s = {C, displacement, previousFoam};
    if (texel.x >= uint(s.Resolution()) || texel.y >= uint(s.Resolution()))
        return;
    WaveGradients gradients = s.GradientsAt(float2(texel));
    surface.write(Surface::SurfaceMoments(gradients), texel);
    nextFoam.write(s.NextFoam(float2(texel), Surface::Jacobian(gradients)), texel);
}

// ---- vw_fft_mip.hlsl ----

// BoxAverage from the level above (SourceTexelsPerPixel 2, TapSpread 1)
kernel void vw_fft_mip(texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::write> target [[texture(1)]],
                       uint2 pixel [[thread_position_in_grid]])
{
    if (pixel.x >= target.get_width() || pixel.y >= target.get_height())
        return;
    uint2 first = pixel * 2;
    float4 sum = source.read(first) + source.read(first + uint2(1, 0)) + source.read(first + uint2(0, 1)) +
                 source.read(first + uint2(1, 1));
    target.write(0.25 * sum, pixel);
}

// ---- vf_common.hlsli (the members the water passes use, as in vf_fog.metal) ----

#define cWorldViewportPx C[0]
#define cPixelGrid C[1]
#define cViewToNdc C[2]
#define cDepthToViewZ C[3]

struct Varyings
{
    float4 position [[position]];
};

vertex Varyings vw_fullscreen(uint vid [[vertex_id]])
{
    float2 ndc[3] = {float2(-1.5, -1.5), float2(4.5, -1.5), float2(-1.5, 4.5)};
    Varyings out;
    out.position = float4(ndc[vid], 0.0, 1.0);
    return out;
}

// The members of vf_common.hlsli, in every struct that reads the common registers (Metal has no base classes).
// ViewToWorldDirection is mul(viewDirection, (float3x3)cViewToWorld) with cViewToWorld row_major in c4..c7.
#define COMMON_MEMBERS \
 \
    float2 ViewportOrigin() const { return cWorldViewportPx.xy; } \
    float2 ViewportSize() const { return cWorldViewportPx.zw; } \
    float2 ViewToNdcScale() const { return cViewToNdc.xy; } \
    float2 ViewToNdcOffset() const { return cViewToNdc.zw; } \
    float DepthAtInfiniteViewZ() const { return cDepthToViewZ.x; } \
    float DepthPerInverseViewZ() const { return cDepthToViewZ.y; } \
    float MaxFogDistance() const { return cDepthToViewZ.z; } \
    float DeepestWorldDepth() const { return cDepthToViewZ.w; } \
    float3 CameraPositionWorld() const { return C[7].xyz; } \
 \
    float3 ViewToWorldDirection(float3 v) const { return v.x * C[4].xyz + v.y * C[5].xyz + v.z * C[6].xyz; } \
 \
    bool BeyondFarClip(float depth) const { return depth > DeepestWorldDepth(); } \
 \
    float LinearDepth(float depth) const \
    { \
        return BeyondFarClip(depth) ? MaxFogDistance() \
                                    : min(DepthPerInverseViewZ() / (depth - DepthAtInfiniteViewZ()), MaxFogDistance()); \
    } \
 \
    float2 PixelToNdc(float2 pixel) const \
    { \
        return float2((pixel.x - ViewportOrigin().x) / ViewportSize().x * 2 - 1, \
                      1 - (pixel.y - ViewportOrigin().y) / ViewportSize().y * 2); \
    } \
 \
    float2 NdcToPixel(float2 ndc) const \
    { \
        return ViewportOrigin() + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * ViewportSize(); \
    } \
 \
    float3 ViewRayAtUnitDepth(float2 pixel) const \
    { \
        float2 ndc = PixelToNdc(pixel); \
        return float3((ndc.x - ViewToNdcOffset().x) / ViewToNdcScale().x, \
                      (ndc.y - ViewToNdcOffset().y) / ViewToNdcScale().y, 1); \
    }

struct Common
{
    constant float4* C;
    COMMON_MEMBERS
};

// ---- vw_linear_depth.hlsl (PACKED_DEPTH 0: the copies are R32Float) ----

// The depth texel the pixel lies in (tex2Dlod with point filtering at the pixel's centre)
template <typename DepthTexture>
static float DepthAt(DepthTexture depth, float2 pixel);

template <>
float DepthAt(depth2d<float> depth, float2 pixel)
{
    return depth.read(uint2(clamp(pixel, float2(0), float2(depth.get_width() - 1, depth.get_height() - 1))));
}

template <>
float DepthAt(depth2d_ms<float> depth, float2 pixel)
{
    return depth.read(uint2(clamp(pixel, float2(0), float2(depth.get_width() - 1, depth.get_height() - 1))), 0);
}

fragment float4 vw_depth(Varyings in [[stage_in]], constant float4* C [[buffer(0)]], depth2d<float> depth [[texture(0)]])
{
    Common common = {C};
    float2 pixel = common.ViewportOrigin() + in.position.xy;
    return float4(common.LinearDepth(DepthAt(depth, pixel)));
}

fragment float4 vw_depth_ms(Varyings in [[stage_in]], constant float4* C [[buffer(0)]], depth2d_ms<float> depth [[texture(0)]])
{
    Common common = {C};
    float2 pixel = common.ViewportOrigin() + in.position.xy;
    return float4(common.LinearDepth(DepthAt(depth, pixel)));
}

// The world before the liquids in one pass: its colour (the samples averaged, as a multisample resolve) and its
// linear depth (vw_linear_depth.hlsl, sample 0), read as textures instead of resolving the frame's attachments
struct SceneCopy
{
    float4 colour [[color(0)]];
    float4 depth [[color(1)]];
};

fragment SceneCopy vw_scene_copy(Varyings in [[stage_in]], constant float4* C [[buffer(0)]],
                                 depth2d<float> depth [[texture(0)]], texture2d<float> colour [[texture(1)]])
{
    Common common = {C};
    float2 pixel = common.ViewportOrigin() + in.position.xy;
    SceneCopy out;
    out.colour = colour.read(uint2(pixel));
    out.depth = float4(common.LinearDepth(DepthAt(depth, pixel)));
    return out;
}

fragment SceneCopy vw_scene_copy_ms(Varyings in [[stage_in]], constant float4* C [[buffer(0)]],
                                    depth2d_ms<float> depth [[texture(0)]], texture2d_ms<float> colour [[texture(1)]])
{
    Common common = {C};
    float2 pixel = common.ViewportOrigin() + in.position.xy;
    uint samples = colour.get_num_samples();
    float4 sum = 0;
    for (uint s = 0; s < samples; ++s)
        sum += colour.read(uint2(pixel), s);
    SceneCopy out;
    out.colour = sum / samples;
    out.depth = float4(common.LinearDepth(DepthAt(depth, pixel)));
    return out;
}

// ---- vw_constants.hlsli ----

#define cLight C[9]
#define cSunColour C[10]
#define cAmbient C[11]
#define cIsotropicLight C[12]
#define cSkyTop C[13]
#define cSkyMiddle C[14]
#define cSkyUpperBand C[15]
#define cSkyLowerBand C[16]
#define cSkyHorizon C[17]
#define cStockFogColour C[18]
#define cStockFog C[19]
#define cAbsorption C[20]
#define cScatteringIntensities C[21]
#define cScatteringTop C[22]
#define cScatteringBottom C[23]
#define cDepthFadeFoam C[24]
#define cShoreFoam C[25]
#define cWaveFoam C[26]
#define cWaveFoamScaling C[27]
#define cSurfaceResponse C[28]
#define cInverseTileSizes C[29]
#define cWaveControl C[30]
#define cFoamScroll C[31]
#define cDepthFoamScroll C[32]
#define cDepthDecode C[33]
#define cMaskTints(i) C[34 + (i)]
#define cReflectionFogLayers(i) C[44 + (i)]
#define cReflectionFogRange C[64]
#define cRippleWindow C[65]
#define cRippleShape C[66]
#define cRippleFade C[67]

constant int kHighFoamSlot = 0;
constant int kMidFoamSlot = 1;
constant int kLowFoamSlot = 2;
constant int kShoreFoamSlot = 3;
constant int kDepthFoamSlot = 4;
constant float kMinStockFogVisibility = 0.05;

// vw_surface.hlsli
constant float kPi = 3.14159265;
constant float kShallowTileAmplitude = 800;
constant float kMaxTanhArgument = 10;
constant float kAdtGridOrigin = 17066.666;
constant float kAdtTilesPerYard = 0.001875;
constant float kSlopeToAdtUv = 0.001;
constant float kMinFoamFade = 1e-4;
constant float kMinRayRise = 1e-4;
constant float kRippleSlopeScale = 3;
constant float kRippleFullDetailTexels = 2;
constant float kRippleDetailFadeTexels = 2;

// vw_optics.hlsli
constant float kWaterF0 = 0.02;
constant float kInverseFourPi = 0.0795775;
constant float kFoamRoughness = 0.8;
constant float kRoughnessExponent = 0.25;
constant float kMinRoughnessMoment = 1e-8;
constant float kRefractionUvPerSlopeYard = 0.005;
constant float kMaxRefractionDepth = 50;
constant float kShoreFadePerYard = 2;
constant float kFoamStateGain = 10;
constant float kFoamWrap = 0.25;
constant float kFoamCrestSharpness = 8;
constant float kCrestColourBlend = 0.5;
constant float kMinCosine = 1e-5;
constant float kMinPhaseDenominator = 1e-6;
constant float kSunDiscAlpha = 0.02;
constant float kMinSunAlpha = 1e-3;
constant float kMinVisibilityDenominator = 1e-5;

// vw_reflection.hlsli
constant float kSkyLowerBandElevation = 0.06;
constant float kSkyUpperBandElevation = 0.15;
constant float kSkyMiddleElevation = 0.35;
constant float kSkyTopElevation = 0.8;
constant int kReflectionSteps = SSR_STEPS;
constant int kReflectionRefinements = 4;
constant float kMinReflectionViewZ = 1e-3;
constant float kMinInverseViewZ = 1e-8;
constant float kMinScreenStep = 1e-6;
constant float kReflectionThicknessYards = 1.5;
constant float kReflectionRelativeThickness = 0.05;
constant float kVerticalFadeStart = 0.9;
constant float kVerticalFadeRate = 10;
constant float kNoReflectionHit = 2;
constant int kReflectionFogLayers = 4;
constant int kReflectionFogLayerRegisters = 5;
constant float kSkyHitDepthFraction = 0.99;
constant float kMinPhaseDistance = 1e-6;
constant float kMinProfileHeightSpan = 1e-3;
constant float kMinExponentSpan = 1e-2;
constant float kMinPathLength = 1e-3;

// vw_water.hlsl
constant float kDebugNormal = 1;
constant float kDebugFoamCoverage = 2;
constant float kDebugTransmittance = 3;
constant float kDebugReflection = 4;
constant float kDebugLiquidClass = 5;
constant float kDebugRippleHeightGain = 4;
constant float kDebugRippleWindowShade = 0.25;

struct WaterPixel
{
    float2 uv;
    float waterZ;
    float sceneZ;
    float3 position;
    float3 toCamera;
    float cameraDistance;
    float columnDepth;
    float2 footprintX;
    float2 footprintY;
};

struct WaveState
{
    float4 moments;
    float2 foam;
};

struct RippleSample
{
    float height;
    float2 slope;
    float coverage;
};

struct Refraction
{
    float3 copied;
    float3 scene;
    float pathDepth;
};

struct ReflectionRay
{
    float2 startPixel;
    float2 vanishingPixel;
    float startInverseZ;
};

struct ReflectedPath
{
    float from;
    float to;
    float startHeight;
    float heightPerYard;
    float skyRise;
    float cosToLight;
};

// The samplers of WaterRenderer::ShadeClasses and BindClassTextures
constexpr sampler kPointMirror(filter::nearest, address::mirrored_repeat);
constexpr sampler kPointClamp(filter::nearest, address::clamp_to_edge);
constexpr sampler kLinearWrapMip(filter::linear, mip_filter::linear, address::repeat);
constexpr sampler kLinearWrap(filter::linear, mip_filter::none, address::repeat);
constexpr sampler kLinearClamp(filter::linear, mip_filter::none, address::clamp_to_edge);

struct Water
{
    constant float4* C;
    COMMON_MEMBERS
    texture2d<float> sceneColour;    // sSceneColour, point, mirrored
    texture2d<float> sceneDepth;     // sSceneDepth, point, mirrored
    texture2d<float> waterDepth;     // sWaterDepth, point, clamped
    texture2d<float> surface0;       // sSurface0..3, linear with mips, wrapped
    texture2d<float> surface1;
    texture2d<float> surface2;
    texture2d<float> surface3;
    texture2d<float> foamState0;     // sFoamState0..3, linear, wrapped
    texture2d<float> foamState1;
    texture2d<float> foamState2;
    texture2d<float> foamState3;
    texture2d<float> waveFoamMasks;  // sWaveFoamMasks, linear with mips, wrapped
    texture2d<float> ripples;        // sRipples, linear, clamped
    texture2d<float> shoreFoamMask;  // sShoreFoamMask, linear with mips, wrapped
    texture2d<float> depthFoamMask;  // sDepthFoamMask, linear with mips, wrapped

    // ---- vw_constants.hlsli ----

    float3 ToLight() const { return cLight.xyz; }
    float SunVisibility() const { return cLight.w; }
    float3 SunColour() const { return cSunColour.rgb; }
    float SunTransmission() const { return cSunColour.w; }
    float3 Ambient() const { return cAmbient.rgb; }
    float3 IsotropicLight() const { return cIsotropicLight.rgb; }
    bool StockFogApplies() const { return cStockFogColour.w > 0.5; }
    float3 StockFogColour() const { return cStockFogColour.rgb; }
    float StockFogVisibility(float viewZ) const { return saturate((cStockFog.x - viewZ) * cStockFog.y); }

    float3 WithoutStockFog(float3 encoded, float viewZ) const
    {
        if (!StockFogApplies())
            return encoded;
        float visibility = StockFogVisibility(viewZ);
        return saturate((encoded - StockFogColour() * (1 - visibility)) / max(visibility, kMinStockFogVisibility));
    }

    float3 AbsorptionPerYard() const { return cAbsorption.rgb; }
    float3 CrestScatteringColour() const { return cScatteringTop.rgb; }
    float3 TroughScatteringColour() const { return cScatteringBottom.rgb; }
    float ScatteringAnisotropy() const { return cScatteringBottom.w; }
    float WaveFoamIntensity() const { return cWaveFoam.x; }
    float ShoreDistancePerDepth() const { return cShoreFoam.w; }
    float SunRoughnessParameter() const { return cSurfaceResponse.x; }
    float EnvironmentRoughnessParameter() const { return cSurfaceResponse.y; }
    float ReflectionStrength() const { return cSurfaceResponse.z; }
    float SpecularStrength() const { return cSurfaceResponse.w; }
    float InverseTileCount() const { return cWaveControl.x; }
    float WaterClassIndex() const { return cWaveControl.z; }
    float WaterDebugView() const { return cWaveControl.w; }
    float3 MaskTintLow(int slot) const { return cMaskTints(slot * 2).rgb; }
    float MaskPresent(int slot) const { return cMaskTints(slot * 2).w; }
    float3 MaskTintHigh(int slot) const { return cMaskTints(slot * 2 + 1).rgb; }
    bool ReflectionFogActive() const { return cReflectionFogRange.w > 0.5; }
    float ReflectionFogCurveRange() const { return cReflectionFogRange.x; }
    float ReflectionFogSkyEnd() const { return cReflectionFogRange.y; }
    float2 RippleOrigin() const { return cRippleWindow.xy; }
    float RippleInverseExtent() const { return cRippleWindow.z; }
    float RippleTexelUv() const { return cRippleWindow.w; }
    float RippleWeight() const { return cRippleShape.x; }
    float RippleGain() const { return cRippleShape.y; }
    float RippleExtent() const { return cRippleShape.z; }
    float RippleFadeEnd() const { return cRippleFade.x; }
    float RippleInverseFadeWidth() const { return cRippleFade.y; }
    float RippleTexelYards() const { return cRippleFade.z; }

    // ---- vw_surface.hlsli ----

    float2 CopyUv(float2 pixel) const { return (pixel - ViewportOrigin()) / ViewportSize(); }

    float CopiedViewDepth(texture2d<float> copy, float2 uv, sampler s) const
    {
        return dot(copy.sample(s, uv, level(0)).rgb, cDepthDecode.rgb);
    }

    static float3 GammaToLinear(float3 colour) { return pow(max(colour, 0.0), 2.2); }
    static float3 LinearToGamma(float3 colour) { return pow(saturate(colour), 1 / 2.2); }

    static float2 PlaneFootprint(float3 ray, float3 rayStep, float viewZ)
    {
        float rise = ray.z < 0 ? min(ray.z, -kMinRayRise) : max(ray.z, kMinRayRise);
        return viewZ * (rayStep.xy - ray.xy * rayStep.z / rise);
    }

    WaterPixel ReconstructWaterPixel(float2 pixel) const
    {
        WaterPixel w;
        w.uv = CopyUv(pixel);
        w.waterZ = CopiedViewDepth(waterDepth, w.uv, kPointClamp);
        w.sceneZ = max(CopiedViewDepth(sceneDepth, w.uv, kPointMirror), w.waterZ);
        float3 worldRay = ViewToWorldDirection(ViewRayAtUnitDepth(pixel));
        w.position = CameraPositionWorld() + worldRay * w.waterZ;
        w.toCamera = normalize(-worldRay);
        w.cameraDistance = w.waterZ * length(worldRay);
        w.columnDepth = max(0.0, (w.sceneZ - w.waterZ) * -worldRay.z);
        float3 rayStepX = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(1, 0))) - worldRay;
        float3 rayStepY = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(0, 1))) - worldRay;
        w.footprintX = PlaneFootprint(worldRay, rayStepX, w.waterZ);
        w.footprintY = PlaneFootprint(worldRay, rayStepY, w.waterZ);
        return w;
    }

    static float4 SampleTile(texture2d<float> tile, WaterPixel w, float inverseSize)
    {
        return tile.sample(kLinearWrapMip, w.position.xy * inverseSize,
                           gradient2d(w.footprintX * inverseSize, w.footprintY * inverseSize));
    }

    static float2 SampleFoamState(texture2d<float> state, WaterPixel w, float inverseSize)
    {
        return state.sample(kLinearWrap, w.position.xy * inverseSize, level(0)).xy;
    }

    static float Tanh(float x)
    {
        float e = exp(2 * min(x, kMaxTanhArgument));
        return (e - 1) / (e + 1);
    }

    static float ShallowTileWeight(float inverseTileSize, float columnDepth)
    {
        float shallowAmplitude = kShallowTileAmplitude * inverseTileSize * inverseTileSize;
        return saturate(mix(shallowAmplitude, 1.0, Tanh(4 * kPi * columnDepth * inverseTileSize)));
    }

    float RippleHeight(float2 uv) const
    {
        float2 state = ripples.sample(kLinearClamp, uv, level(0)).rg;
        return mix(state.g, state.r, RippleWeight());
    }

    float RippleCoverage(WaterPixel w, float2 uv) const
    {
        float2 fromCentre = abs(uv - 0.5) * RippleExtent();
        float window = saturate((RippleFadeEnd() - max(fromCentre.x, fromCentre.y)) * RippleInverseFadeWidth());
        float footprintTexels = max(length(w.footprintX), length(w.footprintY)) / RippleTexelYards();
        return window * saturate(1 - (footprintTexels - kRippleFullDetailTexels) / kRippleDetailFadeTexels);
    }

    RippleSample SampleRipples(WaterPixel w) const
    {
        RippleSample r = {0, float2(0), 0};
        float2 uv = (w.position.xy - RippleOrigin()) * RippleInverseExtent();
        r.coverage = RippleGain() > 0 ? RippleCoverage(w, uv) : 0;
        if (r.coverage > 0)
        {
            float step = RippleTexelUv();
            r.height = RippleHeight(uv);
            float dx = RippleHeight(uv + float2(step, 0)) - r.height;
            float dy = RippleHeight(uv + float2(0, step)) - r.height;
            float2 gradient = float2(dx, dy) * rsqrt((1 + dx * dx) * (1 + dy * dy));
            r.slope = gradient * kRippleSlopeScale * RippleGain() * r.coverage;
        }
        return r;
    }

    WaveState SampleWaves(WaterPixel w, float2 rippleSlope) const
    {
        float4 inverseSize = cInverseTileSizes;
        float4 weight = float4(ShallowTileWeight(inverseSize.x, w.columnDepth), ShallowTileWeight(inverseSize.y, w.columnDepth),
                               ShallowTileWeight(inverseSize.z, w.columnDepth), ShallowTileWeight(inverseSize.w, w.columnDepth));
        WaveState waves;
        waves.moments = weight.x * SampleTile(surface0, w, inverseSize.x) + weight.y * SampleTile(surface1, w, inverseSize.y) +
                        weight.z * SampleTile(surface2, w, inverseSize.z) + weight.w * SampleTile(surface3, w, inverseSize.w);
        waves.moments.xy += rippleSlope;
        waves.foam = weight.x * SampleFoamState(foamState0, w, inverseSize.x) + weight.y * SampleFoamState(foamState1, w, inverseSize.y) +
                     weight.z * SampleFoamState(foamState2, w, inverseSize.z) + weight.w * SampleFoamState(foamState3, w, inverseSize.w);
        return waves;
    }

    float SlopeVariance(float4 moments) const
    {
        float2 meanSlope = moments.xy * InverseTileCount();
        return 0.5 * max(0.0, moments.z * InverseTileCount() - dot(meanSlope, meanSlope));
    }

    float CrestTilt(float4 moments) const
    {
        float coarseSlope = moments.w * InverseTileCount();
        return sqrt(coarseSlope / (coarseSlope + 1));
    }

    static float2 AdtUv(float3 position, float2 slope)
    {
        return (kAdtGridOrigin - position.yx) * kAdtTilesPerYard + slope * kSlopeToAdtUv;
    }

    float4 TintedFoam(int slot, float maskCoverage) const
    {
        float coverage = maskCoverage * MaskPresent(slot);
        return float4(mix(MaskTintLow(slot), MaskTintHigh(slot), coverage), coverage);
    }

    float4 FoamLayer(texture2d<float> mask, int slot, float2 uv, float2 dx, float2 dy) const
    {
        return TintedFoam(slot, mask.sample(kLinearWrapMip, uv, gradient2d(dx, dy)).r);
    }

    static float FoamFade(float distance, float fadeDistance)
    {
        float t = saturate(1 - distance / max(fadeDistance, kMinFoamFade));
        float eased = t * t * (3 - 2 * t);
        eased *= eased;
        return eased * eased;
    }

    float4 WaveFoam(float2 adtUv, float2 dx, float2 dy, float f) const
    {
        float3 scale = cWaveFoamScaling.xyz;
        float4 high = TintedFoam(kHighFoamSlot, waveFoamMasks.sample(kLinearWrapMip, adtUv * scale.x + cFoamScroll.xy,
                                                                     gradient2d(dx * scale.x, dy * scale.x)).r);
        float4 mid = TintedFoam(kMidFoamSlot, waveFoamMasks.sample(kLinearWrapMip, adtUv * scale.y + cFoamScroll.xy,
                                                                   gradient2d(dx * scale.y, dy * scale.y)).g);
        float4 low = TintedFoam(kLowFoamSlot, waveFoamMasks.sample(kLinearWrapMip, adtUv * scale.z + cFoamScroll.xy,
                                                                   gradient2d(dx * scale.z, dy * scale.z)).b);
        float rootF = sqrt(f);
        return saturate(low * rootF + mid * f * rootF + high * pow(f, 4.5)) * WaveFoamIntensity();
    }

    float4 FoamAlbedo(WaterPixel w, WaveState waves) const
    {
        float2 adtUv = AdtUv(w.position, waves.moments.xy);
        float2 dx = -w.footprintX.yx * kAdtTilesPerYard;
        float2 dy = -w.footprintY.yx * kAdtTilesPerYard;
        float f = max(waves.foam.x, 0.0);
        float shoreWeight = cShoreFoam.x * FoamFade(w.columnDepth * ShoreDistancePerDepth(), cShoreFoam.z);
        float depthWeight = cDepthFadeFoam.x * FoamFade(w.sceneZ - w.waterZ, cDepthFadeFoam.z);
        float4 foam = 0;
        if (f * WaveFoamIntensity() > 0)
            foam = WaveFoam(adtUv, dx, dy, f);
        if (shoreWeight > 0)
            foam += shoreWeight * FoamLayer(shoreFoamMask, kShoreFoamSlot, adtUv * cShoreFoam.y + cFoamScroll.zw,
                                            dx * cShoreFoam.y, dy * cShoreFoam.y);
        if (depthWeight > 0)
            foam += depthWeight * FoamLayer(depthFoamMask, kDepthFoamSlot, adtUv * cDepthFadeFoam.y + cDepthFoamScroll.xy,
                                            dx * cDepthFadeFoam.y, dy * cDepthFadeFoam.y);
        return saturate(foam);
    }

    // ---- vw_optics.hlsli ----

    static float FresnelSchlick(float cosine) { return kWaterF0 + (1 - kWaterF0) * pow(1 - saturate(cosine), 5.0); }

    static float PerceptualRoughness(float parameter, float variance)
    {
        return saturate(pow(max(parameter * abs(parameter) + variance, kMinRoughnessMoment), kRoughnessExponent));
    }

    static float EnvironmentBrdf(float roughness, float NoV)
    {
        float4 r = roughness * float4(-1, -0.0275, -0.572, 0.022) + float4(1, 0.0425, 1.04, -0.04);
        float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
        float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
        return kWaterF0 * ab.x + ab.y;
    }

    Refraction RefractScene(WaterPixel w, float2 slope) const
    {
        float2 refractedUv = w.uv + slope * kRefractionUvPerSlopeYard * min(kMaxRefractionDepth, w.sceneZ - w.waterZ);
        float refractedZ = CopiedViewDepth(sceneDepth, refractedUv, kPointMirror);
        bool unrefracted = refractedZ < w.waterZ;
        float2 colourUv = unrefracted ? w.uv : refractedUv;
        float colourZ = unrefracted ? w.sceneZ : refractedZ;
        Refraction r;
        r.copied = sceneColour.sample(kPointMirror, colourUv, level(0)).rgb;
        r.scene = GammaToLinear(WithoutStockFog(r.copied, colourZ));
        r.pathDepth = max(0.0, max(refractedZ, w.sceneZ) - w.waterZ);
        return r;
    }

    float3 Transmittance(float pathDepth) const { return exp(-AbsorptionPerYard() * pathDepth); }

    float3 InScattering(float3 V, float NoV, float crestTilt, float oxygen, float3 transmittance) const
    {
        float g = ScatteringAnisotropy();
        float cosTheta = -dot(V, ToLight());
        float phase = (1 - g * g) / pow(max(1 + g * g - 2 * g * cosTheta, kMinPhaseDenominator), 1.5);
        float4 intensity = cScatteringIntensities;
        float viewPart = NoV * NoV * intensity.x + intensity.y * crestTilt;
        float sunPart = SunTransmission() * intensity.z;
        float oxygenPart = saturate(ToLight().z) * oxygen * intensity.w;
        float normaliser = 1 / max(1.0, sunPart + viewPart + oxygenPart);
        float3 colour = mix(TroughScatteringColour(), CrestScatteringColour(), kCrestColourBlend);
        float3 anisotropic = phase * SunVisibility() * SunColour() * viewPart * normaliser;
        float3 isotropic = (oxygenPart * normaliser + sunPart) * IsotropicLight();
        return (1 - transmittance) * kInverseFourPi * (isotropic + anisotropic) * colour;
    }

    static float WrappedFoamDiffuse(float NoL, float3 L) { return saturate(NoL + kFoamWrap * saturate(L.z)); }

    float3 FoamLight(float3 N, float foamState) const
    {
        float3 L = ToLight();
        float NoL = clamp(dot(N, L), kMinCosine, 1.0);
        float state = saturate(foamState * kFoamStateGain);
        float crest = saturate(state * state * (3 - 2 * state) * L.z);
        float diffuse = mix(WrappedFoamDiffuse(NoL, L), pow(NoL, kFoamCrestSharpness), crest) * SunVisibility();
        return mix(Ambient(), SunColour(), diffuse);
    }

    static float SunGlint(float roughness, float3 N, float3 V, float3 L)
    {
        float3 H = normalize(V + L);
        float NoH = saturate(dot(N, H));
        float NoL = saturate(dot(N, L));
        float NoV = max(dot(N, V), kMinCosine);
        float alpha = max(roughness * roughness, kMinSunAlpha);
        float widened = saturate(alpha + kSunDiscAlpha);
        float widened2 = widened * widened;
        float lobe = NoH * NoH * (widened2 - 1) + 1;
        float energy = alpha / widened;
        float distribution = widened2 / (kPi * lobe * lobe) * energy * energy;
        float visibility = 0.5 / max(NoL * (NoV * (1 - alpha) + alpha) + NoV * (NoL * (1 - alpha) + alpha),
                                     kMinVisibilityDenominator);
        return distribution * visibility * FresnelSchlick(dot(V, H)) * NoL;
    }

    // ---- vw_reflection.hlsli ----

    static float3 ReflectedDirection(float3 V, float3 N)
    {
        float3 r = reflect(-V, N);
        return float3(r.xy, abs(r.z));
    }

    float3 SkyColour(float elevation) const
    {
        float3 colour = mix(cSkyHorizon.rgb, cSkyLowerBand.rgb, smoothstep(0.0, kSkyLowerBandElevation, elevation));
        colour = mix(colour, cSkyUpperBand.rgb, smoothstep(kSkyLowerBandElevation, kSkyUpperBandElevation, elevation));
        colour = mix(colour, cSkyMiddle.rgb, smoothstep(kSkyUpperBandElevation, kSkyMiddleElevation, elevation));
        return mix(colour, cSkyTop.rgb, smoothstep(kSkyMiddleElevation, kSkyTopElevation, elevation));
    }

    float3 WorldToViewDirection(float3 direction) const
    {
        return float3(dot(direction, C[4].xyz), dot(direction, C[5].xyz), dot(direction, C[6].xyz));
    }

    float2 ViewDirectionToPixel(float3 viewDirection) const
    {
        return NdcToPixel(viewDirection.xy / viewDirection.z * ViewToNdcScale() + ViewToNdcOffset());
    }

    static float RayViewZ(ReflectionRay ray, float s) { return 1 / max(ray.startInverseZ * (1 - s), kMinInverseViewZ); }

    float2 RayUv(ReflectionRay ray, float s) const { return CopyUv(mix(ray.startPixel, ray.vanishingPixel, s)); }

    float OnScreenExtent(ReflectionRay ray) const
    {
        float rise = ray.vanishingPixel.y - ray.startPixel.y;
        float top = ViewportOrigin().y;
        float bottom = top + ViewportSize().y;
        float limit = rise < 0 ? (top - ray.startPixel.y) / rise : (bottom - ray.startPixel.y) / max(rise, kMinScreenStep);
        return saturate(limit);
    }

    bool RayBehindScene(ReflectionRay ray, float s) const
    {
        return RayViewZ(ray, s) > CopiedViewDepth(sceneDepth, RayUv(ray, s), kPointMirror);
    }

    float4 TraceScreenReflection(WaterPixel w, float2 pixel, float3 R, thread float2& hitUv) const
    {
        hitUv = 0;
        if (kReflectionSteps == 0)
            return 0;
        float3 viewDirection = WorldToViewDirection(R);
        if (viewDirection.z < kMinReflectionViewZ)
            return 0;
        ReflectionRay ray;
        ray.startPixel = pixel;
        ray.vanishingPixel = ViewDirectionToPixel(viewDirection);
        ray.startInverseZ = 1 / w.waterZ;
        float extent = OnScreenExtent(ray);
        float previous = 0;
        float front = 0;
        float behind = kNoReflectionHit;
        for (int step = 1; step <= kReflectionSteps; ++step)
        {
            if (behind == kNoReflectionHit)
            {
                float s = extent * step / kReflectionSteps;
                float sceneZ = CopiedViewDepth(sceneDepth, RayUv(ray, s), kPointMirror);
                float thickness = max(kReflectionThicknessYards, sceneZ * kReflectionRelativeThickness);
                bool crossed = RayViewZ(ray, s) > sceneZ && RayViewZ(ray, previous) < sceneZ + thickness;
                front = crossed ? previous : front;
                behind = crossed ? s : behind;
                previous = s;
            }
        }
        if (behind == kNoReflectionHit)
            return 0;
        for (int refinement = 0; refinement < kReflectionRefinements; ++refinement)
        {
            float middle = 0.5 * (front + behind);
            bool middleBehind = RayBehindScene(ray, middle);
            front = middleBehind ? front : middle;
            behind = middleBehind ? middle : behind;
        }
        float2 uv = RayUv(ray, behind);
        hitUv = uv;
        float fade = saturate(1 - saturate((max(uv.y, 1 - uv.y) - kVerticalFadeStart) * kVerticalFadeRate));
        return float4(GammaToLinear(sceneColour.sample(kPointMirror, uv, level(0)).rgb), fade);
    }

    float ReflectionFogCurveIntegral(float4 curve, float distance) const
    {
        float travelled = max(distance - curve.x, 0.0);
        float range = ReflectionFogCurveRange();
        float ramp = saturate(travelled / range);
        float exponent = curve.w + 1;
        return travelled + curve.z * (range * pow(ramp, exponent) / exponent + max(travelled - range, 0.0));
    }

    static float LogHeightProfile(float4 height, float worldHeight)
    {
        return min((height.x - worldHeight) * height.y, 0.0) + min((worldHeight - height.z) * height.w, 0.0);
    }

    static float MeanExpAlongRamp(float2 fromLogAndExp, float2 toLogAndExp)
    {
        float logSpan = toLogAndExp.x - fromLogAndExp.x;
        return abs(logSpan) < kMinExponentSpan ? fromLogAndExp.y * (1 + 0.5 * logSpan)
                                               : (toLogAndExp.y - fromLogAndExp.y) / logSpan;
    }

    static float2 HeightProfileLogAndExp(float4 height, float worldHeight)
    {
        float logProfile = LogHeightProfile(height, worldHeight);
        return float2(logProfile, exp(logProfile));
    }

    static float MeanHeightProfile(float4 height, float lowHeight, float highHeight)
    {
        float heightSpan = highHeight - lowHeight;
        float firstKink = clamp(min(height.x, height.z), lowHeight, highHeight);
        float secondKink = clamp(max(height.x, height.z), lowHeight, highHeight);
        float2 atLow = HeightProfileLogAndExp(height, lowHeight);
        float2 atFirstKink = HeightProfileLogAndExp(height, firstKink);
        float2 atSecondKink = HeightProfileLogAndExp(height, secondKink);
        float2 atHigh = HeightProfileLogAndExp(height, highHeight);
        float integral = (firstKink - lowHeight) * MeanExpAlongRamp(atLow, atFirstKink) +
                         (secondKink - firstKink) * MeanExpAlongRamp(atFirstKink, atSecondKink) +
                         (highHeight - secondKink) * MeanExpAlongRamp(atSecondKink, atHigh);
        return heightSpan < kMinProfileHeightSpan ? atLow.y : integral / heightSpan;
    }

    static float ReflectionFogPhase(float4 scattering, float cosToLight)
    {
        float g = scattering.x;
        float r = (1 - g) / sqrt(max(1 + g * g - 2 * g * cosToLight, kMinPhaseDistance));
        return mix(r * r * r, 1.0, scattering.y);
    }

    static float PathHeight(ReflectedPath path, float distance)
    {
        return path.startHeight + path.heightPerYard * (distance - path.from);
    }

    float4 ReflectedPathFog(ReflectedPath path) const
    {
        float3 inscatter = 0;
        float opticalDepth = 0;
        for (int j = 0; j < kReflectionFogLayers; ++j)
        {
            float4 curve = cReflectionFogLayers(j * kReflectionFogLayerRegisters);
            float4 height = cReflectionFogLayers(j * kReflectionFogLayerRegisters + 1);
            float3 emissive = cReflectionFogLayers(j * kReflectionFogLayerRegisters + 2).rgb;
            float3 diffuse = cReflectionFogLayers(j * kReflectionFogLayerRegisters + 3).rgb;
            float4 scattering = cReflectionFogLayers(j * kReflectionFogLayerRegisters + 4);
            float start = max(path.from, curve.x);
            float end = min(path.to, scattering.z);
            if (curve.y <= 0 || end <= start)
                continue;
            float span = ReflectionFogCurveIntegral(curve, end) - ReflectionFogCurveIntegral(curve, start);
            float startHeight = PathHeight(path, start);
            float endHeight = PathHeight(path, end);
            float profile = MeanHeightProfile(height, min(startHeight, endHeight), max(startHeight, endHeight));
            float layerDepth = curve.y * span * profile * exp(-path.skyRise * scattering.w);
            inscatter += (diffuse * ReflectionFogPhase(scattering, path.cosToLight) + emissive) * layerDepth;
            opticalDepth += layerDepth;
        }
        float transmittance = exp(-opticalDepth);
        return float4(opticalDepth > 0 ? inscatter / opticalDepth * (1 - transmittance) : float3(0), transmittance);
    }

    ReflectedPath SkyReflectionPath(WaterPixel w, float3 R) const
    {
        ReflectedPath path;
        path.from = w.cameraDistance;
        path.to = max(ReflectionFogSkyEnd(), w.cameraDistance);
        path.startHeight = w.position.z;
        path.heightPerYard = max(R.z, 0.0);
        path.skyRise = path.heightPerYard;
        path.cosToLight = dot(R, ToLight());
        return path;
    }

    ReflectedPath SurfaceReflectionPath(WaterPixel w, float3 R, float2 hitUv, float hitZ) const
    {
        float2 hitPixel = ViewportOrigin() + hitUv * ViewportSize();
        float3 hit = CameraPositionWorld() + ViewToWorldDirection(ViewRayAtUnitDepth(hitPixel)) * hitZ;
        float pathLength = distance(hit, w.position);
        ReflectedPath path;
        path.from = w.cameraDistance;
        path.to = w.cameraDistance + pathLength;
        path.startHeight = w.position.z;
        path.heightPerYard = (hit.z - w.position.z) / max(pathLength, kMinPathLength);
        path.skyRise = 0;
        path.cosToLight = dot(R, ToLight());
        return path;
    }

    float3 FoggedSkyReflection(WaterPixel w, float3 R, float3 sky) const
    {
        if (!ReflectionFogActive())
            return sky;
        float4 fog = ReflectedPathFog(SkyReflectionPath(w, R));
        return sky * fog.a + fog.rgb;
    }

    float3 FoggedScreenReflection(WaterPixel w, float3 R, float3 colour, float2 hitUv) const
    {
        if (!ReflectionFogActive())
            return colour;
        float hitZ = CopiedViewDepth(sceneDepth, hitUv, kPointMirror);
        ReflectedPath path = SkyReflectionPath(w, R);
        if (hitZ < kSkyHitDepthFraction * MaxFogDistance())
            path = SurfaceReflectionPath(w, R, hitUv, hitZ);
        float4 fog = ReflectedPathFog(path);
        return colour * fog.a + fog.rgb;
    }

    // ---- vw_water.hlsl ----

    static float3 ClassColour(float waterClass)
    {
        float3 primaries = saturate(1 - abs(waterClass - float3(1, 2, 3)));
        return primaries + saturate(1 - abs(waterClass - 4)) * float3(1, 1, 0);
    }

    static float3 RippleColour(RippleSample r)
    {
        float height = 0.5 + 0.5 * clamp(r.height * kDebugRippleHeightGain, -1.0, 1.0);
        return float3(height * mix(1 - kDebugRippleWindowShade, 1.0, r.coverage));
    }

    float3 DebugColour(float3 N, float4 foamAlbedo, float3 transmittance, float3 reflected, RippleSample r) const
    {
        float view = WaterDebugView();
        if (view < kDebugNormal + 0.5)
            return N * 0.5 + 0.5;
        if (view < kDebugFoamCoverage + 0.5)
            return foamAlbedo.aaa;
        if (view < kDebugTransmittance + 0.5)
            return transmittance;
        if (view < kDebugReflection + 0.5)
            return LinearToGamma(reflected);
        if (view < kDebugLiquidClass + 0.5)
            return ClassColour(WaterClassIndex());
        return RippleColour(r);
    }

    float4 Shade(float2 pixel) const
    {
        WaterPixel w = ReconstructWaterPixel(pixel);
        RippleSample r = SampleRipples(w);
        WaveState waves = SampleWaves(w, r.slope);
        float4 foamAlbedo = FoamAlbedo(w, waves);

        float3 N = normalize(float3(-waves.moments.xy, 1));
        float3 V = w.toCamera;
        float3 L = ToLight();
        float NoV = clamp(dot(N, V), kMinCosine, 1.0);
        float variance = SlopeVariance(waves.moments);
        float envRoughness = PerceptualRoughness(mix(EnvironmentRoughnessParameter(), kFoamRoughness, foamAlbedo.a), variance);
        float sunRoughness = PerceptualRoughness(mix(SunRoughnessParameter(), kFoamRoughness, foamAlbedo.a), variance);

        Refraction refraction = RefractScene(w, waves.moments.xy);
        float3 transmittance = Transmittance(refraction.pathDepth);
        float3 inscatter = InScattering(V, NoV, CrestTilt(waves.moments), waves.foam.y, transmittance);
        float3 water = transmittance * refraction.scene + inscatter;
        float3 foam = FoamLight(N, waves.foam.x) * foamAlbedo.rgb + inscatter;
        float3 colour = mix(water, foam, foamAlbedo.a);

        float3 R = ReflectedDirection(V, N);
        float2 hitUv;
        float4 screenReflection = TraceScreenReflection(w, pixel, R, hitUv);
        float3 sky = 0;
        if (screenReflection.a < 1)
            sky = FoggedSkyReflection(w, R, SkyColour(R.z));
        if (screenReflection.a > 0)
            screenReflection.rgb = FoggedScreenReflection(w, R, screenReflection.rgb, hitUv);
        float3 reflected = mix(sky, screenReflection.rgb, screenReflection.a);
        float3 environment = reflected * EnvironmentBrdf(envRoughness, NoV) * ReflectionStrength();
        float3 H = normalize(V + L);
        float environmentAttenuation = 1 - FresnelSchlick(dot(H, V)) * SunVisibility();
        float3 sunSpecular = SunVisibility() * SunColour() * SpecularStrength() * SunGlint(sunRoughness, N, V, L);
        colour += sunSpecular + environmentAttenuation * environment;

        if (WaterDebugView() > 0.5)
            return float4(DebugColour(N, foamAlbedo, transmittance, reflected, r), 1);
        float3 encoded = LinearToGamma(colour);
        if (StockFogApplies())
            encoded = mix(StockFogColour(), encoded, StockFogVisibility(w.waterZ));
        encoded = mix(refraction.copied, encoded, saturate(kShoreFadePerYard * refraction.pathDepth));
        return float4(encoded, 1);
    }
};

fragment float4 vw_water(Varyings in [[stage_in]], constant float4* C [[buffer(0)]],
                         texture2d<float> sceneColour [[texture(0)]], texture2d<float> sceneDepth [[texture(1)]],
                         texture2d<float> waterDepth [[texture(2)]], texture2d<float> surface0 [[texture(3)]],
                         texture2d<float> surface1 [[texture(4)]], texture2d<float> surface2 [[texture(5)]],
                         texture2d<float> surface3 [[texture(6)]], texture2d<float> foamState0 [[texture(7)]],
                         texture2d<float> foamState1 [[texture(8)]], texture2d<float> foamState2 [[texture(9)]],
                         texture2d<float> foamState3 [[texture(10)]], texture2d<float> waveFoamMasks [[texture(11)]],
                         texture2d<float> ripples [[texture(12)]], texture2d<float> shoreFoamMask [[texture(14)]],
                         texture2d<float> depthFoamMask [[texture(15)]])
{
    Water water;
    water.C = C;
    water.sceneColour = sceneColour;
    water.sceneDepth = sceneDepth;
    water.waterDepth = waterDepth;
    water.surface0 = surface0;
    water.surface1 = surface1;
    water.surface2 = surface2;
    water.surface3 = surface3;
    water.foamState0 = foamState0;
    water.foamState1 = foamState1;
    water.foamState2 = foamState2;
    water.foamState3 = foamState3;
    water.waveFoamMasks = waveFoamMasks;
    water.ripples = ripples;
    water.shoreFoamMask = shoreFoamMask;
    water.depthFoamMask = depthFoamMask;
    // HLSL VPOS + 0.5: the pixel's centre
    return water.Shade(in.position.xy);
}

// ---- vw_ripple_step.hlsl ----

constant int kMaxDisturbances = 48;
constant float kDamping = 0.97;
constant float kEdgeRampPerUv = 16;
constant float kNeighbourWeight = 0.5;
constant float kFootprintCore = 0.20;
constant float kFootprintRim = 0.79;

#define cRippleStep C[0]
#define cRippleSegments(i) C[1 + (i)]
#define cRippleShapes(i) C[49 + (i)]

struct RippleStep
{
    constant float4* C;
    texture2d<float> rippleState; // sRippleState, point, clamped

    float2 WindowShift() const { return cRippleStep.xy; }
    float InverseTexels() const { return cRippleStep.z; }
    float DisturbanceCount() const { return cRippleStep.w; }
    float2 State(float2 uv) const { return rippleState.sample(kPointClamp, uv, level(0)).rg; }

    static float EdgeRamp(float2 uv)
    {
        float2 inward = min(uv, 1 - uv) * kEdgeRampPerUv;
        return saturate(min(inward.x, inward.y));
    }

    float2 Propagate(float2 uv) const
    {
        float texel = InverseTexels();
        float2 centre = State(uv);
        float left = State(uv - float2(texel, 0)).r;
        float down = State(uv - float2(0, texel)).r;
        float right = State(uv + float2(texel, 0)).r;
        float up = State(uv + float2(0, texel)).r;
        float next = kDamping * EdgeRamp(uv) * (kNeighbourWeight * (left + down + right + up) - centre.g);
        return float2(next, centre.r);
    }

    static float SegmentDistance(float2 p, float2 from, float2 to)
    {
        float2 along = to - from;
        float lengthSquared = dot(along, along);
        float t = lengthSquared > 0 ? saturate(dot(p - from, along) / lengthSquared) : 0;
        return length(p - from - along * t);
    }

    static float Footprint(float distance, float inverseRadius)
    {
        return 1 - smoothstep(kFootprintCore, kFootprintRim, distance * inverseRadius);
    }

    static float2 Disturb(float2 state, float2 texelCentre, float4 segment, float4 shape)
    {
        float weight = Footprint(SegmentDistance(texelCentre, segment.xy, segment.zw), shape.x);
        float held = weight * shape.z;
        float height = mix(state.x, shape.y, held) + shape.y * (weight - held);
        return float2(height, mix(state.y, height, held));
    }

    float4 At(float2 texel) const
    {
        float2 state = Propagate((texel + WindowShift() + 0.5) * InverseTexels());
        for (int i = 0; i < kMaxDisturbances; i++)
        {
            if (i < DisturbanceCount())
                state = Disturb(state, texel + 0.5, cRippleSegments(i), cRippleShapes(i));
        }
        return float4(state, 0, 0);
    }
};

fragment float4 vw_ripple_step(Varyings in [[stage_in]], constant float4* C [[buffer(0)]],
                               texture2d<float> rippleState [[texture(0)]])
{
    RippleStep step = {C, rippleState};
    return step.At(floor(in.position.xy));
}
