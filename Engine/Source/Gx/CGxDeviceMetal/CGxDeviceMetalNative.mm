// The native Metal graphics device, slice 1: the device, its window, the window's render targets,
// clearing and presenting. Textures, buffers, state mapping, shaders and draws follow (the checklist
// is in WOWRE.md); until they exist this device draws nothing, which is what WRATHMODERN_GX=native
// shows, while the default device on the D3D9 shim keeps the client intact.
//
// The window is the platform's (OsMacCreateGameWindow), as for the shim: its content view is
// layer-backed by a CAMetalLayer and the device presents into that layer through GxMetal::Context.

#import <QuartzCore/CAMetalLayer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include "Engine/Source/Gx/CGxDeviceMetal/CGxDeviceMetalNative.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/CGxDevice/EmergencyMem.h"
#include "Engine/Source/Gx/CGxDeviceMetal/MetalFormats.h"
#include "Engine/Source/Os/Mac/MacOs.h"
#include "Engine/Source/Os/Error.h"

namespace {

// The drawable is in pixels; the window's client size is in points and the layer scales it
void DrawableSize(void* window, int32_t* width, int32_t* height) {
    *width = 0;
    *height = 0;
    OsMacGetClientSize(window, width, height);
}

}

CGxDeviceMetalNative::CGxDeviceMetalNative() {
    this->m_hwnd = nullptr;
    this->m_ownhwnd = 0;
    this->m_metal = nullptr;
    this->m_pipelines = nullptr;
    this->m_inScene = 0;
    this->m_backBuffer = nullptr;
    this->m_depthBuffer = nullptr;
    this->m_samples = 1;
    this->m_savedDepthTarget = GxMetal::Target();

    for (uint32_t i = 0; i < 16; i++) {
        this->m_streamBufs[i] = nullptr;
        this->m_streamOffsets[i] = 0;
        this->m_streamStrides[i] = 0;
    }

    this->m_indexBuf = nullptr;
    this->m_streamPassId = 0;
}

CGxDeviceMetalNative::~CGxDeviceMetalNative() {
    this->DeviceDestroy();
}

int32_t CGxDeviceMetalNative::ICreateMetal() {
    if (this->m_metal) {
        return 1;
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();

    if (!device) {
        GxMetal::Log("metal: no device");
        return 0;
    }

    this->m_metal = new GxMetal::Context();

    if (!this->m_metal->Init(device)) {
        delete this->m_metal;
        this->m_metal = nullptr;
        return 0;
    }

    this->m_pipelines = new GxMetal::PipelineCache();
    this->m_pipelines->Init(device);
    GxMetal::Log("metal: native device on %s", [[device name] UTF8String]);
    return 1;
}

void CGxDeviceMetalNative::IDestroyMetal() {
    if (this->m_metal) {
        this->m_metal->EndEncoders();
        this->m_metal->Flush(true);
        delete this->m_metal;
        this->m_metal = nullptr;
    }

    delete this->m_pipelines;
    this->m_pipelines = nullptr;

    for (uint32_t i = 0; i < 16; i++) {
        GxMetal::Clear(this->m_streamBufs[i]);
    }

    GxMetal::Clear(this->m_indexBuf);
    this->m_streamPassId = 0;
}

int32_t CGxDeviceMetalNative::ICreateWindow(CGxFormat& format) {
    OsMacWindowParams params;
    params.m_width = format.m_size.x;
    params.m_height = format.m_size.y;
    params.m_fullscreen = !format.m_window;
    params.m_maximize = format.m_window && format.m_maximize == 1;

    this->m_hwnd = OsMacCreateGameWindow(params, "World of Warcraft");

    if (!this->m_hwnd) {
        return 0;
    }

    this->m_ownhwnd = 1;

    // Fullscreen renders at the format's resolution and the compositor scales it; a window renders at
    // its own size in pixels (as the shim device does)
    if (!format.m_window && format.m_size.x > 0 && format.m_size.y > 0) {
        OsMacSetDrawableSize(this->m_hwnd, format.m_size.x, format.m_size.y);
    } else {
        OsMacSetDrawableSize(this->m_hwnd, 0, 0);
    }

    int32_t width = 0;
    int32_t height = 0;
    DrawableSize(this->m_hwnd, &width, &height);

    if (format.m_window) {
        format.m_size.x = width;
        format.m_size.y = height;
    }

    format.m_pos.x = 0;
    format.m_pos.y = 0;

    CRect rect;
    rect.minX = 0.0f;
    rect.minY = 0.0f;
    rect.maxX = static_cast<float>(width);
    rect.maxY = static_cast<float>(height);
    this->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 0);
    return 1;
}

void CGxDeviceMetalNative::ISetCaps(const CGxFormat& format) {
    GxMetal::FormatInfo color;
    GxMetal::WindowFormatInfo(format.m_colorFormat, color);

    this->m_caps.m_numTmus = 8;
    this->m_caps.m_pixelCenterOnEdge = 0;
    this->m_caps.m_texelCenterOnEdge = 1;
    this->m_caps.m_numStreams = 16;
    // Every stream keeps its own byte offset; with 0 the client would fold the offset of stream 0
    // into a base vertex index and read the other streams from the start of their buffers
    this->m_caps.m_streamOffset = 1;
    this->m_caps.m_colorFormat = GxCF_argb;
    this->m_caps.m_maxIndex = 65535;
    this->m_caps.m_generateMipMaps = 1;
    this->m_caps.m_texMaxSize[GxTex_2d] = 16384;
    this->m_caps.m_texMaxSize[GxTex_CubeMap] = 16384;
    this->m_caps.m_texNonPow2 = 1;
    this->m_caps.m_texFilterTrilinear = 1;
    this->m_caps.m_texFilterAnisotropic = 1;
    this->m_caps.m_maxTexAnisotropy = 16;
    this->m_caps.m_depthBias = 1;
    this->m_caps.m_colorWrite = 1;
    this->m_caps.m_maxClipPlanes = 6;
    this->m_caps.m_hwCursor = 0;
    this->m_caps.m_occlusionQuery = 1;
    this->m_caps.m_pointParameters = 1;
    this->m_caps.m_pointScaleMax = 64.0f;
    this->m_caps.m_pointSprite = 1;
    this->m_caps.m_blendFactor = 1;
    this->m_caps.m_stereoAvailable = 0;
    this->m_caps.m_shaderAlphaRef = 1;

    for (int32_t i = 0; i < GxTexFormats_Last; i++) {
        GxMetal::FormatInfo info;
        this->m_caps.m_texFmt[i] = GxMetal::TexFormatInfo(static_cast<EGxTexFormat>(i), info) ? 1 : 0;
        this->m_caps.m_rttFormat[i] = info.m_renderable ? 1 : 0;
    }

    // The client checks these before it sends lights past the fourth (St0ny's #67 uses 8)
    this->m_caps.m_shaderTargets[GxSh_Vertex] = 1;
    this->m_caps.m_shaderTargets[GxSh_Pixel] = 1;
    this->m_caps.m_shaderConstants[GxSh_Vertex] = 256;
    this->m_caps.m_shaderConstants[GxSh_Pixel] = 256;
}

bool CGxDeviceMetalNative::ICreateWindowTargets(const CGxFormat& format) {
    GxMetal::Clear(this->m_backBuffer);
    GxMetal::Clear(this->m_depthBuffer);

    int32_t width = 0;
    int32_t height = 0;
    DrawableSize(this->m_hwnd, &width, &height);

    if (width <= 0 || height <= 0) {
        return false;
    }

    GxMetal::FormatInfo color;
    GxMetal::FormatInfo depth;

    if (!GxMetal::WindowFormatInfo(format.m_colorFormat, color) || !GxMetal::WindowFormatInfo(format.m_depthFormat, depth)) {
        GxMetal::Log("metal: unsupported window format %d/%d", static_cast<int32_t>(format.m_colorFormat), static_cast<int32_t>(format.m_depthFormat));
        return false;
    }

    uint32_t samples = format.m_multisampleCount > 1 ? format.m_multisampleCount : 1;

    this->m_samples = samples;
    this->m_backBuffer = GxMetal::Texture::CreateTarget(this->m_metal, color, static_cast<uint32_t>(width), static_cast<uint32_t>(height), samples);
    this->m_depthBuffer = GxMetal::Texture::CreateTarget(this->m_metal, depth, static_cast<uint32_t>(width), static_cast<uint32_t>(height), samples);

    if (!this->m_backBuffer || !this->m_depthBuffer) {
        return false;
    }

    this->ISetHwTargets(this->m_backBuffer ? GxMetal::Target { this->m_backBuffer, 0, 0 } : GxMetal::Target(), this->m_depthBuffer ? GxMetal::Target { this->m_depthBuffer, 0, 0 } : GxMetal::Target());
    return true;
}

void CGxDeviceMetalNative::ISetHwTargets(const GxMetal::Target& color, const GxMetal::Target& depth) {
    if (this->m_metal) {
        this->m_metal->SetTargets(color, depth);
    }
}

void CGxDeviceMetalNative::IResetDevice() {
    this->m_inScene = 0;

    if (!this->m_metal) {
        return;
    }

    this->ITargetsFromState();
}

int32_t CGxDeviceMetalNative::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    CGxFormat created = format;

    if (!this->ICreateWindow(created)) {
        return 0;
    }

    int32_t result = this->DeviceCreate(this->m_hwnd, created);
    this->m_format = created;
    return result;
}

int32_t CGxDeviceMetalNative::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_hwnd = window;
    this->m_format = format;

    if (!this->ICreateMetal()) {
        return 0;
    }

    CAMetalLayer* layer = (__bridge CAMetalLayer*)OsMacGetMetalLayer(window);
    this->m_metal->SetLayer(layer);
    this->ISetCaps(format);

    if (!this->ICreateWindowTargets(format)) {
        return 0;
    }

    this->IResetDevice();
    GxMetal::Log("metal: window %dx%d, %u samples", format.m_size.x, format.m_size.y, this->m_samples);
    return 1;
}

void CGxDeviceMetalNative::DeviceDestroy() {
    if (this->m_metal) {
        this->m_metal->EndEncoders();
        this->m_metal->Flush(true);
    }

    GxMetal::Clear(this->m_backBuffer);
    GxMetal::Clear(this->m_depthBuffer);

    if (this->m_metal) {
        this->m_metal->SetTargets(GxMetal::Target(), GxMetal::Target());
        this->m_metal->SetLayer(nil);
    }

    this->IDestroyMetal();

    if (this->m_hwnd && this->m_ownhwnd) {
        OsMacDestroyGameWindow(this->m_hwnd);
    }

    this->m_hwnd = nullptr;
    this->m_ownhwnd = 0;
}

void CGxDeviceMetalNative::DeviceEvictResources() {
    // Nothing cached outside the context yet
}

int32_t CGxDeviceMetalNative::DeviceSetFormat(const CGxFormat& format) {
    this->m_format = format;

    if (!this->m_metal) {
        return 0;
    }

    return this->ICreateWindowTargets(format) ? 1 : 0;
}

void* CGxDeviceMetalNative::DeviceWindow() {
    return this->m_hwnd;
}

void CGxDeviceMetalNative::CapsWindowSize(CRect& rect) {
    rect = this->DeviceCurWindow();
}

void CGxDeviceMetalNative::CapsWindowSizeInScreenCoords(CRect& rect) {
    rect = this->DeviceCurWindow();
}

GxMetal::Target CGxDeviceMetalNative::ITexTarget(CGxTex* texId, uint32_t face, uint32_t level) {
    if (!texId || !texId->m_apiSpecificData) {
        return GxMetal::Target();
    }

    return GxMetal::Target { static_cast<GxMetal::Texture*>(texId->m_apiSpecificData), face, level };
}

// The colour and depth targets the client set: its own texture targets where it set one, the
// window's buffers otherwise (CGxDeviceD3d's m_defColorSurface / m_defDepthStencilSurface)
void CGxDeviceMetalNative::ITargetsFromState() {
    GxMetal::Target color = this->ITexTarget(this->m_textureTarget[GxBuffers_Color].m_texture, this->m_textureTarget[GxBuffers_Color].m_plane, 0);
    GxMetal::Target depth = this->ITexTarget(this->m_textureTarget[GxBuffers_Depth].m_texture, this->m_textureTarget[GxBuffers_Depth].m_plane, 0);

    if (!color.m_texture && this->m_backBuffer) {
        color = GxMetal::Target { this->m_backBuffer, 0, 0 };
    }

    if (!depth.m_texture) {
        depth = this->m_depthBuffer ? GxMetal::Target { this->m_depthBuffer, 0, 0 } : GxMetal::Target();
    }

    this->ISetHwTargets(color, depth);
}

void CGxDeviceMetalNative::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_metal) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    if (target.m_texture == texture && target.m_plane == plane) {
        return;
    }

    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);
    this->ITargetsFromState();
}

void CGxDeviceMetalNative::DeviceSaveDepthSurface() {
    if (this->m_metal) {
        this->m_savedDepthTarget = this->m_metal->DepthTarget();
    }
}

void CGxDeviceMetalNative::DeviceRestoreDepthSurface() {
    if (this->m_metal) {
        this->m_metal->SetTargets(this->m_metal->ColorTarget(), this->m_savedDepthTarget);
    }
}

int32_t CGxDeviceMetalNative::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size: {
            CRect* rect = reinterpret_cast<CRect*>(param1);

            if (!rect || !this->m_hwnd) {
                return 0;
            }

            if (this->m_metal) {
                this->ICreateWindowTargets(this->m_format);
            }

            return 0;
        }
        case GxWM_DisplayChange:
        case GxWM_Destroy:
        case GxWM_SetFocus:
        case GxWM_KillFocus:
        default:
            return 0;
    }
}

void CGxDeviceMetalNative::IPollWindow() {
}

void CGxDeviceMetalNative::SceneClear(uint32_t mask, CImVector color) {
    if (!this->m_metal || !this->m_backBuffer) {
        return;
    }

    int32_t width = 0;
    int32_t height = 0;
    DrawableSize(this->m_hwnd, &width, &height);

    // CImVector is the client's 8-bit b, g, r, a; Metal wants 0-1 r, g, b, a
    float rgba[4] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
    GxMetal::Rect area = { 0, 0, width, height };
    this->m_metal->Clear(area, (mask & 0x1) != 0, (mask & 0x2) != 0, rgba, 1.0f);
}

void CGxDeviceMetalNative::ISceneBegin() {
    if (!this->m_metal) {
        return;
    }

    // The constants of the frame start empty, as on the shim device
    this->m_inScene = 1;
}

void CGxDeviceMetalNative::ISceneEnd() {
    this->m_inScene = 0;
}

void CGxDeviceMetalNative::ScenePresent() {
    if (!this->m_metal) {
        return;
    }

    this->m_metal->MarkTargetsRendered();
    this->m_metal->Present(this->m_backBuffer);
    this->ISceneEnd();
}

void CGxDeviceMetalNative::DeviceSetGamma(float gamma) {
    if (!this->m_metal) {
        return;
    }

    CGxGammaRamp ramp;
    ramp.Set(gamma);
    this->m_metal->SetGammaRamp(ramp.m_red, ramp.m_green, ramp.m_blue);
}

void CGxDeviceMetalNative::DeviceSetGamma(const CGxGammaRamp& ramp) {
    if (this->m_metal) {
        this->m_metal->SetGammaRamp(ramp.m_red, ramp.m_green, ramp.m_blue);
    }
}

void CGxDeviceMetalNative::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    int32_t minX = rect.minX;
    int32_t minY = rect.minY;
    int32_t maxX = rect.maxX;
    int32_t maxY = rect.maxY;

    if (maxX <= minX || maxY <= minY) {
        return;
    }

    pixels.SetCount(static_cast<uint32_t>((maxX - minX) * (maxY - minY)));

    if (!this->m_metal || !this->m_backBuffer) {
        memset(pixels.m_data, 0, pixels.m_count * sizeof(CImVector));
        return;
    }

    GxMetal::Rect area = { minX, minY, maxX, maxY };
    std::vector<uint8_t> bytes;
    uint32_t pitch = 0;

    if (!this->m_metal->Read(GxMetal::Target { this->m_backBuffer, 0, 0 }, area, bytes, pitch)) {
        memset(pixels.m_data, 0, pixels.m_count * sizeof(CImVector));
        return;
    }

    // The back buffer is BGRA8, which is CImVector's byte order
    for (int32_t y = 0; y < maxY - minY; y++) {
        memcpy(pixels.m_data + y * (maxX - minX), bytes.data() + y * pitch, (maxX - minX) * sizeof(CImVector));
    }
}

void CGxDeviceMetalNative::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    depths.SetCount(0);
}

// The client's texture formats, substituted before creation and retried with a tolerable one when the
// GPU cannot make it (CGxDeviceMetal.cpp:30 and :56; the first table is deliberately mutable)
EGxTexFormat CGxDeviceMetalNative::s_GxTexFormatToUse[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Abgr8888, GxTex_Argb8888, GxTex_Argb4444, GxTex_Argb1555, GxTex_Rgb565, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8 };

const EGxTexFormat CGxDeviceMetalNative::s_tolerableTexFmtMapping[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Argb4444, GxTex_Argb4444, GxTex_Argb4444, GxTex_Argb4444, GxTex_Argb4444, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8 };

GxMetal::Texture*& CGxDeviceMetalNative::ITexData(CGxTex* texId) {
    return *reinterpret_cast<GxMetal::Texture**>(&texId->m_apiSpecificData);
}

// The texture a CGxTex holds after ITexForceRecreation: a valid object the client can keep bound
// (opaque green, as the shim's 8x8 m_texture3B58), re-uploaded on its next use
GxMetal::Texture* CGxDeviceMetalNative::IDefaultTexture() {
    if (!this->m_defaultTexture && this->m_metal) {
        GxMetal::FormatInfo info;

        if (GxMetal::TexFormatInfo(GxTex_Argb8888, info)) {
            this->m_defaultTexture = GxMetal::Texture::Create(this->m_metal, MTLTextureType2D, 8, 8, 1, info, false, false);
        }

        if (this->m_defaultTexture) {
            GxMetal::UploadAllocation staging = this->m_metal->AllocateUpload(8 * 8 * 4, 16);

            for (uint32_t i = 0; i < 64; i++) {
                memcpy(staging.m_data + i * 4, "\x00\xFF\x00\xFF", 4);
            }

            this->m_metal->CopyToTexture(staging, 32, 256, this->m_defaultTexture->m_texture, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(8, 8, 1), this->m_defaultTexture->m_use);
        }
    }

    return this->m_defaultTexture;
}

// CGxDeviceMetal.cpp:2527 - create the GPU texture of a client texture (CFR: ITexWHDStartEnd decides
// the level count; the base mip is already folded into width and height)
void CGxDeviceMetalNative::ITexCreate(CGxTex* texId) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t baseMip = 0;
    uint32_t mipCount = 0;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    texId->m_format = s_GxTexFormatToUse[texId->m_format];

    GxMetal::FormatInfo info;

    if (!GxMetal::TexFormatInfo(texId->m_format, info)) {
        return;
    }

    bool renderTarget = texId->m_flags.m_renderTarget != 0;
    bool autoMips = texId->m_flags.m_generateMipMaps != 0;
    GxMetal::Texture* texture = nullptr;

    if (info.m_depth) {
        texture = GxMetal::Texture::Create(this->m_metal, MTLTextureType2D, width, height, 1, info, true, false);
    } else {
        uint32_t levels = mipCount > baseMip ? mipCount - baseMip : 1;
        MTLTextureType type = texId->m_target == GxTex_CubeMap ? MTLTextureTypeCube : MTLTextureType2D;
        texture = GxMetal::Texture::Create(this->m_metal, type, width, height, levels, info, renderTarget, autoMips);

        if (!texture && !renderTarget) {
            // The GPU cannot make the format: use the client's tolerable one for every texture after
            // this, exactly as the shim does
            s_GxTexFormatToUse[texId->m_format] = s_tolerableTexFmtMapping[texId->m_format];
            texId->m_format = s_GxTexFormatToUse[texId->m_format];

            if (!GxMetal::TexFormatInfo(texId->m_format, info)) {
                return;
            }

            texture = GxMetal::Texture::Create(this->m_metal, type, width, height, levels, info, false, autoMips);
        }
    }

    if (!texture) {
        return;
    }

    GxMetal::Assign(this->ITexData(texId), texture);
    texture->Release();
    texId->m_needsCreation = 0;
}

// CGxDeviceMetal.cpp:2585 - upload the updated rectangle into every level and cube face; the client's
// user function hands out the texels through GxTex_Lock/Latch/Unlock
void CGxDeviceMetalNative::ITexUpload(CGxTex* texId) {
    GxMetal::Texture* texture = this->ITexData(texId);

    if (!texture || !texId->m_userFunc) {
        return;
    }

    uint32_t texelStrideInBytes = 0;
    const void* texels = nullptr;
    texId->m_userFunc(GxTex_Lock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t baseMip = 0;
    uint32_t mipCount = 0;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    bool compressedData = texId->m_dataFormat >= GxTex_Dxt1 && texId->m_dataFormat <= GxTex_Dxt5;
    bool renderTarget = texId->m_flags.m_renderTarget != 0;
    uint32_t faceCount = texId->m_target == GxTex_CubeMap ? 6 : 1;
    const GxMetal::FormatInfo& info = texture->m_format;

    for (uint32_t face = 0; face < faceCount; face++) {
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        uint32_t level = 0;

        for (uint32_t mip = baseMip; mip != mipCount; mip++, level++) {
            texels = nullptr;
            texId->m_userFunc(GxTex_Latch, mipWidth, mipHeight, face, mip, texId->m_userArg, texelStrideInBytes, texels);

            if (texels && !renderTarget) {
                int32_t left = texId->m_updateRect.minX >> mip;
                int32_t top = texId->m_updateRect.minY >> mip;
                int32_t right = texId->m_updateRect.maxX >> mip;
                int32_t bottom = texId->m_updateRect.maxY >> mip;

                if (right <= left) {
                    right = left + 1;
                }

                if (bottom <= top) {
                    bottom = top + 1;
                }

                if (texId->m_format >= GxTex_Dxt1 && texId->m_format <= GxTex_Dxt5) {
                    left &= ~3;
                    top &= ~3;
                    right = (right + 3) & ~3;
                    bottom = (bottom + 3) & ~3;

                    if (right > static_cast<int32_t>(mipWidth)) {
                        right = static_cast<int32_t>(mipWidth);
                    }

                    if (bottom > static_cast<int32_t>(mipHeight)) {
                        bottom = static_cast<int32_t>(mipHeight);
                    }
                }

                const uint8_t* source = static_cast<const uint8_t*>(texels);

                if (!texId->m_flags.m_bit15) {
                    if (compressedData) {
                        source += s_texFormatBytesPerBlock[texId->m_dataFormat] * (left >> 2) + texelStrideInBytes * (top >> 2);
                    } else {
                        source += ((left * s_texFormatBitDepth[texId->m_dataFormat]) >> 3) + texelStrideInBytes * top;
                    }
                }

                uint32_t copyWidth = static_cast<uint32_t>(right - left);
                uint32_t copyHeight = static_cast<uint32_t>(bottom - top);
                uint32_t pitch = info.RowPitch(copyWidth);
                uint32_t rows = info.RowCount(copyHeight);
                GxMetal::UploadAllocation staging = this->m_metal->AllocateUpload(pitch * rows, 16);

                C2iVector size;
                size.x = static_cast<int32_t>(copyWidth);
                size.y = static_cast<int32_t>(copyHeight);

                if (Blit(size, BlitAlpha_0, source, texelStrideInBytes, GxGetBlitFormat(texId->m_dataFormat), staging.m_data, pitch, GxGetBlitFormat(texId->m_format))) {
                    this->m_metal->CopyToTexture(staging, pitch, pitch * rows, texture->m_texture, face, level, MTLOriginMake(left, top, 0), MTLSizeMake(copyWidth, copyHeight, 1), texture->m_use);
                    texture->MarkWritten(level);
                }
            }

            mipWidth >>= 1;
            mipHeight >>= 1;

            if (!mipWidth) {
                mipWidth = 1;
            }

            if (!mipHeight) {
                mipHeight = 1;
            }
        }
    }

    texels = nullptr;
    texId->m_userFunc(GxTex_Unlock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
}

// CGxDeviceMetal.cpp:2493 - drop the GPU object of the render targets so they are made again
void CGxDeviceMetalNative::ITexForceRecreation(int32_t all) {
    for (CGxTex* texId = this->m_textures.Head(); texId; texId = this->m_textures.Next(texId)) {
        if (!texId->m_apiSpecificData || !(all || texId->m_flags.m_renderTarget)) {
            continue;
        }

        CiRect updateRect;

        if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
            GxMetal::Clear(this->ITexData(texId));
        }

        texId->m_apiSpecificData = this->IDefaultTexture();
        texId->m_needsCreation = 1;
        this->TexMarkForUpdate(texId, updateRect, 0);

        uint32_t texelStrideInBytes = 0;
        const void* texels = nullptr;

        if (texId->m_userFunc) {
            texId->m_userFunc(GxTex_3, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
        }
    }

    if (this->m_hwnd) {
        this->NotifyOnTextureRecreation();
    }
}

void CGxDeviceMetalNative::ITexMarkAsUpdated(CGxTex* texId) {
    if (!texId->m_needsUpdate || !this->m_metal) {
        return;
    }

    if (texId->m_needsCreation || (!texId->m_apiSpecificData && !texId->m_apiSpecificData2)) {
        this->ITexCreate(texId);
    }

    if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
        this->ITexUpload(texId);
        CGxDevice::ITexMarkAsUpdated(texId);
    }
}

void CGxDeviceMetalNative::TexDestroy(CGxTex* texId) {
    if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
        GxMetal::Clear(this->ITexData(texId));
    }

    CGxDevice::TexDestroy(texId);
}

void CGxDeviceMetalNative::DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane) {
    if (!this->m_metal) {
        return;
    }

    GxMetal::Texture* sourceTexture = nullptr;
    GxMetal::Texture* destTexture = nullptr;

    if (source) {
        if (source->m_needsCreation || (!source->m_apiSpecificData && !source->m_apiSpecificData2)) {
            this->ITexCreate(source);
        }

        if (!source->m_needsCreation) {
            sourceTexture = this->ITexData(source);
        }
    }

    if (dest) {
        if (dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) {
            this->ITexCreate(dest);
        }

        if (!dest->m_needsCreation) {
            destTexture = this->ITexData(dest);
        }
    }

    if (!sourceTexture || !destTexture) {
        return;
    }

    GxMetal::Target from = { sourceTexture, source->m_target == GxTex_CubeMap ? sourcePlane : 0, 0 };
    GxMetal::Target to = { destTexture, dest->m_target == GxTex_CubeMap ? destPlane : 0, 0 };
    this->m_metal->Stretch(from, nullptr, to, nullptr, false);
}

void CGxDeviceMetalNative::DeviceResolveDepthBuffer(CGxTex* texture) {
    if (!this->m_metal) {
        return;
    }

    if (texture && (texture->m_needsCreation || (!texture->m_apiSpecificData && !texture->m_apiSpecificData2))) {
        this->ITexCreate(texture);
    }

    GxMetal::Texture* depth = texture ? this->ITexData(texture) : nullptr;

    if (!depth || !depth->m_format.m_depth) {
        return;
    }

    // The colour target stays what it was; the texture becomes the depth target
    this->ISetHwTargets(this->m_metal->ColorTarget(), GxMetal::Target { depth, 0, 0 });
}

void CGxDeviceMetalNative::DeviceSetBaseMipLevel(uint32_t level) {
    // The recreation is lazy (only m_needsCreation is set) and ITexCreate reads the level later
    this->ITexForceRecreation(1);
    CGxDevice::DeviceSetBaseMipLevel(level);
}

void CGxDeviceMetalNative::IShaderCreate(CGxShader* shader) {
}

void CGxDeviceMetalNative::IShaderReload(CGxShader* shader, const char* path, const char* name) {
}

void CGxDeviceMetalNative::IRsSendToHw(EGxRenderState which) {
}

namespace {

GxMetal::Buffer* PoolBuffer(CGxPool* pool) {
    return pool ? static_cast<GxMetal::Buffer*>(pool->m_apiSpecific) : nullptr;
}

}

GxMetal::Buffer* CGxDeviceMetalNative::ICreateBuffer(CGxPool* pool) {
    if (!pool || !this->m_metal || pool->m_size <= 0) {
        return nullptr;
    }

    // Stream and dynamic pools are written while the GPU may still read them, so their buffers follow
    // the lock's rules; static pools are only written when the serials say the GPU is done with them.
    // Index buffers keep a CPU copy of their contents because triangle fans are expanded on the CPU.
    bool dynamic = pool->m_usage == GxPoolUsage_Stream || pool->m_usage == GxPoolUsage_Dynamic;
    bool keepShadow = pool->m_target == GxPoolTarget_Index;
    GxMetal::Buffer* buffer = new GxMetal::Buffer(this->m_metal, static_cast<uint32_t>(pool->m_size), dynamic, keepShadow);

    if (!buffer->Valid()) {
        buffer->Release();
        return nullptr;
    }

    return buffer;
}

int32_t CGxDeviceMetalNative::CreatePoolAPI(CGxPool* pool) {
    if (!pool) {
        return 0;
    }

    if (pool->m_target == GxPoolTarget_Vertex || pool->m_target == GxPoolTarget_Index) {
        pool->m_apiSpecific = this->ICreateBuffer(pool);
    }

    return 1;
}

void CGxDeviceMetalNative::IReleasePool(CGxPool* pool) {
    if (!pool) {
        return;
    }

    if (pool->m_usage == GxPoolUsage_Stream) {
        pool->m_offset = 0;
    }

    pool->Invalidate();
    GxMetal::Clear(reinterpret_cast<GxMetal::Buffer*&>(pool->m_apiSpecific));
}

void CGxDeviceMetalNative::IReleasePools(int32_t freePools) {
    for (CGxPool* pool = this->m_poolList.Head(); pool; pool = this->m_poolList.Next(pool)) {
        if (freePools || pool->m_usage == GxPoolUsage_Stream || pool->m_usage == GxPoolUsage_Dynamic) {
            this->IReleasePool(pool);
        }
    }

    if (freePools) {
        GxMetal::Clear(this->m_indexBuf);
        this->m_streamPassId = 0;
    }
}

void CGxDeviceMetalNative::PoolSizeSet(CGxPool* pool, uint32_t size) {
    if (!pool || !this->m_metal) {
        return;
    }

    this->IReleasePool(pool);
    pool->m_size = static_cast<int32_t>(size);
    this->CreatePoolAPI(pool);
}

void CGxDeviceMetalNative::PoolDestroy(CGxPool* pool) {
    if (pool) {
        this->IReleasePool(pool);
        CGxDevice::PoolDestroy(pool);
    }
}

char* CGxDeviceMetalNative::IBufLock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;
    EmergencyMem& emergency = this->m_emergencyMem[pool->m_target];

    if (!this->m_metal) {
        return static_cast<char*>(emergency.Lock(buf->m_size));
    }

    GxMetal::BufferLock kind = GxMetal::BufferLock_Normal;

    if (pool->m_usage == GxPoolUsage_Stream) {
        // A stream pool appends the buffer in item-sized slots and wraps to the start when it is full
        uint32_t index = (buf->m_itemSize + pool->m_offset - 1) - (buf->m_itemSize + pool->m_offset - 1) % buf->m_itemSize;

        if (index + buf->m_size > static_cast<uint32_t>(pool->m_size)) {
            kind = GxMetal::BufferLock_Discard;
            pool->Discard();
            buf->m_index = 0;
            pool->m_offset = static_cast<int32_t>(buf->m_size);
        } else {
            kind = GxMetal::BufferLock_NoOverwrite;
            buf->m_index = index;
            pool->m_offset = static_cast<int32_t>(index + buf->m_size);
        }
    } else if (pool->m_usage == GxPoolUsage_Dynamic) {
        kind = GxMetal::BufferLock_NoOverwrite;
    }

    if (!pool->m_apiSpecific) {
        this->CreatePoolAPI(pool);
    }

    GxMetal::Buffer* buffer = PoolBuffer(pool);

    if (!buffer) {
        return static_cast<char*>(emergency.Lock(buf->m_size));
    }

    char* data = buffer->Lock(buf->m_index, buf->m_size, kind);

    if (!data) {
        // Buffer::Lock pushed no record for a failed lock, and Unlock() pops the last record: calling
        // it here would pop a live one
        return static_cast<char*>(emergency.Lock(buf->m_size));
    }

    return data;
}

void CGxDeviceMetalNative::IBufUnlock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;
    EmergencyMem& emergency = this->m_emergencyMem[pool->m_target];

    if (emergency.m_lock) {
        emergency.Unlock();
        buf->m_valid = 0;
        this->m_intF5C = 1;
        return;
    }

    GxMetal::Buffer* buffer = PoolBuffer(pool);

    if (buffer) {
        buffer->Unlock();
    }

    buf->m_valid = buffer != nullptr;
}

char* CGxDeviceMetalNative::BufLock(CGxBuf* buf) {
    CGxDevice::BufLock(buf);
    return this->IBufLock(buf);
}

int32_t CGxDeviceMetalNative::BufUnlock(CGxBuf* buf, uint32_t size) {
    CGxDevice::BufUnlock(buf, size);
    this->IBufUnlock(buf);
    return 1;
}

void CGxDeviceMetalNative::BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset) {
    CGxDevice::BufData(buf, data, size, offset);

    char* destination = this->IBufLock(buf);

    if (!destination) {
        return;
    }

    memcpy(destination + offset, data, size);
    this->IBufUnlock(buf);
}

void CGxDeviceMetalNative::ISetVertexBuffer(uint32_t stream, GxMetal::Buffer* buffer, uint32_t offset, uint32_t stride) {
    if (stream >= 16) {
        return;
    }

    if (!this->m_caps.m_streamOffset) {
        offset = 0;
    }

    if (this->m_streamBufs[stream] != buffer || this->m_streamOffsets[stream] != offset || this->m_streamStrides[stream] != stride) {
        GxMetal::Assign(this->m_streamBufs[stream], buffer);
        this->m_streamOffsets[stream] = offset;
        this->m_streamStrides[stream] = stride;
    }
}

// CGxDeviceMetal.cpp:3227 - copy a rectangle of the window's back buffer into a texture
int32_t CGxDeviceMetalNative::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    if (!this->m_metal || !this->m_backBuffer) {
        return 0;
    }

    if (dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) {
        this->ITexCreate(dest);
    }

    GxMetal::Texture* texture = this->ITexData(dest);

    if (dest->m_needsCreation || !texture) {
        return 0;
    }

    GxMetal::Rect from = { sourcePos.x, sourcePos.y, sourcePos.x + size.x, sourcePos.y + size.y };
    GxMetal::Rect to = { destPos.x, destPos.y, destPos.x + size.x, destPos.y + size.y };
    GxMetal::Target source = { this->m_backBuffer, 0, 0 };
    GxMetal::Target target = { texture, dest->m_target == GxTex_CubeMap ? plane : 0, level };
    return this->m_metal->Stretch(source, &from, target, &to, false) ? 1 : 0;
}

// CGxDeviceMetal.cpp:3260 - the same from the back buffer, scaling when the rectangles differ in size
int32_t CGxDeviceMetalNative::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    if (!this->m_metal || !this->m_backBuffer) {
        return 0;
    }

    if (dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) {
        this->ITexCreate(dest);
    }

    GxMetal::Texture* texture = this->ITexData(dest);

    if (dest->m_needsCreation || !texture) {
        return 0;
    }

    GxMetal::Rect from = { 0, 0, 0, 0 };
    GxMetal::Rect to = { 0, 0, 0, 0 };

    if (sourceRect) {
        from.left = sourceRect->minX;
        from.top = sourceRect->minY;
        from.right = sourceRect->maxX;
        from.bottom = sourceRect->maxY;
    }

    if (destRect) {
        to.left = destRect->minX;
        to.top = destRect->minY;
        to.right = destRect->maxX;
        to.bottom = destRect->maxY;
    }

    // The shim asks for D3DTEXF_NONE only when both rectangles are given and the sizes match; Metal
    // picks blit or a quad by itself, so this only selects the sampler of a scaling copy
    bool linear = !(destRect && sourceRect && (to.right - to.left) == (from.right - from.left) && (to.bottom - to.top) == (from.bottom - from.top));
    GxMetal::Target source = { this->m_backBuffer, 0, 0 };
    GxMetal::Target target = { texture, dest->m_target == GxTex_CubeMap ? face : 0, level };
    return this->m_metal->Stretch(source, sourceRect ? &from : nullptr, target, destRect ? &to : nullptr, linear) ? 1 : 0;
}

bool CGxDeviceMetalNative::QueryBegin(CGxQuery* query) {
    return false;
}

bool CGxDeviceMetalNative::QueryEnd(CGxQuery* query) {
    return false;
}

bool CGxDeviceMetalNative::QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) {
    data = 0;
    return true;
}

bool CGxDeviceMetalNative::QueryGetData(CGxQuery* query, uint32_t& data) {
    data = 0;
    return true;
}

void CGxDeviceMetalNative::StereoSetConvergence(float convergence) {
}

float CGxDeviceMetalNative::StereoGetConvergence() {
    return 0.0f;
}

void CGxDeviceMetalNative::StereoSetSeparation(float separation) {
}

float CGxDeviceMetalNative::StereoGetSeparation() {
    return 0.0f;
}

bool CGxDeviceMetalNative::StereoEnabled() {
    return false;
}

void CGxDeviceMetalNative::DeviceUnk81(uint32_t a, uint32_t b) {
}

void CGxDeviceMetalNative::DeviceUnk82() {
}

void CGxDeviceMetalNative::DeviceUnk83(uint32_t a, uint32_t b) {
}

CGxDevice* CreateCGxDeviceMetalNative() {
    return new CGxDeviceMetalNative();
}
