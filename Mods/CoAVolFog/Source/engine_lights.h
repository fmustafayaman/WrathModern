#pragma once

#include <cstdint>

constexpr uint32_t kMaxLocalPointLights = 8;
constexpr uint32_t kLocalLightCandidatePool = 24;
constexpr float kLocalPointLightContributionCutoff = 1.0f / 256.0f;
constexpr float kMaxLocalPointLightRadius = 200.0f;
constexpr float kLocalLightGamma = 2.2f;

constexpr uintptr_t kWmoInstanceRoot = 0xF4;
constexpr uintptr_t kWmoRootLoaded = 0x1E0;
constexpr uintptr_t kWmoRootGroupCount = 0x1F4;
constexpr uintptr_t kWmoRootGroups = 0x1F8;
constexpr uintptr_t kWmoGroupLoaded = 0x198;
constexpr uintptr_t kWmoGroupFlags = 0x30;
constexpr uint32_t kWmoExteriorFogFlags = 0x48;

struct CameraInteriorWords
{
    uintptr_t instance;
    uintptr_t groupCount;
    uintptr_t groupIds;
    uintptr_t fogBlend;
};

struct LightCaptureSources
{
    uintptr_t sceneSlot;
    CameraInteriorWords interior;
};

struct PointLightUpload
{
    bool linear = true;
    float intensity = 1.0f;
};

struct LocalLightCamera
{
    float position[3] = {};
    float cameraRelativeView[16] = {};
    float glProjection[16] = {};
};

struct LocalPointLight
{
    float position[3] = {};
    float color[3] = {};
    float attenuation[3] = {};
    float uploadedColor[3] = {};
    float cutoff = 0.0f;
    uint32_t enabled = 1;
    uintptr_t nativeId = 0;
};

enum class LocalLightCapture : uint32_t
{
    Captured,
    UnsupportedClient,
    CameraOutOfRange,
    DamagedTable,
    TableTruncated,
    TableChanged,
    ReadFault,
    InteriorRejected,
};

struct LocalLightInputs
{
    LocalPointLight pointLights[kMaxLocalPointLights] = {};
    uint32_t pointLightCount = 0;
    LocalPointLight candidates[kLocalLightCandidatePool] = {};
    uint32_t candidateCount = 0;
    uint32_t skippedDisabled = 0;
    uint32_t skippedNonPoint = 0;
    float interiorBlend = 0.0f;
    bool cameraInterior = false;
    LocalLightCapture capture = LocalLightCapture::Captured;
};

bool PointLightMeetsView(const LocalPointLight& light, const LocalLightCamera& camera, float margin = 0.0f);

namespace engine
{
void UploadedPointLightColor(const float captured[3], const PointLightUpload& upload, float uploaded[3]);
float PointLightCutoff(const float color[3], const float attenuation[3]);
double LocalLightPriority(const LocalPointLight& light, const float cameraPosition[3]);
bool SelectLocalPointLight(LocalLightInputs& out, const LocalPointLight& light, const float cameraPosition[3],
                           const PointLightUpload& upload);
bool SelectLocalPointLightInView(LocalLightInputs& out, const LocalPointLight& light,
                                 const LocalLightCamera& camera, const PointLightUpload& upload);
bool CapturedLightsUsable(LocalLightCapture capture);
const char* LocalLightCaptureName(LocalLightCapture capture);
LocalLightCapture CapturePointLightTable(uintptr_t sceneSlot, const LocalLightCamera& camera,
                                         const PointLightUpload& upload, LocalLightInputs& out);
LocalLightCapture CaptureLightInputsFrom(const LightCaptureSources& sources, const LocalLightCamera& camera,
                                         bool withPointLights, const PointLightUpload& upload,
                                         LocalLightInputs& out);
bool CaptureLocalLightInputs(const LocalLightCamera& camera, bool withPointLights, const PointLightUpload& upload,
                             LocalLightInputs& out);
}
