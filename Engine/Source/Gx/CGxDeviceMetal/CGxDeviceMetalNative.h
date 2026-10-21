// The native Metal graphics device (Objective-C++ only): CGxDevice's states go straight to Metal,
// with no Direct3D 9 layer in between.
//
// The device is written in slices so each one can be compared with the shim device:
//   1. life, window, targets, clear and present            (this file today)
//   2. textures
//   3. buffers and pools
//   4. state mapping (IRsSendToHw, IStateSync*)
//   5. shaders and draws
// The method checklist is in WOWRE.md. CreateCGxDeviceMetal returns this device when the environment
// variable WRATHMODERN_GX is "native"; the shim device stays the default until it draws the same.

#ifndef ENGINE_SOURCE_GX_CGXDEVICEMETAL_CGXDEVICEMETALNATIVE_H
#define ENGINE_SOURCE_GX_CGXDEVICEMETAL_CGXDEVICEMETALNATIVE_H

#import <Metal/Metal.h>

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDeviceMetal/MetalContext.h"
#include "Engine/Source/Gx/CGxDeviceMetal/MetalPipeline.h"
#include "Engine/Source/Gx/CGxDeviceMetal/MetalResources.h"

class CGxDeviceMetalNative : public CGxDevice {
    public:
    void* m_hwnd;
    int32_t m_ownhwnd;
    GxMetal::Context* m_metal;
    GxMetal::PipelineCache* m_pipelines;
    int32_t m_inScene;
    // The window's targets: back buffer and the depth buffer it renders with
    GxMetal::Texture* m_backBuffer;
    GxMetal::Texture* m_depthBuffer;
    uint32_t m_samples;

    virtual void IRsSendToHw(EGxRenderState which);
    virtual int32_t DeviceCreate(GxWindowProc windowProc, const CGxFormat& format);
    virtual int32_t DeviceCreate(void* window, const CGxFormat& format);
    virtual void DeviceDestroy();
    virtual void DeviceEvictResources();
    virtual int32_t DeviceSetFormat(const CGxFormat& format);
    virtual void* DeviceWindow();
    virtual void DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels);
    virtual void DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths);
    virtual int32_t DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2);
    virtual void CapsWindowSize(CRect& rect);
    virtual void CapsWindowSizeInScreenCoords(CRect& rect);
    virtual void ScenePresent();
    virtual void SceneClear(uint32_t mask, CImVector color);
    virtual void DeviceSetGamma(float gamma);
    virtual void DeviceSetGamma(const CGxGammaRamp& ramp);
    virtual void PoolSizeSet(CGxPool* pool, uint32_t size);
    virtual int32_t TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane);
    virtual int32_t TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face);
    virtual bool QueryBegin(CGxQuery* query);
    virtual bool QueryEnd(CGxQuery* query);
    virtual bool QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data);
    virtual bool QueryGetData(CGxQuery* query, uint32_t& data);
    virtual void IShaderReload(CGxShader* shader, const char* path, const char* name);
    virtual void IShaderCreate(CGxShader* shader);
    virtual void StereoSetConvergence(float convergence);
    virtual float StereoGetConvergence();
    virtual void StereoSetSeparation(float separation);
    virtual float StereoGetSeparation();
    virtual bool StereoEnabled();
    virtual void DeviceUnk81(uint32_t a, uint32_t b);
    virtual void DeviceUnk82();
    virtual void DeviceUnk83(uint32_t a, uint32_t b);

    CGxDeviceMetalNative();
    virtual ~CGxDeviceMetalNative();
    int32_t ICreateMetal();
    void IDestroyMetal();
    int32_t ICreateWindow(CGxFormat& format);
    bool ICreateWindowTargets(const CGxFormat& format);
    void ISetCaps(const CGxFormat& format);
    void IResetDevice();
    void ISceneBegin();
    void ISceneEnd();
    void ISetHwTargets(const GxMetal::Target& color, const GxMetal::Target& depth);
    void IPollWindow();
    // The window's client size in pixels
    void IClientSize(int32_t* width, int32_t* height);
};

CGxDevice* CreateCGxDeviceMetalNative();

#endif
