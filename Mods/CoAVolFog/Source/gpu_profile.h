#pragma once

// GPU time of each of the mod's passes (the Windows build's GpuTimer, per pass): with COAVOLFOG_GPU_PROFILE set,
// every encoder the mod opens is timed with the GPU's stage-boundary timestamps, and the averages go to the log
// every few seconds. Without it the encoders are opened as usual. Objective-C++.

#import <Metal/Metal.h>

id<MTLRenderCommandEncoder> ProfiledRenderEncoder(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor* pass,
                                                  const char* name);
id<MTLComputeCommandEncoder> ProfiledComputeEncoder(id<MTLCommandBuffer> commands, const char* name);
id<MTLBlitCommandEncoder> ProfiledBlitEncoder(id<MTLCommandBuffer> commands, const char* name);
