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
    this->m_caps.m_streamOffset = 0;
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

    this->ISetHwTargets(GxMetal::Target { this->m_backBuffer, 0, 0 }, GxMetal::Target { this->m_depthBuffer, 0, 0 });
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

void CGxDeviceMetalNative::IShaderCreate(CGxShader* shader) {
}

void CGxDeviceMetalNative::IShaderReload(CGxShader* shader, const char* path, const char* name) {
}

void CGxDeviceMetalNative::IRsSendToHw(EGxRenderState which) {
}

void CGxDeviceMetalNative::PoolSizeSet(CGxPool* pool, uint32_t size) {
}

int32_t CGxDeviceMetalNative::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    return 1;
}

int32_t CGxDeviceMetalNative::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    return 1;
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
