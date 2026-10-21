#pragma once

// The settings of CoAVolFog.ini, with the keys, defaults and ranges of the Windows CoAVolFog
// (src/config.h and src/config.cpp of Raz0r1337/coa-vfog). The overlay hotkey has no macOS
// equivalent yet; the in-game panel replaces it in a later slice.

#include <string>

constexpr float kEngineFarClipMin = 183.333f;
constexpr float kEngineFarClipMax = 1583.33f;
constexpr float kFarClipMaxKeepsClientCap = 0.0f;

struct Config
{
    bool enable = true;
    bool hooks = true;
    bool lightStay = true;
    bool overlay = true;
    int quality = 2;
    float density = 1.0f;
    float haze = 1.0f;
    float groundFog = 0.6f;
    float farFog = 1.0f;
    int stockFog = 1;
    bool transparentFog = false;
    int dataMode = 1;
    int colorSpace = 1;
    int classicPhase = 0;
    float sunScatter = 1.0f;
    float ambient = 1.0f;
    float exposure = 1.0f;
    float classicExposure = 1.0f;
    bool localLights = true;
    float localLightIntensity = 1.0f;
    float localLightPhase = 0.3f;
    bool interiorAware = true;
    float interiorDensity = 0.15f;
    float interiorFarFog = 1.0f;
    float noiseAmount = 0.15f;
    float noiseScale = 0.025f;
    float noiseWindSpeed = 0.5f;
    bool classicNoise = true;
    float godRays = 0.0f;
    bool glowCompensation = true;
    int foreverGlow = 0;
    float colorGrading = 0.0f;
    float farClipMax = kFarClipMaxKeepsClientCap;
    float maxDistance = 5000.0f;
    float temporal = 0.85f;
    bool underwater = false;
    bool liquidDepth = true;
    bool multisampling = true;
    int debugView = 0;
    bool sunMarker = false;
    int logLevel = 1;
    bool water = true;
    int waterQuality = 2;
    float waterWaves = 1.0f;
    float waterWind = 2.0f;
    float waterFoam = 1.0f;
    float waterReflections = 1.0f;
    float waterSpecular = 1.0f;
    float waterClarity = 1.0f;
    float waterZoneColors = 0.5f;
    float waterRipples = 0.5f;
    bool waterClientSplashes = false;
    int waterDebugView = 0;
};

// Reads the [CoAVolFog] section of path; a missing file or key keeps the default. Returns whether the
// file was read.
bool LoadConfig(const std::string& path, Config& out);

// Whether two configurations draw the same fog (the temporal history survives only that)
bool SameFogSettings(const Config& a, const Config& b);

// "Key=value" lines for every setting, for the log
std::string DescribeConfig(const Config& config);
