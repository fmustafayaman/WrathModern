#include "DeviceInternal.h"

#import <AppKit/AppKit.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace D3D9Metal {

namespace {

// Matrices are Direct3D's: row-major, transforming row vectors (v * M)

typedef std::array<float, 16> Matrix;

Matrix ToMatrix(const D3DMATRIX& m) {
    Matrix result;
    memcpy(result.data(), &m, sizeof(float) * 16);
    return result;
}

Matrix Identity() {
    return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}

Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix result;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            result[i * 4 + j] = sum;
        }
    }
    return result;
}

Matrix Transpose(const Matrix& m) {
    Matrix result;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            result[i * 4 + j] = m[j * 4 + i];
        }
    }
    return result;
}

Matrix Inverse(const Matrix& m) {
    Matrix inv;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        return Identity();
    }

    float invDet = 1.0f / det;
    for (float& value : inv) {
        value *= invDet;
    }
    return inv;
}

void TransformRow(const float* v, const Matrix& m, float* out) {
    for (int j = 0; j < 4; j++) {
        out[j] = v[0] * m[0 * 4 + j] + v[1] * m[1 * 4 + j] + v[2] * m[2 * 4 + j] + v[3] * m[3 * 4 + j];
    }
}

void DecodeColor(uint32_t color, float* rgba) {
    rgba[0] = ((color >> 16) & 0xFF) / 255.0f;
    rgba[1] = ((color >> 8) & 0xFF) / 255.0f;
    rgba[2] = (color & 0xFF) / 255.0f;
    rgba[3] = ((color >> 24) & 0xFF) / 255.0f;
}

float AsFloat(uint32_t value) {
    float result;
    memcpy(&result, &value, 4);
    return result;
}

uint32_t AsUint(float value) {
    uint32_t result;
    memcpy(&result, &value, 4);
    return result;
}

MTLBlendFactor BlendFactor(uint32_t blend) {
    switch (blend) {
        case D3DBLEND_ZERO: return MTLBlendFactorZero;
        case D3DBLEND_ONE: return MTLBlendFactorOne;
        case D3DBLEND_SRCCOLOR: return MTLBlendFactorSourceColor;
        case D3DBLEND_INVSRCCOLOR: return MTLBlendFactorOneMinusSourceColor;
        case D3DBLEND_SRCALPHA: return MTLBlendFactorSourceAlpha;
        case D3DBLEND_INVSRCALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case D3DBLEND_DESTALPHA: return MTLBlendFactorDestinationAlpha;
        case D3DBLEND_INVDESTALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case D3DBLEND_DESTCOLOR: return MTLBlendFactorDestinationColor;
        case D3DBLEND_INVDESTCOLOR: return MTLBlendFactorOneMinusDestinationColor;
        case D3DBLEND_SRCALPHASAT: return MTLBlendFactorSourceAlphaSaturated;
        case D3DBLEND_BLENDFACTOR: return MTLBlendFactorBlendColor;
        case D3DBLEND_INVBLENDFACTOR: return MTLBlendFactorOneMinusBlendColor;
        default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation BlendOperation(uint32_t op) {
    switch (op) {
        case D3DBLENDOP_SUBTRACT: return MTLBlendOperationSubtract;
        case D3DBLENDOP_REVSUBTRACT: return MTLBlendOperationReverseSubtract;
        case D3DBLENDOP_MIN: return MTLBlendOperationMin;
        case D3DBLENDOP_MAX: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction CompareFunction(uint32_t func) {
    switch (func) {
        case D3DCMP_NEVER: return MTLCompareFunctionNever;
        case D3DCMP_LESS: return MTLCompareFunctionLess;
        case D3DCMP_EQUAL: return MTLCompareFunctionEqual;
        case D3DCMP_LESSEQUAL: return MTLCompareFunctionLessEqual;
        case D3DCMP_GREATER: return MTLCompareFunctionGreater;
        case D3DCMP_NOTEQUAL: return MTLCompareFunctionNotEqual;
        case D3DCMP_GREATEREQUAL: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
    }
}

MTLSamplerAddressMode AddressMode(uint32_t address) {
    switch (address) {
        case D3DTADDRESS_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
        case D3DTADDRESS_CLAMP: return MTLSamplerAddressModeClampToEdge;
        case D3DTADDRESS_BORDER: return MTLSamplerAddressModeClampToBorderColor;
        case D3DTADDRESS_MIRRORONCE: return MTLSamplerAddressModeMirrorClampToEdge;
        default: return MTLSamplerAddressModeRepeat;
    }
}

bool DeclarationFormat(uint8_t type, MTLVertexFormat& format, uint8_t& attributeClass, uint8_t& components) {
    attributeClass = ATTRIBCLASS_FLOAT;
    switch (type) {
        case D3DDECLTYPE_FLOAT1: format = MTLVertexFormatFloat; components = 1; return true;
        case D3DDECLTYPE_FLOAT2: format = MTLVertexFormatFloat2; components = 2; return true;
        case D3DDECLTYPE_FLOAT3: format = MTLVertexFormatFloat3; components = 3; return true;
        case D3DDECLTYPE_FLOAT4: format = MTLVertexFormatFloat4; components = 4; return true;
        case D3DDECLTYPE_D3DCOLOR: format = MTLVertexFormatUChar4Normalized_BGRA; components = 4; return true;
        case D3DDECLTYPE_UBYTE4: format = MTLVertexFormatUChar4; components = 4; attributeClass = ATTRIBCLASS_UINT; return true;
        case D3DDECLTYPE_SHORT2: format = MTLVertexFormatShort2; components = 2; attributeClass = ATTRIBCLASS_SINT; return true;
        case D3DDECLTYPE_SHORT4: format = MTLVertexFormatShort4; components = 4; attributeClass = ATTRIBCLASS_SINT; return true;
        case D3DDECLTYPE_UBYTE4N: format = MTLVertexFormatUChar4Normalized; components = 4; return true;
        case D3DDECLTYPE_SHORT2N: format = MTLVertexFormatShort2Normalized; components = 2; return true;
        case D3DDECLTYPE_SHORT4N: format = MTLVertexFormatShort4Normalized; components = 4; return true;
        case D3DDECLTYPE_USHORT2N: format = MTLVertexFormatUShort2Normalized; components = 2; return true;
        case D3DDECLTYPE_USHORT4N: format = MTLVertexFormatUShort4Normalized; components = 4; return true;
        case D3DDECLTYPE_DEC3N: format = MTLVertexFormatInt1010102Normalized; components = 3; return true;
        case D3DDECLTYPE_FLOAT16_2: format = MTLVertexFormatHalf2; components = 2; return true;
        case D3DDECLTYPE_FLOAT16_4: format = MTLVertexFormatHalf4; components = 4; return true;
        default: return false;
    }
}

// The viewport origin moves by almost half a pixel: Direct3D 9 puts pixel centers at integer
// coordinates, Metal at half-integers. Slightly less than half keeps exact-texel quads stable.
const float kPixelCenterOffset = 0.5f - 1.0f / 128.0f;

const char* const kUtilityShaders = R"MSL(#include <metal_stdlib>
using namespace metal;

struct UtilityVaryings {
    float4 position [[position]];
    float2 uv;
};

struct UtilityClear {
    float4 color;
    float depth;
};

vertex UtilityVaryings utility_vertex(uint vid [[vertex_id]], constant float4& rect [[buffer(0)]], constant float& depth [[buffer(1)]]) {
    // rect: destination x0 y0 x1 y1 in clip space; the uv covers 0..1 over the destination
    float2 corner = float2((vid & 1) ? 1.0f : 0.0f, (vid & 2) ? 1.0f : 0.0f);
    UtilityVaryings out;
    out.position = float4(mix(rect.xy, rect.zw, corner), depth, 1.0f);
    out.uv = corner;
    return out;
}

fragment float4 utility_clear(constant float4& color [[buffer(0)]]) {
    return color;
}

struct BlitParameters {
    float4 sourceRect;   // u0 v0 u1 v1
    uint gamma;
};

fragment float4 utility_blit(UtilityVaryings in [[stage_in]], texture2d<float> source [[texture(0)]], sampler smp [[sampler(0)]], constant BlitParameters& parameters [[buffer(0)]], texture2d<float> gammaRamp [[texture(1)]]) {
    float2 uv = mix(parameters.sourceRect.xy, parameters.sourceRect.zw, in.uv);
    float4 color = source.sample(smp, uv);

    if (parameters.gamma != 0u) {
        uint3 index = uint3(saturate(color.rgb) * 255.0f + 0.5f);
        color.r = gammaRamp.read(uint2(index.r, 0)).r;
        color.g = gammaRamp.read(uint2(index.g, 0)).g;
        color.b = gammaRamp.read(uint2(index.b, 0)).b;
    }

    return color;
}
)MSL";

}

struct DeviceImpl::ImplState {
    CAMetalLayer* m_layer = nil;
    UploadHeap m_uploads;

    // Command buffers of the current serial, and the encoder open in the render command buffer
    id<MTLCommandBuffer> m_commands = nil;
    id<MTLCommandBuffer> m_uploadCommands = nil;
    id<MTLBlitCommandEncoder> m_uploadEncoder = nil;
    id<MTLRenderCommandEncoder> m_render = nil;
    id<MTLBlitCommandEncoder> m_blit = nil;
    dispatch_semaphore_t m_inFlight = nullptr;

    // Swap chain
    SurfaceImpl* m_backBuffer = nullptr;
    SurfaceImpl* m_autoDepth = nullptr;
    bool m_gammaIdentity = true;
    id<MTLTexture> m_gammaTexture = nil;

    // Render targets and clears waiting for the next pass
    SurfaceImpl* m_renderTarget = nullptr;
    SurfaceImpl* m_depthStencil = nullptr;
    bool m_pendingClearColor = false;
    bool m_pendingClearDepth = false;
    float m_clearColor[4] = {};
    float m_clearDepth = 1.0f;

    // Occlusion queries
    id<MTLBuffer> m_visibility = nil;
    uint32_t m_visibilityUsed = 0;
    std::vector<QueryImpl*> m_activeQueries;

    // Direct3D state
    uint32_t m_renderStates[D3DRS_COUNT] = {};
    uint32_t m_stageStates[8][D3DTSS_COUNT] = {};
    uint32_t m_samplerStates[16][D3DSAMP_COUNT] = {};
    BaseTexture* m_textures[16] = {};
    Matrix m_world = Identity();
    Matrix m_view = Identity();
    Matrix m_projection = Identity();
    Matrix m_textureTransforms[8];
    D3DMATERIAL9 m_material = {};
    D3DLIGHT9 m_lights[8] = {};
    bool m_lightEnabled[8] = {};
    float m_clipPlanes[6][4] = {};
    Rect m_scissor = {};
    D3DVIEWPORT9 m_viewport = {};

    struct Stream {
        VertexBufferImpl* m_buffer = nullptr;
        uint32_t m_offset = 0;
        uint32_t m_stride = 0;
    };

    Stream m_streams[16];
    IndexBufferImpl* m_indices = nullptr;
    VertexDeclarationImpl* m_declaration = nullptr;
    VertexShaderImpl* m_vertexShader = nullptr;
    PixelShaderImpl* m_pixelShader = nullptr;

    float m_vsConstants[256 * 4] = {};
    float m_psConstants[224 * 4] = {};
    int32_t m_vsIntConstants[16 * 4] = {};
    int32_t m_psIntConstants[16 * 4] = {};
    uint32_t m_vsBoolConstants[16] = {};
    uint32_t m_psBoolConstants[16] = {};

    // What changed since the encoder last saw it
    bool m_vsConstantsDirty = true;
    bool m_psConstantsDirty = true;
    bool m_vertexStateDirty = true;
    bool m_pixelStateDirty = true;
    bool m_fixedVertexDirty = true;

    // Encoder state, valid while m_render is open
    struct Bound {
        id<MTLRenderPipelineState> m_pipeline = nil;
        id<MTLDepthStencilState> m_depthStencil = nil;
        int32_t m_cull = -1;
        int32_t m_fill = -1;
        float m_slopeBias = NAN;
        uint32_t m_blendFactor = 0xFFFFFFFF;
        bool m_blendFactorSet = false;
        MTLViewport m_viewport = {};
        MTLScissorRect m_scissor = {};
        bool m_viewportSet = false;
        id<MTLBuffer> m_vertexBuffers[16] = {};
        uint32_t m_vertexOffsets[16] = {};
        id<MTLTexture> m_textures[16] = {};
        id<MTLSamplerState> m_samplers[16] = {};
        uint64_t m_vertexFunction = 0;
        uint64_t m_fragmentFunction = 0;
    } m_bound;

    // Caches
    std::unordered_map<PipelineKey, id<MTLRenderPipelineState>, BytewiseHash<PipelineKey>> m_pipelines;
    std::unordered_map<SamplerKey, id<MTLSamplerState>, BytewiseHash<SamplerKey>> m_samplers;
    std::unordered_map<uint32_t, id<MTLDepthStencilState>> m_depthStates;
    std::unordered_map<FixedVertexKey, CompiledFunction, BytewiseHash<FixedVertexKey>> m_fixedVertex;
    std::unordered_map<FixedPixelKey, CompiledFunction, BytewiseHash<FixedPixelKey>> m_fixedPixel;
    std::unordered_map<std::string, CompiledFunction> m_functions;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> m_utilityPipelines;

    id<MTLLibrary> m_utilityLibrary = nil;
    id<MTLSamplerState> m_pointSampler = nil;
    id<MTLSamplerState> m_linearSampler = nil;
    id<MTLTexture> m_dummy2d = nil;
    id<MTLTexture> m_dummyCube = nil;
    id<MTLTexture> m_dummyDepth = nil;
    id<MTLDepthStencilState> m_clearDepthWrite = nil;
    id<MTLDepthStencilState> m_clearDepthKeep = nil;

    // Cursor: the last bitmap, kept to rebuild the cursor at another scale
    NSCursor* m_cursor = nil;
    bool m_cursorShown = false;
    float m_cursorScale = 1.0f;
    std::vector<uint8_t> m_cursorPixels;    // RGBA, non-premultiplied
    uint32_t m_cursorWidth = 0;
    uint32_t m_cursorHeight = 0;
    uint32_t m_cursorHotspotX = 0;
    uint32_t m_cursorHotspotY = 0;
};

// Device lifetime

DeviceImpl::DeviceImpl() : m_state(new ImplState()) {
}

DeviceImpl::~DeviceImpl() {
    ImplState& s = *this->m_state;

    this->Flush(true);

    auto release = [](Unknown* object) {
        if (object) {
            object->Release();
        }
    };

    for (BaseTexture* texture : s.m_textures) {
        release(texture);
    }
    for (ImplState::Stream& stream : s.m_streams) {
        release(stream.m_buffer);
    }
    release(s.m_indices);
    release(s.m_declaration);
    release(s.m_vertexShader);
    release(s.m_pixelShader);
    release(s.m_renderTarget);
    release(s.m_depthStencil);
    release(s.m_backBuffer);
    release(s.m_autoDepth);
}

bool DeviceImpl::Init(id<MTLDevice> device, const PresentParameters& parameters) {
    ImplState& s = *this->m_state;

    this->m_device = device;
    this->m_queue = [device newCommandQueueWithMaxCommandBufferCount:64];
    if (!this->m_queue) {
        return false;
    }

    Direct3D* adapter = Direct3D::Create();
    adapter->GetDeviceCaps(0, &this->m_caps);
    adapter->Release();

    s.m_uploads.Init(device);
    s.m_inFlight = dispatch_semaphore_create(3);
    s.m_layer = (__bridge CAMetalLayer*)parameters.Layer;

    if (s.m_layer) {
        s.m_layer.device = device;
        s.m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        s.m_layer.framebufferOnly = NO;
        s.m_layer.maximumDrawableCount = 3;
    }

    NSError* error = nil;
    s.m_utilityLibrary = [device newLibraryWithSource:@(kUtilityShaders) options:nil error:&error];
    if (!s.m_utilityLibrary) {
        Log("utility shaders failed to compile: %s", error.localizedDescription.UTF8String);
        return false;
    }

    MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
    sampler.minFilter = MTLSamplerMinMagFilterNearest;
    sampler.magFilter = MTLSamplerMinMagFilterNearest;
    sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s.m_pointSampler = [device newSamplerStateWithDescriptor:sampler];
    sampler.minFilter = MTLSamplerMinMagFilterLinear;
    sampler.magFilter = MTLSamplerMinMagFilterLinear;
    s.m_linearSampler = [device newSamplerStateWithDescriptor:sampler];

    MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionAlways;
    depth.depthWriteEnabled = YES;
    s.m_clearDepthWrite = [device newDepthStencilStateWithDescriptor:depth];
    depth.depthWriteEnabled = NO;
    s.m_clearDepthKeep = [device newDepthStencilStateWithDescriptor:depth];

    // Unbound samplers read opaque black
    const uint32_t black = 0xFF000000;
    MTLTextureDescriptor* dummy = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
    dummy.storageMode = MTLStorageModeShared;
    s.m_dummy2d = [device newTextureWithDescriptor:dummy];
    [s.m_dummy2d replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];

    MTLTextureDescriptor* cube = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:1 mipmapped:NO];
    cube.storageMode = MTLStorageModeShared;
    s.m_dummyCube = [device newTextureWithDescriptor:cube];
    for (NSUInteger face = 0; face < 6; face++) {
        [s.m_dummyCube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:face withBytes:&black bytesPerRow:4 bytesPerImage:4];
    }

    MTLTextureDescriptor* depthDummy = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:1 height:1 mipmapped:NO];
    depthDummy.storageMode = MTLStorageModePrivate;
    depthDummy.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    s.m_dummyDepth = [device newTextureWithDescriptor:depthDummy];

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = s.m_dummyDepth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.clearDepth = 1.0;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> commands = [this->m_queue commandBuffer];
    [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    [commands commit];

    this->m_parameters = parameters;
    if (!this->CreateSwapChainResources()) {
        return false;
    }

    this->ResetState();
    return true;
}

bool DeviceImpl::CreateSwapChainResources() {
    ImplState& s = *this->m_state;
    const PresentParameters& p = this->m_parameters;

    if (s.m_backBuffer) {
        s.m_backBuffer->Release();
        s.m_backBuffer = nullptr;
    }
    if (s.m_autoDepth) {
        s.m_autoDepth->Release();
        s.m_autoDepth = nullptr;
    }

    uint32_t width = std::max(1u, p.BackBufferWidth);
    uint32_t height = std::max(1u, p.BackBufferHeight);
    uint32_t samples = p.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES ? static_cast<uint32_t>(p.MultiSampleType) : 1;
    if (samples > 1 && ![this->m_device supportsTextureSampleCount:samples]) {
        samples = 1;
    }

    // The back buffer is the window's pixel format; its alpha is never written (X8R8G8B8)
    s.m_backBuffer = new SurfaceImpl(this, D3DFMT_X8R8G8B8, width, height, samples, true);
    if (!s.m_backBuffer->Valid()) {
        return false;
    }

    if (p.EnableAutoDepthStencil) {
        s.m_autoDepth = new SurfaceImpl(this, p.AutoDepthStencilFormat, width, height, samples, false);
        if (!s.m_autoDepth->Valid()) {
            return false;
        }
    }

    return true;
}

void DeviceImpl::ResetState() {
    ImplState& s = *this->m_state;

    auto setRender = [&](D3DRENDERSTATETYPE state, uint32_t value) { s.m_renderStates[state] = value; };

    memset(s.m_renderStates, 0, sizeof(s.m_renderStates));
    setRender(D3DRS_ZENABLE, this->m_parameters.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE);
    setRender(D3DRS_FILLMODE, D3DFILL_SOLID);
    setRender(D3DRS_SHADEMODE, 2);
    setRender(D3DRS_ZWRITEENABLE, 1);
    setRender(D3DRS_LASTPIXEL, 1);
    setRender(D3DRS_SRCBLEND, D3DBLEND_ONE);
    setRender(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    setRender(D3DRS_CULLMODE, D3DCULL_CCW);
    setRender(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    setRender(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
    setRender(D3DRS_FOGEND, AsUint(1.0f));
    setRender(D3DRS_FOGDENSITY, AsUint(1.0f));
    setRender(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
    setRender(D3DRS_CLIPPING, 1);
    setRender(D3DRS_LIGHTING, 1);
    setRender(D3DRS_COLORVERTEX, 1);
    setRender(D3DRS_LOCALVIEWER, 1);
    setRender(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    setRender(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
    setRender(D3DRS_POINTSIZE, AsUint(1.0f));
    setRender(D3DRS_POINTSIZE_MIN, AsUint(1.0f));
    setRender(D3DRS_POINTSCALE_A, AsUint(1.0f));
    setRender(D3DRS_MULTISAMPLEANTIALIAS, 1);
    setRender(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
    setRender(D3DRS_POINTSIZE_MAX, AsUint(this->m_caps.MaxPointSize));
    setRender(D3DRS_COLORWRITEENABLE, 0xF);
    setRender(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    setRender(D3DRS_BLENDFACTOR, 0xFFFFFFFF);
    setRender(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    setRender(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
    setRender(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);

    for (uint32_t stage = 0; stage < 8; stage++) {
        uint32_t* tss = s.m_stageStates[stage];
        memset(tss, 0, sizeof(s.m_stageStates[stage]));
        tss[D3DTSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        tss[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        tss[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        tss[D3DTSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        tss[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        tss[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        tss[D3DTSS_TEXCOORDINDEX] = stage;
        tss[D3DTSS_COLORARG0] = D3DTA_CURRENT;
        tss[D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
        tss[D3DTSS_RESULTARG] = D3DTA_CURRENT;
        s.m_textureTransforms[stage] = Identity();
    }

    for (uint32_t sampler = 0; sampler < 16; sampler++) {
        uint32_t* ss = s.m_samplerStates[sampler];
        memset(ss, 0, sizeof(s.m_samplerStates[sampler]));
        ss[D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        ss[D3DSAMP_MAXANISOTROPY] = 1;
    }

    for (uint32_t i = 0; i < 16; i++) {
        this->SetTextureImpl(i, nullptr);
        this->SetStreamSourceImpl(i, nullptr, 0, 0);
    }
    this->SetIndicesImpl(nullptr);
    this->SetVertexDeclarationImpl(nullptr);
    this->SetVertexShaderImpl(nullptr);
    this->SetPixelShaderImpl(nullptr);

    s.m_world = Identity();
    s.m_view = Identity();
    s.m_projection = Identity();
    memset(&s.m_material, 0, sizeof(s.m_material));
    memset(s.m_lights, 0, sizeof(s.m_lights));
    for (D3DLIGHT9& light : s.m_lights) {
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse = { 1.0f, 1.0f, 1.0f, 0.0f };
        light.Direction = { 0.0f, 0.0f, 1.0f };
    }
    memset(s.m_lightEnabled, 0, sizeof(s.m_lightEnabled));
    memset(s.m_clipPlanes, 0, sizeof(s.m_clipPlanes));

    memset(s.m_vsConstants, 0, sizeof(s.m_vsConstants));
    memset(s.m_psConstants, 0, sizeof(s.m_psConstants));
    memset(s.m_vsIntConstants, 0, sizeof(s.m_vsIntConstants));
    memset(s.m_psIntConstants, 0, sizeof(s.m_psIntConstants));
    memset(s.m_vsBoolConstants, 0, sizeof(s.m_vsBoolConstants));
    memset(s.m_psBoolConstants, 0, sizeof(s.m_psBoolConstants));

    // Render targets: the back buffer and the automatic depth buffer
    this->SetDepthStencilSurfaceImpl(s.m_autoDepth);
    this->SetRenderTargetImpl(0, s.m_backBuffer);

    s.m_vsConstantsDirty = true;
    s.m_psConstantsDirty = true;
    s.m_vertexStateDirty = true;
    s.m_pixelStateDirty = true;
    s.m_fixedVertexDirty = true;
}

// Commands and submission

id<MTLCommandBuffer> DeviceImpl::Commands() {
    ImplState& s = *this->m_state;

    if (!s.m_commands) {
        // Limit the command buffers in flight, which paces the CPU to the GPU
        dispatch_semaphore_wait(s.m_inFlight, DISPATCH_TIME_FOREVER);
        s.m_commands = [this->m_queue commandBuffer];
        s.m_commands.label = @"D3D9 frame";
    }

    return s.m_commands;
}

void DeviceImpl::EndEncoders() {
    ImplState& s = *this->m_state;

    if (s.m_render) {
        // Occlusion counting stops with the encoder; open queries continue in the next pass
        [s.m_render endEncoding];
        s.m_render = nil;
    }

    if (s.m_blit) {
        [s.m_blit endEncoding];
        s.m_blit = nil;
    }
}

void DeviceImpl::Flush(bool wait) {
    ImplState& s = *this->m_state;

    this->ExecutePendingClears();
    this->EndEncoders();

    if (!s.m_commands && !s.m_uploadCommands) {
        if (wait) {
            this->WaitForSerial(this->m_serial - 1);
        }
        return;
    }

    if (s.m_uploadEncoder) {
        [s.m_uploadEncoder endEncoding];
        s.m_uploadEncoder = nil;
    }

    if (s.m_uploadCommands) {
        [s.m_uploadCommands commit];
        s.m_uploadCommands = nil;
    }

    id<MTLCommandBuffer> commands = this->Commands();
    uint64_t serial = this->m_serial;
    dispatch_semaphore_t inFlight = s.m_inFlight;
    std::atomic<uint64_t>* completed = &this->m_completed;

    [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        if (buffer.status == MTLCommandBufferStatusError) {
            Log("command buffer %llu failed: %s", serial, buffer.error.localizedDescription.UTF8String);
        }

        uint64_t previous = completed->load(std::memory_order_relaxed);
        while (previous < serial && !completed->compare_exchange_weak(previous, serial, std::memory_order_release)) {
        }

        dispatch_semaphore_signal(inFlight);
    }];

    [commands commit];
    s.m_commands = nil;
    s.m_visibility = nil;
    s.m_visibilityUsed = 0;
    this->m_serial++;

    if (wait) {
        [commands waitUntilCompleted];
        this->WaitForSerial(serial);
    }
}

void DeviceImpl::WaitForSerial(uint64_t serial) {
    while (this->CompletedSerial() < serial) {
        // Completion handlers run on a Metal thread shortly after the command buffer finishes
        usleep(50);
    }
}

// Uploads

UploadAllocation DeviceImpl::AllocateUpload(uint32_t size, uint32_t alignment) {
    return this->m_state->m_uploads.Allocate(std::max(size, 4u), alignment, this->m_serial, this->CompletedSerial());
}

id<MTLBlitCommandEncoder> DeviceImpl::InlineBlit() {
    ImplState& s = *this->m_state;

    if (!s.m_blit) {
        this->ExecutePendingClears();
        if (s.m_render) {
            [s.m_render endEncoding];
            s.m_render = nil;
        }
        s.m_blit = [this->Commands() blitCommandEncoder];
    }

    return s.m_blit;
}

void DeviceImpl::CopyBuffer(const UploadAllocation& source, id<MTLBuffer> dest, uint32_t destOffset, uint32_t size, GpuUse& use) {
    ImplState& s = *this->m_state;
    id<MTLBlitCommandEncoder> blit;

    if (use.m_lastRenderUse == this->m_serial) {
        blit = this->InlineBlit();
    } else {
        if (!s.m_uploadCommands) {
            s.m_uploadCommands = [this->m_queue commandBuffer];
            s.m_uploadCommands.label = @"D3D9 uploads";
        }
        if (!s.m_uploadEncoder) {
            s.m_uploadEncoder = [s.m_uploadCommands blitCommandEncoder];
        }
        blit = s.m_uploadEncoder;
    }

    [blit copyFromBuffer:source.m_buffer sourceOffset:source.m_offset toBuffer:dest destinationOffset:destOffset size:size];
    use.m_lastUse = this->m_serial;
}

void DeviceImpl::CopyToTexture(const UploadAllocation& source, uint32_t bytesPerRow, uint32_t bytesPerImage, id<MTLTexture> dest, uint32_t slice, uint32_t level, MTLOrigin origin, MTLSize size, GpuUse& use) {
    ImplState& s = *this->m_state;
    id<MTLBlitCommandEncoder> blit;

    if (use.m_lastRenderUse == this->m_serial) {
        blit = this->InlineBlit();
    } else {
        if (!s.m_uploadCommands) {
            s.m_uploadCommands = [this->m_queue commandBuffer];
            s.m_uploadCommands.label = @"D3D9 uploads";
        }
        if (!s.m_uploadEncoder) {
            s.m_uploadEncoder = [s.m_uploadCommands blitCommandEncoder];
        }
        blit = s.m_uploadEncoder;
    }

    [blit copyFromBuffer:source.m_buffer sourceOffset:source.m_offset sourceBytesPerRow:bytesPerRow sourceBytesPerImage:bytesPerImage sourceSize:size toTexture:dest destinationSlice:slice destinationLevel:level destinationOrigin:origin];
    use.m_lastUse = this->m_serial;
}

void DeviceImpl::ResolveSurface(SurfaceImpl* surface) {
    ImplState& s = *this->m_state;

    if (surface->m_samples <= 1 || !surface->m_resolveDirty || !surface->m_resolve) {
        return;
    }

    this->ExecutePendingClears();
    this->EndEncoders();

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = surface->m_texture;
    pass.colorAttachments[0].resolveTexture = surface->m_resolve;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    [[this->Commands() renderCommandEncoderWithDescriptor:pass] endEncoding];

    surface->m_resolveDirty = false;
    surface->Use().m_lastUse = this->m_serial;
    (void)s;
}

void DeviceImpl::ReadSurface(SurfaceImpl* surface, id<MTLBuffer> dest, uint32_t bytesPerRow) {
    id<MTLTexture> texture = surface->ResolvedTexture();
    id<MTLBlitCommandEncoder> blit = this->InlineBlit();
    uint32_t rows = surface->m_info.RowCount(surface->m_height);

    [blit copyFromTexture:texture sourceSlice:surface->m_face sourceLevel:surface->m_level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(surface->m_width, surface->m_height, 1) toBuffer:dest destinationOffset:0 destinationBytesPerRow:bytesPerRow destinationBytesPerImage:bytesPerRow * rows];

    this->Flush(true);
}

// Render passes

bool DeviceImpl::BeginPass() {
    ImplState& s = *this->m_state;

    if (s.m_render) {
        return true;
    }

    if (!s.m_renderTarget) {
        return false;
    }

    if (s.m_blit) {
        [s.m_blit endEncoding];
        s.m_blit = nil;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    SurfaceImpl* color = s.m_renderTarget;

    pass.colorAttachments[0].texture = color->m_texture;
    pass.colorAttachments[0].level = color->m_level;
    pass.colorAttachments[0].slice = color->m_face;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    if (s.m_pendingClearColor) {
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(s.m_clearColor[0], s.m_clearColor[1], s.m_clearColor[2], s.m_clearColor[3]);
    } else {
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }

    if (SurfaceImpl* depth = s.m_depthStencil) {
        pass.depthAttachment.texture = depth->m_texture;
        pass.depthAttachment.level = depth->m_level;
        pass.depthAttachment.slice = depth->m_face;
        pass.depthAttachment.storeAction = MTLStoreActionStore;

        if (s.m_pendingClearDepth) {
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = s.m_clearDepth;
        } else {
            pass.depthAttachment.loadAction = MTLLoadActionLoad;
        }

        depth->Use().m_lastUse = this->m_serial;
    }

    s.m_pendingClearColor = false;
    s.m_pendingClearDepth = false;

    if (!s.m_activeQueries.empty() || s.m_visibility) {
        if (!s.m_visibility) {
            s.m_visibility = [this->m_device newBufferWithLength:64 * 1024 options:MTLResourceStorageModeShared];
            s.m_visibilityUsed = 0;
        }
        pass.visibilityResultBuffer = s.m_visibility;
    }

    s.m_render = [this->Commands() renderCommandEncoderWithDescriptor:pass];
    if (!s.m_render) {
        return false;
    }

    color->Use().m_lastUse = this->m_serial;
    color->MarkRendered();

    s.m_bound = ImplState::Bound();
    s.m_vsConstantsDirty = true;
    s.m_psConstantsDirty = true;
    s.m_vertexStateDirty = true;
    s.m_pixelStateDirty = true;
    s.m_fixedVertexDirty = true;

    [s.m_render setFrontFacingWinding:MTLWindingClockwise];

    // The query still being built counts in this pass as well
    if (!s.m_activeQueries.empty()) {
        this->BeginOcclusionQuery(s.m_activeQueries.back());
    }

    return true;
}

void DeviceImpl::ExecutePendingClears() {
    ImplState& s = *this->m_state;

    if (!s.m_render && (s.m_pendingClearColor || s.m_pendingClearDepth)) {
        if (this->BeginPass()) {
            [s.m_render endEncoding];
            s.m_render = nil;
        }
    }
}

// External passes

bool DeviceImpl::AcquireExternalTargetsImpl(void** commandBuffer, void** color, void** depth, uint32_t* width, uint32_t* height, uint32_t* sampleCount) {
    ImplState& s = *this->m_state;
    SurfaceImpl* target = s.m_renderTarget;

    if (!target) {
        return false;
    }

    this->ExecutePendingClears();
    this->EndEncoders();

    // The external passes read and write both targets in this command buffer
    target->Use().m_lastUse = this->m_serial;
    target->MarkRendered();

    if (s.m_depthStencil) {
        s.m_depthStencil->Use().m_lastUse = this->m_serial;
    }

    *commandBuffer = (__bridge void*)this->Commands();
    *color = (__bridge void*)target->m_texture;
    *depth = s.m_depthStencil ? (__bridge void*)s.m_depthStencil->m_texture : nullptr;
    *width = target->m_width;
    *height = target->m_height;
    *sampleCount = static_cast<uint32_t>(target->m_texture.sampleCount);
    return true;
}

// Queries

void DeviceImpl::BeginOcclusionQuery(QueryImpl* query) {
    ImplState& s = *this->m_state;

    if (std::find(s.m_activeQueries.begin(), s.m_activeQueries.end(), query) == s.m_activeQueries.end()) {
        query->m_slots.clear();
        s.m_activeQueries.push_back(query);
    }

    // Counting needs a visibility buffer in the pass; start a pass that has one
    if (s.m_render && !s.m_visibility) {
        this->ExecutePendingClears();
        [s.m_render endEncoding];
        s.m_render = nil;
    }

    if (!s.m_render) {
        // The next pass begins counting for the query
        s.m_visibility = s.m_visibility ?: [this->m_device newBufferWithLength:64 * 1024 options:MTLResourceStorageModeShared];
        return;
    }

    if (s.m_visibilityUsed + 8 > s.m_visibility.length) {
        // Out of slots in this command buffer: continue in a new one
        this->Flush(false);
        s.m_visibility = [this->m_device newBufferWithLength:64 * 1024 options:MTLResourceStorageModeShared];
        return;
    }

    uint32_t offset = s.m_visibilityUsed;
    s.m_visibilityUsed += 8;
    memset(static_cast<uint8_t*>(s.m_visibility.contents) + offset, 0, 8);
    query->m_slots.push_back({ s.m_visibility, offset });
    [s.m_render setVisibilityResultMode:MTLVisibilityResultModeCounting offset:offset];
}

void DeviceImpl::EndOcclusionQuery(QueryImpl* query) {
    ImplState& s = *this->m_state;

    auto found = std::find(s.m_activeQueries.begin(), s.m_activeQueries.end(), query);
    if (found == s.m_activeQueries.end()) {
        return;
    }

    bool wasCounting = found + 1 == s.m_activeQueries.end();
    s.m_activeQueries.erase(found);

    if (s.m_render && wasCounting) {
        if (s.m_activeQueries.empty()) {
            [s.m_render setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        } else {
            // An enclosing query resumes counting
            this->BeginOcclusionQuery(s.m_activeQueries.back());
        }
    }
}

void DeviceImpl::QueryDestroyed(QueryImpl* query) {
    ImplState& s = *this->m_state;
    auto found = std::find(s.m_activeQueries.begin(), s.m_activeQueries.end(), query);
    if (found != s.m_activeQueries.end()) {
        this->EndOcclusionQuery(query);
    }
}

// Shader compilation

bool DeviceImpl::CompileFunction(const std::string& source, bool vertex, CompiledFunction& function) {
    ImplState& s = *this->m_state;

    auto cached = s.m_functions.find(source);
    if (cached != s.m_functions.end()) {
        function = cached->second;
        return function.m_function != nil;
    }

    MTLCompileOptions* options = [MTLCompileOptions new];
    options.preserveInvariance = YES;
    options.mathMode = MTLMathModeFast;

    NSError* error = nil;
    id<MTLLibrary> library = [this->m_device newLibraryWithSource:@(source.c_str()) options:options error:&error];
    function.m_function = [library newFunctionWithName:@(vertex ? kVertexFunctionName : kFragmentFunctionName)];
    function.m_id = this->NextId();

    if (!function.m_function) {
        Log("shader compilation failed: %s\n%s", error.localizedDescription.UTF8String, source.c_str());
    }

    s.m_functions[source] = function;
    return function.m_function != nil;
}

// Pipelines and fixed state objects

id<MTLRenderPipelineState> DeviceImpl::PipelineFor(const CompiledFunction& vertex, const CompiledFunction& fragment, VertexDeclarationImpl* declaration) {
    ImplState& s = *this->m_state;
    const uint32_t* rs = s.m_renderStates;
    SurfaceImpl* color = s.m_renderTarget;
    SurfaceImpl* depth = s.m_depthStencil;

    PipelineKey key;
    key.m_vertexFunction = vertex.m_id;
    key.m_fragmentFunction = fragment.m_id;
    key.m_declaration = declaration->m_id;
    for (uint32_t stream = 0; stream < 8; stream++) {
        if (declaration->m_streamMask & (1u << stream)) {
            key.m_strides[stream] = static_cast<uint16_t>(s.m_streams[stream].m_stride);
        }
    }
    key.m_colorFormat = static_cast<uint32_t>(color->m_info.m_pixelFormat);
    key.m_depthFormat = depth ? static_cast<uint32_t>(depth->m_info.m_pixelFormat) : 0;
    key.m_samples = static_cast<uint8_t>(color->m_samples);
    key.m_blendEnable = rs[D3DRS_ALPHABLENDENABLE] != 0;

    if (key.m_blendEnable) {
        uint32_t src = rs[D3DRS_SRCBLEND];
        uint32_t dst = rs[D3DRS_DESTBLEND];
        if (src == D3DBLEND_BOTHSRCALPHA) {
            src = D3DBLEND_SRCALPHA;
            dst = D3DBLEND_INVSRCALPHA;
        } else if (src == D3DBLEND_BOTHINVSRCALPHA) {
            src = D3DBLEND_INVSRCALPHA;
            dst = D3DBLEND_SRCALPHA;
        }
        key.m_srcBlend = static_cast<uint8_t>(src);
        key.m_dstBlend = static_cast<uint8_t>(dst);
        key.m_blendOp = static_cast<uint8_t>(rs[D3DRS_BLENDOP]);

        if (rs[D3DRS_SEPARATEALPHABLENDENABLE]) {
            key.m_srcBlendAlpha = static_cast<uint8_t>(rs[D3DRS_SRCBLENDALPHA]);
            key.m_dstBlendAlpha = static_cast<uint8_t>(rs[D3DRS_DESTBLENDALPHA]);
            key.m_blendOpAlpha = static_cast<uint8_t>(rs[D3DRS_BLENDOPALPHA]);
        } else {
            key.m_srcBlendAlpha = key.m_srcBlend;
            key.m_dstBlendAlpha = key.m_dstBlend;
            key.m_blendOpAlpha = key.m_blendOp;
        }
    }

    uint32_t writeMask = rs[D3DRS_COLORWRITEENABLE] & 0xF;
    // Formats without alpha (X8R8G8B8) keep their stored alpha at one
    if (color->m_format == D3DFMT_X8R8G8B8 || color->m_format == D3DFMT_X1R5G5B5 || color->m_format == D3DFMT_X8B8G8R8) {
        writeMask &= ~D3DCOLORWRITEENABLE_ALPHA;
    }
    key.m_writeMask = static_cast<uint8_t>(writeMask);

    auto found = s.m_pipelines.find(key);
    if (found != s.m_pipelines.end()) {
        return found->second;
    }

    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex.m_function;
    descriptor.fragmentFunction = fragment.m_function;
    descriptor.rasterSampleCount = color->m_samples;

    MTLVertexDescriptor* vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    for (const VertexDeclarationImpl::Element& element : declaration->m_mapped) {
        vertexDescriptor.attributes[element.m_attribute].format = element.m_format;
        vertexDescriptor.attributes[element.m_attribute].offset = element.m_offset;
        vertexDescriptor.attributes[element.m_attribute].bufferIndex = element.m_stream;
    }
    for (uint32_t stream = 0; stream < 16; stream++) {
        if (!(declaration->m_streamMask & (1u << stream))) {
            continue;
        }
        uint32_t stride = stream < 8 ? key.m_strides[stream] : s.m_streams[stream].m_stride;
        if (stride == 0) {
            vertexDescriptor.layouts[stream].stepFunction = MTLVertexStepFunctionConstant;
            vertexDescriptor.layouts[stream].stepRate = 0;
            vertexDescriptor.layouts[stream].stride = 16;
        } else {
            vertexDescriptor.layouts[stream].stride = stride;
        }
    }
    descriptor.vertexDescriptor = vertexDescriptor;

    MTLRenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[0];
    attachment.pixelFormat = color->m_info.m_pixelFormat;
    attachment.writeMask = ((writeMask & D3DCOLORWRITEENABLE_RED) ? MTLColorWriteMaskRed : 0) | ((writeMask & D3DCOLORWRITEENABLE_GREEN) ? MTLColorWriteMaskGreen : 0) | ((writeMask & D3DCOLORWRITEENABLE_BLUE) ? MTLColorWriteMaskBlue : 0) | ((writeMask & D3DCOLORWRITEENABLE_ALPHA) ? MTLColorWriteMaskAlpha : 0);

    if (key.m_blendEnable) {
        attachment.blendingEnabled = YES;
        attachment.sourceRGBBlendFactor = BlendFactor(key.m_srcBlend);
        attachment.destinationRGBBlendFactor = BlendFactor(key.m_dstBlend);
        attachment.rgbBlendOperation = BlendOperation(key.m_blendOp);
        attachment.sourceAlphaBlendFactor = BlendFactor(key.m_srcBlendAlpha);
        attachment.destinationAlphaBlendFactor = BlendFactor(key.m_dstBlendAlpha);
        attachment.alphaBlendOperation = BlendOperation(key.m_blendOpAlpha);
    }

    if (depth) {
        descriptor.depthAttachmentPixelFormat = depth->m_info.m_pixelFormat;
    }

    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [this->m_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) {
        Log("pipeline creation failed: %s", error.localizedDescription.UTF8String);
    }

    s.m_pipelines[key] = pipeline;
    return pipeline;
}

id<MTLDepthStencilState> DeviceImpl::DepthStencilFor(bool enable, bool write, uint32_t func) {
    ImplState& s = *this->m_state;
    uint32_t key = (enable ? 1u : 0u) | (write ? 2u : 0u) | (func << 2);

    auto found = s.m_depthStates.find(key);
    if (found != s.m_depthStates.end()) {
        return found->second;
    }

    MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
    // Direct3D 9 writes depth only while the depth test is enabled
    descriptor.depthCompareFunction = enable ? CompareFunction(func) : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = enable && write;

    id<MTLDepthStencilState> state = [this->m_device newDepthStencilStateWithDescriptor:descriptor];
    s.m_depthStates[key] = state;
    return state;
}

id<MTLSamplerState> DeviceImpl::SamplerFor(uint32_t sampler, bool compare) {
    ImplState& s = *this->m_state;
    const uint32_t* ss = s.m_samplerStates[sampler];

    SamplerKey key;
    key.m_addressU = static_cast<uint8_t>(ss[D3DSAMP_ADDRESSU]);
    key.m_addressV = static_cast<uint8_t>(ss[D3DSAMP_ADDRESSV]);
    key.m_addressW = static_cast<uint8_t>(ss[D3DSAMP_ADDRESSW]);
    key.m_magFilter = static_cast<uint8_t>(ss[D3DSAMP_MAGFILTER]);
    key.m_minFilter = static_cast<uint8_t>(ss[D3DSAMP_MINFILTER]);
    key.m_mipFilter = static_cast<uint8_t>(ss[D3DSAMP_MIPFILTER]);
    key.m_maxAnisotropy = static_cast<uint8_t>(std::clamp<uint32_t>(ss[D3DSAMP_MAXANISOTROPY], 1, 16));
    key.m_compare = compare ? 1 : 0;
    key.m_borderColor = ss[D3DSAMP_BORDERCOLOR];
    key.m_maxMipLevel = ss[D3DSAMP_MAXMIPLEVEL];

    auto found = s.m_samplers.find(key);
    if (found != s.m_samplers.end()) {
        return found->second;
    }

    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    descriptor.sAddressMode = AddressMode(key.m_addressU);
    descriptor.tAddressMode = AddressMode(key.m_addressV);
    descriptor.rAddressMode = AddressMode(key.m_addressW);
    descriptor.magFilter = key.m_magFilter == D3DTEXF_POINT || key.m_magFilter == D3DTEXF_NONE ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    descriptor.minFilter = key.m_minFilter == D3DTEXF_POINT || key.m_minFilter == D3DTEXF_NONE ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;

    switch (key.m_mipFilter) {
        case D3DTEXF_NONE: descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped; break;
        case D3DTEXF_POINT: descriptor.mipFilter = MTLSamplerMipFilterNearest; break;
        default: descriptor.mipFilter = MTLSamplerMipFilterLinear; break;
    }

    // Direct3D 9 filters anisotropically only with the anisotropic filter selected
    bool anisotropic = key.m_minFilter == D3DTEXF_ANISOTROPIC || key.m_magFilter == D3DTEXF_ANISOTROPIC;
    descriptor.maxAnisotropy = anisotropic ? key.m_maxAnisotropy : 1;
    descriptor.lodMinClamp = static_cast<float>(key.m_maxMipLevel);

    if (key.m_addressU == D3DTADDRESS_BORDER || key.m_addressV == D3DTADDRESS_BORDER || key.m_addressW == D3DTADDRESS_BORDER) {
        // Metal offers three border colors; pick the nearest
        uint32_t alpha = key.m_borderColor >> 24;
        uint32_t luminance = ((key.m_borderColor >> 16) & 0xFF) + ((key.m_borderColor >> 8) & 0xFF) + (key.m_borderColor & 0xFF);
        descriptor.borderColor = alpha < 128 ? MTLSamplerBorderColorTransparentBlack : luminance >= 384 ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
    }

    if (compare) {
        // Hardware shadow mapping: the texture coordinate's depth passes when not behind the stored one
        descriptor.compareFunction = MTLCompareFunctionLessEqual;
    }

    id<MTLSamplerState> state = [this->m_device newSamplerStateWithDescriptor:descriptor];
    s.m_samplers[key] = state;
    return state;
}

// Drawing

void DeviceImpl::UpdateFixedVertexState() {
    ImplState& s = *this->m_state;
    const uint32_t* rs = s.m_renderStates;

    FixedVertexStateBlock block;
    memset(&block, 0, sizeof(block));

    Matrix worldView = Multiply(s.m_world, s.m_view);
    Matrix normal = Transpose(Inverse(worldView));

    memcpy(block.m_world, s.m_world.data(), sizeof(block.m_world));
    memcpy(block.m_worldView, worldView.data(), sizeof(block.m_worldView));
    memcpy(block.m_normal, normal.data(), sizeof(block.m_normal));
    memcpy(block.m_projection, s.m_projection.data(), sizeof(block.m_projection));
    for (uint32_t i = 0; i < 8; i++) {
        memcpy(block.m_texture[i], s.m_textureTransforms[i].data(), sizeof(block.m_texture[i]));
    }

    DecodeColor(rs[D3DRS_AMBIENT], block.m_globalAmbient);

    const D3DMATERIAL9& material = s.m_material;
    memcpy(block.m_materialDiffuse, &material.Diffuse, 16);
    memcpy(block.m_materialAmbient, &material.Ambient, 16);
    memcpy(block.m_materialSpecular, &material.Specular, 16);
    memcpy(block.m_materialEmissive, &material.Emissive, 16);
    block.m_materialPower[0] = material.Power;

    // Lights are given in world space and lit in view space
    for (uint32_t i = 0; i < 4; i++) {
        const D3DLIGHT9& light = s.m_lights[i];
        FixedLightBlock& out = block.m_lights[i];
        memcpy(out.m_diffuse, &light.Diffuse, 16);
        memcpy(out.m_specular, &light.Specular, 16);
        memcpy(out.m_ambient, &light.Ambient, 16);

        float position[4] = { light.Position.x, light.Position.y, light.Position.z, 1.0f };
        float direction[4] = { light.Direction.x, light.Direction.y, light.Direction.z, 0.0f };
        TransformRow(position, s.m_view, out.m_position);
        TransformRow(direction, s.m_view, out.m_direction);

        out.m_range = light.Range;
        out.m_falloff = light.Falloff;
        out.m_attenuation[0] = light.Attenuation0;
        out.m_attenuation[1] = light.Attenuation1;
        out.m_attenuation[2] = light.Attenuation2;
        out.m_cosHalfTheta = cosf(light.Theta * 0.5f);
        out.m_cosHalfPhi = cosf(light.Phi * 0.5f);
        out.m_type = light.Type;
    }

    float fogStart = AsFloat(rs[D3DRS_FOGSTART]);
    float fogEnd = AsFloat(rs[D3DRS_FOGEND]);
    block.m_fog[0] = fogStart;
    block.m_fog[1] = fogEnd;
    block.m_fog[2] = fogEnd != fogStart ? 1.0f / (fogEnd - fogStart) : 0.0f;
    block.m_fog[3] = AsFloat(rs[D3DRS_FOGDENSITY]);

    block.m_pointScale[0] = AsFloat(rs[D3DRS_POINTSCALE_A]);
    block.m_pointScale[1] = AsFloat(rs[D3DRS_POINTSCALE_B]);
    block.m_pointScale[2] = AsFloat(rs[D3DRS_POINTSCALE_C]);
    block.m_pointScale[3] = static_cast<float>(s.m_viewport.Height);

    [s.m_render setVertexBytes:&block length:sizeof(block) atIndex:VS_BUFFER_FIXED_FUNCTION];
}

void DeviceImpl::BindTextures(const ShaderInfo* pixelInfo, uint32_t samplerMask) {
    ImplState& s = *this->m_state;

    for (uint32_t i = 0; i < 16; i++) {
        if (!(samplerMask & (1u << i))) {
            continue;
        }

        TextureCore* core = GetTextureCore(s.m_textures[i]);

        // The kind the fragment function was built for
        SamplerKind expected = SAMPLERKIND_2D;
        if (pixelInfo && pixelInfo->m_major >= 2) {
            expected = static_cast<SamplerKind>((pixelInfo->m_declaredKinds >> (i * 2)) & 3);
            if (expected == SAMPLERKIND_2D && core && core->Kind() == SAMPLERKIND_DEPTH) {
                expected = SAMPLERKIND_DEPTH;
            }
        } else if (core) {
            expected = core->Kind();
        }

        id<MTLTexture> texture;
        if (core && core->Kind() == expected) {
            texture = core->SampleTexture();
            core->m_use.m_lastUse = this->m_serial;
            core->m_use.m_lastRenderUse = this->m_serial;
        } else {
            texture = expected == SAMPLERKIND_CUBE ? s.m_dummyCube : expected == SAMPLERKIND_DEPTH ? s.m_dummyDepth : s.m_dummy2d;
        }

        id<MTLSamplerState> sampler = this->SamplerFor(i, expected == SAMPLERKIND_DEPTH);

        if (s.m_bound.m_textures[i] != texture) {
            [s.m_render setFragmentTexture:texture atIndex:i];
            s.m_bound.m_textures[i] = texture;
        }
        if (s.m_bound.m_samplers[i] != sampler) {
            [s.m_render setFragmentSamplerState:sampler atIndex:i];
            s.m_bound.m_samplers[i] = sampler;
        }
    }
}

bool DeviceImpl::PrepareDraw(D3DPRIMITIVETYPE type) {
    ImplState& s = *this->m_state;
    const uint32_t* rs = s.m_renderStates;
    VertexDeclarationImpl* declaration = s.m_declaration;

    if (!s.m_renderTarget || !declaration) {
        return false;
    }

    // Mipmaps generated from levels rendered or written since they were last made
    for (uint32_t i = 0; i < 16; i++) {
        TextureCore* core = GetTextureCore(s.m_textures[i]);
        if (core && core->m_mipsDirty && core->m_levels > 1) {
            [this->InlineBlit() generateMipmapsForTexture:core->m_texture];
            core->m_mipsDirty = false;
            core->m_use.m_lastUse = this->m_serial;
        }
    }

    if (!this->BeginPass()) {
        return false;
    }

    bool points = type == D3DPT_POINTLIST;
    bool pointSprite = points && rs[D3DRS_POINTSPRITEENABLE];

    // Vertex function
    CompiledFunction vertex;
    if (VertexShaderImpl* shader = s.m_vertexShader) {
        bool found = false;
        for (auto& entry : shader->m_functions) {
            if (entry.first == declaration->m_signature) {
                vertex = entry.second;
                found = true;
                break;
            }
        }
        if (!found) {
            std::string source;
            std::string error;
            if (!TranslateVertexShader(shader->m_program, declaration->m_signature, source, error)) {
                Log("vertex shader translation failed: %s", error.c_str());
            } else {
                this->CompileFunction(source, true, vertex);
            }
            shader->m_functions.emplace_back(declaration->m_signature, vertex);
        }
    } else {
        FixedVertexKey key;
        for (uint32_t i = 0; i < ATTRIB_COUNT; i++) {
            key.m_attributes[i] = declaration->m_signature.m_class[i];
        }
        memcpy(key.m_texcoordComponents, declaration->m_texcoordComponents, 8);
        key.m_lighting = rs[D3DRS_LIGHTING] != 0;
        if (key.m_lighting) {
            for (uint32_t i = 0; i < 4; i++) {
                key.m_lightTypes[i] = s.m_lightEnabled[i] ? static_cast<uint8_t>(s.m_lights[i].Type) : 0;
            }
            bool colorVertex = rs[D3DRS_COLORVERTEX] != 0;
            auto resolve = [&](uint32_t source) -> uint8_t {
                if (!colorVertex) return D3DMCS_MATERIAL;
                if (source == D3DMCS_COLOR1 && !key.m_attributes[ATTRIB_COLOR0]) return D3DMCS_MATERIAL;
                if (source == D3DMCS_COLOR2 && !key.m_attributes[ATTRIB_COLOR1]) return D3DMCS_MATERIAL;
                return static_cast<uint8_t>(source);
            };
            key.m_diffuseSource = resolve(rs[D3DRS_DIFFUSEMATERIALSOURCE]);
            key.m_ambientSource = resolve(rs[D3DRS_AMBIENTMATERIALSOURCE]);
            key.m_specularSource = resolve(rs[D3DRS_SPECULARMATERIALSOURCE]);
            key.m_emissiveSource = resolve(rs[D3DRS_EMISSIVEMATERIALSOURCE]);
            key.m_localViewer = rs[D3DRS_LOCALVIEWER] != 0;
        }
        key.m_normalizeNormals = rs[D3DRS_NORMALIZENORMALS] != 0;
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t index = s.m_stageStates[i][D3DTSS_TEXCOORDINDEX];
            key.m_texcoordIndex[i] = static_cast<uint8_t>(index & 7);
            key.m_texgen[i] = static_cast<uint8_t>((index >> 16) & 0xF);
            key.m_transformCount[i] = static_cast<uint8_t>(s.m_stageStates[i][D3DTSS_TEXTURETRANSFORMFLAGS] & 7);
        }
        if (rs[D3DRS_FOGENABLE]) {
            uint32_t mode = rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE ? rs[D3DRS_FOGTABLEMODE] : rs[D3DRS_FOGVERTEXMODE];
            key.m_fogMode = static_cast<uint8_t>(mode + 1);
            key.m_rangeFog = rs[D3DRS_RANGEFOGENABLE] != 0;
        }
        key.m_pointScale = points && rs[D3DRS_POINTSCALEENABLE];

        auto found = s.m_fixedVertex.find(key);
        if (found != s.m_fixedVertex.end()) {
            vertex = found->second;
        } else {
            std::string source;
            std::string error;
            if (GenerateFixedVertexShader(key, source, error)) {
                this->CompileFunction(source, true, vertex);
            } else {
                Log("fixed-function vertex shader generation failed: %s", error.c_str());
            }
            s.m_fixedVertex[key] = vertex;
        }
    }

    // Fragment function
    CompiledFunction fragment;
    uint32_t samplerMask = 0;
    const ShaderInfo* pixelInfo = nullptr;

    if (PixelShaderImpl* shader = s.m_pixelShader) {
        pixelInfo = &shader->m_info;
        samplerMask = shader->m_info.m_samplerMask;

        PixelShaderVariant variant;
        for (uint32_t i = 0; i < 16; i++) {
            if (!(samplerMask & (1u << i))) {
                continue;
            }
            TextureCore* core = GetTextureCore(s.m_textures[i]);
            uint32_t kind;
            if (shader->m_info.m_major >= 2) {
                kind = (shader->m_info.m_declaredKinds >> (i * 2)) & 3;
                if (kind == SAMPLERKIND_2D && core && core->Kind() == SAMPLERKIND_DEPTH) {
                    kind = SAMPLERKIND_DEPTH;
                }
            } else {
                kind = core ? core->Kind() : SAMPLERKIND_2D;
            }
            variant.m_samplerKinds |= kind << (i * 2);
        }

        if (shader->m_info.m_major == 1 && shader->m_info.m_minor < 4) {
            for (uint32_t stage = 0; stage < 4; stage++) {
                uint32_t flags = s.m_stageStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS];
                if (flags & D3DTTFF_PROJECTED) {
                    uint32_t count = flags & 7;
                    variant.m_projected |= static_cast<uint16_t>((count ? count : 4) << (stage * 3));
                }
            }
        }

        variant.m_pointSprite = pointSprite;

        bool found = false;
        for (auto& entry : shader->m_functions) {
            if (entry.first == variant) {
                fragment = entry.second;
                found = true;
                break;
            }
        }
        if (!found) {
            std::string source;
            std::string error;
            if (!TranslatePixelShader(shader->m_program, variant, source, error)) {
                Log("pixel shader translation failed: %s", error.c_str());
            } else {
                this->CompileFunction(source, false, fragment);
            }
            shader->m_functions.emplace_back(variant, fragment);
        }
    } else {
        FixedPixelKey key;
        for (uint32_t i = 0; i < 8; i++) {
            const uint32_t* tss = s.m_stageStates[i];
            FixedPixelStage& stage = key.m_stages[i];

            if (tss[D3DTSS_COLOROP] == D3DTOP_DISABLE) {
                stage.m_colorOp = D3DTOP_DISABLE;
                break;
            }

            TextureCore* core = GetTextureCore(s.m_textures[i]);
            stage.m_colorOp = static_cast<uint8_t>(tss[D3DTSS_COLOROP]);
            stage.m_colorArg[0] = static_cast<uint8_t>(tss[D3DTSS_COLORARG0]);
            stage.m_colorArg[1] = static_cast<uint8_t>(tss[D3DTSS_COLORARG1]);
            stage.m_colorArg[2] = static_cast<uint8_t>(tss[D3DTSS_COLORARG2]);
            stage.m_alphaOp = static_cast<uint8_t>(tss[D3DTSS_ALPHAOP]);
            stage.m_alphaArg[0] = static_cast<uint8_t>(tss[D3DTSS_ALPHAARG0]);
            stage.m_alphaArg[1] = static_cast<uint8_t>(tss[D3DTSS_ALPHAARG1]);
            stage.m_alphaArg[2] = static_cast<uint8_t>(tss[D3DTSS_ALPHAARG2]);
            stage.m_resultTemp = (tss[D3DTSS_RESULTARG] & D3DTA_SELECTMASK) == D3DTA_TEMP;
            stage.m_textureBound = core != nullptr;
            stage.m_samplerKind = static_cast<uint8_t>(core ? core->Kind() : SAMPLERKIND_2D);

            uint32_t flags = tss[D3DTSS_TEXTURETRANSFORMFLAGS];
            if (flags & D3DTTFF_PROJECTED) {
                uint32_t count = flags & 7;
                stage.m_projected = static_cast<uint8_t>(count ? count : 4);
            }

            if (core) {
                samplerMask |= 1u << i;
            }
        }
        key.m_specularEnable = rs[D3DRS_SPECULARENABLE] != 0;
        key.m_pointSprite = pointSprite;

        auto found = s.m_fixedPixel.find(key);
        if (found != s.m_fixedPixel.end()) {
            fragment = found->second;
        } else {
            std::string source;
            std::string error;
            if (GenerateFixedPixelShader(key, source, error)) {
                this->CompileFunction(source, false, fragment);
            } else {
                Log("fixed-function pixel shader generation failed: %s", error.c_str());
            }
            s.m_fixedPixel[key] = fragment;
        }
    }

    if (!vertex.m_function || !fragment.m_function) {
        return false;
    }

    id<MTLRenderPipelineState> pipeline = this->PipelineFor(vertex, fragment, declaration);
    if (!pipeline) {
        return false;
    }

    id<MTLRenderCommandEncoder> encoder = s.m_render;

    if (s.m_bound.m_pipeline != pipeline) {
        [encoder setRenderPipelineState:pipeline];
        s.m_bound.m_pipeline = pipeline;
    }

    bool shaderChanged = s.m_bound.m_vertexFunction != vertex.m_id;
    bool pixelShaderChanged = s.m_bound.m_fragmentFunction != fragment.m_id;
    s.m_bound.m_vertexFunction = vertex.m_id;
    s.m_bound.m_fragmentFunction = fragment.m_id;

    // Depth
    if (s.m_depthStencil) {
        id<MTLDepthStencilState> depth = this->DepthStencilFor(rs[D3DRS_ZENABLE] != D3DZB_FALSE, rs[D3DRS_ZWRITEENABLE] != 0, rs[D3DRS_ZFUNC]);
        if (s.m_bound.m_depthStencil != depth) {
            [encoder setDepthStencilState:depth];
            s.m_bound.m_depthStencil = depth;
        }
    }

    // Rasterizer: the front face is clockwise, D3DCULL_CCW culls the back
    int32_t cull = rs[D3DRS_CULLMODE] == D3DCULL_CW ? MTLCullModeFront : rs[D3DRS_CULLMODE] == D3DCULL_CCW ? MTLCullModeBack : MTLCullModeNone;
    if (s.m_bound.m_cull != cull) {
        [encoder setCullMode:static_cast<MTLCullMode>(cull)];
        s.m_bound.m_cull = cull;
    }

    int32_t fill = rs[D3DRS_FILLMODE] == D3DFILL_SOLID ? MTLTriangleFillModeFill : MTLTriangleFillModeLines;
    if (s.m_bound.m_fill != fill) {
        [encoder setTriangleFillMode:static_cast<MTLTriangleFillMode>(fill)];
        s.m_bound.m_fill = fill;
    }

    float slope = AsFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    if (!(s.m_bound.m_slopeBias == slope)) {
        [encoder setDepthBias:0.0f slopeScale:slope clamp:0.0f];
        s.m_bound.m_slopeBias = slope;
    }

    uint32_t blendFactor = rs[D3DRS_BLENDFACTOR];
    if (!s.m_bound.m_blendFactorSet || s.m_bound.m_blendFactor != blendFactor) {
        float rgba[4];
        DecodeColor(blendFactor, rgba);
        [encoder setBlendColorRed:rgba[0] green:rgba[1] blue:rgba[2] alpha:rgba[3]];
        s.m_bound.m_blendFactor = blendFactor;
        s.m_bound.m_blendFactorSet = true;
    }

    // Viewport and scissor, both limited to the render target
    uint32_t targetWidth = s.m_renderTarget->m_width;
    uint32_t targetHeight = s.m_renderTarget->m_height;
    const D3DVIEWPORT9& vp = s.m_viewport;

    MTLViewport viewport;
    viewport.originX = vp.X + kPixelCenterOffset;
    viewport.originY = vp.Y + kPixelCenterOffset;
    viewport.width = vp.Width;
    viewport.height = vp.Height;
    float zBias = vp.MinZ >= 0.5f ? 0.0f : 0.001f;
    viewport.znear = std::clamp(vp.MinZ, 0.0f, 1.0f);
    viewport.zfar = std::clamp(std::max(vp.MaxZ, vp.MinZ + zBias), 0.0f, 1.0f);

    int32_t x0 = static_cast<int32_t>(vp.X);
    int32_t y0 = static_cast<int32_t>(vp.Y);
    int32_t x1 = static_cast<int32_t>(vp.X + vp.Width);
    int32_t y1 = static_cast<int32_t>(vp.Y + vp.Height);
    if (rs[D3DRS_SCISSORTESTENABLE]) {
        x0 = std::max(x0, s.m_scissor.left);
        y0 = std::max(y0, s.m_scissor.top);
        x1 = std::min(x1, s.m_scissor.right);
        y1 = std::min(y1, s.m_scissor.bottom);
    }
    x0 = std::clamp<int32_t>(x0, 0, targetWidth);
    y0 = std::clamp<int32_t>(y0, 0, targetHeight);
    x1 = std::clamp<int32_t>(x1, x0, targetWidth);
    y1 = std::clamp<int32_t>(y1, y0, targetHeight);

    if (x1 <= x0 || y1 <= y0) {
        // Nothing can be drawn
        return false;
    }

    MTLScissorRect scissor = { static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0), static_cast<NSUInteger>(x1 - x0), static_cast<NSUInteger>(y1 - y0) };

    if (!s.m_bound.m_viewportSet || memcmp(&s.m_bound.m_viewport, &viewport, sizeof(viewport)) != 0) {
        [encoder setViewport:viewport];
        s.m_bound.m_viewport = viewport;
        s.m_vertexStateDirty = true;
        s.m_fixedVertexDirty = true;
    }
    if (!s.m_bound.m_viewportSet || memcmp(&s.m_bound.m_scissor, &scissor, sizeof(scissor)) != 0) {
        [encoder setScissorRect:scissor];
        s.m_bound.m_scissor = scissor;
    }
    s.m_bound.m_viewportSet = true;

    // Vertex streams
    for (uint32_t stream = 0; stream < 16; stream++) {
        if (!(declaration->m_streamMask & (1u << stream))) {
            continue;
        }
        const ImplState::Stream& binding = s.m_streams[stream];
        if (!binding.m_buffer || !binding.m_buffer->m_core.Valid()) {
            return false;
        }
        BufferCore& core = binding.m_buffer->m_core;
        core.m_use.m_lastUse = this->m_serial;
        core.m_use.m_lastRenderUse = this->m_serial;

        if (s.m_bound.m_vertexBuffers[stream] != core.m_buffer) {
            [encoder setVertexBuffer:core.m_buffer offset:binding.m_offset atIndex:stream];
            s.m_bound.m_vertexBuffers[stream] = core.m_buffer;
            s.m_bound.m_vertexOffsets[stream] = binding.m_offset;
        } else if (s.m_bound.m_vertexOffsets[stream] != binding.m_offset) {
            [encoder setVertexBufferOffset:binding.m_offset atIndex:stream];
            s.m_bound.m_vertexOffsets[stream] = binding.m_offset;
        }
    }

    // Vertex constants
    if (s.m_vsConstantsDirty || shaderChanged) {
        uint32_t count = s.m_vertexShader ? std::max(1u, s.m_vertexShader->m_info.m_constantCount) : 1;
        [encoder setVertexBytes:s.m_vsConstants length:count * 16 atIndex:VS_BUFFER_FLOAT_CONSTANTS];
        [encoder setVertexBytes:s.m_vsIntConstants length:sizeof(s.m_vsIntConstants) atIndex:VS_BUFFER_INT_CONSTANTS];
        [encoder setVertexBytes:s.m_vsBoolConstants length:sizeof(s.m_vsBoolConstants) atIndex:VS_BUFFER_BOOL_CONSTANTS];
        s.m_vsConstantsDirty = false;
    }

    if (s.m_vertexStateDirty) {
        VertexStateBlock block;
        memset(&block, 0, sizeof(block));
        // Depth bias in Direct3D 9 is an offset in post-divide depth units
        block.m_posFixup[2] = AsFloat(rs[D3DRS_DEPTHBIAS]);
        memcpy(block.m_clipPlanes, s.m_clipPlanes, sizeof(block.m_clipPlanes));
        block.m_flags[0] = rs[D3DRS_CLIPPLANEENABLE] & 0x3F;
        block.m_pointSize[0] = AsFloat(rs[D3DRS_POINTSIZE]);
        block.m_pointSize[1] = std::max(AsFloat(rs[D3DRS_POINTSIZE_MIN]), 1.0f);
        block.m_pointSize[2] = std::min(AsFloat(rs[D3DRS_POINTSIZE_MAX]), this->m_caps.MaxPointSize);
        [encoder setVertexBytes:&block length:sizeof(block) atIndex:VS_BUFFER_STATE];
        s.m_vertexStateDirty = false;
    }

    if (!s.m_vertexShader && (s.m_fixedVertexDirty || shaderChanged)) {
        this->UpdateFixedVertexState();
        s.m_fixedVertexDirty = false;
    }

    // Pixel constants and state
    if (s.m_pixelShader && (s.m_psConstantsDirty || pixelShaderChanged)) {
        uint32_t count = std::max(1u, s.m_pixelShader->m_info.m_constantCount);
        [encoder setFragmentBytes:s.m_psConstants length:count * 16 atIndex:PS_BUFFER_FLOAT_CONSTANTS];
        [encoder setFragmentBytes:s.m_psIntConstants length:sizeof(s.m_psIntConstants) atIndex:PS_BUFFER_INT_CONSTANTS];
        [encoder setFragmentBytes:s.m_psBoolConstants length:sizeof(s.m_psBoolConstants) atIndex:PS_BUFFER_BOOL_CONSTANTS];
        s.m_psConstantsDirty = false;
    }

    if (s.m_pixelStateDirty || pixelShaderChanged) {
        PixelStateBlock block;
        memset(&block, 0, sizeof(block));
        DecodeColor(rs[D3DRS_FOGCOLOR], block.m_fogColor);
        // Fixed-function fog follows pixel shaders before shader model 3
        bool fog = rs[D3DRS_FOGENABLE] && (!s.m_pixelShader || s.m_pixelShader->m_info.m_major < 3);
        block.m_flags[0] = fog ? 1 : 0;
        block.m_flags[1] = rs[D3DRS_ALPHATESTENABLE] ? rs[D3DRS_ALPHAFUNC] : D3DCMP_ALWAYS;
        block.m_flags[2] = rs[D3DRS_ALPHAREF] & 0xFF;
        for (uint32_t stage = 0; stage < 8; stage++) {
            const uint32_t* tss = s.m_stageStates[stage];
            block.m_bumpEnvMat[stage][0] = AsFloat(tss[D3DTSS_BUMPENVMAT00]);
            block.m_bumpEnvMat[stage][1] = AsFloat(tss[D3DTSS_BUMPENVMAT01]);
            block.m_bumpEnvMat[stage][2] = AsFloat(tss[D3DTSS_BUMPENVMAT10]);
            block.m_bumpEnvMat[stage][3] = AsFloat(tss[D3DTSS_BUMPENVMAT11]);
            block.m_bumpEnvLum[stage][0] = AsFloat(tss[D3DTSS_BUMPENVLSCALE]);
            block.m_bumpEnvLum[stage][1] = AsFloat(tss[D3DTSS_BUMPENVLOFFSET]);
        }
        DecodeColor(rs[D3DRS_TEXTUREFACTOR], block.m_textureFactor);
        [encoder setFragmentBytes:&block length:sizeof(block) atIndex:PS_BUFFER_STATE];
        s.m_pixelStateDirty = false;
    }

    this->BindTextures(pixelInfo, samplerMask);

    s.m_renderTarget->MarkRendered();
    return true;
}

HRESULT DeviceImpl::DrawImpl(D3DPRIMITIVETYPE type, bool indexed, int32_t baseVertex, uint32_t start, uint32_t primitiveCount) {
    ImplState& s = *this->m_state;

    if (primitiveCount == 0) {
        return D3D_OK;
    }

    uint32_t count;
    MTLPrimitiveType primitive;
    switch (type) {
        case D3DPT_POINTLIST: count = primitiveCount; primitive = MTLPrimitiveTypePoint; break;
        case D3DPT_LINELIST: count = primitiveCount * 2; primitive = MTLPrimitiveTypeLine; break;
        case D3DPT_LINESTRIP: count = primitiveCount + 1; primitive = MTLPrimitiveTypeLineStrip; break;
        case D3DPT_TRIANGLELIST: count = primitiveCount * 3; primitive = MTLPrimitiveTypeTriangle; break;
        case D3DPT_TRIANGLESTRIP: count = primitiveCount + 2; primitive = MTLPrimitiveTypeTriangleStrip; break;
        case D3DPT_TRIANGLEFAN: count = primitiveCount + 2; primitive = MTLPrimitiveTypeTriangle; break;
        default: return D3DERR_INVALIDCALL;
    }

    if (indexed && (!s.m_indices || !s.m_indices->m_core.Valid())) {
        return D3DERR_INVALIDCALL;
    }

    if (!this->PrepareDraw(type)) {
        return D3D_OK;
    }

    id<MTLRenderCommandEncoder> encoder = s.m_render;

    if (type == D3DPT_TRIANGLEFAN) {
        // Metal has no fans: draw the equivalent triangle list
        UploadAllocation fan = this->AllocateUpload(primitiveCount * 3 * 4, 4);
        uint32_t* indices = reinterpret_cast<uint32_t*>(fan.m_data);

        if (indexed) {
            BufferCore& core = s.m_indices->m_core;
            const uint16_t* source = reinterpret_cast<const uint16_t*>(core.m_shadow.data());
            uint32_t available = core.m_length / 2;
            for (uint32_t i = 0; i < primitiveCount; i++) {
                uint32_t a = start, b = start + i + 1, c = start + i + 2;
                indices[i * 3 + 0] = a < available ? source[a] : 0;
                indices[i * 3 + 1] = b < available ? source[b] : 0;
                indices[i * 3 + 2] = c < available ? source[c] : 0;
            }
            [encoder drawIndexedPrimitives:primitive indexCount:primitiveCount * 3 indexType:MTLIndexTypeUInt32 indexBuffer:fan.m_buffer indexBufferOffset:fan.m_offset instanceCount:1 baseVertex:baseVertex baseInstance:0];
        } else {
            for (uint32_t i = 0; i < primitiveCount; i++) {
                indices[i * 3 + 0] = start;
                indices[i * 3 + 1] = start + i + 1;
                indices[i * 3 + 2] = start + i + 2;
            }
            [encoder drawIndexedPrimitives:primitive indexCount:primitiveCount * 3 indexType:MTLIndexTypeUInt32 indexBuffer:fan.m_buffer indexBufferOffset:fan.m_offset];
        }
        return D3D_OK;
    }

    if (indexed) {
        BufferCore& core = s.m_indices->m_core;
        core.m_use.m_lastUse = this->m_serial;
        core.m_use.m_lastRenderUse = this->m_serial;
        [encoder drawIndexedPrimitives:primitive indexCount:count indexType:MTLIndexTypeUInt16 indexBuffer:core.m_buffer indexBufferOffset:start * 2 instanceCount:1 baseVertex:baseVertex baseInstance:0];
    } else {
        [encoder drawPrimitives:primitive vertexStart:start vertexCount:count];
    }

    return D3D_OK;
}

// Clears

void DeviceImpl::ClearWithQuad(const Rect& rect, bool color, bool depth, const float* rgba, float z) {
    ImplState& s = *this->m_state;

    if (!this->BeginPass()) {
        return;
    }

    SurfaceImpl* target = s.m_renderTarget;
    uint64_t key = static_cast<uint64_t>(target->m_info.m_pixelFormat) | (static_cast<uint64_t>(s.m_depthStencil ? s.m_depthStencil->m_info.m_pixelFormat : 0) << 16) | (static_cast<uint64_t>(target->m_samples) << 32) | (color ? (1ull << 40) : 0) | (1ull << 48);

    id<MTLRenderPipelineState> pipeline;
    auto found = s.m_utilityPipelines.find(key);
    if (found != s.m_utilityPipelines.end()) {
        pipeline = found->second;
    } else {
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [s.m_utilityLibrary newFunctionWithName:@"utility_vertex"];
        descriptor.fragmentFunction = [s.m_utilityLibrary newFunctionWithName:@"utility_clear"];
        descriptor.colorAttachments[0].pixelFormat = target->m_info.m_pixelFormat;
        descriptor.colorAttachments[0].writeMask = color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        descriptor.depthAttachmentPixelFormat = s.m_depthStencil ? s.m_depthStencil->m_info.m_pixelFormat : MTLPixelFormatInvalid;
        descriptor.rasterSampleCount = target->m_samples;
        NSError* error = nil;
        pipeline = [this->m_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) {
            Log("clear pipeline creation failed: %s", error.localizedDescription.UTF8String);
        }
        s.m_utilityPipelines[key] = pipeline;
    }

    if (!pipeline) {
        return;
    }

    id<MTLRenderCommandEncoder> encoder = s.m_render;
    MTLViewport viewport = { 0.0, 0.0, static_cast<double>(target->m_width), static_cast<double>(target->m_height), 0.0, 1.0 };
    MTLScissorRect scissor = { static_cast<NSUInteger>(rect.left), static_cast<NSUInteger>(rect.top), static_cast<NSUInteger>(rect.right - rect.left), static_cast<NSUInteger>(rect.bottom - rect.top) };

    [encoder setRenderPipelineState:pipeline];
    if (s.m_depthStencil) {
        [encoder setDepthStencilState:depth ? s.m_clearDepthWrite : s.m_clearDepthKeep];
    }
    [encoder setCullMode:MTLCullModeNone];
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];

    float clipRect[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    float color4[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
    [encoder setVertexBytes:clipRect length:sizeof(clipRect) atIndex:0];
    [encoder setVertexBytes:&z length:sizeof(z) atIndex:1];
    [encoder setFragmentBytes:color4 length:sizeof(color4) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

    // The draw replaced the state the next draw expects
    s.m_bound = ImplState::Bound();
    s.m_vsConstantsDirty = true;
    s.m_psConstantsDirty = true;
    s.m_vertexStateDirty = true;
    s.m_pixelStateDirty = true;
    s.m_fixedVertexDirty = true;
}

HRESULT DeviceImpl::ClearImpl(uint32_t count, const D3DRECT* rects, uint32_t flags, uint32_t color, float z) {
    ImplState& s = *this->m_state;

    if (!s.m_renderTarget) {
        return D3DERR_INVALIDCALL;
    }

    bool clearColor = (flags & D3DCLEAR_TARGET) != 0;
    bool clearDepth = (flags & D3DCLEAR_ZBUFFER) && s.m_depthStencil;
    if (!clearColor && !clearDepth) {
        return D3D_OK;
    }

    // The cleared area is the viewport, limited by the scissor rectangle when enabled
    const D3DVIEWPORT9& vp = s.m_viewport;
    Rect area = { static_cast<int32_t>(vp.X), static_cast<int32_t>(vp.Y), static_cast<int32_t>(vp.X + vp.Width), static_cast<int32_t>(vp.Y + vp.Height) };
    if (s.m_renderStates[D3DRS_SCISSORTESTENABLE]) {
        area.left = std::max(area.left, s.m_scissor.left);
        area.top = std::max(area.top, s.m_scissor.top);
        area.right = std::min(area.right, s.m_scissor.right);
        area.bottom = std::min(area.bottom, s.m_scissor.bottom);
    }

    std::vector<Rect> areas;
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            Rect r = { std::max(area.left, rects[i].left), std::max(area.top, rects[i].top), std::min(area.right, rects[i].right), std::min(area.bottom, rects[i].bottom) };
            areas.push_back(r);
        }
    } else {
        areas.push_back(area);
    }

    float rgba[4];
    DecodeColor(color, rgba);
    // Formats without alpha read their alpha as one
    if (s.m_renderTarget->m_format == D3DFMT_X8R8G8B8 || s.m_renderTarget->m_format == D3DFMT_X8B8G8R8 || s.m_renderTarget->m_format == D3DFMT_X1R5G5B5) {
        rgba[3] = 1.0f;
    }

    int32_t width = static_cast<int32_t>(s.m_renderTarget->m_width);
    int32_t height = static_cast<int32_t>(s.m_renderTarget->m_height);

    for (Rect r : areas) {
        bool fullColor = r.left <= 0 && r.top <= 0 && r.right >= width && r.bottom >= height;
        bool fullDepth = !clearDepth || (r.left <= 0 && r.top <= 0 && r.right >= static_cast<int32_t>(s.m_depthStencil->m_width) && r.bottom >= static_cast<int32_t>(s.m_depthStencil->m_height));

        if (!s.m_render && fullColor && fullDepth) {
            // Clear when the next pass loads its attachments
            if (clearColor) {
                s.m_pendingClearColor = true;
                memcpy(s.m_clearColor, rgba, sizeof(rgba));
            }
            if (clearDepth) {
                s.m_pendingClearDepth = true;
                s.m_clearDepth = std::clamp(z, 0.0f, 1.0f);
            }
            continue;
        }

        r.left = std::clamp(r.left, 0, width);
        r.top = std::clamp(r.top, 0, height);
        r.right = std::clamp(r.right, r.left, width);
        r.bottom = std::clamp(r.bottom, r.top, height);
        if (r.right > r.left && r.bottom > r.top) {
            this->ClearWithQuad(r, clearColor, clearDepth, rgba, std::clamp(z, 0.0f, 1.0f));
        }
    }

    return D3D_OK;
}

// Swap chain

HRESULT DeviceImpl::TestCooperativeLevelImpl() {
    return this->m_needsReset ? D3DERR_DEVICENOTRESET : D3D_OK;
}

HRESULT DeviceImpl::ResetImpl(const PresentParameters& parameters) {
    ImplState& s = *this->m_state;

    this->Flush(true);

    PresentParameters previous = this->m_parameters;
    this->m_parameters = parameters;
    if (!parameters.Layer) {
        this->m_parameters.Layer = previous.Layer;
    }

    if (this->m_parameters.Layer != previous.Layer) {
        s.m_layer = (__bridge CAMetalLayer*)this->m_parameters.Layer;
        s.m_layer.device = this->m_device;
        s.m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        s.m_layer.framebufferOnly = NO;
        s.m_layer.maximumDrawableCount = 3;
    }

    // Unbind the old swap chain before replacing it
    this->SetRenderTargetImpl(0, nullptr);
    this->SetDepthStencilSurfaceImpl(nullptr);

    if (!this->CreateSwapChainResources()) {
        this->m_needsReset = true;
        return D3DERR_INVALIDCALL;
    }

    this->m_needsReset = false;
    this->ResetState();
    return D3D_OK;
}

HRESULT DeviceImpl::GetBackBufferImpl(Surface** surface) {
    ImplState& s = *this->m_state;
    if (!surface || !s.m_backBuffer) {
        return D3DERR_INVALIDCALL;
    }
    s.m_backBuffer->AddRef();
    *surface = s.m_backBuffer;
    return D3D_OK;
}

void DeviceImpl::SetGammaRampImpl(const D3DGAMMARAMP* ramp) {
    ImplState& s = *this->m_state;
    if (!ramp) {
        return;
    }

    bool identity = true;
    for (uint32_t i = 0; i < 256 && identity; i++) {
        uint16_t expected = static_cast<uint16_t>(i * 257);
        identity = ramp->red[i] == expected && ramp->green[i] == expected && ramp->blue[i] == expected;
    }
    s.m_gammaIdentity = identity;

    if (identity) {
        return;
    }

    if (!s.m_gammaTexture) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Unorm width:256 height:1 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        s.m_gammaTexture = [this->m_device newTextureWithDescriptor:descriptor];
    }

    uint16_t texels[256 * 4];
    for (uint32_t i = 0; i < 256; i++) {
        texels[i * 4 + 0] = ramp->red[i];
        texels[i * 4 + 1] = ramp->green[i];
        texels[i * 4 + 2] = ramp->blue[i];
        texels[i * 4 + 3] = 0xFFFF;
    }

    // The ramp may be in use by a frame in flight; a new texture avoids changing it under the GPU
    if (s.m_commands || this->CompletedSerial() + 1 < this->m_serial) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Unorm width:256 height:1 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        s.m_gammaTexture = [this->m_device newTextureWithDescriptor:descriptor];
    }

    [s.m_gammaTexture replaceRegion:MTLRegionMake2D(0, 0, 256, 1) mipmapLevel:0 withBytes:texels bytesPerRow:sizeof(texels)];
}

HRESULT DeviceImpl::PresentImpl() {
    ImplState& s = *this->m_state;

    this->ExecutePendingClears();
    this->EndEncoders();

    if (!s.m_layer || !s.m_backBuffer) {
        this->Flush(false);
        return D3D_OK;
    }

    id<MTLTexture> source = s.m_backBuffer->ResolvedTexture();
    id<CAMetalDrawable> drawable = [s.m_layer nextDrawable];

    if (drawable) {
        id<MTLTexture> target = drawable.texture;

        uint64_t key = (2ull << 48) | static_cast<uint64_t>(target.pixelFormat);
        id<MTLRenderPipelineState> pipeline;
        auto found = s.m_utilityPipelines.find(key);
        if (found != s.m_utilityPipelines.end()) {
            pipeline = found->second;
        } else {
            MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [s.m_utilityLibrary newFunctionWithName:@"utility_vertex"];
            descriptor.fragmentFunction = [s.m_utilityLibrary newFunctionWithName:@"utility_blit"];
            descriptor.colorAttachments[0].pixelFormat = target.pixelFormat;
            NSError* error = nil;
            pipeline = [this->m_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            if (!pipeline) {
                Log("present pipeline creation failed: %s", error.localizedDescription.UTF8String);
            }
            s.m_utilityPipelines[key] = pipeline;
        }

        if (pipeline) {
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;

            id<MTLRenderCommandEncoder> encoder = [this->Commands() renderCommandEncoderWithDescriptor:pass];
            bool sameSize = target.width == source.width && target.height == source.height;

            struct {
                float m_sourceRect[4];
                uint32_t m_gamma;
                uint32_t m_padding[3];
            } parameters = { { 0.0f, 0.0f, 1.0f, 1.0f }, s.m_gammaIdentity ? 0u : 1u, {} };

            float clipRect[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
            float depth = 0.0f;

            [encoder setRenderPipelineState:pipeline];
            [encoder setVertexBytes:clipRect length:sizeof(clipRect) atIndex:0];
            [encoder setVertexBytes:&depth length:sizeof(depth) atIndex:1];
            [encoder setFragmentTexture:source atIndex:0];
            [encoder setFragmentTexture:s.m_gammaTexture ?: s.m_dummy2d atIndex:1];
            [encoder setFragmentSamplerState:sameSize ? s.m_pointSampler : s.m_linearSampler atIndex:0];
            [encoder setFragmentBytes:&parameters length:sizeof(parameters) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [encoder endEncoding];
        }

        [this->Commands() presentDrawable:drawable];
    }

    this->Flush(false);
    return D3D_OK;
}

// Cursor

static NSCursor* MakeCursor(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t hotspotX, uint32_t hotspotY, float scale) {
    NSBitmapImageRep* source = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:width * 4 bitsPerPixel:32];
    memcpy(source.bitmapData, pixels.data(), pixels.size());

    // The image measures scale points per bitmap pixel and carries a bitmap at twice that, so
    // Retina displays show it at full resolution and at the intended size
    NSSize size = NSMakeSize(std::max(1.0f, width * scale), std::max(1.0f, height * scale));
    NSInteger pixelsWide = static_cast<NSInteger>(ceil(size.width * 2.0));
    NSInteger pixelsHigh = static_cast<NSInteger>(ceil(size.height * 2.0));

    NSBitmapImageRep* scaled = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:pixelsWide pixelsHigh:pixelsHigh bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:0 bytesPerRow:0 bitsPerPixel:32];
    scaled.size = size;

    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext* context = [NSGraphicsContext graphicsContextWithBitmapImageRep:scaled];
    NSGraphicsContext.currentContext = context;
    context.imageInterpolation = NSImageInterpolationHigh;
    [[NSColor clearColor] set];
    NSRectFillUsingOperation(NSMakeRect(0, 0, size.width, size.height), NSCompositingOperationCopy);
    [source drawInRect:NSMakeRect(0, 0, size.width, size.height) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0 respectFlipped:YES hints:nil];
    [NSGraphicsContext restoreGraphicsState];

    NSImage* image = [[NSImage alloc] initWithSize:size];
    [image addRepresentation:scaled];

    return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(hotspotX * scale, hotspotY * scale)];
}

HRESULT DeviceImpl::SetCursorPropertiesImpl(uint32_t hotspotX, uint32_t hotspotY, Surface* bitmap) {
    ImplState& s = *this->m_state;
    SurfaceImpl* surface = static_cast<SurfaceImpl*>(bitmap);

    if (!surface || surface->m_format != D3DFMT_A8R8G8B8 || surface->m_mirror.empty()) {
        return D3DERR_INVALIDCALL;
    }

    uint32_t width = surface->m_width;
    uint32_t height = surface->m_height;
    s.m_cursorPixels.resize(width * height * 4);

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = surface->m_mirror.data() + y * surface->m_mirrorPitch;
        uint8_t* out = s.m_cursorPixels.data() + y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            // A8R8G8B8 is stored B, G, R, A
            out[x * 4 + 0] = row[x * 4 + 2];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4 + 0];
            out[x * 4 + 3] = row[x * 4 + 3];
        }
    }

    s.m_cursorWidth = width;
    s.m_cursorHeight = height;
    s.m_cursorHotspotX = hotspotX;
    s.m_cursorHotspotY = hotspotY;
    s.m_cursor = MakeCursor(s.m_cursorPixels, width, height, hotspotX, hotspotY, s.m_cursorScale);
    return D3D_OK;
}

int32_t DeviceImpl::ShowCursorImpl(int32_t show) {
    ImplState& s = *this->m_state;
    int32_t previous = s.m_cursorShown ? 1 : 0;
    s.m_cursorShown = show != 0;
    return previous;
}

// Object creation

HRESULT DeviceImpl::CreateVertexDeclarationImpl(const D3DVERTEXELEMENT9* elements, VertexDeclaration** declaration) {
    if (!elements || !declaration) {
        return D3DERR_INVALIDCALL;
    }

    VertexDeclarationImpl* created = new VertexDeclarationImpl();

    for (const D3DVERTEXELEMENT9* element = elements; element->Stream != 0xFF; element++) {
        created->m_elements.push_back(*element);

        int32_t attribute = AttributeIndex(element->Usage, element->UsageIndex);
        MTLVertexFormat format;
        uint8_t attributeClass;
        uint8_t components;

        if (attribute < 0 || element->Stream >= 16 || !DeclarationFormat(element->Type, format, attributeClass, components)) {
            continue;
        }

        if (created->m_signature.m_class[attribute]) {
            continue;
        }

        created->m_mapped.push_back({ static_cast<uint32_t>(attribute), element->Stream, element->Offset, format, attributeClass, components });
        created->m_signature.m_class[attribute] = attributeClass;
        created->m_streamMask |= 1u << element->Stream;

        if (attribute >= ATTRIB_TEXCOORD0) {
            created->m_texcoordComponents[attribute - ATTRIB_TEXCOORD0] = components;
        }
    }

    created->m_id = this->NextId();
    *declaration = created;
    return D3D_OK;
}

HRESULT DeviceImpl::CreateShaderImpl(const uint32_t* function, uint32_t sizeInBytes, bool vertex, Unknown** shader) {
    if (!function || !shader) {
        return D3DERR_INVALIDCALL;
    }

    *shader = nullptr;

    // Without a size, the parser reads up to the end token at an instruction boundary
    size_t tokens = sizeInBytes ? sizeInBytes / 4 : SIZE_MAX / 4;
    ShaderProgram program;
    ShaderInfo info;
    std::string error;

    if (!ParseShader(function, tokens, program, error) || !AnalyzeShader(program, info, error)) {
        Log("shader rejected: %s", error.c_str());
        return D3DERR_INVALIDCALL;
    }

    if ((program.m_stage == STAGE_VERTEX) != vertex) {
        return D3DERR_INVALIDCALL;
    }

    if (vertex) {
        VertexShaderImpl* created = new VertexShaderImpl();
        created->m_program = std::move(program);
        created->m_info = info;
        *shader = created;
    } else {
        PixelShaderImpl* created = new PixelShaderImpl();
        created->m_program = std::move(program);
        created->m_info = info;
        *shader = created;
    }

    return D3D_OK;
}

// Render targets

HRESULT DeviceImpl::SetRenderTargetImpl(uint32_t index, Surface* surface) {
    ImplState& s = *this->m_state;

    if (index != 0) {
        return surface ? D3DERR_INVALIDCALL : D3D_OK;
    }

    SurfaceImpl* target = static_cast<SurfaceImpl*>(surface);
    if (target == s.m_renderTarget) {
        return D3D_OK;
    }

    if (target && (target->m_info.m_depth || !target->Valid())) {
        return D3DERR_INVALIDCALL;
    }

    this->ExecutePendingClears();
    if (s.m_render) {
        [s.m_render endEncoding];
        s.m_render = nil;
    }

    if (target) {
        target->AddRef();
    }
    if (s.m_renderTarget) {
        s.m_renderTarget->Release();
    }
    s.m_renderTarget = target;

    // Setting render target 0 resets the viewport and scissor rectangle to the whole target
    if (target) {
        s.m_viewport = { 0, 0, target->m_width, target->m_height, 0.0f, 1.0f };
        s.m_scissor = { 0, 0, static_cast<int32_t>(target->m_width), static_cast<int32_t>(target->m_height) };
    }

    return D3D_OK;
}

HRESULT DeviceImpl::GetRenderTargetImpl(uint32_t index, Surface** surface) {
    ImplState& s = *this->m_state;
    if (!surface || index != 0) {
        return D3DERR_INVALIDCALL;
    }
    *surface = s.m_renderTarget;
    if (!s.m_renderTarget) {
        return D3DERR_NOTAVAILABLE;
    }
    s.m_renderTarget->AddRef();
    return D3D_OK;
}

HRESULT DeviceImpl::SetDepthStencilSurfaceImpl(Unknown* object) {
    ImplState& s = *this->m_state;

    SurfaceImpl* depth = nullptr;
    if (object) {
        if (SurfaceImpl* surface = dynamic_cast<SurfaceImpl*>(object)) {
            depth = surface;
            depth->AddRef();
        } else if (BaseTexture* texture = dynamic_cast<BaseTexture*>(object)) {
            // A depth texture stands for its top level
            TextureCore* core = GetTextureCore(texture);
            Surface* level = nullptr;
            if (!core || !Succeeded(core->GetSurface(texture, 0, 0, &level))) {
                return D3DERR_INVALIDCALL;
            }
            depth = static_cast<SurfaceImpl*>(level);
        } else {
            return D3DERR_INVALIDCALL;
        }

        if (!depth->m_info.m_depth) {
            depth->Release();
            return D3DERR_INVALIDCALL;
        }
    }

    if (depth == s.m_depthStencil) {
        if (depth) {
            depth->Release();
        }
        return D3D_OK;
    }

    this->ExecutePendingClears();
    if (s.m_render) {
        [s.m_render endEncoding];
        s.m_render = nil;
    }

    if (s.m_depthStencil) {
        s.m_depthStencil->Release();
    }
    s.m_depthStencil = depth;
    return D3D_OK;
}

HRESULT DeviceImpl::GetDepthStencilSurfaceImpl(Surface** surface) {
    ImplState& s = *this->m_state;
    if (!surface) {
        return D3DERR_INVALIDCALL;
    }
    *surface = s.m_depthStencil;
    if (!s.m_depthStencil) {
        return D3DERR_NOTAVAILABLE;
    }
    s.m_depthStencil->AddRef();
    return D3D_OK;
}

HRESULT DeviceImpl::StretchRectImpl(Surface* sourceSurface, const Rect* sourceRect, Surface* destSurface, const Rect* destRect, D3DTEXTUREFILTERTYPE filter) {
    ImplState& s = *this->m_state;
    SurfaceImpl* source = static_cast<SurfaceImpl*>(sourceSurface);
    SurfaceImpl* dest = static_cast<SurfaceImpl*>(destSurface);

    if (!source || !dest || !source->Valid() || !dest->Valid() || source->m_info.m_depth || dest->m_info.m_depth) {
        return D3DERR_INVALIDCALL;
    }

    Rect from = sourceRect ? *sourceRect : Rect { 0, 0, static_cast<int32_t>(source->m_width), static_cast<int32_t>(source->m_height) };
    Rect to = destRect ? *destRect : Rect { 0, 0, static_cast<int32_t>(dest->m_width), static_cast<int32_t>(dest->m_height) };

    if (from.left < 0 || from.top < 0 || from.right > static_cast<int32_t>(source->m_width) || from.bottom > static_cast<int32_t>(source->m_height) || from.left >= from.right || from.top >= from.bottom) {
        return D3DERR_INVALIDCALL;
    }
    if (to.left < 0 || to.top < 0 || to.right > static_cast<int32_t>(dest->m_width) || to.bottom > static_cast<int32_t>(dest->m_height) || to.left >= to.right || to.top >= to.bottom) {
        return D3DERR_INVALIDCALL;
    }

    this->ExecutePendingClears();

    id<MTLTexture> sourceTexture = source->ResolvedTexture();
    uint32_t width = static_cast<uint32_t>(to.right - to.left);
    uint32_t height = static_cast<uint32_t>(to.bottom - to.top);
    bool sameSize = width == static_cast<uint32_t>(from.right - from.left) && height == static_cast<uint32_t>(from.bottom - from.top);

    if (sameSize && dest->m_samples == 1 && source->m_info.m_pixelFormat == dest->m_info.m_pixelFormat) {
        id<MTLBlitCommandEncoder> blit = this->InlineBlit();
        [blit copyFromTexture:sourceTexture sourceSlice:source->m_face sourceLevel:source->m_level sourceOrigin:MTLOriginMake(from.left, from.top, 0) sourceSize:MTLSizeMake(width, height, 1) toTexture:dest->m_texture destinationSlice:dest->m_face destinationLevel:dest->m_level destinationOrigin:MTLOriginMake(to.left, to.top, 0)];
    } else {
        // Scale (or convert) by drawing the source over the destination rectangle
        this->EndEncoders();

        uint64_t key = (3ull << 48) | static_cast<uint64_t>(dest->m_info.m_pixelFormat) | (static_cast<uint64_t>(dest->m_samples) << 32);
        id<MTLRenderPipelineState> pipeline;
        auto found = s.m_utilityPipelines.find(key);
        if (found != s.m_utilityPipelines.end()) {
            pipeline = found->second;
        } else {
            MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [s.m_utilityLibrary newFunctionWithName:@"utility_vertex"];
            descriptor.fragmentFunction = [s.m_utilityLibrary newFunctionWithName:@"utility_blit"];
            descriptor.colorAttachments[0].pixelFormat = dest->m_info.m_pixelFormat;
            descriptor.rasterSampleCount = dest->m_samples;
            NSError* error = nil;
            pipeline = [this->m_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            if (!pipeline) {
                Log("stretch pipeline creation failed: %s", error.localizedDescription.UTF8String);
            }
            s.m_utilityPipelines[key] = pipeline;
        }

        if (!pipeline) {
            return D3DERR_INVALIDCALL;
        }

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = dest->m_texture;
        pass.colorAttachments[0].level = dest->m_level;
        pass.colorAttachments[0].slice = dest->m_face;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLTexture> view = sourceTexture;
        if (source->m_level != 0 || source->m_face != 0 || sourceTexture.textureType != MTLTextureType2D) {
            view = [sourceTexture newTextureViewWithPixelFormat:sourceTexture.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(source->m_level, 1) slices:NSMakeRange(source->m_face, 1)];
        }

        float sourceWidth = static_cast<float>(source->m_width);
        float sourceHeight = static_cast<float>(source->m_height);
        struct {
            float m_sourceRect[4];
            uint32_t m_gamma;
            uint32_t m_padding[3];
        } parameters = { { from.left / sourceWidth, from.top / sourceHeight, from.right / sourceWidth, from.bottom / sourceHeight }, 0, {} };

        float destWidth = static_cast<float>(dest->m_width);
        float destHeight = static_cast<float>(dest->m_height);
        float clipRect[4] = { to.left / destWidth * 2.0f - 1.0f, 1.0f - to.top / destHeight * 2.0f, to.right / destWidth * 2.0f - 1.0f, 1.0f - to.bottom / destHeight * 2.0f };
        float depth = 0.0f;

        id<MTLRenderCommandEncoder> encoder = [this->Commands() renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBytes:clipRect length:sizeof(clipRect) atIndex:0];
        [encoder setVertexBytes:&depth length:sizeof(depth) atIndex:1];
        [encoder setFragmentTexture:view atIndex:0];
        [encoder setFragmentTexture:s.m_dummy2d atIndex:1];
        [encoder setFragmentSamplerState:filter == D3DTEXF_POINT || filter == D3DTEXF_NONE ? s.m_pointSampler : s.m_linearSampler atIndex:0];
        [encoder setFragmentBytes:&parameters length:sizeof(parameters) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
    }

    source->Use().m_lastUse = this->m_serial;
    source->Use().m_lastRenderUse = this->m_serial;
    dest->Use().m_lastUse = this->m_serial;
    dest->MarkRendered();
    return D3D_OK;
}

// State

HRESULT DeviceImpl::SetRenderStateImpl(D3DRENDERSTATETYPE state, uint32_t value) {
    ImplState& s = *this->m_state;
    if (state >= D3DRS_COUNT) {
        return D3DERR_INVALIDCALL;
    }
    if (s.m_renderStates[state] == value) {
        return D3D_OK;
    }
    s.m_renderStates[state] = value;

    switch (state) {
        case D3DRS_FOGCOLOR:
        case D3DRS_FOGENABLE:
        case D3DRS_ALPHATESTENABLE:
        case D3DRS_ALPHAFUNC:
        case D3DRS_ALPHAREF:
        case D3DRS_TEXTUREFACTOR:
            s.m_pixelStateDirty = true;
            break;
        default:
            break;
    }

    switch (state) {
        case D3DRS_DEPTHBIAS:
        case D3DRS_CLIPPLANEENABLE:
        case D3DRS_POINTSIZE:
        case D3DRS_POINTSIZE_MIN:
        case D3DRS_POINTSIZE_MAX:
            s.m_vertexStateDirty = true;
            break;
        case D3DRS_AMBIENT:
        case D3DRS_FOGSTART:
        case D3DRS_FOGEND:
        case D3DRS_FOGDENSITY:
        case D3DRS_POINTSCALE_A:
        case D3DRS_POINTSCALE_B:
        case D3DRS_POINTSCALE_C:
            s.m_fixedVertexDirty = true;
            break;
        default:
            break;
    }

    return D3D_OK;
}

HRESULT DeviceImpl::SetSamplerStateImpl(uint32_t sampler, D3DSAMPLERSTATETYPE type, uint32_t value) {
    ImplState& s = *this->m_state;
    if (sampler >= 16 || type >= D3DSAMP_COUNT) {
        return D3DERR_INVALIDCALL;
    }
    s.m_samplerStates[sampler][type] = value;
    return D3D_OK;
}

HRESULT DeviceImpl::SetTextureStageStateImpl(uint32_t stage, D3DTEXTURESTAGESTATETYPE type, uint32_t value) {
    ImplState& s = *this->m_state;
    if (stage >= 8 || type >= D3DTSS_COUNT) {
        return D3DERR_INVALIDCALL;
    }
    s.m_stageStates[stage][type] = value;

    switch (type) {
        case D3DTSS_BUMPENVMAT00:
        case D3DTSS_BUMPENVMAT01:
        case D3DTSS_BUMPENVMAT10:
        case D3DTSS_BUMPENVMAT11:
        case D3DTSS_BUMPENVLSCALE:
        case D3DTSS_BUMPENVLOFFSET:
            s.m_pixelStateDirty = true;
            break;
        default:
            break;
    }

    return D3D_OK;
}

HRESULT DeviceImpl::SetTextureImpl(uint32_t sampler, BaseTexture* texture) {
    ImplState& s = *this->m_state;
    if (sampler >= 16) {
        return D3DERR_INVALIDCALL;
    }
    if (s.m_textures[sampler] == texture) {
        return D3D_OK;
    }
    if (texture) {
        texture->AddRef();
    }
    if (s.m_textures[sampler]) {
        s.m_textures[sampler]->Release();
    }
    s.m_textures[sampler] = texture;
    return D3D_OK;
}

HRESULT DeviceImpl::SetTransformImpl(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    ImplState& s = *this->m_state;
    if (!matrix) {
        return D3DERR_INVALIDCALL;
    }

    switch (state) {
        case D3DTS_WORLD: s.m_world = ToMatrix(*matrix); break;
        case D3DTS_VIEW: s.m_view = ToMatrix(*matrix); break;
        case D3DTS_PROJECTION: s.m_projection = ToMatrix(*matrix); break;
        default:
            if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7) {
                s.m_textureTransforms[state - D3DTS_TEXTURE0] = ToMatrix(*matrix);
                break;
            }
            return D3D_OK;
    }

    s.m_fixedVertexDirty = true;
    return D3D_OK;
}

HRESULT DeviceImpl::SetMaterialImpl(const D3DMATERIAL9* material) {
    ImplState& s = *this->m_state;
    if (!material) {
        return D3DERR_INVALIDCALL;
    }
    s.m_material = *material;
    s.m_fixedVertexDirty = true;
    return D3D_OK;
}

HRESULT DeviceImpl::SetLightImpl(uint32_t index, const D3DLIGHT9* light) {
    ImplState& s = *this->m_state;
    if (!light || index >= 8) {
        return D3DERR_INVALIDCALL;
    }
    s.m_lights[index] = *light;
    s.m_fixedVertexDirty = true;
    return D3D_OK;
}

HRESULT DeviceImpl::LightEnableImpl(uint32_t index, int32_t enable) {
    ImplState& s = *this->m_state;
    if (index >= 8) {
        return D3DERR_INVALIDCALL;
    }
    s.m_lightEnabled[index] = enable != 0;
    return D3D_OK;
}

HRESULT DeviceImpl::SetClipPlaneImpl(uint32_t index, const float* plane) {
    ImplState& s = *this->m_state;
    if (!plane || index >= 6) {
        return D3DERR_INVALIDCALL;
    }
    memcpy(s.m_clipPlanes[index], plane, sizeof(float) * 4);
    s.m_vertexStateDirty = true;
    return D3D_OK;
}

HRESULT DeviceImpl::SetScissorRectImpl(const Rect* rect) {
    ImplState& s = *this->m_state;
    if (!rect) {
        return D3DERR_INVALIDCALL;
    }
    s.m_scissor = *rect;
    return D3D_OK;
}

HRESULT DeviceImpl::SetViewportImpl(const D3DVIEWPORT9* viewport) {
    ImplState& s = *this->m_state;
    if (!viewport) {
        return D3DERR_INVALIDCALL;
    }
    s.m_viewport = *viewport;
    s.m_fixedVertexDirty = true;
    return D3D_OK;
}

HRESULT DeviceImpl::SetStreamSourceImpl(uint32_t stream, VertexBuffer* buffer, uint32_t offset, uint32_t stride) {
    ImplState& s = *this->m_state;
    if (stream >= 16) {
        return D3DERR_INVALIDCALL;
    }

    ImplState::Stream& binding = s.m_streams[stream];
    VertexBufferImpl* impl = static_cast<VertexBufferImpl*>(buffer);
    if (impl != binding.m_buffer) {
        if (impl) {
            impl->AddRef();
        }
        if (binding.m_buffer) {
            binding.m_buffer->Release();
        }
        binding.m_buffer = impl;
    }
    binding.m_offset = offset;
    binding.m_stride = stride;
    return D3D_OK;
}

HRESULT DeviceImpl::SetIndicesImpl(IndexBuffer* buffer) {
    ImplState& s = *this->m_state;
    IndexBufferImpl* impl = static_cast<IndexBufferImpl*>(buffer);
    if (impl == s.m_indices) {
        return D3D_OK;
    }
    if (impl) {
        impl->AddRef();
    }
    if (s.m_indices) {
        s.m_indices->Release();
    }
    s.m_indices = impl;
    return D3D_OK;
}

HRESULT DeviceImpl::SetVertexDeclarationImpl(VertexDeclaration* declaration) {
    ImplState& s = *this->m_state;
    VertexDeclarationImpl* impl = static_cast<VertexDeclarationImpl*>(declaration);
    if (impl == s.m_declaration) {
        return D3D_OK;
    }
    if (impl) {
        impl->AddRef();
    }
    if (s.m_declaration) {
        s.m_declaration->Release();
    }
    s.m_declaration = impl;
    return D3D_OK;
}

HRESULT DeviceImpl::SetVertexShaderImpl(VertexShader* shader) {
    ImplState& s = *this->m_state;
    VertexShaderImpl* impl = static_cast<VertexShaderImpl*>(shader);
    if (impl == s.m_vertexShader) {
        return D3D_OK;
    }
    if (impl) {
        impl->AddRef();
    }
    if (s.m_vertexShader) {
        s.m_vertexShader->Release();
    }
    s.m_vertexShader = impl;
    s.m_vsConstantsDirty = true;
    s.m_fixedVertexDirty = true;
    return D3D_OK;
}

HRESULT DeviceImpl::SetPixelShaderImpl(PixelShader* shader) {
    ImplState& s = *this->m_state;
    PixelShaderImpl* impl = static_cast<PixelShaderImpl*>(shader);
    if (impl == s.m_pixelShader) {
        return D3D_OK;
    }
    if (impl) {
        impl->AddRef();
    }
    if (s.m_pixelShader) {
        s.m_pixelShader->Release();
    }
    s.m_pixelShader = impl;
    s.m_psConstantsDirty = true;
    s.m_pixelStateDirty = true;
    return D3D_OK;
}

HRESULT DeviceImpl::SetShaderConstantsImpl(bool vertex, uint32_t start, const float* data, uint32_t count) {
    ImplState& s = *this->m_state;
    uint32_t limit = vertex ? 256 : 224;
    if (!data || start >= limit) {
        return D3DERR_INVALIDCALL;
    }
    count = std::min(count, limit - start);
    memcpy((vertex ? s.m_vsConstants : s.m_psConstants) + start * 4, data, count * 16);
    (vertex ? s.m_vsConstantsDirty : s.m_psConstantsDirty) = true;
    return D3D_OK;
}

// Public interface

HRESULT Device::GetDeviceCaps(D3DCAPS9* caps) {
    if (!caps) {
        return D3DERR_INVALIDCALL;
    }
    *caps = static_cast<DeviceImpl*>(this)->m_caps;
    return D3D_OK;
}

HRESULT Device::TestCooperativeLevel() { return static_cast<DeviceImpl*>(this)->TestCooperativeLevelImpl(); }
HRESULT Device::Reset(const PresentParameters& parameters) { return static_cast<DeviceImpl*>(this)->ResetImpl(parameters); }
HRESULT Device::Present() { return static_cast<DeviceImpl*>(this)->PresentImpl(); }
HRESULT Device::GetBackBuffer(uint32_t swapChain, uint32_t index, D3DBACKBUFFER_TYPE type, Surface** surface) { return static_cast<DeviceImpl*>(this)->GetBackBufferImpl(surface); }
void Device::SetGammaRamp(const D3DGAMMARAMP* ramp) { static_cast<DeviceImpl*>(this)->SetGammaRampImpl(ramp); }
HRESULT Device::EvictManagedResources() { return D3D_OK; }
HRESULT Device::SetCursorProperties(uint32_t hotspotX, uint32_t hotspotY, Surface* bitmap) { return static_cast<DeviceImpl*>(this)->SetCursorPropertiesImpl(hotspotX, hotspotY, bitmap); }
int32_t Device::ShowCursor(int32_t show) { return static_cast<DeviceImpl*>(this)->ShowCursorImpl(show); }

void* Device::GetCursorImage() {
    return (__bridge void*)static_cast<DeviceImpl*>(this)->m_state->m_cursor;
}

bool Device::IsCursorShown() {
    return static_cast<DeviceImpl*>(this)->m_state->m_cursorShown;
}

void Device::SetCursorScale(float scale) {
    DeviceImpl::ImplState& s = *static_cast<DeviceImpl*>(this)->m_state;
    scale = std::clamp(scale, 0.5f, 4.0f);

    if (scale == s.m_cursorScale) {
        return;
    }

    s.m_cursorScale = scale;

    if (!s.m_cursorPixels.empty()) {
        s.m_cursor = MakeCursor(s.m_cursorPixels, s.m_cursorWidth, s.m_cursorHeight, s.m_cursorHotspotX, s.m_cursorHotspotY, scale);
    }
}

HRESULT Device::CreateVertexBuffer(uint32_t length, uint32_t usage, uint32_t fvf, D3DPOOL pool, VertexBuffer** buffer) {
    if (!buffer || !length) {
        return D3DERR_INVALIDCALL;
    }
    VertexBufferImpl* created = new VertexBufferImpl(static_cast<DeviceImpl*>(this), length, (usage & D3DUSAGE_DYNAMIC) != 0);
    if (!created->m_core.Valid()) {
        created->Release();
        *buffer = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *buffer = created;
    return D3D_OK;
}

HRESULT Device::CreateIndexBuffer(uint32_t length, uint32_t usage, D3DFORMAT format, D3DPOOL pool, IndexBuffer** buffer) {
    if (!buffer || !length || format != D3DFMT_INDEX16) {
        return D3DERR_INVALIDCALL;
    }
    IndexBufferImpl* created = new IndexBufferImpl(static_cast<DeviceImpl*>(this), length, (usage & D3DUSAGE_DYNAMIC) != 0);
    if (!created->m_core.Valid()) {
        created->Release();
        *buffer = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *buffer = created;
    return D3D_OK;
}

HRESULT Device::CreateTexture(uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, D3DFORMAT format, D3DPOOL pool, Texture** texture) {
    if (!texture) {
        return D3DERR_INVALIDCALL;
    }
    TextureImpl* created = new TextureImpl(static_cast<DeviceImpl*>(this));
    if (!created->m_core.Create(MTLTextureType2D, width, height, levels, usage, format, pool)) {
        created->Release();
        *texture = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *texture = created;
    return D3D_OK;
}

HRESULT Device::CreateCubeTexture(uint32_t edgeLength, uint32_t levels, uint32_t usage, D3DFORMAT format, D3DPOOL pool, CubeTexture** texture) {
    if (!texture) {
        return D3DERR_INVALIDCALL;
    }
    CubeTextureImpl* created = new CubeTextureImpl(static_cast<DeviceImpl*>(this));
    if (!created->m_core.Create(MTLTextureTypeCube, edgeLength, edgeLength, levels, usage, format, pool)) {
        created->Release();
        *texture = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *texture = created;
    return D3D_OK;
}

HRESULT Device::CreateRenderTarget(uint32_t width, uint32_t height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, uint32_t quality, int32_t lockable, Surface** surface) {
    if (!surface) {
        return D3DERR_INVALIDCALL;
    }
    uint32_t samples = multisample >= D3DMULTISAMPLE_2_SAMPLES ? static_cast<uint32_t>(multisample) : 1;
    SurfaceImpl* created = new SurfaceImpl(static_cast<DeviceImpl*>(this), format, width, height, samples, lockable != 0);
    if (!created->Valid() || created->m_info.m_depth) {
        created->Release();
        *surface = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *surface = created;
    return D3D_OK;
}

HRESULT Device::CreateDepthStencilSurface(uint32_t width, uint32_t height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, uint32_t quality, int32_t discard, Surface** surface) {
    if (!surface) {
        return D3DERR_INVALIDCALL;
    }
    uint32_t samples = multisample >= D3DMULTISAMPLE_2_SAMPLES ? static_cast<uint32_t>(multisample) : 1;
    SurfaceImpl* created = new SurfaceImpl(static_cast<DeviceImpl*>(this), format, width, height, samples, false);
    if (!created->Valid() || !created->m_info.m_depth) {
        created->Release();
        *surface = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *surface = created;
    return D3D_OK;
}

HRESULT Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, VertexDeclaration** declaration) { return static_cast<DeviceImpl*>(this)->CreateVertexDeclarationImpl(elements, declaration); }
HRESULT Device::CreateVertexShader(const uint32_t* function, VertexShader** shader, uint32_t sizeInBytes) {
    Unknown* created = nullptr;
    HRESULT hr = static_cast<DeviceImpl*>(this)->CreateShaderImpl(function, sizeInBytes, true, &created);
    if (shader) {
        *shader = static_cast<VertexShaderImpl*>(created);
    }
    return hr;
}

HRESULT Device::CreatePixelShader(const uint32_t* function, PixelShader** shader, uint32_t sizeInBytes) {
    Unknown* created = nullptr;
    HRESULT hr = static_cast<DeviceImpl*>(this)->CreateShaderImpl(function, sizeInBytes, false, &created);
    if (shader) {
        *shader = static_cast<PixelShaderImpl*>(created);
    }
    return hr;
}

HRESULT Device::CreateQuery(D3DQUERYTYPE type, Query** query) {
    if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION) {
        return D3DERR_NOTAVAILABLE;
    }
    if (query) {
        *query = new QueryImpl(static_cast<DeviceImpl*>(this), type);
    }
    return D3D_OK;
}

HRESULT Device::SetRenderTarget(uint32_t index, Surface* surface) { return static_cast<DeviceImpl*>(this)->SetRenderTargetImpl(index, surface); }
HRESULT Device::GetRenderTarget(uint32_t index, Surface** surface) { return static_cast<DeviceImpl*>(this)->GetRenderTargetImpl(index, surface); }
HRESULT Device::SetDepthStencilSurface(Unknown* surface) { return static_cast<DeviceImpl*>(this)->SetDepthStencilSurfaceImpl(surface); }
HRESULT Device::GetDepthStencilSurface(Surface** surface) { return static_cast<DeviceImpl*>(this)->GetDepthStencilSurfaceImpl(surface); }
HRESULT Device::StretchRect(Surface* source, const Rect* sourceRect, Surface* dest, const Rect* destRect, D3DTEXTUREFILTERTYPE filter) { return static_cast<DeviceImpl*>(this)->StretchRectImpl(source, sourceRect, dest, destRect, filter); }
HRESULT Device::BeginScene() { return D3D_OK; }
HRESULT Device::EndScene() { return D3D_OK; }
HRESULT Device::Clear(uint32_t count, const D3DRECT* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) { return static_cast<DeviceImpl*>(this)->ClearImpl(count, rects, flags, color, z); }
HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, uint32_t startVertex, uint32_t primitiveCount) { return static_cast<DeviceImpl*>(this)->DrawImpl(type, false, 0, startVertex, primitiveCount); }
HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, int32_t baseVertexIndex, uint32_t minVertexIndex, uint32_t numVertices, uint32_t startIndex, uint32_t primitiveCount) { return static_cast<DeviceImpl*>(this)->DrawImpl(type, true, baseVertexIndex, startIndex, primitiveCount); }
HRESULT Device::SetRenderState(D3DRENDERSTATETYPE state, uint32_t value) { return static_cast<DeviceImpl*>(this)->SetRenderStateImpl(state, value); }
HRESULT Device::SetSamplerState(uint32_t sampler, D3DSAMPLERSTATETYPE type, uint32_t value) { return static_cast<DeviceImpl*>(this)->SetSamplerStateImpl(sampler, type, value); }
HRESULT Device::SetTextureStageState(uint32_t stage, D3DTEXTURESTAGESTATETYPE type, uint32_t value) { return static_cast<DeviceImpl*>(this)->SetTextureStageStateImpl(stage, type, value); }
HRESULT Device::SetTexture(uint32_t sampler, BaseTexture* texture) { return static_cast<DeviceImpl*>(this)->SetTextureImpl(sampler, texture); }
HRESULT Device::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) { return static_cast<DeviceImpl*>(this)->SetTransformImpl(state, matrix); }
HRESULT Device::SetMaterial(const D3DMATERIAL9* material) { return static_cast<DeviceImpl*>(this)->SetMaterialImpl(material); }
HRESULT Device::SetLight(uint32_t index, const D3DLIGHT9* light) { return static_cast<DeviceImpl*>(this)->SetLightImpl(index, light); }
HRESULT Device::LightEnable(uint32_t index, int32_t enable) { return static_cast<DeviceImpl*>(this)->LightEnableImpl(index, enable); }
HRESULT Device::SetClipPlane(uint32_t index, const float* plane) { return static_cast<DeviceImpl*>(this)->SetClipPlaneImpl(index, plane); }
HRESULT Device::SetScissorRect(const Rect* rect) { return static_cast<DeviceImpl*>(this)->SetScissorRectImpl(rect); }
HRESULT Device::SetViewport(const D3DVIEWPORT9* viewport) { return static_cast<DeviceImpl*>(this)->SetViewportImpl(viewport); }
HRESULT Device::SetStreamSource(uint32_t stream, VertexBuffer* buffer, uint32_t offset, uint32_t stride) { return static_cast<DeviceImpl*>(this)->SetStreamSourceImpl(stream, buffer, offset, stride); }
HRESULT Device::SetIndices(IndexBuffer* buffer) { return static_cast<DeviceImpl*>(this)->SetIndicesImpl(buffer); }
HRESULT Device::SetVertexDeclaration(VertexDeclaration* declaration) { return static_cast<DeviceImpl*>(this)->SetVertexDeclarationImpl(declaration); }
HRESULT Device::SetVertexShader(VertexShader* shader) { return static_cast<DeviceImpl*>(this)->SetVertexShaderImpl(shader); }
HRESULT Device::SetPixelShader(PixelShader* shader) { return static_cast<DeviceImpl*>(this)->SetPixelShaderImpl(shader); }
HRESULT Device::SetVertexShaderConstantF(uint32_t start, const float* data, uint32_t count) { return static_cast<DeviceImpl*>(this)->SetShaderConstantsImpl(true, start, data, count); }
HRESULT Device::SetPixelShaderConstantF(uint32_t start, const float* data, uint32_t count) { return static_cast<DeviceImpl*>(this)->SetShaderConstantsImpl(false, start, data, count); }
void* Device::GetMetalDevice() { return (__bridge void*)static_cast<DeviceImpl*>(this)->m_device; }
bool Device::AcquireExternalTargets(void** commandBuffer, void** color, void** depth, uint32_t* width, uint32_t* height, uint32_t* sampleCount) {
    return static_cast<DeviceImpl*>(this)->AcquireExternalTargetsImpl(commandBuffer, color, depth, width, height, sampleCount);
}

}
