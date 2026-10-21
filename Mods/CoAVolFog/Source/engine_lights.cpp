// engine_lights.cpp of the Windows CoAVolFog without its reads of Wow.exe memory: light selection,
// ranking and view tests are unchanged; CaptureLocalLightInputs walks the client's M2 scene in engine_mac.mm.

#include "engine_lights.h"


#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
constexpr float kMaxWorldCoordinate = 1000000.0f;
constexpr float kMaxLightComponent = 10000.0f;
constexpr float kMaxUploadedLightComponent = 1000000.0f;
constexpr float kMaxAttenuationCoefficient = 1000000.0f;

bool ValidVector(const float values[3], float minimum, float maximum)
{
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(values[i]) || values[i] < minimum || values[i] > maximum)
            return false;
    return true;
}

double SquaredDistance(const float a[3], const float b[3])
{
    double result = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const double delta = static_cast<double>(a[i]) - b[i];
        result += delta * delta;
    }
    return result;
}

float PeakColor(const float color[3])
{
    return std::max(color[0], std::max(color[1], color[2]));
}

float LinearChannel(float channel, float peak)
{
    const float gammaChannel = std::max(channel, 0.0f);
    if (peak <= 1.0f)
        return std::pow(gammaChannel, kLocalLightGamma);
    return std::pow(gammaChannel / peak, kLocalLightGamma) * peak;
}

bool Precedes(const LocalPointLight& a, const LocalPointLight& b, const float cameraPosition[3])
{
    const double aPriority = engine::LocalLightPriority(a, cameraPosition);
    const double bPriority = engine::LocalLightPriority(b, cameraPosition);
    if (aPriority != bPriority)
        return aPriority > bPriority;
    const double aDistance = SquaredDistance(a.position, cameraPosition);
    const double bDistance = SquaredDistance(b.position, cameraPosition);
    if (aDistance != bDistance)
        return aDistance < bDistance;
    for (int i = 0; i < 3; ++i)
        if (a.position[i] != b.position[i])
            return a.position[i] < b.position[i];
    for (int i = 0; i < 3; ++i)
        if (a.color[i] != b.color[i])
            return a.color[i] > b.color[i];
    for (int i = 0; i < 3; ++i)
        if (a.attenuation[i] != b.attenuation[i])
            return a.attenuation[i] < b.attenuation[i];
    return false;
}

bool InsertRankedLight(LocalPointLight* list, uint32_t& count, uint32_t capacity, const LocalPointLight& candidate,
                       const float cameraPosition[3])
{
    uint32_t index = 0;
    while (index < count && !Precedes(candidate, list[index], cameraPosition))
        ++index;
    if (index == capacity)
        return false;
    const uint32_t newCount = std::min(count + 1, capacity);
    for (uint32_t i = newCount - 1; i > index; --i)
        list[i] = list[i - 1];
    list[index] = candidate;
    count = newCount;
    return true;
}

struct ViewPlane
{
    float normal[3];
    float offset;
};

void ViewSpacePosition(const LocalPointLight& light, const LocalLightCamera& camera, float out[3])
{
    float relative[3];
    for (int i = 0; i < 3; ++i)
        relative[i] = light.position[i] - camera.position[i];
    const float* m = camera.cameraRelativeView;
    for (int j = 0; j < 3; ++j)
        out[j] = relative[0] * m[0 * 4 + j] + relative[1] * m[1 * 4 + j] + relative[2] * m[2 * 4 + j];
}

float ProjectionNear(const float* projection)
{
    const float divisor = 1.0f + projection[10];
    const float nearPlane = divisor != 0.0f ? -projection[14] / divisor : 0.0f;
    return std::isfinite(nearPlane) ? std::max(nearPlane, 0.0f) : 0.0f;
}

bool ProjectsView(const LocalLightCamera& camera)
{
    const float* p = camera.glProjection;
    return std::isfinite(p[0]) && std::isfinite(p[5]) && p[0] != 0.0f && p[5] != 0.0f;
}

bool SphereBeyondPlane(const float centre[3], float radius, const ViewPlane& plane)
{
    const float* n = plane.normal;
    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(length > 0.0f))
        return false;
    const float signedDistance = (n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] + plane.offset) / length;
    return signedDistance > radius;
}

bool CandidateInputsValid(const LocalLightInputs& out, const LocalPointLight& light, const float cameraPosition[3],
                          const PointLightUpload& upload)
{
    return out.pointLightCount <= kMaxLocalPointLights && out.candidateCount <= kLocalLightCandidatePool &&
           ValidVector(cameraPosition, -kMaxWorldCoordinate, kMaxWorldCoordinate) &&
           ValidVector(light.position, -kMaxWorldCoordinate, kMaxWorldCoordinate) &&
           ValidVector(light.color, 0.0f, kMaxLightComponent) && std::isfinite(upload.intensity) &&
           upload.intensity >= 0.0f;
}

bool BoundCandidate(const LocalPointLight& light, const PointLightUpload& upload, LocalPointLight& candidate)
{
    candidate = light;
    engine::UploadedPointLightColor(candidate.color, upload, candidate.uploadedColor);
    candidate.cutoff = engine::PointLightCutoff(candidate.uploadedColor, candidate.attenuation);
    return candidate.cutoff > 0.0f;
}

bool RankCandidate(LocalLightInputs& out, const LocalPointLight& candidate, const float cameraPosition[3])
{
    InsertRankedLight(out.candidates, out.candidateCount, kLocalLightCandidatePool, candidate, cameraPosition);
    return InsertRankedLight(out.pointLights, out.pointLightCount, kMaxLocalPointLights, candidate, cameraPosition);
}
}

bool PointLightMeetsView(const LocalPointLight& light, const LocalLightCamera& camera, float margin)
{
    if (!ProjectsView(camera))
        return true;
    float centre[3];
    ViewSpacePosition(light, camera, centre);
    const float* p = camera.glProjection;
    const ViewPlane planes[] = {
        {{0.0f, 0.0f, -1.0f}, ProjectionNear(p)},
        {{p[0], 0.0f, p[8] - 1.0f}, 0.0f},
        {{-p[0], 0.0f, -p[8] - 1.0f}, 0.0f},
        {{0.0f, p[5], p[9] - 1.0f}, 0.0f},
        {{0.0f, -p[5], -p[9] - 1.0f}, 0.0f},
    };
    for (const ViewPlane& plane : planes)
        if (SphereBeyondPlane(centre, light.cutoff + margin, plane))
            return false;
    return true;
}

namespace engine
{
void UploadedPointLightColor(const float captured[3], const PointLightUpload& upload, float uploaded[3])
{
    const float peak = PeakColor(captured);
    for (int channel = 0; channel < 3; ++channel)
        uploaded[channel] = (upload.linear ? LinearChannel(captured[channel], peak)
                                           : std::max(captured[channel], 0.0f)) * upload.intensity;
}

float PointLightCutoff(const float color[3], const float attenuation[3])
{
    if (!ValidVector(color, 0.0f, kMaxUploadedLightComponent) ||
        !ValidVector(attenuation, 0.0f, kMaxAttenuationCoefficient))
        return 0.0f;
    const double peak = PeakColor(color);
    const double target = peak / kLocalPointLightContributionCutoff;
    if (peak <= 0.0 || target <= std::max(static_cast<double>(attenuation[0]), 1.0))
        return 0.0f;
    const double remaining = target - attenuation[0];
    double radius = kMaxLocalPointLightRadius;
    if (attenuation[2] > 0.0f)
    {
        const double linear = attenuation[1];
        radius = 2.0 * remaining /
                 (linear + std::sqrt(linear * linear + 4.0 * attenuation[2] * remaining));
    }
    else if (attenuation[1] > 0.0f)
        radius = remaining / attenuation[1];
    return static_cast<float>(std::min(radius, static_cast<double>(kMaxLocalPointLightRadius)));
}

double LocalLightPriority(const LocalPointLight& light, const float cameraPosition[3])
{
    const double distanceSquared = SquaredDistance(light.position, cameraPosition);
    const double denominator = light.attenuation[0] + light.attenuation[1] * std::sqrt(distanceSquared) +
                               light.attenuation[2] * distanceSquared;
    return PeakColor(light.uploadedColor) / std::max(denominator, 1.0);
}

bool SelectLocalPointLight(LocalLightInputs& out, const LocalPointLight& light, const float cameraPosition[3],
                           const PointLightUpload& upload)
{
    LocalPointLight candidate;
    return CandidateInputsValid(out, light, cameraPosition, upload) && BoundCandidate(light, upload, candidate) &&
           RankCandidate(out, candidate, cameraPosition);
}

bool SelectLocalPointLightInView(LocalLightInputs& out, const LocalPointLight& light,
                                 const LocalLightCamera& camera, const PointLightUpload& upload)
{
    LocalPointLight candidate;
    return CandidateInputsValid(out, light, camera.position, upload) && BoundCandidate(light, upload, candidate) &&
           PointLightMeetsView(candidate, camera) && RankCandidate(out, candidate, camera.position);
}

bool CapturedLightsUsable(LocalLightCapture capture)
{
    return capture != LocalLightCapture::UnsupportedClient && capture != LocalLightCapture::CameraOutOfRange;
}

const char* LocalLightCaptureName(LocalLightCapture capture)
{
    switch (capture)
    {
    case LocalLightCapture::Captured:
        return "captured";
    case LocalLightCapture::UnsupportedClient:
        return "client code differs from build 12340";
    case LocalLightCapture::CameraOutOfRange:
        return "camera position out of range";
    case LocalLightCapture::DamagedTable:
        return "damaged light table";
    case LocalLightCapture::TableTruncated:
        return "light table walk stopped at 8192 nodes";
    case LocalLightCapture::TableChanged:
        return "table changed during the walk";
    case LocalLightCapture::ReadFault:
        return "read fault";
    case LocalLightCapture::InteriorRejected:
        return "camera interior groups rejected";
    }
    return "unknown";
}

}
