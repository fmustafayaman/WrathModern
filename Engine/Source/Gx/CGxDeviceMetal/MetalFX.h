#ifndef ENGINE_SOURCE_GX_CGXDEVICEMETAL_METALFX_H
#define ENGINE_SOURCE_GX_CGXDEVICEMETAL_METALFX_H
#import <MetalFX/MetalFX.h>
#include "Engine/Source/Gx/CGxDeviceMetal/MetalContext.h"

namespace GxMetal {
class SpatialUpscaler {
public:
    ~SpatialUpscaler();
    bool Supported(id<MTLDevice> device) const;
    bool Prepare(Context* context, const FormatInfo& format, uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh);
    Texture* Encode(Context* context, Texture* source);
    MTLTextureUsage InputUsage() const { return m_scaler.colorTextureUsage; }
private:
    id<MTLFXSpatialScaler> m_scaler = nil;
    Texture* m_output = nullptr;
};
}
#endif
