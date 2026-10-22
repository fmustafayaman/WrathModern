#include "gpu_profile.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "log.h"

namespace
{
constexpr NSUInteger kSamplesPerBuffer = 256;
constexpr int kBuffers = 6;
constexpr double kReportSeconds = 5.0;

struct Timed
{
    std::string name;
    NSUInteger first;
};

struct Frame
{
    id<MTLCounterSampleBuffer> samples = nil;
    std::vector<Timed> passes;
    NSUInteger used = 0;
    bool busy = false;
};

struct Totals
{
    double milliseconds = 0.0;
    uint64_t passes = 0;
};

struct Profiler
{
    bool enabled = false;
    bool ready = false;
    id<MTLDevice> device = nil;
    Frame frames[kBuffers];
    int current = -1;
    __weak id<MTLCommandBuffer> commands = nil;
    std::mutex lock;
    std::map<std::string, Totals> totals;
    uint64_t framesTimed = 0;
    double reportStart = 0.0;
    double nanosecondsPerTick = 1.0;

    Profiler() { enabled = getenv("COAVOLFOG_GPU_PROFILE") != nullptr; }

    bool Prepare(id<MTLDevice> dev)
    {
        if (ready || !enabled)
            return ready;
        device = dev;
        id<MTLCounterSet> timestamps = nil;
        for (id<MTLCounterSet> set in device.counterSets)
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
                timestamps = set;
        if (!timestamps || ![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
        {
            VF_LOG_ERROR("gpu profile: stage-boundary timestamps unavailable");
            enabled = false;
            return false;
        }
        MTLCounterSampleBufferDescriptor* descriptor = [MTLCounterSampleBufferDescriptor new];
        descriptor.counterSet = timestamps;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = kSamplesPerBuffer;
        for (Frame& frame : frames)
        {
            NSError* error = nil;
            frame.samples = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
            if (!frame.samples)
            {
                VF_LOG_ERROR("gpu profile: %s", error.localizedDescription.UTF8String);
                enabled = false;
                return false;
            }
        }
        MTLTimestamp cpu0 = 0, gpu0 = 0, cpu1 = 0, gpu1 = 0;
        [device sampleTimestamps:&cpu0 gpuTimestamp:&gpu0];
        usleep(20000);
        [device sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
        nanosecondsPerTick = gpu1 > gpu0 ? static_cast<double>(cpu1 - cpu0) / static_cast<double>(gpu1 - gpu0) : 1.0;
        VF_LOG_INFO("gpu profile on (%.3f ns per tick)", nanosecondsPerTick);
        ready = true;
        return true;
    }

    // The frame of this command buffer: a new one starts a slot and reads it back when it completes
    Frame* FrameOf(id<MTLCommandBuffer> buffer)
    {
        if (current >= 0 && commands == buffer)
            return &frames[current];
        int slot = -1;
        {
            std::lock_guard<std::mutex> guard(lock);
            for (int i = 1; i <= kBuffers; ++i)
            {
                int candidate = (current + i + kBuffers) % kBuffers;
                if (!frames[candidate].busy)
                {
                    slot = candidate;
                    break;
                }
            }
            if (slot < 0)
                return nullptr;
            frames[slot].busy = true;
        }
        current = slot;
        commands = buffer;
        Frame* frame = &frames[slot];
        frame->passes.clear();
        frame->used = 0;
        [buffer addCompletedHandler:^(id<MTLCommandBuffer>) { this->Read(frame); }];
        return frame;
    }

    void Read(Frame* frame)
    {
        if (frame->used)
        {
            NSData* data = [frame->samples resolveCounterRange:NSMakeRange(0, frame->used)];
            const MTLCounterResultTimestamp* stamps = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
            std::lock_guard<std::mutex> guard(lock);
            // Passes overlap on the GPU (a pass starts while the one before drains), so a pass is charged the
            // time from the later of its start and the end of the mod's previous pass to its own end
            uint64_t previousEnd = 0;
            for (const Timed& pass : frame->passes)
            {
                uint64_t start = stamps[pass.first].timestamp;
                uint64_t end = stamps[pass.first + 1].timestamp;
                if (start == MTLCounterErrorValue || end == MTLCounterErrorValue || end < start)
                    continue;
                uint64_t from = std::max(start, previousEnd);
                previousEnd = std::max(previousEnd, end);
                Totals& t = totals[pass.name];
                t.milliseconds += (end > from ? end - from : 0) * nanosecondsPerTick * 1e-6;
                ++t.passes;
            }
            ++framesTimed;
            double now = CACurrentMediaTimeSeconds();
            if (reportStart == 0.0)
                reportStart = now;
            if (now - reportStart >= kReportSeconds && framesTimed)
            {
                std::string line;
                double sum = 0.0;
                for (const auto& [name, t] : totals)
                {
                    double perFrame = t.milliseconds / framesTimed;
                    sum += perFrame;
                    char text[128];
                    snprintf(text, sizeof(text), "%s %.2f, ", name.c_str(), perFrame);
                    line += text;
                }
                VF_LOG_INFO("gpu ms per frame (%llu frames): %stotal %.2f", static_cast<unsigned long long>(framesTimed),
                            line.c_str(), sum);
                totals.clear();
                framesTimed = 0;
                reportStart = now;
            }
        }
        std::lock_guard<std::mutex> guard(lock);
        frame->busy = false;
    }

    static double CACurrentMediaTimeSeconds()
    {
        return [[NSProcessInfo processInfo] systemUptime];
    }

    // Two samples (start, end) for the pass; NSNotFound when the frame's buffer is full
    NSUInteger Reserve(Frame* frame, const char* name)
    {
        if (!frame || frame->used + 2 > kSamplesPerBuffer)
            return NSNotFound;
        NSUInteger first = frame->used;
        frame->used += 2;
        frame->passes.push_back({name, first});
        return first;
    }
};

Profiler& Instance()
{
    static Profiler profiler;
    return profiler;
}
}

id<MTLRenderCommandEncoder> ProfiledRenderEncoder(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor* pass, const char* name)
{
    Profiler& p = Instance();
    if (p.enabled && p.Prepare(commands.device))
    {
        Frame* frame = p.FrameOf(commands);
        NSUInteger first = p.Reserve(frame, name);
        if (first != NSNotFound)
        {
            MTLRenderPassSampleBufferAttachmentDescriptor* attachment = pass.sampleBufferAttachments[0];
            attachment.sampleBuffer = frame->samples;
            attachment.startOfVertexSampleIndex = first;
            attachment.endOfVertexSampleIndex = MTLCounterDontSample;
            attachment.startOfFragmentSampleIndex = MTLCounterDontSample;
            attachment.endOfFragmentSampleIndex = first + 1;
        }
    }
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    encoder.label = @(name);
    return encoder;
}

id<MTLComputeCommandEncoder> ProfiledComputeEncoder(id<MTLCommandBuffer> commands, const char* name)
{
    MTLComputePassDescriptor* pass = [MTLComputePassDescriptor computePassDescriptor];
    Profiler& p = Instance();
    if (p.enabled && p.Prepare(commands.device))
    {
        Frame* frame = p.FrameOf(commands);
        NSUInteger first = p.Reserve(frame, name);
        if (first != NSNotFound)
        {
            pass.sampleBufferAttachments[0].sampleBuffer = frame->samples;
            pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = first;
            pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = first + 1;
        }
    }
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoderWithDescriptor:pass];
    encoder.label = @(name);
    return encoder;
}

id<MTLBlitCommandEncoder> ProfiledBlitEncoder(id<MTLCommandBuffer> commands, const char* name)
{
    MTLBlitPassDescriptor* pass = [MTLBlitPassDescriptor blitPassDescriptor];
    Profiler& p = Instance();
    if (p.enabled && p.Prepare(commands.device))
    {
        Frame* frame = p.FrameOf(commands);
        NSUInteger first = p.Reserve(frame, name);
        if (first != NSNotFound)
        {
            pass.sampleBufferAttachments[0].sampleBuffer = frame->samples;
            pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = first;
            pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = first + 1;
        }
    }
    id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoderWithDescriptor:pass];
    encoder.label = @(name);
    return encoder;
}
