#include "water_ripples_metal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "log.h"

namespace
{
constexpr float kMaxWindowCentreYards = 100000.0f;
constexpr float kHeldFootprint = 1.0f;
constexpr float kAddedImpulse = 0.0f;

int WindowOrigin(float centre, int texels)
{
    return static_cast<int>(std::floor(static_cast<double>(centre) / kWaterRippleTexelYards)) - texels / 2;
}

bool UsableWindowCentre(const float centre[2])
{
    for (int axis = 0; axis < 2; ++axis)
        if (!(std::fabs(centre[axis]) <= kMaxWindowCentreYards))
            return false;
    return true;
}

bool OutsideWindow(const float from[2], const float to[2], float radius, int texels)
{
    for (int axis = 0; axis < 2; ++axis)
        if (std::max(from[axis], to[axis]) < -radius || std::min(from[axis], to[axis]) > texels + radius)
            return true;
    return false;
}
}

WaterRipples::WaterRipples(id<MTLDevice> device, id<MTLLibrary> library) : m_device(device)
{
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"vw_fullscreen"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"vw_ripple_step"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;
    NSError* error = nil;
    m_step = descriptor.fragmentFunction ? [device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
    if (!m_step)
        VF_LOG_ERROR("water ripple step failed: %s", error ? error.localizedDescription.UTF8String : "no function");
}

void WaterRipples::Restart()
{
    m_running = false;
    m_mapsCleared = false;
    m_quietSteps = 0;
    m_current = 0;
}

bool WaterRipples::Fail(const char* reason)
{
    m_failure = reason;
    return false;
}

bool WaterRipples::EnsureMaps(int texels)
{
    if (m_maps[0] && m_maps[1] && m_texels == texels)
        return true;
    m_maps[0] = m_maps[1] = nil;
    m_texels = 0;
    Restart();
    const NSUInteger side = static_cast<NSUInteger>(texels);
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                                                                         width:side height:side mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;
    for (int i = 0; i < 2; ++i)
    {
        m_maps[i] = [m_device newTextureWithDescriptor:descriptor];
        if (!m_maps[i])
        {
            m_maps[0] = m_maps[1] = nil;
            return Fail("ripple map creation failed");
        }
    }
    m_texels = texels;
    return true;
}

bool WaterRipples::Prepare(int texels)
{
    m_failure = "";
    if (!m_step)
        return Fail("ripple shader unsupported");
    if (texels != kWaterRippleTexels && texels != kWaterRippleTexelsLow)
        return Fail("ripple map size is not 256 or 512");
    return EnsureMaps(texels);
}

WaterRippleSchedule WaterRipples::Schedule(double seconds)
{
    WaterRippleSchedule schedule;
    if (!m_running || seconds < m_lastScheduled)
    {
        Restart();
        m_running = true;
        m_epoch = seconds;
        m_stepsDone = 0;
        m_lastScheduled = seconds;
        return schedule;
    }
    m_lastScheduled = seconds;
    const int64_t due = static_cast<int64_t>(std::floor((seconds - m_epoch) * kWaterRippleStepsPerSecond)) - m_stepsDone;
    if (due <= 0)
        return schedule;
    const int64_t dropped = std::max<int64_t>(0, due - kMaxWaterRippleStepsPerFrame);
    m_stepsDone += dropped;
    m_droppedSteps += static_cast<uint32_t>(dropped);
    schedule.dropped = static_cast<uint32_t>(dropped);
    schedule.steps = static_cast<int>(due - dropped);
    for (int i = 0; i < schedule.steps; ++i)
        schedule.stepSeconds[i] = m_epoch + static_cast<double>(m_stepsDone + 1 + i) / kWaterRippleStepsPerSecond;
    m_stepsDone += schedule.steps;
    return schedule;
}

void WaterRipples::ClearMaps(id<MTLCommandBuffer> commands)
{
    for (id<MTLTexture> map : {m_maps[0], m_maps[1]})
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = map;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        encoder.label = @"CoAVolFog ripple clear";
        [encoder endEncoding];
    }
    m_mapsCleared = true;
}

WaterRipples::WindowShift WaterRipples::Recentre(id<MTLCommandBuffer> commands, const float centre[2])
{
    const bool usable = UsableWindowCentre(centre);
    const int desired[2] = {usable ? WindowOrigin(centre[0], m_texels) : m_origin[0],
                            usable ? WindowOrigin(centre[1], m_texels) : m_origin[1]};
    WindowShift shift = {desired[0] - m_origin[0], desired[1] - m_origin[1]};
    if (!m_mapsCleared || std::abs(shift.x) >= m_texels || std::abs(shift.y) >= m_texels)
    {
        ClearMaps(commands);
        shift = {0, 0};
    }
    m_origin[0] = desired[0];
    m_origin[1] = desired[1];
    return shift;
}

// UploadDisturbances: c0 the step, c1.. the segments, c49.. the shapes (vw_ripple_step)
uint32_t WaterRipples::BuildDisturbances(WindowShift shift, const WaterRippleDisturbance* disturbances, uint32_t count,
                                         float (&constants)[1 + 2 * kMaxWaterRippleDisturbances][4])
{
    float (*segments)[4] = &constants[1];
    float (*shapes)[4] = &constants[1 + kMaxWaterRippleDisturbances];
    uint32_t used = 0;
    for (uint32_t i = 0; i < count && used < kMaxWaterRippleDisturbances; ++i)
    {
        const WaterRippleDisturbance& d = disturbances[i];
        const float radius = d.radius / kWaterRippleTexelYards;
        if (!(radius > 0.0f) || !std::isfinite(d.amplitude) || (d.amplitude == 0.0f && !d.held))
            continue;
        float from[2];
        float to[2];
        for (int axis = 0; axis < 2; ++axis)
        {
            from[axis] = static_cast<float>(d.from[axis] / kWaterRippleTexelYards - m_origin[axis]);
            to[axis] = static_cast<float>(d.to[axis] / kWaterRippleTexelYards - m_origin[axis]);
        }
        if (OutsideWindow(from, to, radius, m_texels))
            continue;
        segments[used][0] = from[0];
        segments[used][1] = from[1];
        segments[used][2] = to[0];
        segments[used][3] = to[1];
        shapes[used][0] = 1.0f / radius;
        shapes[used][1] = d.amplitude;
        shapes[used][2] = d.held ? kHeldFootprint : kAddedImpulse;
        ++used;
    }
    constants[0][0] = static_cast<float>(shift.x);
    constants[0][1] = static_cast<float>(shift.y);
    constants[0][2] = 1.0f / m_texels;
    constants[0][3] = static_cast<float>(used);
    return used;
}

void WaterRipples::Step(id<MTLCommandBuffer> commands, const float centre[2], const WaterRippleDisturbance* disturbances,
                        uint32_t count)
{
    if (!m_maps[0] || !m_maps[1] || !m_step)
        return;
    const WindowShift shift = Recentre(commands, centre);
    float constants[1 + 2 * kMaxWaterRippleDisturbances][4] = {};
    const uint32_t used = BuildDisturbances(shift, disturbances, count, constants);

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = m_maps[1 - m_current];
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    encoder.label = @"CoAVolFog ripple step";
    [encoder setRenderPipelineState:m_step];
    [encoder setFragmentBytes:constants length:sizeof(constants) atIndex:0];
    [encoder setFragmentTexture:m_maps[m_current] atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];

    m_current = 1 - m_current;
    ++m_stepsRun;
    m_quietSteps = used ? 0 : m_quietSteps + 1;
    if (m_quietSteps >= kWaterRippleQuietSteps)
        Restart();
}

WaterRippleWindow WaterRipples::Window(double seconds) const
{
    WaterRippleWindow window;
    window.origin[0] = static_cast<float>(m_origin[0] * static_cast<double>(kWaterRippleTexelYards));
    window.origin[1] = static_cast<float>(m_origin[1] * static_cast<double>(kWaterRippleTexelYards));
    window.extent = m_texels * kWaterRippleTexelYards;
    window.texels = m_texels;
    const double progress = (seconds - m_epoch) * kWaterRippleStepsPerSecond - static_cast<double>(m_stepsDone);
    window.weight = static_cast<float>(std::clamp(progress, 0.0, 1.0));
    return window;
}
