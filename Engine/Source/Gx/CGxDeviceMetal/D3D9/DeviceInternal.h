// Implementation classes of the Metal-backed Direct3D 9 device (Objective-C++ only).
//
// GPU work is ordered by serials: every command buffer the device commits has one, and a resource
// records the serial of the last command buffer that used it. Comparing that with the serial of
// the last completed command buffer tells whether the CPU may write the resource's memory
// directly or must stage the write and copy it on the GPU in order.

#ifndef ENGINE_SOURCE_GX_CGXDEVICEMETAL_D3D9_DEVICEINTERNAL_H
#define ENGINE_SOURCE_GX_CGXDEVICEMETAL_D3D9_DEVICEINTERNAL_H

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "Device.h"
#include "FixedFunction.h"
#include "Engine/Source/Gx/CGxDeviceMetal/Shader/ShaderBytecode.h"
#include "Engine/Source/Gx/CGxDeviceMetal/Shader/ShaderTranslator.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace D3D9Metal {

class DeviceImpl;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Formats

struct FormatInfo {
    MTLPixelFormat m_pixelFormat = MTLPixelFormatInvalid;
    MTLTextureSwizzleChannels m_swizzle = { MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha };
    bool m_swizzled = false;
    bool m_depth = false;
    bool m_compressed = false;
    bool m_renderable = false;
    // Bytes per pixel, or per 4x4 block for compressed formats
    uint32_t m_bytesPerUnit = 0;

    uint32_t RowPitch(uint32_t width) const { return this->m_compressed ? ((width + 3) / 4) * this->m_bytesPerUnit : width * this->m_bytesPerUnit; }
    uint32_t RowCount(uint32_t height) const { return this->m_compressed ? (height + 3) / 4 : height; }
};

bool LookupFormat(D3DFORMAT format, FormatInfo& info);

// Transient GPU memory: constants, per-draw state and staged uploads. Allocations stay valid
// until the command buffer of the serial they were made for completes.

struct UploadAllocation {
    id<MTLBuffer> m_buffer = nil;
    uint32_t m_offset = 0;
    uint8_t* m_data = nullptr;
};

class UploadHeap {
public:
    void Init(id<MTLDevice> device);
    UploadAllocation Allocate(uint32_t size, uint32_t alignment, uint64_t serial, uint64_t completed);

private:
    struct Chunk {
        id<MTLBuffer> m_buffer;
        uint32_t m_size;
        uint32_t m_used;
        uint64_t m_lastSerial;
    };

    id<MTLDevice> m_device = nil;
    std::vector<Chunk> m_chunks;
    size_t m_current = SIZE_MAX;
};

// Resources

struct GpuUse {
    // Last command buffer that read or wrote the resource on the GPU
    uint64_t m_lastUse = 0;
    // Last render command buffer that read it in a draw
    uint64_t m_lastRenderUse = 0;
};

class BufferCore {
public:
    BufferCore(DeviceImpl* device, uint32_t length, bool dynamic, bool keepShadow);
    ~BufferCore();

    bool Valid() const { return this->m_buffer != nil; }
    HRESULT Lock(uint32_t offset, uint32_t size, void** data, uint32_t flags);
    HRESULT Unlock();

    DeviceImpl* m_device;
    uint32_t m_length;
    bool m_dynamic;
    id<MTLBuffer> m_buffer = nil;
    GpuUse m_use;
    // Copy of the contents for CPU-side primitive conversion (triangle fans)
    std::vector<uint8_t> m_shadow;

private:
    struct LockRecord {
        uint32_t m_offset;
        uint32_t m_size;
        uint8_t* m_data;
        bool m_staged;
        UploadAllocation m_staging;
    };

    // Buffers renamed away by D3DLOCK_DISCARD, reusable once their last use completes
    std::vector<std::pair<id<MTLBuffer>, uint64_t>> m_spares;
    std::vector<LockRecord> m_locks;
};

class VertexBufferImpl final : public VertexBuffer {
public:
    VertexBufferImpl(DeviceImpl* device, uint32_t length, bool dynamic) : m_core(device, length, dynamic, false) {}
    BufferCore m_core;
};

class IndexBufferImpl final : public IndexBuffer {
public:
    IndexBufferImpl(DeviceImpl* device, uint32_t length, bool dynamic) : m_core(device, length, dynamic, true) {}
    BufferCore m_core;
};

class SurfaceImpl;

class TextureCore {
public:
    TextureCore(DeviceImpl* device);
    ~TextureCore();

    bool Create(MTLTextureType type, uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, D3DFORMAT format, D3DPOOL pool);
    HRESULT GetSurface(BaseTexture* owner, uint32_t face, uint32_t level, Surface** surface);
    void SurfaceDestroyed(SurfaceImpl* surface);
    // The texture to sample: a swizzled view where the format needs one
    id<MTLTexture> SampleTexture();
    SamplerKind Kind() const;

    DeviceImpl* m_device;
    D3DFORMAT m_format = D3DFMT_UNKNOWN;
    FormatInfo m_info;
    MTLTextureType m_type = MTLTextureType2D;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_levels = 0;
    uint32_t m_usage = 0;
    D3DPOOL m_pool = D3DPOOL_MANAGED;
    id<MTLTexture> m_texture = nil;
    id<MTLTexture> m_view = nil;
    GpuUse m_use;
    bool m_mipsDirty = false;
    std::vector<SurfaceImpl*> m_surfaces;
};

class TextureImpl final : public Texture {
public:
    TextureImpl(DeviceImpl* device) : m_core(device) {}
    TextureCore m_core;
};

class CubeTextureImpl final : public CubeTexture {
public:
    CubeTextureImpl(DeviceImpl* device) : m_core(device) {}
    TextureCore m_core;
};

TextureCore* GetTextureCore(BaseTexture* texture);

class SurfaceImpl final : public Surface {
public:
    // A level of a texture
    SurfaceImpl(DeviceImpl* device, BaseTexture* owner, TextureCore* container, uint32_t face, uint32_t level);
    // A standalone render target or depth surface
    SurfaceImpl(DeviceImpl* device, D3DFORMAT format, uint32_t width, uint32_t height, uint32_t samples, bool lockable);
    ~SurfaceImpl() override;

    bool Valid() const { return this->m_texture != nil; }
    // The single-sample texture holding the contents, resolving multisampled contents first
    id<MTLTexture> ResolvedTexture();
    GpuUse& Use() { return this->m_container ? this->m_container->m_use : this->m_ownUse; }
    void MarkRendered();

    DeviceImpl* m_device;
    BaseTexture* m_owner = nullptr;
    TextureCore* m_container = nullptr;
    uint32_t m_face = 0;
    uint32_t m_level = 0;
    D3DFORMAT m_format;
    FormatInfo m_info;
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_samples = 1;
    // Render attachment: the texture itself, or the multisampled texture of a standalone surface
    id<MTLTexture> m_texture = nil;
    // Single-sample resolve target of a multisampled surface
    id<MTLTexture> m_resolve = nil;
    bool m_resolveDirty = false;
    GpuUse m_ownUse;

    // LockRect state
    bool m_locked = false;
    bool m_lockReadOnly = false;
    Rect m_lockRect = {};
    UploadAllocation m_lockStaging;
    id<MTLBuffer> m_readback = nil;
    // CPU copy of the last data written to a small texture level (cursor bitmaps)
    std::vector<uint8_t> m_mirror;
    uint32_t m_mirrorPitch = 0;
};

class VertexDeclarationImpl final : public VertexDeclaration {
public:
    struct Element {
        uint32_t m_attribute;
        uint32_t m_stream;
        uint32_t m_offset;
        MTLVertexFormat m_format;
        uint8_t m_class;
        uint8_t m_components;
    };

    std::vector<D3DVERTEXELEMENT9> m_elements;
    std::vector<Element> m_mapped;
    VertexInputSignature m_signature;
    uint8_t m_texcoordComponents[8] = {};
    uint32_t m_streamMask = 0;
    uint64_t m_id = 0;
};

struct CompiledFunction {
    id<MTLFunction> m_function = nil;
    uint64_t m_id = 0;
};

class VertexShaderImpl final : public VertexShader {
public:
    ShaderProgram m_program;
    ShaderInfo m_info;
    std::vector<std::pair<VertexInputSignature, CompiledFunction>> m_functions;
};

class PixelShaderImpl final : public PixelShader {
public:
    ShaderProgram m_program;
    ShaderInfo m_info;
    std::vector<std::pair<PixelShaderVariant, CompiledFunction>> m_functions;
};

class QueryImpl final : public Query {
public:
    QueryImpl(DeviceImpl* device, D3DQUERYTYPE type) : m_device(device), m_type(type) {}
    ~QueryImpl() override;

    enum State {
        STATE_IDLE,
        STATE_BUILDING,
        STATE_ISSUED,
    };

    struct Slot {
        id<MTLBuffer> m_buffer;
        uint32_t m_offset;
    };

    DeviceImpl* m_device;
    D3DQUERYTYPE m_type;
    State m_state = STATE_IDLE;
    uint64_t m_serial = 0;
    std::vector<Slot> m_slots;
};

// Cache keys

struct PipelineKey {
    uint64_t m_vertexFunction;
    uint64_t m_fragmentFunction;
    uint64_t m_declaration;
    uint16_t m_strides[8];
    uint32_t m_colorFormat;
    uint32_t m_depthFormat;
    uint8_t m_samples;
    uint8_t m_blendEnable;
    uint8_t m_srcBlend;
    uint8_t m_dstBlend;
    uint8_t m_blendOp;
    uint8_t m_srcBlendAlpha;
    uint8_t m_dstBlendAlpha;
    uint8_t m_blendOpAlpha;
    uint8_t m_writeMask;
    uint8_t m_padding[7];

    PipelineKey() { memset(this, 0, sizeof(*this)); }
    bool operator==(const PipelineKey& other) const { return memcmp(this, &other, sizeof(*this)) == 0; }
};

struct SamplerKey {
    uint8_t m_addressU;
    uint8_t m_addressV;
    uint8_t m_addressW;
    uint8_t m_magFilter;
    uint8_t m_minFilter;
    uint8_t m_mipFilter;
    uint8_t m_maxAnisotropy;
    uint8_t m_compare;
    uint32_t m_borderColor;
    float m_lodBias;
    uint32_t m_maxMipLevel;

    SamplerKey() { memset(this, 0, sizeof(*this)); }
    bool operator==(const SamplerKey& other) const { return memcmp(this, &other, sizeof(*this)) == 0; }
};

template <typename T>
struct BytewiseHash {
    size_t operator()(const T& value) const {
        // FNV-1a over the key's bytes; keys are zero-initialized including padding
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < sizeof(T); i++) {
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        }
        return static_cast<size_t>(hash);
    }
};

// The device

class DeviceImpl final : public Device {
public:
    DeviceImpl();
    ~DeviceImpl() override;

    bool Init(id<MTLDevice> device, const PresentParameters& parameters);

    // Serials and command submission
    uint64_t CurrentSerial() const { return this->m_serial; }
    uint64_t CompletedSerial() const { return this->m_completed.load(std::memory_order_acquire); }
    id<MTLCommandBuffer> Commands();
    // Commits the work recorded so far; waits for it when wait is set
    void Flush(bool wait);
    void WaitForSerial(uint64_t serial);

    // Staged copies. A destination already read by the current command buffer is updated in
    // order within it; otherwise the copy runs ahead of it, in the upload command buffer.
    UploadAllocation AllocateUpload(uint32_t size, uint32_t alignment);
    void CopyBuffer(const UploadAllocation& source, id<MTLBuffer> dest, uint32_t destOffset, uint32_t size, GpuUse& use);
    void CopyToTexture(const UploadAllocation& source, uint32_t bytesPerRow, uint32_t bytesPerImage, id<MTLTexture> dest, uint32_t slice, uint32_t level, MTLOrigin origin, MTLSize size, GpuUse& use);
    id<MTLBlitCommandEncoder> InlineBlit();
    void ResolveSurface(SurfaceImpl* surface);
    void ReadSurface(SurfaceImpl* surface, id<MTLBuffer> dest, uint32_t bytesPerRow);

    // Queries
    void BeginOcclusionQuery(QueryImpl* query);
    void EndOcclusionQuery(QueryImpl* query);
    void QueryDestroyed(QueryImpl* query);

    id<MTLDevice> m_device = nil;
    id<MTLCommandQueue> m_queue = nil;
    PresentParameters m_parameters = {};
    D3DCAPS9 m_caps = {};

    // Resource creation helpers used by the resource classes
    uint64_t NextId() { return ++this->m_nextId; }
    bool CompileFunction(const std::string& source, bool vertex, CompiledFunction& function);

    // Public interface, implemented in Device.mm
    HRESULT TestCooperativeLevelImpl();
    HRESULT ResetImpl(const PresentParameters& parameters);
    HRESULT PresentImpl();
    HRESULT GetBackBufferImpl(Surface** surface);
    void SetGammaRampImpl(const D3DGAMMARAMP* ramp);
    HRESULT SetCursorPropertiesImpl(uint32_t hotspotX, uint32_t hotspotY, Surface* bitmap);
    int32_t ShowCursorImpl(int32_t show);
    HRESULT CreateVertexDeclarationImpl(const D3DVERTEXELEMENT9* elements, VertexDeclaration** declaration);
    HRESULT CreateShaderImpl(const uint32_t* function, uint32_t sizeInBytes, bool vertex, Unknown** shader);
    HRESULT SetRenderTargetImpl(uint32_t index, Surface* surface);
    HRESULT GetRenderTargetImpl(uint32_t index, Surface** surface);
    HRESULT SetDepthStencilSurfaceImpl(Unknown* surface);
    HRESULT GetDepthStencilSurfaceImpl(Surface** surface);
    HRESULT StretchRectImpl(Surface* source, const Rect* sourceRect, Surface* dest, const Rect* destRect, D3DTEXTUREFILTERTYPE filter);
    HRESULT ClearImpl(uint32_t count, const D3DRECT* rects, uint32_t flags, uint32_t color, float z);
    bool AcquireExternalTargetsImpl(void** commandBuffer, void** color, void** depth, uint32_t* width, uint32_t* height, uint32_t* sampleCount);
    HRESULT DrawImpl(D3DPRIMITIVETYPE type, bool indexed, int32_t baseVertex, uint32_t start, uint32_t primitiveCount);
    HRESULT SetRenderStateImpl(D3DRENDERSTATETYPE state, uint32_t value);
    HRESULT SetSamplerStateImpl(uint32_t sampler, D3DSAMPLERSTATETYPE type, uint32_t value);
    HRESULT SetTextureStageStateImpl(uint32_t stage, D3DTEXTURESTAGESTATETYPE type, uint32_t value);
    HRESULT SetTextureImpl(uint32_t sampler, BaseTexture* texture);
    HRESULT SetTransformImpl(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
    HRESULT SetMaterialImpl(const D3DMATERIAL9* material);
    HRESULT SetLightImpl(uint32_t index, const D3DLIGHT9* light);
    HRESULT LightEnableImpl(uint32_t index, int32_t enable);
    HRESULT SetClipPlaneImpl(uint32_t index, const float* plane);
    HRESULT SetScissorRectImpl(const Rect* rect);
    HRESULT SetViewportImpl(const D3DVIEWPORT9* viewport);
    HRESULT SetStreamSourceImpl(uint32_t stream, VertexBuffer* buffer, uint32_t offset, uint32_t stride);
    HRESULT SetIndicesImpl(IndexBuffer* buffer);
    HRESULT SetVertexDeclarationImpl(VertexDeclaration* declaration);
    HRESULT SetVertexShaderImpl(VertexShader* shader);
    HRESULT SetPixelShaderImpl(PixelShader* shader);
    HRESULT SetShaderConstantsImpl(bool vertex, uint32_t start, const float* data, uint32_t count);

    struct ImplState;
    std::unique_ptr<ImplState> m_state;

private:

    uint64_t m_serial = 1;
    // A Metal device is never lost; a failed Reset leaves it needing another, as a lost one does
    bool m_needsReset = false;
    std::atomic<uint64_t> m_completed { 0 };
    uint64_t m_nextId = 0;

    friend struct DeviceState;
    void ResetState();
    bool CreateSwapChainResources();
    void EndEncoders();
    bool BeginPass();
    void ExecutePendingClears();
    bool PrepareDraw(D3DPRIMITIVETYPE type);
    void BindTextures(const ShaderInfo* pixelInfo, uint32_t samplerMask);
    id<MTLSamplerState> SamplerFor(uint32_t sampler, bool compare);
    id<MTLRenderPipelineState> PipelineFor(const CompiledFunction& vertex, const CompiledFunction& fragment, VertexDeclarationImpl* declaration);
    id<MTLDepthStencilState> DepthStencilFor(bool enable, bool write, uint32_t func);
    void ClearWithQuad(const Rect& rect, bool color, bool depth, const float* rgba, float z);
    void UpdateFixedVertexState();
    void SubmitFrame();
};

}

#endif
