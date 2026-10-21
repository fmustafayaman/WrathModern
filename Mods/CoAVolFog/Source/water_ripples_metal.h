#pragma once

// WaterRipples of the Windows CoAVolFog (src/water_ripples.h, .cpp) on Metal: a height field around the camera
// target, stepped at a fixed rate by vw_ripple_step (vw_water.metal) with the contacts' disturbances. The
// schedule, the window and the disturbance upload are the original's; the maps are RG16F render targets.
// Objective-C++.

#import <Metal/Metal.h>

#include <cstdint>
#include "water_contacts.h"

constexpr float kWaterRippleTexelYards = 0.125f;
constexpr double kWaterRippleStepsPerSecond = 30.0;
constexpr double kWaterRippleStepSeconds = 1.0 / kWaterRippleStepsPerSecond;
constexpr int kMaxWaterRippleStepsPerFrame = 4;
constexpr uint32_t kMaxWaterRippleDisturbances = 48;
constexpr int kWaterRippleTexelsLow = 256;
constexpr int kWaterRippleTexels = 512;
constexpr uint32_t kWaterRippleQuietSteps = 450;

struct WaterRippleSchedule
{
    int steps = 0;
    double stepSeconds[kMaxWaterRippleStepsPerFrame] = {};
    uint32_t dropped = 0;
};

struct WaterRippleWindow
{
    float origin[2] = {};
    float extent = 0.0f;
    int texels = 0;
    float weight = 0.0f;
};

class WaterRipples
{
public:
    // The step comes from library (vw_water.metal)
    WaterRipples(id<MTLDevice> device, id<MTLLibrary> library);

    void Restart();
    bool Prepare(int texels);
    WaterRippleSchedule Schedule(double seconds);
    void Step(id<MTLCommandBuffer> commands, const float centre[2], const WaterRippleDisturbance* disturbances,
              uint32_t count);

    bool Running() const { return m_running; }
    bool Visible() const { return m_running && m_mapsCleared; }
    int Texels() const { return m_texels; }
    id<MTLTexture> Map() const { return m_maps[m_current]; }
    WaterRippleWindow Window(double seconds) const;
    uint64_t StepsRun() const { return m_stepsRun; }
    uint32_t DroppedSteps() const { return m_droppedSteps; }
    const char* LastFailure() const { return m_failure; }

private:
    struct WindowShift
    {
        int x;
        int y;
    };

    bool Fail(const char* reason);
    bool EnsureMaps(int texels);
    void ClearMaps(id<MTLCommandBuffer> commands);
    WindowShift Recentre(id<MTLCommandBuffer> commands, const float centre[2]);
    uint32_t BuildDisturbances(WindowShift shift, const WaterRippleDisturbance* disturbances, uint32_t count,
                               float (&constants)[1 + 2 * kMaxWaterRippleDisturbances][4]);

    id<MTLDevice> m_device;
    id<MTLRenderPipelineState> m_step = nil;
    id<MTLTexture> m_maps[2] = {nil, nil};
    const char* m_failure = "";
    int m_texels = 0;
    int m_current = 0;
    bool m_mapsCleared = false;
    int m_origin[2] = {};
    bool m_running = false;
    double m_epoch = 0.0;
    double m_lastScheduled = 0.0;
    int64_t m_stepsDone = 0;
    uint32_t m_quietSteps = 0;
    uint64_t m_stepsRun = 0;
    uint32_t m_droppedSteps = 0;
};
