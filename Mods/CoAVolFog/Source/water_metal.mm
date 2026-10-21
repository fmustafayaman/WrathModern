#include "water_metal.h"

#import <Metal/Metal.h>
#include <string>
#include "WMModAPI.h"
#include "Engine/Source/Gx/Gx.h"
#include "engine.h"
#include "log.h"
#include "vf_fog_source.h"
#include "water_classify.h"

struct WaterRenderer::Impl
{
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = nil;
    id<MTLRenderPipelineState> classPipeline = nil;
    MTLPixelFormat classFormat = MTLPixelFormatInvalid;
    NSUInteger classSamples = 0;
    id<MTLDepthStencilState> classStencil = nil;
    bool failed = false;
};

void WaterRenderer::Release()
{
    delete m_impl;
    m_impl = nullptr;
}

void WaterRenderer::LiquidsBegin(const Config& cfg)
{
    m_inLiquids = cfg.enable && cfg.water;
    m_classesDrawn = 0;
}

void WaterRenderer::LiquidDraw(const void* settings, bool begin)
{
    if (!m_inLiquids)
        return;

    if (begin)
    {
        WaterClass waterClass = engine::ClassifyWaterSettings(settings);
        GxMetalSetStencilTag(static_cast<uint32_t>(waterClass));
        m_tagged = waterClass != WaterClass::None;
        if (m_tagged)
            m_classesDrawn |= 1u << static_cast<uint32_t>(waterClass);
    }
    else if (m_tagged)
    {
        GxMetalSetStencilTag(0);
        m_tagged = false;
    }
}

void WaterRenderer::LiquidsEnd(const WMModHost* host, const Config& cfg)
{
    GxMetalSetStencilTag(0);
    m_tagged = false;
    bool wasInLiquids = m_inLiquids;
    m_inLiquids = false;

    if (!wasInLiquids || cfg.waterDebugView != 5 || !m_classesDrawn)
        return;

    if (!m_impl)
    {
        m_impl = new Impl();
        m_impl->device = (__bridge id<MTLDevice>)host->device;
    }

    Impl& impl = *m_impl;
    if (impl.failed)
        return;

    WMFrameTargets targets = {};
    if (!host->AcquireFrameTargets(&targets) || !targets.depth)
        return;

    id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)targets.commandBuffer;
    id<MTLTexture> color = (__bridge id<MTLTexture>)targets.color;
    id<MTLTexture> depth = (__bridge id<MTLTexture>)targets.depth;

    if (depth.pixelFormat != MTLPixelFormatDepth32Float_Stencil8)
        return;

    if (!impl.library)
    {
        NSError* error = nil;
        impl.library = [impl.device newLibraryWithSource:@(kVfFogSource) options:nil error:&error];
        if (!impl.library)
        {
            VF_LOG_ERROR("water shaders failed: %s", error.localizedDescription.UTF8String);
            impl.failed = true;
            return;
        }
    }

    if (!impl.classPipeline || impl.classFormat != color.pixelFormat || impl.classSamples != color.sampleCount)
    {
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [impl.library newFunctionWithName:@"vf_fullscreen"];
        descriptor.fragmentFunction = [impl.library newFunctionWithName:@"vw_debug_class"];
        descriptor.colorAttachments[0].pixelFormat = color.pixelFormat;
        descriptor.depthAttachmentPixelFormat = depth.pixelFormat;
        descriptor.stencilAttachmentPixelFormat = depth.pixelFormat;
        descriptor.rasterSampleCount = color.sampleCount;
        NSError* error = nil;
        impl.classPipeline = [impl.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        impl.classFormat = color.pixelFormat;
        impl.classSamples = color.sampleCount;
        if (!impl.classPipeline)
        {
            VF_LOG_ERROR("water class pipeline failed: %s", error.localizedDescription.UTF8String);
            impl.failed = true;
            return;
        }

        MTLStencilDescriptor* stencil = [MTLStencilDescriptor new];
        stencil.stencilCompareFunction = MTLCompareFunctionEqual;
        stencil.readMask = 0xFF;
        stencil.writeMask = 0;
        MTLDepthStencilDescriptor* state = [MTLDepthStencilDescriptor new];
        state.depthCompareFunction = MTLCompareFunctionAlways;
        state.depthWriteEnabled = NO;
        state.frontFaceStencil = stencil;
        state.backFaceStencil = stencil;
        impl.classStencil = [impl.device newDepthStencilStateWithDescriptor:state];
    }

    // water_renderer.cpp's debug colours by class: lake, river, ocean, interior
    const float colours[5][4] = {{0, 0, 0, 1}, {0.1f, 0.5f, 1, 1}, {0.1f, 1, 0.4f, 1}, {1, 0.3f, 0.1f, 1}, {1, 0.9f, 0.1f, 1}};

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
    encoder.label = @"CoAVolFog water classes";
    [encoder setRenderPipelineState:impl.classPipeline];
    [encoder setDepthStencilState:impl.classStencil];
    for (uint32_t waterClass = 1; waterClass < kWaterClassCount; ++waterClass)
    {
        if (!(m_classesDrawn & (1u << waterClass)))
            continue;
        [encoder setStencilReferenceValue:waterClass];
        [encoder setFragmentBytes:colours[waterClass] length:sizeof(colours[waterClass]) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
}
