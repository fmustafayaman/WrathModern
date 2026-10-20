// A Direct3D 9 device implemented on Metal.
//
// The classes mirror the Direct3D 9 interfaces the client's renderer uses (IDirect3D9,
// IDirect3DDevice9, buffers, textures, surfaces, shaders, declarations and queries) with the same
// methods, arguments and semantics, so CGxDeviceMetal can follow CGxDeviceD3d line by line. They
// are plain reference-counted C++ classes, not COM objects, and this header has no Objective-C so
// engine code includes it as C++.

#ifndef ENGINE_SOURCE_GX_CGXDEVICEMETAL_D3D9_DEVICE_H
#define ENGINE_SOURCE_GX_CGXDEVICEMETAL_D3D9_DEVICE_H

#include "D3D9Types.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace D3D9Metal {

struct Rect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

class Device;
class DeviceImpl;

class Unknown {
public:
    uint32_t AddRef();
    uint32_t Release();

protected:
    Unknown() = default;
    virtual ~Unknown() = default;

private:
    std::atomic<uint32_t> m_refCount { 1 };

    Unknown(const Unknown&) = delete;
    Unknown& operator=(const Unknown&) = delete;
};

class Surface;

class VertexBuffer : public Unknown {
public:
    HRESULT Lock(uint32_t offset, uint32_t size, void** data, uint32_t flags);
    HRESULT Unlock();

protected:
    VertexBuffer() = default;
};

class IndexBuffer : public Unknown {
public:
    HRESULT Lock(uint32_t offset, uint32_t size, void** data, uint32_t flags);
    HRESULT Unlock();

protected:
    IndexBuffer() = default;
};

class BaseTexture : public Unknown {
public:
    void PreLoad();
    uint32_t GetLevelCount();

protected:
    BaseTexture() = default;
};

class Texture : public BaseTexture {
public:
    HRESULT GetSurfaceLevel(uint32_t level, Surface** surface);
    HRESULT GetLevelDesc(uint32_t level, D3DSURFACE_DESC* desc);

protected:
    Texture() = default;
};

class CubeTexture : public BaseTexture {
public:
    HRESULT GetCubeMapSurface(D3DCUBEMAP_FACES face, uint32_t level, Surface** surface);

protected:
    CubeTexture() = default;
};

class Surface : public Unknown {
public:
    HRESULT LockRect(D3DLOCKED_RECT* lockedRect, const Rect* rect, uint32_t flags);
    HRESULT UnlockRect();
    HRESULT GetDesc(D3DSURFACE_DESC* desc);

protected:
    Surface() = default;
};

class VertexShader : public Unknown {
protected:
    VertexShader() = default;
};

class PixelShader : public Unknown {
protected:
    PixelShader() = default;
};

class VertexDeclaration : public Unknown {
protected:
    VertexDeclaration() = default;
};

class Query : public Unknown {
public:
    HRESULT Issue(uint32_t flags);
    HRESULT GetData(void* data, uint32_t size, uint32_t flags);
    uint32_t GetDataSize();

protected:
    Query() = default;
};

struct PresentParameters {
    uint32_t BackBufferWidth;
    uint32_t BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    uint32_t BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    uint32_t MultiSampleQuality;
    // CAMetalLayer the device presents to
    void* Layer;
    int32_t Windowed;
    int32_t EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    uint32_t FullScreen_RefreshRateInHz;
    // 0 (immediate) or the number of vertical blanks to wait
    uint32_t PresentationInterval;
};

struct DisplayMode {
    uint32_t Width;
    uint32_t Height;
    uint32_t RefreshRate;
    D3DFORMAT Format;
};

struct AdapterIdentifier {
    char Driver[512];
    char Description[512];
    char DeviceName[32];
    uint32_t DriverVersionHigh;
    uint32_t DriverVersionLow;
    uint32_t VendorId;
    uint32_t DeviceId;
};

// The adapter: capabilities, formats and display modes of the system's Metal device
class Direct3D : public Unknown {
public:
    static Direct3D* Create();

    uint32_t GetAdapterCount();
    HRESULT GetAdapterIdentifier(uint32_t adapter, uint32_t flags, AdapterIdentifier* identifier);
    HRESULT GetAdapterDisplayMode(uint32_t adapter, DisplayMode* mode);
    HRESULT GetDeviceCaps(uint32_t adapter, D3DCAPS9* caps);
    HRESULT CheckDeviceFormat(uint32_t adapter, D3DFORMAT adapterFormat, uint32_t usage, D3DRESOURCETYPE type, D3DFORMAT format);
    HRESULT CheckDepthStencilMatch(uint32_t adapter, D3DFORMAT adapterFormat, D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat);
    HRESULT CheckDeviceMultiSampleType(uint32_t adapter, D3DFORMAT format, int32_t windowed, D3DMULTISAMPLE_TYPE type, uint32_t* qualityLevels);
    HRESULT CreateDevice(const PresentParameters& parameters, Device** device);

private:
    Direct3D() = default;
};

class Device : public Unknown {
public:
    // Capabilities and state of the swap chain
    HRESULT GetDeviceCaps(D3DCAPS9* caps);
    HRESULT TestCooperativeLevel();
    HRESULT Reset(const PresentParameters& parameters);
    HRESULT Present();
    HRESULT GetBackBuffer(uint32_t swapChain, uint32_t index, D3DBACKBUFFER_TYPE type, Surface** surface);
    void SetGammaRamp(const D3DGAMMARAMP* ramp);
    HRESULT EvictManagedResources();

    // Cursor
    HRESULT SetCursorProperties(uint32_t hotspotX, uint32_t hotspotY, Surface* bitmap);
    int32_t ShowCursor(int32_t show);
    // The NSCursor made by SetCursorProperties, unretained, or null; and whether it is shown
    void* GetCursorImage();
    bool IsCursorShown();
    // Size of the cursor image in points per bitmap pixel (1 shows a 32 x 32 bitmap at 32 points)
    void SetCursorScale(float scale);

    // Resources
    HRESULT CreateVertexBuffer(uint32_t length, uint32_t usage, uint32_t fvf, D3DPOOL pool, VertexBuffer** buffer);
    HRESULT CreateIndexBuffer(uint32_t length, uint32_t usage, D3DFORMAT format, D3DPOOL pool, IndexBuffer** buffer);
    HRESULT CreateTexture(uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, D3DFORMAT format, D3DPOOL pool, Texture** texture);
    HRESULT CreateCubeTexture(uint32_t edgeLength, uint32_t levels, uint32_t usage, D3DFORMAT format, D3DPOOL pool, CubeTexture** texture);
    HRESULT CreateRenderTarget(uint32_t width, uint32_t height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, uint32_t quality, int32_t lockable, Surface** surface);
    HRESULT CreateDepthStencilSurface(uint32_t width, uint32_t height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, uint32_t quality, int32_t discard, Surface** surface);
    HRESULT CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, VertexDeclaration** declaration);
    // The size of the token stream may be given; without it the stream ends at its end token
    HRESULT CreateVertexShader(const uint32_t* function, VertexShader** shader, uint32_t sizeInBytes = 0);
    HRESULT CreatePixelShader(const uint32_t* function, PixelShader** shader, uint32_t sizeInBytes = 0);
    HRESULT CreateQuery(D3DQUERYTYPE type, Query** query);

    // Render targets
    HRESULT SetRenderTarget(uint32_t index, Surface* surface);
    HRESULT GetRenderTarget(uint32_t index, Surface** surface);
    // Accepts a depth texture as well as a surface, standing for its top level
    HRESULT SetDepthStencilSurface(Unknown* surface);
    HRESULT GetDepthStencilSurface(Surface** surface);
    HRESULT StretchRect(Surface* source, const Rect* sourceRect, Surface* dest, const Rect* destRect, D3DTEXTUREFILTERTYPE filter);

    // Scenes and drawing
    HRESULT BeginScene();
    HRESULT EndScene();
    HRESULT Clear(uint32_t count, const D3DRECT* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, uint32_t startVertex, uint32_t primitiveCount);
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, int32_t baseVertexIndex, uint32_t minVertexIndex, uint32_t numVertices, uint32_t startIndex, uint32_t primitiveCount);

    // State
    HRESULT SetRenderState(D3DRENDERSTATETYPE state, uint32_t value);
    HRESULT SetSamplerState(uint32_t sampler, D3DSAMPLERSTATETYPE type, uint32_t value);
    HRESULT SetTextureStageState(uint32_t stage, D3DTEXTURESTAGESTATETYPE type, uint32_t value);
    HRESULT SetTexture(uint32_t sampler, BaseTexture* texture);
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
    HRESULT SetMaterial(const D3DMATERIAL9* material);
    HRESULT SetLight(uint32_t index, const D3DLIGHT9* light);
    HRESULT LightEnable(uint32_t index, int32_t enable);
    HRESULT SetClipPlane(uint32_t index, const float* plane);
    HRESULT SetScissorRect(const Rect* rect);
    HRESULT SetViewport(const D3DVIEWPORT9* viewport);
    HRESULT SetStreamSource(uint32_t stream, VertexBuffer* buffer, uint32_t offset, uint32_t stride);
    HRESULT SetIndices(IndexBuffer* buffer);
    HRESULT SetVertexDeclaration(VertexDeclaration* declaration);
    HRESULT SetVertexShader(VertexShader* shader);
    HRESULT SetPixelShader(PixelShader* shader);
    HRESULT SetVertexShaderConstantF(uint32_t start, const float* data, uint32_t count);
    HRESULT SetPixelShaderConstantF(uint32_t start, const float* data, uint32_t count);

    // Metal access for code that draws with Metal itself. GetMetalDevice returns the id<MTLDevice>.
    // AcquireExternalTargets runs pending clears, ends the open encoders and returns the frame's
    // command buffer and the current colour and depth textures, unretained; drawing resumes into the
    // same targets afterwards. It fails while no colour target is set.
    void* GetMetalDevice();
    bool AcquireExternalTargets(void** commandBuffer, void** color, void** depth, uint32_t* width, uint32_t* height, uint32_t* sampleCount);

protected:
    Device() = default;
};

}

#endif
