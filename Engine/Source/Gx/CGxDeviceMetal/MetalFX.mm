#include "Engine/Source/Gx/CGxDeviceMetal/MetalFX.h"

namespace GxMetal {
SpatialUpscaler::~SpatialUpscaler() {
    Clear(m_output);
}

bool SpatialUpscaler::Supported(id<MTLDevice> device) const {
    if (@available(macOS 13.0, *)) {
        return [MTLFXSpatialScalerDescriptor supportsDevice:device];
    }
    return false;
}

bool SpatialUpscaler::Prepare(Context* context, const FormatInfo& format, uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh) {
    if (m_scaler && m_output && m_scaler.inputWidth == iw && m_scaler.inputHeight == ih &&
        m_scaler.outputWidth == ow && m_scaler.outputHeight == oh && m_scaler.colorTextureFormat == format.m_pixelFormat) {
        return true;
    }
    if (!Supported(context->m_device)) return false;
    MTLFXSpatialScalerDescriptor* descriptor = [MTLFXSpatialScalerDescriptor new];
    descriptor.inputWidth = iw; descriptor.inputHeight = ih;
    descriptor.outputWidth = ow; descriptor.outputHeight = oh;
    descriptor.colorTextureFormat = format.m_pixelFormat;
    descriptor.outputTextureFormat = format.m_pixelFormat;
    // WoW writes LDR perceptual colour; the display gamma ramp still runs after UI composition.
    descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
    id<MTLFXSpatialScaler> scaler = [descriptor newSpatialScalerWithDevice:context->m_device];
    if (!scaler) return false;
    Texture* output = Texture::CreateTarget(context, format, ow, oh, 1, scaler.outputTextureUsage);
    if (!output) return false;
    Clear(m_output);
    m_output = output;
    m_scaler = scaler;
    Log("metalfx: spatial %ux%u -> %ux%u", iw, ih, ow, oh);
    return true;
}

Texture* SpatialUpscaler::Encode(Context* context, Texture* source) {
    context->ExecutePendingClears();
    context->EndEncoders();
    id<MTLTexture> input = source->ResolvedTexture();
    if (!m_scaler || !m_output || !input) return nullptr;
    context->SetProfileSection("metalfx spatial");
    m_scaler.colorTexture = input;
    m_scaler.outputTexture = m_output->m_texture;
    m_scaler.inputContentWidth = input.width;
    m_scaler.inputContentHeight = input.height;
    [m_scaler encodeToCommandBuffer:context->Commands()];
    return m_output;
}
}
