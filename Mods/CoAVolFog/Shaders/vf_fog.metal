// The volumetric fog march and composite of the Windows CoAVolFog (shaders/vf_common.hlsli,
// vf_density_variation.hlsli, vf_authored_noise.hlsli, vf_local_lights.hlsli, vf_integrate.hlsli,
// vf_march.hlsl, vf_composite.hlsli, vf_composite.hlsl), ported to Metal function by function.
//
// The HLSL keeps its constants in shader registers c0..c98. Here they stay in one float4 buffer C with the
// same register numbers, and each name is a macro on it, so the CPU side uploads exactly what the Windows
// renderer uploads. HLSL globals (constants, textures) become members of struct Fog and the functions its
// member functions; the bodies are the HLSL's. Variants: STEPS, LOCAL_LIGHTS, SAMPLES_AUTHORED_NOISE and
// DEPTH_MS (the depth target is multisampled; the passes read sample 0, as the Windows build reads its copy).

#include <metal_stdlib>
using namespace metal;

#ifndef STEPS
#define STEPS 24
#endif
#ifndef LOCAL_LIGHTS
#define LOCAL_LIGHTS 1
#endif
#ifndef SAMPLES_AUTHORED_NOISE
#define SAMPLES_AUTHORED_NOISE 0
#endif
#ifndef DEPTH_MS
#define DEPTH_MS 0
#endif

#if DEPTH_MS
typedef depth2d_ms<float> DepthTexture;
#else
typedef depth2d<float> DepthTexture;
#endif

// vf_common.hlsli
#define cWorldViewportPx C[0]
#define cPixelGrid C[1]
#define cViewToNdc C[2]
#define cDepthToViewZ C[3]
#define cLowResTarget C[8]
// vf_integrate.hlsli
#define cLight C[9]
#define cMarch C[11]
#define cLayers(i) C[12 + (i)]
// vf_authored_noise.hlsli
#define cLayerNoise(i) C[36 + (i)]
// vf_local_lights.hlsli
#define cLocalLightControl C[53]
// vf_density_variation.hlsli
#define cDensityVariation C[78]
// vf_composite.hlsli
#define cComposite C[96]
#define cGodRayColour C[97]
#define cSun C[98]

constant float kSkyMinDepth = 0.99903;

constant int kFogLayers = 4;
constant int kRegistersPerLayer = 6;
constant float kGoldenRatioFraction = 0.618034;
constant bool kMarchesLocalLights = LOCAL_LIGHTS;
constant bool kUnrollsLayers = !kMarchesLocalLights;

constant bool kSamplesAuthoredNoise = SAMPLES_AUTHORED_NOISE;
constant int kNoisyLayers = 3;
constant int kRegistersPerLayerNoise = 4;
constant int kOctave0Register = 0;
constant int kOctave1Register = 1;
constant int kFadeRegister = 2;
constant int kOctaveWeightRegister = 3;
constant float kNoiseCurveContrast = 20;
constant float kNoiseCurvePivot = 0.5;
constant float kNoiseCurveShape = (kNoiseCurveContrast - 1) / (2 - kNoiseCurveContrast);

constant float kFirstQuadratureFraction = 0.2113248654;
constant float kQuadratureFractionSpacing = 0.5773502692;
constant float kBeyondEveryLight = 1e30;

constant float kHighlightKnee = 0.8;
constant float kSunMarkerRadius = 6;
constant float4 kSunMarkerColour = float4(1, 0, 0, 1);
constant float kDebugRadiance = 1;
constant float kDebugTransmittance = 2;
constant float kDebugLinearDepth = 3;
constant float kSameSurfaceAbsoluteDepth = 0.5;
constant float kSameSurfaceRelativeDepth = 0.02;
constant float kTapPlaneRelativeDepth = 0.005;
constant float kWholeBilinearWeight = 0.999;
constant float kMaxDisplayedMarchCurvature = 2.0 / 255;

struct FogLayer
{
    float start;
    float density;
    float g;
    float isotropic;
    float3 emissive;
    float strength;
    float3 diffuse;
    float exponent;
    float upperHeight;
    float upperFalloff;
    float lowerHeight;
    float lowerFalloff;
    float3 shadowEmissive;
    float shadowDensity;
    float shadowed;
    float skyFalloff;
    float limit;
    float densityVariation;
};

struct MarchRay
{
    float3 viewDirection;
    float3 directionWorld;
    float cameraHeight;
    float heightPerYard;
};

struct StepVariation
{
    float distance;
    float variation;
};

struct ChordCoverage
{
    float nextEndpoint;
    bool covered;
    float firstLight;
    float lightEnd;
};

struct LocalLight
{
    float3 position;
    float radius;
    float projected;
    float2 chord;
};

struct BilinearTaps
{
    float2 baseTexel;
    float2 fraction;
    float4 currentMarch;
    float depth;
    float sameClassWeight;
    float farthestViewZ;
};

struct Fog
{
    constant float4* C;
    constant float4* localLights;   // sLocalLights: the 32x1 texture of light parameters
    DepthTexture depthTexture;      // sDepth
    texture3d<float> densityNoise;  // sDensityNoise, wrapped
    texture3d<float> authoredNoise; // sAuthoredNoise, wrapped
    texture2d<float> fogTexture;    // sFog
    texture2d<float> godRays;       // sGodRays, linear
    texture2d<float> currentMarch;  // sCurrentMarch
    texture2d<float> sceneBeforeFog; // sSceneBeforeFog, the world resolved before the fog
    texture2d<float> history;       // vf_temporal.hlsl sHistory
    texture2d<float> historyDepth;  // vf_temporal.hlsl sHistoryDepth
    sampler wrapped;
    sampler linearClamp;

    // ---- vf_common.hlsli ----

    float2 ViewportOrigin() const { return cWorldViewportPx.xy; }
    float2 ViewportSize() const { return cWorldViewportPx.zw; }
    float2 ViewportLastPixelCentre() const { return ViewportOrigin() + ViewportSize() - 0.5; }

    bool OutsideViewport(float2 pixel) const
    {
        return any(pixel < ViewportOrigin()) || any(pixel > ViewportOrigin() + ViewportSize());
    }

    float FullPixelsPerLowResTexel() const { return cPixelGrid.x; }
    float FrameIndex() const { return cPixelGrid.y; }
    float2 DepthTexelSize() const { return cPixelGrid.zw; }
    float2 ViewToNdcScale() const { return cViewToNdc.xy; }
    float2 ViewToNdcOffset() const { return cViewToNdc.zw; }
    float DepthAtInfiniteViewZ() const { return cDepthToViewZ.x; }
    float DepthPerInverseViewZ() const { return cDepthToViewZ.y; }
    float MaxFogDistance() const { return cDepthToViewZ.z; }
    float DeepestWorldDepth() const { return cDepthToViewZ.w; }
    float3 CameraPositionWorld() const { return C[7].xyz; }

    // mul(viewDirection, (float3x3)cViewToWorld) with cViewToWorld row_major in c4..c7
    float3 ViewToWorldDirection(float3 v) const { return v.x * C[4].xyz + v.y * C[5].xyz + v.z * C[6].xyz; }

    float2 LowResSize() const { return cLowResTarget.xy; }
    float2 LowResTexelSize() const { return cLowResTarget.zw; }

    // tex2Dlod at the pixel's centre with point filtering: the texel the pixel lies in
    float SampleDepth(float2 pixel) const
    {
        uint2 texel = uint2(clamp(pixel, float2(0), float2(depthTexture.get_width() - 1, depthTexture.get_height() - 1)));
#if DEPTH_MS
        return depthTexture.read(texel, 0);
#else
        return depthTexture.read(texel);
#endif
    }

    bool BeyondFarClip(float depth) const { return depth > DeepestWorldDepth(); }
    bool IsSky(float depth) const { return depth >= max(DeepestWorldDepth(), kSkyMinDepth); }

    float LinearDepth(float depth) const
    {
        return BeyondFarClip(depth) ? MaxFogDistance()
                                    : min(DepthPerInverseViewZ() / (depth - DepthAtInfiniteViewZ()), MaxFogDistance());
    }

    float2 LowResTexelToFullPixel(float2 lowResTexel) const
    {
        float2 pixel = ViewportOrigin() + lowResTexel * FullPixelsPerLowResTexel() +
                       floor(FullPixelsPerLowResTexel() * 0.5) + 0.5;
        return min(pixel, ViewportLastPixelCentre());
    }

    float2 FullPixelToLowResTexel(float2 pixel) const
    {
        return (pixel - ViewportOrigin() - floor(FullPixelsPerLowResTexel() * 0.5) - 0.5) / FullPixelsPerLowResTexel();
    }

    float2 PixelToNdc(float2 pixel) const
    {
        return float2((pixel.x - ViewportOrigin().x) / ViewportSize().x * 2 - 1,
                      1 - (pixel.y - ViewportOrigin().y) / ViewportSize().y * 2);
    }

    float2 LowResTexelToUv(float2 lowResTexel) const { return (lowResTexel + 0.5) * LowResTexelSize(); }

    float2 NdcToPixel(float2 ndc) const
    {
        return ViewportOrigin() + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * ViewportSize();
    }

    float3 ViewRayAtUnitDepth(float2 pixel) const
    {
        float2 ndc = PixelToNdc(pixel);
        return float3((ndc.x - ViewToNdcOffset().x) / ViewToNdcScale().x,
                      (ndc.y - ViewToNdcOffset().y) / ViewToNdcScale().y, 1);
    }

    static float InterleavedGradientNoise(float2 pixel)
    {
        return fract(52.9829189 * fract(dot(pixel, float2(0.06711056, 0.00583715))));
    }

    // tex2Dlod(sampler, LowResTexelToUv(texel)) with point filtering on a low-resolution target
    static float4 ReadLowRes(texture2d<float> t, float2 lowResTexel) { return t.read(uint2(lowResTexel)); }

    // ---- vf_density_variation.hlsli ----

    float DensityVariation(float3 worldPosition) const
    {
        if (cDensityVariation.x <= 0)
            return 1;
        float3 uv = (worldPosition - float3(cDensityVariation.zw, 0)) * (cDensityVariation.y / 32) + 0.5 / 32;
        float noise = densityNoise.sample(wrapped, uv, level(0)).r * (4.0 / 3.0);
        uv = uv * 2 + float3(36.5, 16.5, 52.5) / 32;
        float centered = noise + densityNoise.sample(wrapped, uv, level(0)).r * (2.0 / 3.0) - 1;
        return 1 + saturate(cDensityVariation.x) * centered;
    }

    // ---- vf_authored_noise.hlsli ----

    float4 LayerNoiseRegister(int layer, int registerInLayer) const
    {
        return layer == 0 ? cLayerNoise(registerInLayer)
             : layer == 1 ? cLayerNoise(kRegistersPerLayerNoise + registerInLayer)
                          : cLayerNoise(2 * kRegistersPerLayerNoise + registerInLayer);
    }

    float LayerNoiseAlpha(int layer) const { return layer < kNoisyLayers ? LayerNoiseRegister(layer, kFadeRegister).a : 0; }
    float3 LayerNoiseFade(int layer) const { return LayerNoiseRegister(layer, kFadeRegister).rgb; }

    bool AnyLayerNoise() const
    {
        return any(float3(cLayerNoise(kFadeRegister).a, cLayerNoise(kRegistersPerLayerNoise + kFadeRegister).a,
                          cLayerNoise(2 * kRegistersPerLayerNoise + kFadeRegister).a) > 0);
    }

    float NoiseOctaveSample(int layer, int octaveRegister, float3 worldPosition) const
    {
        float4 octave = LayerNoiseRegister(layer, octaveRegister);
        float3 tileCoordinate = fract((worldPosition - octave.xyz) * octave.w);
        return authoredNoise.sample(wrapped, tileCoordinate, level(0)).r;
    }

    float LayerNoiseSample(int layer, float3 worldPosition) const
    {
        float2 weights = LayerNoiseRegister(layer, kOctaveWeightRegister).xy;
        return weights.x * NoiseOctaveSample(layer, kOctave0Register, worldPosition) +
               weights.y * NoiseOctaveSample(layer, kOctave1Register, worldPosition);
    }

    static float NoiseCurveBelowPivot(float x) { return x * x * (kNoiseCurveShape + 1) / (x + kNoiseCurveShape / 2); }

    static float NoiseDensityCurve(float x)
    {
        float nearest = min(x, 1 - x);
        float curve = NoiseCurveBelowPivot(nearest);
        return x < kNoiseCurvePivot ? curve : 1 - curve;
    }

    float LayerNoiseDensity(int layer, float3 worldPosition) const
    {
        return mix(1.0, NoiseDensityCurve(saturate(LayerNoiseSample(layer, worldPosition))), LayerNoiseAlpha(layer));
    }

    float4 LayerNoiseDensities(float3 worldPosition) const
    {
        float4 densities = 1;
        if (!kSamplesAuthoredNoise || !AnyLayerNoise())
            return densities;
        for (int layer = 0; layer < kNoisyLayers; ++layer)
        {
            if (LayerNoiseAlpha(layer) > 0)
                densities = select(densities, float4(LayerNoiseDensity(layer, worldPosition)), int4(layer) == int4(0, 1, 2, 3));
        }
        return densities;
    }

    // ---- vf_integrate.hlsli ----

    FogLayer LoadConstantFogLayer(int index) const
    {
        int first = index * kRegistersPerLayer;
        FogLayer layer;
        layer.start = cLayers(first).x;
        layer.density = cLayers(first).y;
        layer.g = cLayers(first).z;
        layer.isotropic = cLayers(first).w;
        layer.emissive = cLayers(first + 1).rgb;
        layer.strength = cLayers(first + 1).w;
        layer.diffuse = cLayers(first + 2).rgb;
        layer.exponent = cLayers(first + 2).w;
        layer.upperHeight = cLayers(first + 3).x;
        layer.upperFalloff = cLayers(first + 3).y;
        layer.lowerHeight = cLayers(first + 3).z;
        layer.lowerFalloff = cLayers(first + 3).w;
        layer.shadowEmissive = cLayers(first + 4).rgb;
        layer.shadowDensity = cLayers(first + 4).w;
        layer.shadowed = cLayers(first + 5).x;
        layer.skyFalloff = cLayers(first + 5).y;
        layer.limit = cLayers(first + 5).z;
        layer.densityVariation = cLayers(first + 5).w;
        return layer;
    }

    FogLayer LoadFogLayer(int index) const { return LoadConstantFogLayer(clamp(index, 0, 3)); }

    float3 DirectionToLightView() const { return cLight.xyz; }
    float LightAboveHorizon() const { return cLight.w; }
    bool JitterEnabled() const { return cMarch.x > 0; }
    float DistanceCurveRange() const { return cMarch.y; }
    float HorizonBlendStart() const { return cMarch.z; }
    float FarClip() const { return cMarch.w; }

    static float PhaseHG(float g, float cosAngle)
    {
        float r = (1 - g) / sqrt(max(1 + g * g - 2 * g * cosAngle, 1e-6));
        return r * r * r;
    }

    float DistanceCurve(FogLayer layer, float sampleDistance) const
    {
        return 1 + layer.strength * pow(saturate(max(sampleDistance - layer.start, 0.0) / DistanceCurveRange()) + 1e-6,
                                        layer.exponent);
    }

    static float HeightProfile(FogLayer layer, float height)
    {
        return saturate(exp((layer.upperHeight - height) * layer.upperFalloff)) *
               saturate(exp((height - layer.lowerHeight) * layer.lowerFalloff));
    }

    float HorizonShadow(FogLayer layer) const { return layer.shadowed * (1 - LightAboveHorizon()); }
    float ShadowDensityScale(FogLayer layer) const { return mix(1.0, layer.shadowDensity, HorizonShadow(layer)); }

    static float SkyDensityScale(FogLayer layer, float skyMask, float upward)
    {
        return skyMask > 0 ? exp(-upward * layer.skyFalloff) : 1;
    }

    static float LayerPhase(FogLayer layer, float cosAngle) { return mix(PhaseHG(layer.g, cosAngle), 1.0, layer.isotropic); }

    float DensityVariationAlongRay(MarchRay ray, float distanceAlongRay) const
    {
        return DensityVariation(CameraPositionWorld() + ray.directionWorld * distanceAlongRay);
    }

    static float WeightedVariation(float variation, float weight)
    {
        return kSamplesAuthoredNoise ? mix(1.0, variation, weight) : variation;
    }

    float DensityProfile(FogLayer layer, MarchRay ray, float distanceAlongRay, float variation) const
    {
        float density = DistanceCurve(layer, distanceAlongRay) *
                        HeightProfile(layer, ray.cameraHeight + ray.heightPerYard * distanceAlongRay);
        return layer.densityVariation > 0 ? density * WeightedVariation(variation, layer.densityVariation) : density;
    }

    // ---- vf_local_lights.hlsli ----

    float LocalLightCount() const { return cLocalLightControl.x; }
    float LocalLightReachLimit() const { return cLocalLightControl.y; }
    float LocalLightPhaseG() const { return cLocalLightControl.z; }
    float4 LocalLightParameter(int index) const { return localLights[clamp(index, 0, 31)]; }
    float3 LocalLightColour(int light) const { return LocalLightParameter(light * 3 + 1).rgb; }
    float3 LocalLightAttenuation(int light) const { return LocalLightParameter(light * 3 + 2).xyz; }

    LocalLight LoadLocalLight(int index, float3 viewDirection) const
    {
        float4 positionRadius = LocalLightParameter(index * 3);
        LocalLight light;
        light.position = positionRadius.xyz;
        light.radius = positionRadius.w;
        light.projected = dot(light.position, viewDirection);
        float perpendicularSquared = max(dot(light.position, light.position) - light.projected * light.projected, 0.0);
        float discriminant = light.radius * light.radius - perpendicularSquared;
        float halfChord = sqrt(max(discriminant, 0.0));
        light.chord = discriminant > 0 ? float2(light.projected - halfChord, light.projected + halfChord)
                                       : float2(kBeyondEveryLight, -kBeyondEveryLight);
        return light;
    }

    static float QuadratureFraction(int sample) { return kFirstQuadratureFraction + sample * kQuadratureFractionSpacing; }

    ChordCoverage ChordCoverageFrom(ChordCoverage previous, float3 viewDirection, float distanceAlongRay,
                                    float marchLength) const
    {
        ChordCoverage coverage;
        coverage.nextEndpoint = kBeyondEveryLight;
        coverage.covered = false;
        coverage.firstLight = previous.lightEnd;
        coverage.lightEnd = previous.firstLight;
        for (int index = int(previous.firstLight); index < int(previous.lightEnd); ++index)
        {
            float2 chord = LoadLocalLight(index, viewDirection).chord;
            float endpoint = chord.x > distanceAlongRay ? chord.x : chord.y;
            coverage.nextEndpoint = endpoint > distanceAlongRay ? min(coverage.nextEndpoint, endpoint)
                                                                : coverage.nextEndpoint;
            coverage.covered = coverage.covered || (chord.x <= distanceAlongRay && chord.y > distanceAlongRay);
            if (chord.y > distanceAlongRay && chord.x < marchLength)
            {
                coverage.firstLight = min(coverage.firstLight, float(index));
                coverage.lightEnd = float(index + 1);
            }
        }
        return coverage;
    }

    static bool StepMeetsLocalLights(ChordCoverage coverage, float stepEnd)
    {
        return coverage.covered || coverage.nextEndpoint < stepEnd;
    }

    float LayerLightDensityScale(FogLayer layer, float skyMask, float upward) const
    {
        return layer.density * SkyDensityScale(layer, skyMask, upward) * ShadowDensityScale(layer);
    }

    float4 LayerLightDensityScales(float skyMask, float upward) const
    {
        return float4(LayerLightDensityScale(LoadConstantFogLayer(0), skyMask, upward),
                      LayerLightDensityScale(LoadConstantFogLayer(1), skyMask, upward),
                      LayerLightDensityScale(LoadConstantFogLayer(2), skyMask, upward),
                      LayerLightDensityScale(LoadConstantFogLayer(3), skyMask, upward));
    }

    bool LayerLitInStep(FogLayer layer, float stepStart, float stepEnd) const
    {
        float layerStart = max(stepStart, layer.start);
        return min(stepEnd, layer.limit) - layerStart > 0 && layer.density > 0 &&
               (LocalLightReachLimit() <= 0 || layerStart < LocalLightReachLimit());
    }

    bool4 LayersLitInStep(float stepStart, float stepEnd) const
    {
        return bool4(LayerLitInStep(LoadConstantFogLayer(0), stepStart, stepEnd),
                     LayerLitInStep(LoadConstantFogLayer(1), stepStart, stepEnd),
                     LayerLitInStep(LoadConstantFogLayer(2), stepStart, stepEnd),
                     LayerLitInStep(LoadConstantFogLayer(3), stepStart, stepEnd));
    }

    static bool LayerCoversInterval(FogLayer layer, float2 interval)
    {
        return layer.start <= interval.x && layer.limit >= interval.y;
    }

    bool4 LayersCoverInterval(float2 interval) const
    {
        return bool4(LayerCoversInterval(LoadConstantFogLayer(0), interval),
                     LayerCoversInterval(LoadConstantFogLayer(1), interval),
                     LayerCoversInterval(LoadConstantFogLayer(2), interval),
                     LayerCoversInterval(LoadConstantFogLayer(3), interval));
    }

    float LayerStart(int layer) const { return LoadConstantFogLayer(clamp(layer, 0, 3)).start; }
    float LayerLimit(int layer) const { return LoadConstantFogLayer(clamp(layer, 0, 3)).limit; }
    static float4 LayerSelector(int layer) { return float4(int4(layer) == int4(0, 1, 2, 3)); }

    float2 LayerIntervalWithin(float2 interval, int layer) const
    {
        return float2(max(interval.x, LayerStart(layer)), min(interval.y, LayerLimit(layer)));
    }

    float4 DensityProfiles(MarchRay ray, float distanceAlongRay) const
    {
        float variation = DensityVariationAlongRay(ray, distanceAlongRay);
        return float4(DensityProfile(LoadConstantFogLayer(0), ray, distanceAlongRay, variation),
                      DensityProfile(LoadConstantFogLayer(1), ray, distanceAlongRay, variation),
                      DensityProfile(LoadConstantFogLayer(2), ray, distanceAlongRay, variation),
                      DensityProfile(LoadConstantFogLayer(3), ray, distanceAlongRay, variation));
    }

    float SampleDensity(MarchRay ray, float distanceAlongRay, float4 layerMask, float4 densityScales) const
    {
        return dot(DensityProfiles(ray, distanceAlongRay) * layerMask, densityScales);
    }

    float LightSampleScattering(LocalLight light, float3 attenuation, float3 viewDirection, float distanceAlongRay,
                                float density) const
    {
        float3 toLight = light.position - viewDirection * distanceAlongRay;
        float distanceSquared = dot(toLight, toLight);
        float inverseDistance = rsqrt(max(distanceSquared, 1e-6));
        float lightDistance = max(distanceSquared, 1e-6) * inverseDistance;
        float falloff = 1.0 / max(attenuation.x + attenuation.y * lightDistance + attenuation.z * distanceSquared, 1.0);
        float boundaryFade = saturate((light.radius - lightDistance) * 4 / max(light.radius, 1e-3));
        float cosAngle = (light.projected - distanceAlongRay) * inverseDistance;
        return falloff * boundaryFade * PhaseHG(LocalLightPhaseG(), cosAngle) * density;
    }

    float IntervalScattering(LocalLight light, float3 attenuation, float2 interval, float4 layerMask,
                             float4 densityScales, MarchRay ray) const
    {
        float quadrature = 0;
        for (int sample = 0; sample < 2; ++sample)
        {
            float distanceAlongRay = mix(interval.x, interval.y, QuadratureFraction(sample));
            quadrature += LightSampleScattering(light, attenuation, ray.viewDirection, distanceAlongRay,
                                                SampleDensity(ray, distanceAlongRay, layerMask, densityScales));
        }
        return 0.5 * quadrature * (interval.y - interval.x);
    }

    float2 PieceInterval(int piece, float2 interval) const
    {
        return piece == 0 ? interval : LayerIntervalWithin(interval, piece - 1);
    }

    float4 PieceLayers(int piece, float2 interval, float2 pieceInterval, float4 litLayers) const
    {
        return piece == 0 ? select(float4(0), litLayers, LayersCoverInterval(interval))
                          : (any(pieceInterval != interval) ? litLayers * LayerSelector(piece - 1) : float4(0));
    }

    float LightOwnIntervalsScattering(LocalLight light, float3 attenuation, float2 interval, bool wholeStep,
                                      bool layerBoundaryInStep, float4 litLayers, float4 densityScales, MarchRay ray) const
    {
        float scattering = 0;
        int lastPiece = layerBoundaryInStep ? kFogLayers : 0;
        for (int piece = wholeStep ? 1 : 0; piece <= lastPiece; ++piece)
        {
            float2 pieceInterval = PieceInterval(piece, interval);
            float4 pieceLayers = PieceLayers(piece, interval, pieceInterval, litLayers);
            if (pieceInterval.y > pieceInterval.x && any(pieceLayers != 0))
                scattering += IntervalScattering(light, attenuation, pieceInterval, pieceLayers, densityScales, ray);
        }
        return scattering;
    }

    static bool ChordCoversStep(float2 chord, float2 step) { return chord.x <= step.x && chord.y >= step.y; }

    float3 WholeStepLightScattering(ChordCoverage coverage, float4 stepLayers, float4 densityScales, MarchRay ray,
                                    float2 step) const
    {
        float first = mix(step.x, step.y, QuadratureFraction(0));
        float second = mix(step.x, step.y, QuadratureFraction(1));
        float atFirst = SampleDensity(ray, first, stepLayers, densityScales);
        float atSecond = SampleDensity(ray, second, stepLayers, densityScales);
        float3 scattering = 0;
        for (int index = int(coverage.firstLight); index < int(coverage.lightEnd); ++index)
        {
            LocalLight light = LoadLocalLight(index, ray.viewDirection);
            if (ChordCoversStep(light.chord, step))
            {
                float3 attenuation = LocalLightAttenuation(index);
                scattering += LocalLightColour(index) *
                              (LightSampleScattering(light, attenuation, ray.viewDirection, first, atFirst) +
                               LightSampleScattering(light, attenuation, ray.viewDirection, second, atSecond));
            }
        }
        return 0.5 * (step.y - step.x) * scattering;
    }

    float3 LocalLightScattering(ChordCoverage coverage, float4 densityScales, MarchRay ray, float stepStart,
                                float stepEnd, float4 layerNoiseDensities) const
    {
        float2 step = float2(stepStart, stepEnd);
        float4 litLayers = select(float4(0), layerNoiseDensities, LayersLitInStep(stepStart, stepEnd));
        bool4 layersCoverStep = LayersCoverInterval(step);
        bool layerBoundaryInStep = any(select(litLayers, float4(0), layersCoverStep) != 0);
        bool lightCoversStep = coverage.covered;
        float3 scattering = 0;
        if (coverage.nextEndpoint < stepEnd || layerBoundaryInStep)
        {
            lightCoversStep = false;
            for (int index = int(coverage.firstLight); index < int(coverage.lightEnd); ++index)
            {
                LocalLight light = LoadLocalLight(index, ray.viewDirection);
                float2 interval = float2(max(stepStart, light.chord.x), min(stepEnd, light.chord.y));
                bool wholeStep = ChordCoversStep(light.chord, step);
                lightCoversStep = lightCoversStep || wholeStep;
                float amount = 0;
                if (interval.y > interval.x && (!wholeStep || layerBoundaryInStep))
                    amount = LightOwnIntervalsScattering(light, LocalLightAttenuation(index), interval, wholeStep,
                                                         layerBoundaryInStep, litLayers, densityScales, ray);
                if (amount != 0)
                    scattering += LocalLightColour(index) * amount;
            }
        }
        float4 stepLayers = select(float4(0), litLayers, layersCoverStep);
        if (lightCoversStep && any(stepLayers != 0))
            scattering += WholeStepLightScattering(coverage, stepLayers, densityScales, ray, step);
        return scattering;
    }

    // ---- vf_integrate.hlsli (march) ----

    StepVariation StepVariationAt(MarchRay ray, float stepStart, float stepEnd, float jitter) const
    {
        StepVariation step;
        step.distance = stepStart + (stepEnd - stepStart) * jitter;
        step.variation = DensityVariationAlongRay(ray, step.distance);
        return step;
    }

    float LayerVariation(StepVariation step, MarchRay ray, float sampleDistance) const
    {
        if (kUnrollsLayers && sampleDistance == step.distance)
            return step.variation;
        return DensityVariationAlongRay(ray, sampleDistance);
    }

    void AccumulateLayer(FogLayer layer, int layerIndex, float noiseDensity, float cosToLight, float skyDensityScale,
                         float stepStart, float stepEnd, float jitter, MarchRay ray, StepVariation stepVariation,
                         thread float3& radiance, thread float& opticalDepth) const
    {
        float layerStart = max(stepStart, layer.start);
        float layerLength = max(min(stepEnd, layer.limit) - layerStart, 0.0);
        if (layerLength <= 0 || layer.density <= 0)
            return;
        float shadow = HorizonShadow(layer);
        float shadowDensityScale = ShadowDensityScale(layer);
        float sampleDistance = layerStart + layerLength * jitter;
        float sampleHeight = ray.cameraHeight + ray.heightPerYard * sampleDistance;
        float distanceCurve = DistanceCurve(layer, sampleDistance);
        float heightProfile = HeightProfile(layer, sampleHeight);
        float variation = 1;
        if (layer.densityVariation > 0 && cDensityVariation.x > 0)
            variation = WeightedVariation(LayerVariation(stepVariation, ray, sampleDistance), layer.densityVariation);
        float directLight = 1 - shadow;
        float layerOpticalDepth = layer.density * skyDensityScale * layerLength * distanceCurve * heightProfile *
                                  shadowDensityScale * variation * noiseDensity;
        float3 emissive = mix(layer.emissive, layer.shadowEmissive, shadow);
        if (kSamplesAuthoredNoise && layerIndex < kNoisyLayers)
            emissive = mix(emissive, LayerNoiseFade(layerIndex), 1 - noiseDensity);
        float phase = LayerPhase(layer, cosToLight);
        radiance += (layer.diffuse * (directLight * phase) + emissive) * layerOpticalDepth;
        opticalDepth += layerOpticalDepth;
    }

    float3 StepSamplePosition(MarchRay ray, float stepStart, float stepEnd, float jitter) const
    {
        return CameraPositionWorld() + ray.directionWorld * (stepStart + (stepEnd - stepStart) * jitter);
    }

    void AccumulateLayers(float4 noiseDensities, float cosToLight, float skyMask, float upward, float stepStart,
                          float stepEnd, float jitter, MarchRay ray, thread float3& radiance, thread float& opticalDepth) const
    {
        StepVariation stepVariation = StepVariationAt(ray, stepStart, stepEnd, jitter);
        for (int j = 0; j < kFogLayers; j++)
        {
            FogLayer layer = LoadConstantFogLayer(j);
            float noiseDensity = kUnrollsLayers ? noiseDensities[j]
                                                : (kSamplesAuthoredNoise ? dot(noiseDensities, LayerSelector(j)) : 1.0);
            AccumulateLayer(layer, j, noiseDensity, cosToLight, SkyDensityScale(layer, skyMask, upward), stepStart,
                            stepEnd, jitter, ray, stepVariation, radiance, opticalDepth);
        }
    }

    static float LayerMarchEnd(FogLayer layer) { return layer.density > 0 ? layer.limit : 0; }

    float LayersMarchEnd() const
    {
        return max(max(LayerMarchEnd(LoadConstantFogLayer(0)), LayerMarchEnd(LoadConstantFogLayer(1))),
                   max(LayerMarchEnd(LoadConstantFogLayer(2)), LayerMarchEnd(LoadConstantFogLayer(3))));
    }

    float StepJitter(float2 lowResTexel) const
    {
        return JitterEnabled() ? fract(InterleavedGradientNoise(lowResTexel) + FrameIndex() * kGoldenRatioFraction) : 0.5;
    }

    float4 IntegrateFogAtDepth(float2 pixel, float depth, float jitter) const
    {
        float3 viewRay = ViewRayAtUnitDepth(pixel);
        float distancePerViewZ = length(viewRay);
        float3 viewDirection = viewRay / distancePerViewZ;
        float skyMask = IsSky(depth) ? 1 : 0;
        float viewZ = LinearDepth(depth);
        float horizonBlend = max(BeyondFarClip(depth) ? 1.0 : 0.0, smoothstep(HorizonBlendStart(), FarClip(), viewZ));
        viewZ = mix(viewZ, MaxFogDistance(), horizonBlend);
        float marchLength = min(viewZ * distancePerViewZ, MaxFogDistance());

        float3 cameraWorld = CameraPositionWorld();
        float3 directionWorld = ViewToWorldDirection(viewDirection);
        float cosToLight = dot(DirectionToLightView(), viewDirection);
        float upward = max(directionWorld.z, 0.0);
        float riseLevelledAtHorizon = mix(directionWorld.z, upward, horizonBlend);
        MarchRay ray = {viewDirection, directionWorld, cameraWorld.z, riseLevelledAtHorizon};
        ChordCoverage lightCoverage = {0, false, 0, LocalLightCount()};
        float4 lightDensityScales = LayerLightDensityScales(skyMask, upward);

        float layersEnd = LayersMarchEnd();
        float3 inScatteredRadiance = 0;
        float transmittance = 1;
        const float stepFraction = 1.0 / STEPS;
        for (int s = 0; s < STEPS; s++)
        {
            float startFraction = s * stepFraction;
            float endFraction = startFraction + stepFraction;
            float stepStart = marchLength * startFraction * startFraction;
            if (kUnrollsLayers && stepStart >= layersEnd)
                break;
            float stepEnd = marchLength * endFraction * endFraction;
            float3 stepRadiance = 0;
            float stepOpticalDepth = 0;
            float4 noiseDensities = LayerNoiseDensities(StepSamplePosition(ray, stepStart, stepEnd, jitter));
            AccumulateLayers(noiseDensities, cosToLight, skyMask, upward, stepStart, stepEnd, jitter, ray, stepRadiance,
                             stepOpticalDepth);
            if (kMarchesLocalLights && lightCoverage.nextEndpoint <= stepStart)
                lightCoverage = ChordCoverageFrom(lightCoverage, viewDirection, stepStart, marchLength);
            if (kMarchesLocalLights && StepMeetsLocalLights(lightCoverage, stepEnd))
                stepRadiance += LocalLightScattering(lightCoverage, lightDensityScales, ray, stepStart, stepEnd,
                                                     noiseDensities);
            if (stepOpticalDepth > 0)
            {
                float stepOpacity = stepOpticalDepth < 1e-3
                    ? stepOpticalDepth * (1 - 0.5 * stepOpticalDepth + stepOpticalDepth * stepOpticalDepth / 6)
                    : 1 - exp(-stepOpticalDepth);
                inScatteredRadiance += transmittance * stepRadiance * (stepOpacity / stepOpticalDepth);
                transmittance *= 1 - stepOpacity;
            }
        }
        return float4(clamp(inScatteredRadiance, float3(0), float3(65504)), 1 - transmittance);
    }

    float4 IntegrateFogAtPixel(float2 pixel, float jitter) const
    {
        return IntegrateFogAtDepth(pixel, SampleDepth(pixel), jitter);
    }

    // ---- vf_history_depth.hlsl ----

    float4 HistoryDepthAt(float2 lowResTexel) const
    {
        float depth = SampleDepth(LowResTexelToFullPixel(lowResTexel));
        float packed = floor(saturate(LinearDepth(depth) / MaxFogDistance()) * 65535 + 0.5);
        float highByte = floor(packed / 256);
        float lowByte = packed - highByte * 256;
        float depthClass = IsSky(depth) ? 1 : (BeyondFarClip(depth) ? 0.5 : 0);
        return float4(highByte / 255, lowByte / 255, depthClass, 1);
    }

    // ---- vf_temporal.hlsl (cViewToPreviousClip row_major in c9..c12, cTemporal c13) ----

    float HistoryWeight() const { return C[13].x; }
    bool HistoryInvalid() const { return C[13].y <= 0; }

    float LightingHistoryWeight(float3 currentMean, float3 previous) const
    {
        float weight = HistoryWeight();
        if (C[13].z > 0)
        {
            float3 difference = abs(currentMean - previous);
            float peakDifference = max(difference.r, max(difference.g, difference.b));
            float3 peak = max(currentMean, previous);
            float brightness = max(0.05, max(peak.r, max(peak.g, peak.b)));
            weight *= 1 - smoothstep(0.20, 0.40, peakDifference / brightness);
        }
        return weight;
    }

    // tex2Dlod(sCurrent, uv) with point filtering and clamped addressing
    float4 ReadCurrent(float2 uv) const
    {
        float2 texel = clamp(floor(uv * LowResSize()), float2(0), LowResSize() - 1);
        return currentMarch.read(uint2(texel));
    }

    void CurrentNeighbourhood(float2 uv, float4 centre, thread float4& lowest, thread float4& highest, thread float4& mean) const
    {
        lowest = centre;
        highest = centre;
        float4 sum = centre;
        const float2 offsets[8] = {float2(-1, -1), float2(0, -1), float2(1, -1), float2(-1, 0),
                                   float2(1, 0),   float2(-1, 1), float2(0, 1),  float2(1, 1)};
        for (int k = 0; k < 8; k++)
        {
            float4 neighbour = ReadCurrent(uv + offsets[k] * LowResTexelSize());
            lowest = min(lowest, neighbour);
            highest = max(highest, neighbour);
            sum += neighbour;
        }
        mean = sum / 9;
    }

    static bool MatchesSurface(float expectedDepth, float depthClass, float tapViewZ, float tapDepthClass)
    {
        float tolerance = max(0.5, expectedDepth * 0.02);
        return abs(tapDepthClass - depthClass) < 0.25 && (depthClass > 0 || abs(tapViewZ - expectedDepth) <= tolerance);
    }

    float DepthClass(float depth) const { return IsSky(depth) ? 1 : (BeyondFarClip(depth) ? 0.5 : 0); }

    bool ValidatedHistory(float2 uv, float expectedDepth, float depthClass, thread float4& value) const
    {
        float2 texel = uv * LowResSize() - 0.5;
        float2 baseTexel = floor(texel);
        float2 fraction = texel - baseTexel;
        const float2 offsets[4] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
        float4 sum = 0;
        float weightSum = 0;
        for (int i = 0; i < 4; ++i)
        {
            float2 tap = clamp(baseTexel + offsets[i], float2(0), LowResSize() - 1);
            float3 previousDepth = historyDepth.read(uint2(tap)).rgb;
            float previousViewZ = dot(previousDepth.rg, float2(65280, 255)) * (MaxFogDistance() / 65535);
            float valid = MatchesSurface(expectedDepth, depthClass, previousViewZ, previousDepth.b) ? 1 : 0;
            float2 bilinear = mix(1 - fraction, fraction, offsets[i]);
            float weight = bilinear.x * bilinear.y * valid;
            sum += history.read(uint2(tap)) * weight;
            weightSum += weight;
        }
        value = sum / max(weightSum, 1e-5);
        return weightSum > 1e-5;
    }

    bool ReprojectedHistory(float2 pixel, float depth, float viewZ, float depthClass, thread float4& value) const
    {
        if (HistoryInvalid())
            return false;
        float4 v = float4(ViewRayAtUnitDepth(pixel) * viewZ, IsSky(depth) ? 0 : 1);
        float4 previousClip = v.x * C[9] + v.y * C[10] + v.z * C[11] + v.w * C[12];
        if (previousClip.w <= 1e-3)
            return false;
        float2 previousPixel = NdcToPixel(previousClip.xy / previousClip.w);
        float2 previousUv = LowResTexelToUv(FullPixelToLowResTexel(previousPixel));
        if (any(previousUv < 0) || any(previousUv > 1))
            return false;
        return ValidatedHistory(previousUv, previousClip.w, depthClass, value);
    }

    bool InsideLowResTarget(float2 lowResTexel) const { return all(lowResTexel >= 0) && all(lowResTexel < LowResSize()); }

    float4 SameSurfaceCurrentAverage(float2 lowResTexel, float viewZ, float depthClass) const
    {
        float4 sum = 0;
        float weightSum = 0;
        for (int k = 0; k < 9; ++k)
        {
            float2 tap = lowResTexel + float2(k % 3, k / 3) - 1;
            float tapDepth = SampleDepth(LowResTexelToFullPixel(tap));
            float weight = InsideLowResTarget(tap) &&
                           MatchesSurface(viewZ, depthClass, LinearDepth(tapDepth), DepthClass(tapDepth)) ? 1 : 0;
            if (weight > 0)
                sum += currentMarch.read(uint2(tap));
            weightSum += weight;
        }
        return sum / max(weightSum, 1.0);
    }

    float4 Temporal(float2 lowResTexel) const
    {
        float2 uv = LowResTexelToUv(lowResTexel);
        float2 pixel = LowResTexelToFullPixel(lowResTexel);
        float depth = SampleDepth(pixel);
        float viewZ = LinearDepth(depth);
        float depthClass = DepthClass(depth);
        float4 reprojected;
        if (!ReprojectedHistory(pixel, depth, viewZ, depthClass, reprojected))
            return SameSurfaceCurrentAverage(lowResTexel, viewZ, depthClass);

        float4 current = currentMarch.read(uint2(lowResTexel));
        float4 lowest, highest, mean;
        CurrentNeighbourhood(uv, current, lowest, highest, mean);
        float4 previous = clamp(reprojected, lowest, highest);
        return mix(current, previous, LightingHistoryWeight(mean.rgb, previous.rgb));
    }

    // ---- vf_composite.hlsli ----

    float Exposure() const { return cComposite.x; }
    float GodRayStrength() const { return cComposite.y; }
    float DebugView() const { return cComposite.z; }
    bool DebugViewEnabled() const { return DebugView() > 0.5; }
    bool DebugViewAtMost(float view) const { return DebugView() < view + 0.5; }
    float BlendMode() const { return cComposite.w; }
    bool BlendsInLinearLight() const { return BlendMode() > 0.5 && BlendMode() < 2.5; }
    bool BlendsOverSceneCopy() const { return (BlendMode() > 0.5 && BlendMode() < 1.5) || BlendMode() > 2.5; }
    float2 SunPixel() const { return cSun.xy; }
    bool SunMarkerEnabled() const { return cSun.z > 0; }
    float ClientGlowToCompensate() const { return cSun.w; }

    static float3 GammaToLinear(float3 colour) { return pow(max(colour, 0.0), float3(2.2)); }
    static float3 LinearToGamma(float3 colour) { return pow(max(colour, 0.0), float3(1 / 2.2)); }

    static float3 RollOffHighlights(float3 colour, float3 knee)
    {
        float3 span = max(1 - knee, 1e-4);
        return select(knee + span * (1 - exp(-(colour - knee) / span)), colour, colour <= knee);
    }

    static float3 BeforeClientGlow(float3 onScreen, float glow)
    {
        return 2 * onScreen / (1 + sqrt(1 + 4 * glow * onScreen));
    }

    bool SameDepthClass(float depth, float tapDepth) const
    {
        return IsSky(depth) == IsSky(tapDepth) && BeyondFarClip(depth) == BeyondFarClip(tapDepth);
    }

    static float2 TapOffset(int j) { return float2(fract(j * 0.5) * 2, floor(j * 0.5)); }

    BilinearTaps GatherBilinearTaps(float2 pixel, float depth, float viewZ) const
    {
        float2 lowResCoord = FullPixelToLowResTexel(pixel);
        BilinearTaps taps;
        taps.baseTexel = floor(lowResCoord);
        taps.fraction = lowResCoord - taps.baseTexel;
        taps.currentMarch = 0;
        taps.depth = 0;
        taps.sameClassWeight = 0;
        taps.farthestViewZ = viewZ;
        for (int j = 0; j < 4; j++)
        {
            float2 tapOffset = TapOffset(j);
            float2 tapTexel = clamp(taps.baseTexel + tapOffset, float2(0), LowResSize() - 1);
            float tapDepth = SampleDepth(LowResTexelToFullPixel(tapTexel));
            float2 bilinearWeight = mix(1 - taps.fraction, taps.fraction, tapOffset);
            float weight = bilinearWeight.x * bilinearWeight.y;
            taps.currentMarch += ReadLowRes(currentMarch, tapTexel) * weight;
            taps.depth += tapDepth * weight;
            taps.sameClassWeight += SameDepthClass(depth, tapDepth) ? weight : 0;
            taps.farthestViewZ = max(taps.farthestViewZ, weight > 0 ? LinearDepth(tapDepth) : viewZ);
        }
        return taps;
    }

    bool PixelOnTapPlane(BilinearTaps taps, float viewZ) const
    {
        return taps.sameClassWeight > kWholeBilinearWeight && taps.farthestViewZ < HorizonBlendStart() &&
               abs(LinearDepth(taps.depth) - viewZ) <= viewZ * kTapPlaneRelativeDepth;
    }

    bool RayMeetsLocalLight(float2 pixel, float viewZ) const
    {
        float3 viewRay = ViewRayAtUnitDepth(pixel);
        float distancePerViewZ = length(viewRay);
        float3 viewDirection = viewRay / distancePerViewZ;
        float rayLength = viewZ * distancePerViewZ;
        bool meets = false;
        for (int index = 0; index < int(LocalLightCount()); ++index)
        {
            float2 chord = LoadLocalLight(index, viewDirection).chord;
            meets = meets || (chord.y > 0 && chord.x < rayLength);
        }
        return meets;
    }

    float4 DisplayedCurrentMarch(float2 lowResTexel) const
    {
        float4 fog = ReadLowRes(currentMarch, clamp(lowResTexel, float2(0), LowResSize() - 1));
        fog.rgb *= Exposure();
        fog.rgb = BlendsInLinearLight() ? sqrt(max(fog.rgb, 0.0)) : fog.rgb;
        return fog;
    }

    float4 DisplayedMarchCurvature(float2 firstTexel, float2 axis) const
    {
        float4 before = DisplayedCurrentMarch(firstTexel - axis);
        float4 first = DisplayedCurrentMarch(firstTexel);
        float4 second = DisplayedCurrentMarch(firstTexel + axis);
        float4 after = DisplayedCurrentMarch(firstTexel + 2 * axis);
        return max(abs(before - 2 * first + second), abs(first - 2 * second + after));
    }

    bool MarchLinearBetweenTaps(BilinearTaps taps) const
    {
        float4 curvature = 0;
        for (int k = 0; k < 4; k++)
        {
            float2 axis = k < 2 ? float2(0, 1) : float2(1, 0);
            float2 across = 1 - axis;
            bool secondLine = k == 1 || k == 3;
            bool interpolated = dot(taps.fraction, axis) > 0 && (!secondLine || dot(taps.fraction, across) > 0);
            float2 firstTexel = taps.baseTexel + (secondLine ? across : float2(0, 0));
            if (interpolated)
                curvature = max(curvature, DisplayedMarchCurvature(firstTexel, axis));
        }
        return max(max(curvature.r, curvature.g), max(curvature.b, curvature.a)) <= kMaxDisplayedMarchCurvature;
    }

    bool TapsReproduceMarch(BilinearTaps taps, float viewZ) const
    {
        if (!PixelOnTapPlane(taps, viewZ))
            return false;
        return MarchLinearBetweenTaps(taps);
    }

    bool FullResolutionMarchTakesNoiseAtItsMean() const
    {
        return kMarchesLocalLights && !kSamplesAuthoredNoise && AnyLayerNoise();
    }

    bool LightOnRayNeedsFullResolution(float2 pixel, float viewZ) const
    {
        if (!kMarchesLocalLights || FullResolutionMarchTakesNoiseAtItsMean())
            return false;
        return RayMeetsLocalLight(pixel, viewZ);
    }

    float4 FilteredFogBetweenTaps(BilinearTaps taps) const
    {
        float4 fog = 0;
        for (int j = 0; j < 4; j++)
        {
            float2 tapOffset = TapOffset(j);
            float2 tapTexel = clamp(taps.baseTexel + tapOffset, float2(0), LowResSize() - 1);
            float2 bilinearWeight = mix(1 - taps.fraction, taps.fraction, tapOffset);
            fog += ReadLowRes(fogTexture, tapTexel) * bilinearWeight.x * bilinearWeight.y;
        }
        return fog;
    }

    float4 FogWithoutMatchingTap(float2 pixel, float depth, float viewZ) const
    {
        if (!LightOnRayNeedsFullResolution(pixel, viewZ))
        {
            BilinearTaps taps = GatherBilinearTaps(pixel, depth, viewZ);
            if (FullResolutionMarchTakesNoiseAtItsMean())
            {
                if (PixelOnTapPlane(taps, viewZ))
                    return FilteredFogBetweenTaps(taps);
            }
            else
            {
                if (TapsReproduceMarch(taps, viewZ))
                    return taps.currentMarch;
            }
        }
        return IntegrateFogAtDepth(pixel, depth, 0.5);
    }

    float4 DepthAwareUpsample(float2 pixel, float depth, float viewZ) const
    {
        float2 lowResCoord = FullPixelToLowResTexel(pixel);
        float2 baseTexel = floor(lowResCoord);
        float2 bilinearFraction = lowResCoord - baseTexel;

        float4 weightedFog = 0;
        float weightSum = 0;
        for (int j = 0; j < 4; j++)
        {
            float2 tapOffset = TapOffset(j);
            float2 tapTexel = clamp(baseTexel + tapOffset, float2(0), LowResSize() - 1);
            float tapDepth = SampleDepth(LowResTexelToFullPixel(tapTexel));
            float tapViewZ = LinearDepth(tapDepth);
            float2 bilinearWeight = mix(1 - bilinearFraction, bilinearFraction, tapOffset);
            bool sameClass = SameDepthClass(depth, tapDepth);
            bool sameDepth = abs(tapViewZ - viewZ) <= max(kSameSurfaceAbsoluteDepth, viewZ * kSameSurfaceRelativeDepth);
            float relativeDepthDifference = abs(tapViewZ - viewZ) / max(viewZ, 1e-3);
            float weight = sameClass && sameDepth ?
                bilinearWeight.x * bilinearWeight.y / (1e-3 + relativeDepthDifference) : 0;
            weightedFog += ReadLowRes(fogTexture, tapTexel) * weight;
            weightSum += weight;
        }
        if (weightSum > 1e-6)
            return weightedFog / weightSum;
        return FogWithoutMatchingTap(pixel, depth, viewZ);
    }

    float3 DisplaySpaceGodRays(float2 viewportUv) const
    {
        float3 rays = 0;
        if (GodRayStrength() > 0)
            rays = godRays.sample(linearClamp, viewportUv, level(0)).rgb * cGodRayColour.rgb * GodRayStrength();
        return rays;
    }

    float3 BlendGodRays(float3 colour, float3 rays) const
    {
        if (GodRayStrength() > 0)
        {
            float glow = ClientGlowToCompensate();
            float3 displayed = colour * (1 + glow * colour);
            displayed += max(1 - displayed, 0.0) * (1 - exp(-max(rays, 0.0)));
            colour = BeforeClientGlow(displayed, glow);
        }
        return colour;
    }

    float3 FogSceneColour(float3 scene, float4 fog) const
    {
        if (!BlendsInLinearLight())
        {
            float3 fogColour = RollOffHighlights(fog.rgb / max(fog.a, 1e-4), float3(kHighlightKnee));
            return scene * (1 - fog.a) + fogColour * fog.a;
        }
        scene = GammaToLinear(scene);
        float3 colour = LinearToGamma(RollOffHighlights(scene * (1 - fog.a) + fog.rgb, max(float3(kHighlightKnee), scene)));
        if (ClientGlowToCompensate() > 0)
            colour = mix(colour, BeforeClientGlow(colour, ClientGlowToCompensate()), fog.a);
        return colour;
    }

    // tex2Dlod(sSceneBeforeFog, viewportUv) with point filtering: the pixel's own scene colour
    float3 SceneAt(float2 viewportUv) const
    {
        float2 size = float2(sceneBeforeFog.get_width(), sceneBeforeFog.get_height());
        return sceneBeforeFog.read(uint2(clamp(floor(viewportUv * size), float2(0), size - 1))).rgb;
    }

    float4 BlendOverSceneCopy(float2 viewportUv, float4 fog, float3 rays) const
    {
        return float4(BlendGodRays(FogSceneColour(SceneAt(viewportUv), fog), rays), 1);
    }

    float4 PremultipliedForFixedFunctionBlend(float4 fog, float3 rays, bool linearLight) const
    {
        float opacity = max(fog.a, 1e-4);
        float3 unpremultiplied = RollOffHighlights(fog.rgb / opacity, float3(kHighlightKnee));
        if (linearLight)
            unpremultiplied = LinearToGamma(unpremultiplied);
        return float4(unpremultiplied * fog.a + rays, fog.a);
    }

    float4 CompositeAtDepth(float2 pixel, float depth) const
    {
        float viewZ = LinearDepth(depth);
        float4 fog = DepthAwareUpsample(pixel, depth, viewZ);
        fog.rgb *= Exposure();

        float2 viewportUv = (pixel - ViewportOrigin()) / ViewportSize();
        float3 rays = DisplaySpaceGodRays(viewportUv);
        const bool linearLight = BlendsInLinearLight();

        if (DebugViewEnabled())
        {
            if (DebugViewAtMost(kDebugRadiance))
                return float4((linearLight ? LinearToGamma(saturate(fog.rgb)) : fog.rgb) + rays, 1);
            if (DebugViewAtMost(kDebugTransmittance))
                return float4(1 - fog.aaa, 1);
            if (DebugViewAtMost(kDebugLinearDepth))
                return float4(float3(saturate(viewZ / MaxFogDistance())), 1);
        }
        if (SunMarkerEnabled() && distance(pixel, SunPixel()) < kSunMarkerRadius)
            return kSunMarkerColour;

        if (BlendsOverSceneCopy())
            return BlendOverSceneCopy(viewportUv, fog, rays);
        return PremultipliedForFixedFunctionBlend(fog, rays, linearLight);
    }
};

struct Varyings
{
    float4 position [[position]];
};

// vf_fullscreen_vs.hlsl draws one triangle whose edges lie outside the viewport (NDC -1.5 and 4.5)
vertex Varyings vf_fullscreen(uint vid [[vertex_id]])
{
    float2 ndc[3] = {float2(-1.5, -1.5), float2(4.5, -1.5), float2(-1.5, 4.5)};
    Varyings out;
    out.position = float4(ndc[vid], 0.0, 1.0);
    return out;
}

#define FOG_ARGUMENTS                                                                                           \
    constant float4* C [[buffer(0)]], constant float4* localLights [[buffer(1)]],                              \
        DepthTexture depthTexture [[texture(0)]], texture2d<float> fogTexture [[texture(1)]],                  \
        texture2d<float> godRays [[texture(2)]], texture2d<float> currentMarch [[texture(4)]],                 \
        texture2d<float> sceneBeforeFog [[texture(3)]],                                                        \
        texture2d<float> history [[texture(5)]], texture2d<float> historyDepth [[texture(6)]],                 \
        texture3d<float> densityNoise [[texture(9)]], texture3d<float> authoredNoise [[texture(10)]]

#define MAKE_FOG                                                                                                \
    constexpr sampler wrappedSampler(filter::linear, address::repeat);                                          \
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);                                    \
    Fog fog = {C, localLights, depthTexture, densityNoise, authoredNoise, fogTexture, godRays, currentMarch,     \
               sceneBeforeFog, history, historyDepth, wrappedSampler, linearSampler}

// vf_march.hlsl: lowResTexel is ps_3_0's VPOS, the integer texel
fragment float4 vf_march(Varyings in [[stage_in]], FOG_ARGUMENTS)
{
    MAKE_FOG;
    float2 lowResTexel = floor(in.position.xy);
    return fog.IntegrateFogAtPixel(fog.LowResTexelToFullPixel(lowResTexel), fog.StepJitter(lowResTexel));
}

// vf_composite.hlsl: pixel = VPOS + 0.5, Metal's position
fragment float4 vf_composite(Varyings in [[stage_in]], FOG_ARGUMENTS)
{
    MAKE_FOG;
    float2 pixel = in.position.xy;
    return fog.CompositeAtDepth(pixel, fog.SampleDepth(pixel));
}

// vf_temporal.hlsl
fragment float4 vf_temporal(Varyings in [[stage_in]], FOG_ARGUMENTS)
{
    MAKE_FOG;
    return fog.Temporal(floor(in.position.xy));
}

// vf_history_depth.hlsl
fragment float4 vf_history_depth(Varyings in [[stage_in]], FOG_ARGUMENTS)
{
    MAKE_FOG;
    return fog.HistoryDepthAt(floor(in.position.xy));
}

// CopySceneForGodRays: StretchRect with linear filtering of the world into the ray target
fragment float4 vf_downsample(Varyings in [[stage_in]], texture2d<float> source [[texture(0)]])
{
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);
    float2 size = float2(source.get_width(), source.get_height());
    float2 target = float2(max(1.0, floor(size.x / 4)), max(1.0, floor(size.y / 4)));
    return float4(source.sample(linearSampler, in.position.xy / target, level(0)).rgb, 1);
}

// vf_ray_mask.hlsl: cRayMask c9, cRaySource c10; sDownsampledScene linear
fragment float4 vf_ray_mask(Varyings in [[stage_in]], FOG_ARGUMENTS, texture2d<float> downsampled [[texture(7)]])
{
    MAKE_FOG;
    const float3 kLumaWeights = float3(0.299, 0.587, 0.114);
    const float kThresholdRampSlope = 4;
    float2 rayTexel = floor(in.position.xy);
    float2 rayTexelSize = C[10].zw;
    float2 fullPixelsPerRayTexel = fog.ViewportSize() * rayTexelSize;
    float2 pixel = min(fog.ViewportOrigin() + (rayTexel + 0.5) * fullPixelsPerRayTexel, fog.ViewportLastPixelCentre());
    float skyMask = fog.IsSky(fog.SampleDepth(pixel)) ? 1 : 0;
    float3 viewDirection = normalize(fog.ViewRayAtUnitDepth(pixel));
    float sunFalloff = pow(saturate(dot(viewDirection, C[9].xyz)), C[9].w);
    float3 scene = downsampled.sample(linearSampler, (rayTexel + 0.5) * rayTexelSize, level(0)).rgb;
    float luminance = dot(scene, kLumaWeights);
    float brightSky = skyMask * sunFalloff * saturate((luminance - C[10].y) * kThresholdRampSlope);
    return float4(scene * brightSky, 1);
}

// vf_ray_blur.hlsl: cRadialBlur c9, cRayTarget c10; sSource linear
fragment float4 vf_ray_blur(Varyings in [[stage_in]], constant float4* C [[buffer(0)]], texture2d<float> source [[texture(0)]])
{
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);
    const int kTaps = 8;
    const float kDecayPerTap = 0.9;
    float2 uv = (floor(in.position.xy) + 0.5) * C[10].zw;
    float2 tapStep = (C[9].xy - uv) * C[9].z;
    float3 sum = 0;
    float tapWeight = 1;
    for (int k = 0; k < kTaps; k++)
    {
        sum += source.sample(linearSampler, uv + tapStep * k, level(0)).rgb * tapWeight;
        tapWeight *= kDecayPerTap;
    }
    return float4(sum * C[9].w, 1);
}
