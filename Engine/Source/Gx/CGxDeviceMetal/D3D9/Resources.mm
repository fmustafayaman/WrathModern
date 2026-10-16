#include "DeviceInternal.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <os/log.h>

namespace D3D9Metal {

void Log(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    os_log(OS_LOG_DEFAULT, "D3D9Metal: %{public}s", buffer);
    fprintf(stderr, "D3D9Metal: %s\n", buffer);
}

// Formats

bool LookupFormat(D3DFORMAT format, FormatInfo& info) {
    info = FormatInfo();

    auto swizzle = [&](MTLTextureSwizzle r, MTLTextureSwizzle g, MTLTextureSwizzle b, MTLTextureSwizzle a) {
        info.m_swizzle = MTLTextureSwizzleChannelsMake(r, g, b, a);
        info.m_swizzled = true;
    };

    switch (format) {
        case D3DFMT_A8R8G8B8:
            info.m_pixelFormat = MTLPixelFormatBGRA8Unorm;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            return true;

        case D3DFMT_X8R8G8B8:
            info.m_pixelFormat = MTLPixelFormatBGRA8Unorm;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            swizzle(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
            return true;

        case D3DFMT_A8B8G8R8:
            info.m_pixelFormat = MTLPixelFormatRGBA8Unorm;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            return true;

        case D3DFMT_X8B8G8R8:
            info.m_pixelFormat = MTLPixelFormatRGBA8Unorm;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            swizzle(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
            return true;

        case D3DFMT_R5G6B5:
            // Same bit layout: red in the high bits
            info.m_pixelFormat = MTLPixelFormatB5G6R5Unorm;
            info.m_bytesPerUnit = 2;
            info.m_renderable = true;
            return true;

        case D3DFMT_A1R5G5B5:
            info.m_pixelFormat = MTLPixelFormatBGR5A1Unorm;
            info.m_bytesPerUnit = 2;
            info.m_renderable = true;
            return true;

        case D3DFMT_X1R5G5B5:
            info.m_pixelFormat = MTLPixelFormatBGR5A1Unorm;
            info.m_bytesPerUnit = 2;
            info.m_renderable = true;
            swizzle(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
            return true;

        case D3DFMT_A4R4G4B4:
            // ABGR4Unorm stores the nibbles red-high: its r, g, b, a channels read Direct3D's
            // alpha, red, green and blue. It cannot be rendered to with this mapping.
            info.m_pixelFormat = MTLPixelFormatABGR4Unorm;
            info.m_bytesPerUnit = 2;
            swizzle(MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha, MTLTextureSwizzleRed);
            return true;

        case D3DFMT_A8:
            info.m_pixelFormat = MTLPixelFormatA8Unorm;
            info.m_bytesPerUnit = 1;
            return true;

        case D3DFMT_A2B10G10R10:
            info.m_pixelFormat = MTLPixelFormatRGB10A2Unorm;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            return true;

        case D3DFMT_A2R10G10B10:
            info.m_pixelFormat = MTLPixelFormatBGR10A2Unorm;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            return true;

        case D3DFMT_G16R16:
            info.m_pixelFormat = MTLPixelFormatRG16Unorm;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            swizzle(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne);
            return true;

        case D3DFMT_V8U8:
            info.m_pixelFormat = MTLPixelFormatRG8Snorm;
            info.m_bytesPerUnit = 2;
            swizzle(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne);
            return true;

        case D3DFMT_G16R16F:
            info.m_pixelFormat = MTLPixelFormatRG16Float;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            swizzle(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne);
            return true;

        case D3DFMT_R32F:
            info.m_pixelFormat = MTLPixelFormatR32Float;
            info.m_bytesPerUnit = 4;
            info.m_renderable = true;
            swizzle(MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne, MTLTextureSwizzleOne);
            return true;

        case D3DFMT_DXT1:
            info.m_pixelFormat = MTLPixelFormatBC1_RGBA;
            info.m_bytesPerUnit = 8;
            info.m_compressed = true;
            return true;

        case D3DFMT_DXT3:
            info.m_pixelFormat = MTLPixelFormatBC2_RGBA;
            info.m_bytesPerUnit = 16;
            info.m_compressed = true;
            return true;

        case D3DFMT_DXT5:
            info.m_pixelFormat = MTLPixelFormatBC3_RGBA;
            info.m_bytesPerUnit = 16;
            info.m_compressed = true;
            return true;

        case D3DFMT_D16:
        case D3DFMT_D16_LOCKABLE:
        case D3DFMT_D24X8:
        case D3DFMT_D24S8:
        case D3DFMT_D32:
            // Apple GPUs have no 24-bit depth formats; 32-bit float depth is at least as precise
            info.m_pixelFormat = MTLPixelFormatDepth32Float;
            info.m_bytesPerUnit = 4;
            info.m_depth = true;
            info.m_renderable = true;
            return true;

        default:
            return false;
    }
}

// Upload heap

void UploadHeap::Init(id<MTLDevice> device) {
    this->m_device = device;
}

UploadAllocation UploadHeap::Allocate(uint32_t size, uint32_t alignment, uint64_t serial, uint64_t completed) {
    static const uint32_t kChunkSize = 8 * 1024 * 1024;

    auto tryChunk = [&](size_t index, UploadAllocation& allocation) {
        Chunk& chunk = this->m_chunks[index];
        uint32_t offset = (chunk.m_used + alignment - 1) & ~(alignment - 1);
        if (offset + size > chunk.m_size) {
            return false;
        }
        chunk.m_used = offset + size;
        chunk.m_lastSerial = serial;
        allocation.m_buffer = chunk.m_buffer;
        allocation.m_offset = offset;
        allocation.m_data = static_cast<uint8_t*>(chunk.m_buffer.contents) + offset;
        return true;
    };

    UploadAllocation allocation;

    if (this->m_current != SIZE_MAX && tryChunk(this->m_current, allocation)) {
        return allocation;
    }

    // A chunk whose allocations the GPU no longer needs starts over
    for (size_t i = 0; i < this->m_chunks.size(); i++) {
        Chunk& chunk = this->m_chunks[i];
        if (i != this->m_current && chunk.m_lastSerial <= completed && chunk.m_size >= size) {
            chunk.m_used = 0;
            if (tryChunk(i, allocation)) {
                this->m_current = i;
                return allocation;
            }
        }
    }

    Chunk chunk;
    chunk.m_size = std::max(kChunkSize, (size + 0xFFFF) & ~0xFFFFu);
    chunk.m_buffer = [this->m_device newBufferWithLength:chunk.m_size options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    chunk.m_used = 0;
    chunk.m_lastSerial = serial;
    this->m_chunks.push_back(chunk);
    this->m_current = this->m_chunks.size() - 1;
    tryChunk(this->m_current, allocation);
    return allocation;
}

// Reference counting

uint32_t Unknown::AddRef() {
    return ++this->m_refCount;
}

uint32_t Unknown::Release() {
    uint32_t count = --this->m_refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

// Buffers

BufferCore::BufferCore(DeviceImpl* device, uint32_t length, bool dynamic, bool keepShadow) : m_device(device), m_length(length), m_dynamic(dynamic) {
    if (length) {
        this->m_buffer = [device->m_device newBufferWithLength:length options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    }
    if (keepShadow) {
        this->m_shadow.resize(length);
    }
}

BufferCore::~BufferCore() = default;

HRESULT BufferCore::Lock(uint32_t offset, uint32_t size, void** data, uint32_t flags) {
    if (!this->m_buffer || offset > this->m_length || !data) {
        return D3DERR_INVALIDCALL;
    }

    if (size == 0 || offset + size > this->m_length) {
        size = this->m_length - offset;
    }

    uint64_t completed = this->m_device->CompletedSerial();
    LockRecord record = { offset, size, nullptr, false, {} };

    if ((flags & D3DLOCK_DISCARD) && this->m_dynamic && this->m_use.m_lastUse > completed) {
        // Rename: the GPU keeps the old memory, the application gets memory the GPU is done with
        this->m_spares.emplace_back(this->m_buffer, this->m_use.m_lastUse);

        id<MTLBuffer> next = nil;
        for (size_t i = 0; i + 1 < this->m_spares.size(); i++) {
            if (this->m_spares[i].second <= completed) {
                next = this->m_spares[i].first;
                this->m_spares.erase(this->m_spares.begin() + i);
                break;
            }
        }

        if (!next) {
            next = [this->m_device->m_device newBufferWithLength:this->m_length options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            if (!next) {
                return D3DERR_OUTOFVIDEOMEMORY;
            }
        }

        this->m_buffer = next;
        this->m_use = GpuUse();
    }

    if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) || this->m_use.m_lastUse <= completed) {
        // The application guarantees, or the serials show, that the GPU does not read the range
        record.m_data = static_cast<uint8_t*>(this->m_buffer.contents) + offset;
    } else {
        // The GPU still reads the buffer: stage the write and copy it in order. The buffers
        // are write-only, so the staged range needs no previous contents.
        record.m_staged = true;
        record.m_staging = this->m_device->AllocateUpload(size, 16);
        record.m_data = record.m_staging.m_data;
    }

    this->m_locks.push_back(record);
    *data = record.m_data;
    return D3D_OK;
}

HRESULT BufferCore::Unlock() {
    if (this->m_locks.empty()) {
        return D3DERR_INVALIDCALL;
    }

    LockRecord record = this->m_locks.back();
    this->m_locks.pop_back();

    if (!this->m_shadow.empty() && record.m_size) {
        memcpy(this->m_shadow.data() + record.m_offset, record.m_data, record.m_size);
    }

    if (record.m_staged && record.m_size) {
        this->m_device->CopyBuffer(record.m_staging, this->m_buffer, record.m_offset, record.m_size, this->m_use);
    }

    return D3D_OK;
}

HRESULT VertexBuffer::Lock(uint32_t offset, uint32_t size, void** data, uint32_t flags) {
    return static_cast<VertexBufferImpl*>(this)->m_core.Lock(offset, size, data, flags);
}

HRESULT VertexBuffer::Unlock() {
    return static_cast<VertexBufferImpl*>(this)->m_core.Unlock();
}

HRESULT IndexBuffer::Lock(uint32_t offset, uint32_t size, void** data, uint32_t flags) {
    return static_cast<IndexBufferImpl*>(this)->m_core.Lock(offset, size, data, flags);
}

HRESULT IndexBuffer::Unlock() {
    return static_cast<IndexBufferImpl*>(this)->m_core.Unlock();
}

// Textures

TextureCore::TextureCore(DeviceImpl* device) : m_device(device) {
}

TextureCore::~TextureCore() {
    // Surfaces hold a reference to their texture, so none remain
}

bool TextureCore::Create(MTLTextureType type, uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, D3DFORMAT format, D3DPOOL pool) {
    if (!LookupFormat(format, this->m_info) || !width || !height) {
        return false;
    }

    if ((usage & D3DUSAGE_RENDERTARGET) && !this->m_info.m_renderable) {
        return false;
    }

    uint32_t fullChain = 1;
    for (uint32_t size = std::max(width, height); size > 1; size >>= 1) {
        fullChain++;
    }

    if (levels == 0 || levels > fullChain) {
        levels = fullChain;
    }

    // Depth textures and render targets have a single level unless mipmaps are generated
    if (this->m_info.m_depth) {
        levels = 1;
    }

    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = type;
    descriptor.pixelFormat = this->m_info.m_pixelFormat;
    descriptor.width = width;
    descriptor.height = type == MTLTextureTypeCube ? width : height;
    descriptor.mipmapLevelCount = levels;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;

    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || this->m_info.m_depth) {
        descriptor.usage |= MTLTextureUsageRenderTarget;
    }

    if (this->m_info.m_swizzled) {
        descriptor.usage |= MTLTextureUsagePixelFormatView;
    }

    this->m_texture = [this->m_device->m_device newTextureWithDescriptor:descriptor];
    if (!this->m_texture) {
        return false;
    }

    if (this->m_info.m_swizzled) {
        this->m_view = [this->m_texture newTextureViewWithPixelFormat:this->m_info.m_pixelFormat textureType:type levels:NSMakeRange(0, levels) slices:NSMakeRange(0, type == MTLTextureTypeCube ? 6 : 1) swizzle:this->m_info.m_swizzle];
    }

    this->m_format = format;
    this->m_type = type;
    this->m_width = width;
    this->m_height = type == MTLTextureTypeCube ? width : height;
    this->m_levels = levels;
    this->m_usage = usage;
    this->m_pool = pool;
    return true;
}

id<MTLTexture> TextureCore::SampleTexture() {
    return this->m_view ? this->m_view : this->m_texture;
}

SamplerKind TextureCore::Kind() const {
    if (this->m_info.m_depth) {
        return SAMPLERKIND_DEPTH;
    }
    return this->m_type == MTLTextureTypeCube ? SAMPLERKIND_CUBE : SAMPLERKIND_2D;
}

HRESULT TextureCore::GetSurface(BaseTexture* owner, uint32_t face, uint32_t level, Surface** surface) {
    if (!surface || level >= this->m_levels) {
        return D3DERR_INVALIDCALL;
    }

    for (SurfaceImpl* existing : this->m_surfaces) {
        if (existing->m_face == face && existing->m_level == level) {
            existing->AddRef();
            *surface = existing;
            return D3D_OK;
        }
    }

    SurfaceImpl* created = new SurfaceImpl(this->m_device, owner, this, face, level);
    this->m_surfaces.push_back(created);
    *surface = created;
    return D3D_OK;
}

void TextureCore::SurfaceDestroyed(SurfaceImpl* surface) {
    this->m_surfaces.erase(std::remove(this->m_surfaces.begin(), this->m_surfaces.end(), surface), this->m_surfaces.end());
}

TextureCore* GetTextureCore(BaseTexture* texture) {
    if (!texture) {
        return nullptr;
    }
    if (TextureImpl* texture2d = dynamic_cast<TextureImpl*>(texture)) {
        return &texture2d->m_core;
    }
    if (CubeTextureImpl* cube = dynamic_cast<CubeTextureImpl*>(texture)) {
        return &cube->m_core;
    }
    return nullptr;
}

void BaseTexture::PreLoad() {
    // Uploads are scheduled when the texture is written
}

uint32_t BaseTexture::GetLevelCount() {
    TextureCore* core = GetTextureCore(this);
    return core ? core->m_levels : 0;
}

HRESULT Texture::GetSurfaceLevel(uint32_t level, Surface** surface) {
    return static_cast<TextureImpl*>(this)->m_core.GetSurface(this, 0, level, surface);
}

HRESULT Texture::GetLevelDesc(uint32_t level, D3DSURFACE_DESC* desc) {
    Surface* surface = nullptr;
    HRESULT hr = this->GetSurfaceLevel(level, &surface);
    if (Succeeded(hr)) {
        hr = surface->GetDesc(desc);
        surface->Release();
    }
    return hr;
}

HRESULT CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, uint32_t level, Surface** surface) {
    if (face > D3DCUBEMAP_FACE_NEGATIVE_Z) {
        return D3DERR_INVALIDCALL;
    }
    return static_cast<CubeTextureImpl*>(this)->m_core.GetSurface(this, face, level, surface);
}

// Surfaces

SurfaceImpl::SurfaceImpl(DeviceImpl* device, BaseTexture* owner, TextureCore* container, uint32_t face, uint32_t level)
    : m_device(device), m_owner(owner), m_container(container), m_face(face), m_level(level), m_format(container->m_format), m_info(container->m_info) {
    owner->AddRef();
    this->m_width = std::max(1u, container->m_width >> level);
    this->m_height = std::max(1u, container->m_height >> level);
    this->m_texture = container->m_texture;
}

SurfaceImpl::SurfaceImpl(DeviceImpl* device, D3DFORMAT format, uint32_t width, uint32_t height, uint32_t samples, bool lockable)
    : m_device(device), m_format(format), m_width(width), m_height(height), m_samples(samples) {
    if (!LookupFormat(format, this->m_info) || !this->m_info.m_renderable || !width || !height) {
        return;
    }

    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:this->m_info.m_pixelFormat width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

    if (samples > 1) {
        descriptor.textureType = MTLTextureType2DMultisample;
        descriptor.sampleCount = samples;
        descriptor.usage = MTLTextureUsageRenderTarget;
        this->m_texture = [device->m_device newTextureWithDescriptor:descriptor];

        if (!this->m_info.m_depth) {
            MTLTextureDescriptor* resolve = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:this->m_info.m_pixelFormat width:width height:height mipmapped:NO];
            resolve.storageMode = MTLStorageModePrivate;
            resolve.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            this->m_resolve = [device->m_device newTextureWithDescriptor:resolve];
        }
    } else {
        this->m_texture = [device->m_device newTextureWithDescriptor:descriptor];
    }

    (void)lockable;
}

SurfaceImpl::~SurfaceImpl() {
    if (this->m_container) {
        this->m_container->SurfaceDestroyed(this);
        this->m_owner->Release();
    }
}

void SurfaceImpl::MarkRendered() {
    if (this->m_samples > 1) {
        this->m_resolveDirty = true;
    }

    if (this->m_container && (this->m_container->m_usage & D3DUSAGE_AUTOGENMIPMAP) && this->m_level == 0) {
        this->m_container->m_mipsDirty = true;
    }
}

id<MTLTexture> SurfaceImpl::ResolvedTexture() {
    if (this->m_samples > 1) {
        if (this->m_resolveDirty) {
            this->m_device->ResolveSurface(this);
        }
        return this->m_resolve;
    }
    return this->m_texture;
}

HRESULT Surface::GetDesc(D3DSURFACE_DESC* desc) {
    SurfaceImpl* surface = static_cast<SurfaceImpl*>(this);
    if (!desc) {
        return D3DERR_INVALIDCALL;
    }
    desc->Format = surface->m_format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = surface->m_info.m_depth ? D3DUSAGE_DEPTHSTENCIL : (surface->m_container && !(surface->m_container->m_usage & D3DUSAGE_RENDERTARGET) ? 0 : D3DUSAGE_RENDERTARGET);
    desc->Pool = surface->m_container ? surface->m_container->m_pool : D3DPOOL_DEFAULT;
    desc->MultiSampleType = surface->m_samples > 1 ? static_cast<D3DMULTISAMPLE_TYPE>(surface->m_samples) : D3DMULTISAMPLE_NONE;
    desc->MultiSampleQuality = 0;
    desc->Width = surface->m_width;
    desc->Height = surface->m_height;
    return D3D_OK;
}

HRESULT Surface::LockRect(D3DLOCKED_RECT* lockedRect, const Rect* rect, uint32_t flags) {
    SurfaceImpl* surface = static_cast<SurfaceImpl*>(this);

    if (!lockedRect || surface->m_locked || surface->m_info.m_depth || !surface->Valid()) {
        return D3DERR_INVALIDCALL;
    }

    Rect area = rect ? *rect : Rect { 0, 0, static_cast<int32_t>(surface->m_width), static_cast<int32_t>(surface->m_height) };
    if (area.left < 0 || area.top < 0 || area.right > static_cast<int32_t>(surface->m_width) || area.bottom > static_cast<int32_t>(surface->m_height) || area.left >= area.right || area.top >= area.bottom) {
        return D3DERR_INVALIDCALL;
    }

    const FormatInfo& info = surface->m_info;
    bool renderTarget = !surface->m_container || (surface->m_container->m_usage & D3DUSAGE_RENDERTARGET);

    surface->m_locked = true;
    surface->m_lockRect = area;
    surface->m_lockReadOnly = (flags & D3DLOCK_READONLY) != 0;

    if (renderTarget) {
        // Render target contents live on the GPU: read the whole surface back
        uint32_t pitch = info.RowPitch(surface->m_width);
        uint32_t length = pitch * info.RowCount(surface->m_height);
        if (!surface->m_readback || surface->m_readback.length < length) {
            surface->m_readback = [surface->m_device->m_device newBufferWithLength:length options:MTLResourceStorageModeShared];
        }
        surface->m_device->ReadSurface(surface, surface->m_readback, pitch);

        uint32_t x = info.m_compressed ? area.left / 4 : area.left;
        uint32_t y = info.m_compressed ? area.top / 4 : area.top;
        lockedRect->Pitch = static_cast<int32_t>(pitch);
        lockedRect->pBits = static_cast<uint8_t*>(surface->m_readback.contents) + y * pitch + x * info.m_bytesPerUnit;
        surface->m_lockStaging = UploadAllocation();
        return D3D_OK;
    }

    // Texture levels are written through staging memory; their previous contents in the
    // locked area are not read back, as the client only writes whole rectangles
    uint32_t width = static_cast<uint32_t>(area.right - area.left);
    uint32_t height = static_cast<uint32_t>(area.bottom - area.top);
    uint32_t pitch = info.RowPitch(width);
    surface->m_lockStaging = surface->m_device->AllocateUpload(pitch * info.RowCount(height), 16);
    lockedRect->Pitch = static_cast<int32_t>(pitch);
    lockedRect->pBits = surface->m_lockStaging.m_data;
    return D3D_OK;
}

HRESULT Surface::UnlockRect() {
    SurfaceImpl* surface = static_cast<SurfaceImpl*>(this);

    if (!surface->m_locked) {
        return D3DERR_INVALIDCALL;
    }

    surface->m_locked = false;

    if (surface->m_lockReadOnly) {
        return D3D_OK;
    }

    const FormatInfo& info = surface->m_info;
    const Rect& area = surface->m_lockRect;
    uint32_t width = static_cast<uint32_t>(area.right - area.left);
    uint32_t height = static_cast<uint32_t>(area.bottom - area.top);
    uint32_t pitch = info.RowPitch(width);
    uint32_t rows = info.RowCount(height);

    UploadAllocation source = surface->m_lockStaging;

    if (source.m_buffer && area.left == 0 && area.top == 0 && width == surface->m_width && height == surface->m_height && width * height <= 64 * 64 && !info.m_compressed) {
        surface->m_mirror.assign(source.m_data, source.m_data + pitch * rows);
        surface->m_mirrorPitch = pitch;
    }

    if (!source.m_buffer) {
        // A render target written through a readback lock: copy the area back
        uint32_t fullPitch = info.RowPitch(surface->m_width);
        source = surface->m_device->AllocateUpload(pitch * rows, 16);
        uint32_t x = info.m_compressed ? area.left / 4 : area.left;
        uint32_t y = info.m_compressed ? area.top / 4 : area.top;
        const uint8_t* from = static_cast<const uint8_t*>(surface->m_readback.contents) + y * fullPitch + x * info.m_bytesPerUnit;
        for (uint32_t row = 0; row < rows; row++) {
            memcpy(source.m_data + row * pitch, from + row * fullPitch, pitch);
        }
    }

    id<MTLTexture> target = surface->m_samples > 1 ? surface->m_resolve : surface->m_texture;
    surface->m_device->CopyToTexture(source, pitch, pitch * rows, target, surface->m_face, surface->m_level, MTLOriginMake(area.left, area.top, 0), MTLSizeMake(width, height, 1), surface->Use());

    if (surface->m_container) {
        surface->MarkRendered();
    }

    return D3D_OK;
}

// Queries

QueryImpl::~QueryImpl() {
    this->m_device->QueryDestroyed(this);
}

HRESULT Query::Issue(uint32_t flags) {
    QueryImpl* query = static_cast<QueryImpl*>(this);

    if (query->m_type == D3DQUERYTYPE_OCCLUSION) {
        if (flags & D3DISSUE_BEGIN) {
            if (query->m_state == QueryImpl::STATE_BUILDING) {
                query->m_device->EndOcclusionQuery(query);
            }
            query->m_device->BeginOcclusionQuery(query);
            query->m_state = QueryImpl::STATE_BUILDING;
        } else if (flags & D3DISSUE_END) {
            if (query->m_state == QueryImpl::STATE_BUILDING) {
                query->m_device->EndOcclusionQuery(query);
            } else {
                // Ending without beginning counts nothing
                query->m_slots.clear();
            }
            query->m_serial = query->m_device->CurrentSerial();
            query->m_state = QueryImpl::STATE_ISSUED;
        }
        return D3D_OK;
    }

    if (flags & D3DISSUE_END) {
        query->m_serial = query->m_device->CurrentSerial();
        query->m_state = QueryImpl::STATE_ISSUED;
    }

    return D3D_OK;
}

HRESULT Query::GetData(void* data, uint32_t size, uint32_t flags) {
    QueryImpl* query = static_cast<QueryImpl*>(this);

    if (query->m_state == QueryImpl::STATE_BUILDING) {
        return S_FALSE;
    }

    if (query->m_state == QueryImpl::STATE_ISSUED) {
        if (query->m_device->CompletedSerial() < query->m_serial) {
            if ((flags & D3DGETDATA_FLUSH) && query->m_serial == query->m_device->CurrentSerial()) {
                query->m_device->Flush(false);
            }

            if (query->m_device->CompletedSerial() < query->m_serial) {
                return S_FALSE;
            }
        }
    }

    if (data && size >= 4) {
        uint32_t result = 1;

        if (query->m_type == D3DQUERYTYPE_OCCLUSION) {
            uint64_t samples = 0;
            for (const QueryImpl::Slot& slot : query->m_slots) {
                samples += *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(slot.m_buffer.contents) + slot.m_offset);
            }
            result = static_cast<uint32_t>(std::min<uint64_t>(samples, UINT32_MAX));
        }

        memcpy(data, &result, 4);
    }

    return D3D_OK;
}

uint32_t Query::GetDataSize() {
    return 4;
}

// The adapter

Direct3D* Direct3D::Create() {
    if (!MTLCreateSystemDefaultDevice()) {
        return nullptr;
    }
    return new Direct3D();
}

uint32_t Direct3D::GetAdapterCount() {
    return 1;
}

HRESULT Direct3D::GetAdapterIdentifier(uint32_t adapter, uint32_t flags, AdapterIdentifier* identifier) {
    if (adapter != 0 || !identifier) {
        return D3DERR_INVALIDCALL;
    }

    memset(identifier, 0, sizeof(*identifier));
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    snprintf(identifier->Driver, sizeof(identifier->Driver), "Metal");
    snprintf(identifier->Description, sizeof(identifier->Description), "%s", device.name.UTF8String);
    snprintf(identifier->DeviceName, sizeof(identifier->DeviceName), "\\\\.\\DISPLAY1");

    NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
    identifier->DriverVersionHigh = static_cast<uint32_t>((version.majorVersion << 16) | version.minorVersion);
    identifier->DriverVersionLow = static_cast<uint32_t>(version.patchVersion << 16);
    identifier->VendorId = 0x106B;
    identifier->DeviceId = static_cast<uint32_t>(device.registryID & 0xFFFF);
    (void)flags;
    return D3D_OK;
}

HRESULT Direct3D::GetAdapterDisplayMode(uint32_t adapter, DisplayMode* mode) {
    if (adapter != 0 || !mode) {
        return D3DERR_INVALIDCALL;
    }

    CGDirectDisplayID display = CGMainDisplayID();
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (!current) {
        return D3DERR_NOTAVAILABLE;
    }

    mode->Width = static_cast<uint32_t>(CGDisplayModeGetPixelWidth(current));
    mode->Height = static_cast<uint32_t>(CGDisplayModeGetPixelHeight(current));
    double refresh = CGDisplayModeGetRefreshRate(current);
    mode->RefreshRate = refresh > 0.0 ? static_cast<uint32_t>(refresh + 0.5) : 60;
    mode->Format = D3DFMT_X8R8G8B8;
    CGDisplayModeRelease(current);
    return D3D_OK;
}

HRESULT Direct3D::GetDeviceCaps(uint32_t adapter, D3DCAPS9* caps) {
    if (adapter != 0 || !caps) {
        return D3DERR_INVALIDCALL;
    }

    memset(caps, 0, sizeof(*caps));
    // Bit 0 reports stream offsets (D3DDEVCAPS2_STREAMOFFSET as the client reads it)
    caps->Caps2 = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_CANAUTOGENMIPMAP | 0x1;
    caps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    caps->PrimitiveMiscCaps = D3DPMISCCAPS_COLORWRITEENABLE;
    caps->RasterCaps = D3DPRASTERCAPS_DEPTHBIAS;
    caps->SrcBlendCaps = D3DPBLENDCAPS_BLENDFACTOR;
    // Unconditional non-power-of-two support: neither POW2 nor NONPOW2CONDITIONAL
    caps->TextureCaps = D3DPTEXTURECAPS_CUBEMAP;
    caps->TextureFilterCaps = D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MAGFANISOTROPIC | D3DPTFILTERCAPS_MIPFLINEAR;
    caps->CursorCaps = D3DCURSORCAPS_COLOR;
    caps->MaxTextureWidth = 16384;
    caps->MaxTextureHeight = 16384;
    caps->MaxAnisotropy = 16;
    caps->MaxUserClipPlanes = 6;
    caps->MaxSimultaneousTextures = 8;
    caps->MaxStreams = 16;
    caps->MaxVertexIndex = 0x00FFFFFF;
    caps->MaxPointSize = 256.0f;
    caps->VertexShaderVersion = D3DVS_VERSION(3, 0);
    caps->MaxVertexShaderConst = 256;
    caps->PixelShaderVersion = D3DPS_VERSION(3, 0);
    return D3D_OK;
}

HRESULT Direct3D::CheckDeviceFormat(uint32_t adapter, D3DFORMAT adapterFormat, uint32_t usage, D3DRESOURCETYPE type, D3DFORMAT format) {
    FormatInfo info;
    if (adapter != 0 || !LookupFormat(format, info)) {
        return D3DERR_NOTAVAILABLE;
    }

    if ((usage & D3DUSAGE_DEPTHSTENCIL) && !info.m_depth) {
        return D3DERR_NOTAVAILABLE;
    }

    if ((usage & D3DUSAGE_RENDERTARGET) && (!info.m_renderable || info.m_depth)) {
        return D3DERR_NOTAVAILABLE;
    }

    if (type == D3DRTYPE_CUBETEXTURE && info.m_depth) {
        return D3DERR_NOTAVAILABLE;
    }

    (void)adapterFormat;
    return D3D_OK;
}

HRESULT Direct3D::CheckDepthStencilMatch(uint32_t adapter, D3DFORMAT adapterFormat, D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat) {
    FormatInfo info;
    if (adapter != 0 || !LookupFormat(depthStencilFormat, info) || !info.m_depth) {
        return D3DERR_NOTAVAILABLE;
    }
    (void)adapterFormat;
    (void)renderTargetFormat;
    return D3D_OK;
}

HRESULT Direct3D::CheckDeviceMultiSampleType(uint32_t adapter, D3DFORMAT format, int32_t windowed, D3DMULTISAMPLE_TYPE type, uint32_t* qualityLevels) {
    FormatInfo info;
    if (adapter != 0 || !LookupFormat(format, info)) {
        return D3DERR_NOTAVAILABLE;
    }

    uint32_t samples = type <= D3DMULTISAMPLE_NONMASKABLE ? 1 : static_cast<uint32_t>(type);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (samples > 1 && ![device supportsTextureSampleCount:samples]) {
        return D3DERR_NOTAVAILABLE;
    }

    if (qualityLevels) {
        *qualityLevels = 1;
    }
    (void)windowed;
    return D3D_OK;
}

HRESULT Direct3D::CreateDevice(const PresentParameters& parameters, Device** device) {
    if (!device) {
        return D3DERR_INVALIDCALL;
    }

    *device = nullptr;
    DeviceImpl* created = new DeviceImpl();
    if (!created->Init(MTLCreateSystemDefaultDevice(), parameters)) {
        created->Release();
        return D3DERR_NOTAVAILABLE;
    }

    *device = created;
    return D3D_OK;
}

}
