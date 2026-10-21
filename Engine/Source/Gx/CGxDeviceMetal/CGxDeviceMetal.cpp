#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include "Engine/Source/Gx/CGxDeviceMetal/CGxDeviceMetal.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Error.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Os/Gui.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Mac/MacOs.h"

using namespace D3D9Metal;

#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#define D3DCOLOR_ARGB(a, r, g, b) ((static_cast<uint32_t>((a) & 0xFF) << 24) | (static_cast<uint32_t>((r) & 0xFF) << 16) | (static_cast<uint32_t>((g) & 0xFF) << 8) | static_cast<uint32_t>((b) & 0xFF))

const D3DFORMAT CGxDeviceMetal::s_GxFormatToD3dFormat[CGxFormat::Formats_Last] = { D3DFMT_R5G6B5, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_A2R10G10B10, D3DFMT_D16, D3DFMT_D24X8, D3DFMT_D24S8, D3DFMT_D32 };

const uint32_t CGxDeviceMetal::s_gxAttribToD3dAttribSize[7] = { 4, 4, 4, 8, 12, 4, 4 };

static const D3DQUERYTYPE s_d3dQueryTypes[1] = { D3DQUERYTYPE_OCCLUSION };

const EGxTexFormat CGxDeviceMetal::s_tolerableTexFmtMapping[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Argb4444, GxTex_Argb4444, GxTex_Argb4444, GxTex_Argb4444, GxTex_Argb4444, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8 };

const D3DCUBEMAP_FACES CGxDeviceMetal::s_faceTypes[6] = { D3DCUBEMAP_FACE_POSITIVE_X, D3DCUBEMAP_FACE_NEGATIVE_X, D3DCUBEMAP_FACE_POSITIVE_Y, D3DCUBEMAP_FACE_NEGATIVE_Y, D3DCUBEMAP_FACE_POSITIVE_Z, D3DCUBEMAP_FACE_NEGATIVE_Z };

const D3DFORMAT CGxDeviceMetal::s_GxTexFmtToD3dFmt[GxTexFormats_Last] = { D3DFMT_UNKNOWN, D3DFMT_A8B8G8R8, D3DFMT_A8R8G8B8, D3DFMT_A4R4G4B4, D3DFMT_A1R5G5B5, D3DFMT_R5G6B5, D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5, D3DFMT_V8U8, D3DFMT_G16R16F, D3DFMT_R32F, D3DFMT_D24X8 };

const D3DPRIMITIVETYPE CGxDeviceMetal::s_primitiveConversion[GxPrims_Last] = { D3DPT_POINTLIST, D3DPT_LINELIST, D3DPT_LINESTRIP, D3DPT_TRIANGLELIST, D3DPT_TRIANGLESTRIP, D3DPT_TRIANGLEFAN };

const D3DDECLUSAGE CGxDeviceMetal::s_gxAttribToD3dAttribUsage[GxVAs_Last] = { D3DDECLUSAGE_POSITION, D3DDECLUSAGE_BLENDWEIGHT, D3DDECLUSAGE_BLENDINDICES, D3DDECLUSAGE_NORMAL, D3DDECLUSAGE_COLOR, D3DDECLUSAGE_COLOR, D3DDECLUSAGE_TEXCOORD, D3DDECLUSAGE_TEXCOORD, D3DDECLUSAGE_TEXCOORD, D3DDECLUSAGE_TEXCOORD, D3DDECLUSAGE_TEXCOORD, D3DDECLUSAGE_TEXCOORD, D3DDECLUSAGE_TEXCOORD, D3DDECLUSAGE_TEXCOORD };

const uint32_t CGxDeviceMetal::s_gxAttribToD3dAttribUsageIndex[GxVAs_Last] = { 0, 0, 0, 0, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7 };

const D3DBLEND CGxDeviceMetal::s_srcBlend[12] = { D3DBLEND_ONE, D3DBLEND_ONE, D3DBLEND_SRCALPHA, D3DBLEND_SRCALPHA, D3DBLEND_DESTCOLOR, D3DBLEND_DESTCOLOR, D3DBLEND_DESTCOLOR, D3DBLEND_INVSRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLEND_BLENDFACTOR };

const D3DBLEND CGxDeviceMetal::s_dstBlend[12] = { D3DBLEND_ZERO, D3DBLEND_ZERO, D3DBLEND_INVSRCALPHA, D3DBLEND_ONE, D3DBLEND_ZERO, D3DBLEND_SRCCOLOR, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLEND_ZERO, D3DBLEND_ZERO, D3DBLEND_ONE, D3DBLEND_INVBLENDFACTOR };

const D3DTEXTUREADDRESS CGxDeviceMetal::s_wrapModes[2] = { D3DTADDRESS_CLAMP, D3DTADDRESS_WRAP };

static const D3DTEXTUREOP s_texOp[6] = { D3DTOP_MODULATE, D3DTOP_MODULATE2X, D3DTOP_ADD, D3DTOP_SELECTARG2, D3DTOP_BLENDCURRENTALPHA, D3DTOP_BLENDDIFFUSEALPHA };

static const uint32_t s_texArgs[6][2] = { { D3DTA_TEXTURE, D3DTA_CURRENT }, { D3DTA_TEXTURE, D3DTA_CURRENT }, { D3DTA_TEXTURE, D3DTA_CURRENT }, { D3DTA_TEXTURE, D3DTA_CURRENT }, { D3DTA_CURRENT, D3DTA_TEXTURE }, { D3DTA_TEXTURE, D3DTA_CURRENT } };

const D3DCMPFUNC CGxDeviceMetal::s_cmpFunc[4] = { D3DCMP_LESSEQUAL, D3DCMP_EQUAL, D3DCMP_GREATEREQUAL, D3DCMP_LESS };

const D3DCULL CGxDeviceMetal::s_cullMode[3] = { D3DCULL_NONE, D3DCULL_CW, D3DCULL_CCW };

EGxTexFormat CGxDeviceMetal::s_GxTexFormatToUse[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Abgr8888, GxTex_Argb8888, GxTex_Argb4444, GxTex_Argb1555, GxTex_Rgb565, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8 };

D3DDECLTYPE CGxDeviceMetal::s_gxAttribToD3dAttribType[7] = { D3DDECLTYPE_D3DCOLOR, D3DDECLTYPE_UBYTE4, D3DDECLTYPE_UBYTE4N, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_SHORT2, D3DDECLTYPE_FLOAT1 };

D3DTEXTUREFILTERTYPE CGxDeviceMetal::s_filterModes[GxTexFilters_Last][3] = { { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE }, { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE }, { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT }, { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_POINT }, { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR }, { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR } };

static uint32_t s_normalizeNormals = 5;

static Surface* s_savedDepthSurface;

static D3DMATERIAL9 s_d3dMaterial;

static D3DLIGHT9 s_d3dLight;

static int32_t s_isIdent;

// The cursor size set through gxCursorScale, applied to every device
static float s_cursorScale = 1.0f;

void CGxDeviceMetal::IBufUnlock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;
    EmergencyMem& emergencyMem = this->m_emergencyMem[pool->m_target];

    if (emergencyMem.m_lock) {
        emergencyMem.Unlock();
        buf->m_valid = 0;
        this->m_intF5C = 1;
        return;
    }

    HRESULT result = D3D_OK;

    switch (pool->m_target) {
        case GxPoolTarget_Vertex:
            result = static_cast<VertexBuffer*>(pool->m_apiSpecific)->Unlock();
            break;

        case GxPoolTarget_Index:
            result = static_cast<IndexBuffer*>(pool->m_apiSpecific)->Unlock();
            break;

        default:
            break;
    }

    buf->m_valid = SUCCEEDED(result);
}

int32_t CGxDeviceMetal::BufUnlock(CGxBuf* buf, uint32_t size) {
    CGxDevice::BufUnlock(buf, size);
    this->IBufUnlock(buf);

    return 1;
}

char* CGxDeviceMetal::IBufLock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;

    if (!this->m_context) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    uint32_t flags = 0;

    if (pool->m_usage == GxPoolUsage_Stream) {
        uint32_t index = (buf->m_itemSize + pool->m_offset - 1) - (buf->m_itemSize + pool->m_offset - 1) % buf->m_itemSize;

        if (index + buf->m_size > static_cast<uint32_t>(pool->m_size)) {
            flags = D3DLOCK_DISCARD;
            pool->Discard();
            buf->m_index = 0;
            pool->m_offset = buf->m_size;
        } else {
            flags = D3DLOCK_NOOVERWRITE;
            buf->m_index = index;
            pool->m_offset = index + buf->m_size;
        }
    } else if (pool->m_usage == GxPoolUsage_Dynamic) {
        flags = D3DLOCK_NOOVERWRITE;
    }

    char* data = nullptr;

    if (!pool->m_apiSpecific) {
        this->CreatePoolAPI(pool);
    }

    if (!pool->m_apiSpecific) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    HRESULT result = D3D_OK;

    switch (pool->m_target) {
        case GxPoolTarget_Vertex:
            result = static_cast<VertexBuffer*>(pool->m_apiSpecific)->Lock(buf->m_index, buf->m_size, reinterpret_cast<void**>(&data), flags);
            break;

        case GxPoolTarget_Index:
            result = static_cast<IndexBuffer*>(pool->m_apiSpecific)->Lock(buf->m_index, buf->m_size, reinterpret_cast<void**>(&data), flags);
            break;

        default:
            break;
    }

    if (FAILED(result)) {
        this->IBufUnlock(buf);
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    return data;
}

char* CGxDeviceMetal::BufLock(CGxBuf* buf) {
    CGxDevice::BufLock(buf);

    return this->IBufLock(buf);
}

void CGxDeviceMetal::BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset) {
    CGxDevice::BufData(buf, data, size, offset);

    char* dst = this->IBufLock(buf);
    memcpy(dst + offset, data, size);
    this->IBufUnlock(buf);
}

CGxDeviceMetal::CGxDeviceMetal() : m_desktopDisplayMode() {
    this->m_d3dStereoConvergence = 1.0f;
    this->m_d3dStereoSeparation = 25.0f;
    this->m_api = GxApi_Metal;
    this->m_hwnd = nullptr;
    this->m_ownhwnd = 0;
    this->m_d3d = nullptr;
    this->m_d3dDevice = nullptr;
    this->m_d3dIsHwDevice = 0;
    this->m_inScene = 0;
    this->m_ambientOnlyMode = 0;
    this->m_d3dCurrentVertexDecl = nullptr;
    this->m_d3dCurrentIndexBuf = nullptr;
    memset(this->m_d3dVertexStreamBuf, 0, sizeof(this->m_d3dVertexStreamBuf));
    this->m_caps.m_colorFormat = GxCF_argb;
    this->m_caps.unk_0018 = 1;
    memset(this->m_deviceStates, 0xFF, sizeof(this->m_deviceStates));
    memset(this->m_texEnabled, 0, sizeof(this->m_texEnabled));
    memset(this->m_d3dVertexDecl, 0, sizeof(this->m_d3dVertexDecl));
    this->m_defDepthStencilSurface = nullptr;
    this->m_defColorSurface = nullptr;
    this->m_defDepthSurface = nullptr;
    this->m_surface3B44 = nullptr;
    this->m_eventQuery = nullptr;
    this->m_hwCursorDirty = 1;
    this->m_hwCursorTexture = nullptr;
    this->m_hwCursorBitmap = nullptr;
    this->m_texture3B58 = nullptr;
    this->DeviceCreatePools();
    this->DeviceCreateStreamBufs();
}

IndexBuffer* CGxDeviceMetal::ICreateD3dIB(EGxPoolUsage usage, uint32_t size) {
    D3DPOOL d3dPool = D3DPOOL_MANAGED;
    uint32_t d3dUsage = D3DUSAGE_WRITEONLY | (this->m_d3dIsHwDevice ? 0 : D3DUSAGE_SOFTWAREPROCESSING);

    if (usage == GxPoolUsage_Stream || usage == GxPoolUsage_Dynamic) {
        d3dUsage |= D3DUSAGE_DYNAMIC;
        d3dPool = D3DPOOL_DEFAULT;
    }

    IndexBuffer* indexBuffer = nullptr;
    HRESULT hr = this->m_d3dDevice->CreateIndexBuffer(size, d3dUsage, D3DFMT_INDEX16, d3dPool, &indexBuffer);

    return SUCCEEDED(hr) ? indexBuffer : nullptr;
}

void CGxDeviceMetal::IReleaseD3dPool(CGxPool* pool) {
    if (pool->m_usage == GxPoolUsage_Stream) {
        pool->m_offset = 0;
    }

    pool->Invalidate();

    if (pool->m_apiSpecific) {
        switch (pool->m_target) {
            case GxPoolTarget_Vertex:
                static_cast<VertexBuffer*>(pool->m_apiSpecific)->Release();
                break;

            case GxPoolTarget_Index:
                static_cast<IndexBuffer*>(pool->m_apiSpecific)->Release();
                break;

            default:
                break;
        }

        pool->m_apiSpecific = nullptr;
    }
}

void CGxDeviceMetal::DeviceEvictResources() {
    if (!this->m_context || !this->m_d3dDevice) {
        return;
    }

    HRESULT hr = this->m_d3dDevice->EvictManagedResources();

    if (FAILED(hr) && hr == D3DERR_DRIVERINTERNALERROR) {
        SErrDisplayError(STORM_ERROR_FATAL_CONDITION, __FILE__, __LINE__, "Driver internal error on EvictManagedResources().", 0, 1, 0x11111111);
    }
}

void CGxDeviceMetal::DeviceSetGamma(float gamma) {
    CGxDevice::DeviceSetGamma(gamma);

    // The Metal device applies the ramp as it presents, so it works in a window too
    if ((this->m_d3dCaps.Caps2 & D3DCAPS2_FULLSCREENGAMMA) && this->m_d3dDevice) {
        this->m_d3dDevice->SetGammaRamp(reinterpret_cast<const D3DGAMMARAMP*>(&this->m_gammaRamp));
    }
}

void CGxDeviceMetal::PoolDestroy(CGxPool* pool) {
    if (pool) {
        this->IReleaseD3dPool(pool);
        CGxDevice::PoolDestroy(pool);
    }
}

void CGxDeviceMetal::CursorSetVisible(int32_t visible) {
    CGxDevice::CursorSetVisible(visible);

    if (this->m_hwCursor && this->m_context) {
        this->m_d3dDevice->ShowCursor(this->m_cursorVisible);
        this->IUpdateCursor();
    }
}

void CGxDeviceMetal::CursorUnlock(uint32_t x, uint32_t y) {
    CGxDevice::CursorUnlock(x, y);
    this->m_hwCursorDirty = 1;
}

void CGxDeviceMetal::ICursorDraw() {
    if (!this->m_hwCursor) {
        this->ISceneBegin();
    }

    CGxDevice::ICursorDraw();

    if (!this->m_hwCursor) {
        this->ISceneEnd();
    }

    if (!this->m_hwCursor || !this->m_hwCursorDirty || !this->m_hwCursorBitmap || !this->m_context) {
        return;
    }

    D3DLOCKED_RECT lockedRect;
    if (SUCCEEDED(this->m_hwCursorBitmap->LockRect(&lockedRect, nullptr, 0))) {
        for (uint32_t y = 0; y < 32; y++) {
            memcpy(static_cast<uint8_t*>(lockedRect.pBits) + y * lockedRect.Pitch, &this->m_cursor[y * 32], 32 * sizeof(uint32_t));
        }

        this->m_hwCursorBitmap->UnlockRect();
        this->m_d3dDevice->SetCursorProperties(this->m_cursorHotspotX, this->m_cursorHotspotY, this->m_hwCursorBitmap);
        this->IUpdateCursor();
    }

    this->m_hwCursorDirty = 0;
}

void CGxDeviceMetal::ICursorCreate(const CGxFormat& format) {
    CGxDevice::ICursorCreate(format);

    // Over the game view the system pointer shows the client's cursor or nothing, as WM_SETCURSOR
    // clears it on Windows
    this->IUpdateCursor();

    if (this->m_hwCursor && !this->m_hwCursorTexture) {
        this->m_d3dDevice->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &this->m_hwCursorTexture);

        if (this->m_hwCursorTexture) {
            this->m_hwCursorTexture->GetSurfaceLevel(0, &this->m_hwCursorBitmap);
        }

        this->m_hwCursorDirty = 1;
        this->ICursorDraw();
    }
}

void CGxDeviceMetal::StereoSetConvergence(float convergence) {
    // No stereoscopic output on macOS; the values are kept for the client to read back
    this->m_d3dStereoConvergence = convergence;
}

void CGxDeviceMetal::StereoSetSeparation(float separation) {
    this->m_d3dStereoSeparation = separation;
}

void CGxDeviceMetal::QueryCreate(CGxQuery*& query, EGxQueryType type) {
    CGxDevice::QueryCreate(query, type);

    CGxQuery* gxQuery = query;
    Query* d3dQuery;

    if (!gxQuery->m_apiSpecific && this->m_d3dDevice->CreateQuery(s_d3dQueryTypes[gxQuery->m_type], &d3dQuery) == D3D_OK) {
        gxQuery->m_apiSpecific = d3dQuery;
    }
}

bool CGxDeviceMetal::QueryBegin(CGxQuery* query) {
    if (!this->m_context) {
        return false;
    }

    Query* d3dQuery;

    if (!query->m_apiSpecific) {
        if (this->m_d3dDevice->CreateQuery(s_d3dQueryTypes[query->m_type], &d3dQuery) == D3D_OK) {
            query->m_apiSpecific = d3dQuery;
        }

        if (!query->m_apiSpecific) {
            return false;
        }
    }

    HRESULT hr = D3D_OK;

    if (query->m_type == GxQueryType_Occlusion) {
        hr = static_cast<Query*>(query->m_apiSpecific)->Issue(D3DISSUE_BEGIN);
    }

    return hr == D3D_OK;
}

bool CGxDeviceMetal::QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) {
    if (!this->m_context) {
        return false;
    }

    Query* d3dQuery;

    if (!query->m_apiSpecific) {
        if (this->m_d3dDevice->CreateQuery(s_d3dQueryTypes[query->m_type], &d3dQuery) == D3D_OK) {
            query->m_apiSpecific = d3dQuery;
        }

        if (!query->m_apiSpecific) {
            return false;
        }
    }

    switch (param) {
        case GxQueryParam_Unk0:
            data = static_cast<Query*>(query->m_apiSpecific)->GetDataSize();
            return true;

        case GxQueryParam_Unk1:
            data = static_cast<Query*>(query->m_apiSpecific)->GetData(nullptr, 0, 0) == D3D_OK;
            return false;

        default:
            return false;
    }
}

void CGxDeviceMetal::ISetWindowFocus(int32_t focus) {
    this->m_windowFocus = focus;

    if (focus) {
        // Windows confines the pointer to a fullscreen window; a macOS fullscreen window covers
        // the display, which keeps the pointer over it
        this->m_hwCursorDirty = 1;
    }
}

VertexBuffer* CGxDeviceMetal::ICreateD3dVB(EGxPoolUsage usage, uint32_t size) {
    D3DPOOL d3dPool = D3DPOOL_MANAGED;
    uint32_t d3dUsage = D3DUSAGE_WRITEONLY | (this->m_d3dIsHwDevice ? 0 : D3DUSAGE_SOFTWAREPROCESSING);

    if (usage == GxPoolUsage_Stream || usage == GxPoolUsage_Dynamic) {
        d3dUsage |= D3DUSAGE_DYNAMIC;
        d3dPool = D3DPOOL_DEFAULT;
    }

    VertexBuffer* vertexBuffer = nullptr;
    HRESULT hr = this->m_d3dDevice->CreateVertexBuffer(size, d3dUsage, 0, d3dPool, &vertexBuffer);

    return SUCCEEDED(hr) ? vertexBuffer : nullptr;
}

int32_t CGxDeviceMetal::CreatePoolAPI(CGxPool* pool) {
    switch (pool->m_target) {
        case GxPoolTarget_Vertex:
            pool->m_apiSpecific = this->ICreateD3dVB(pool->m_usage, pool->m_size);
            break;

        case GxPoolTarget_Index:
            pool->m_apiSpecific = this->ICreateD3dIB(pool->m_usage, pool->m_size);
            break;

        default:
            break;
    }

    return 1;
}

void CGxDeviceMetal::DeviceSetGamma(const CGxGammaRamp& ramp) {
    CGxDevice::DeviceSetGamma(ramp);

    // The Metal device applies the ramp as it presents, so it works in a window too
    if ((this->m_d3dCaps.Caps2 & D3DCAPS2_FULLSCREENGAMMA) && this->m_d3dDevice) {
        this->m_d3dDevice->SetGammaRamp(reinterpret_cast<const D3DGAMMARAMP*>(&this->m_gammaRamp));
    }
}

void* CGxDeviceMetal::DeviceWindow() {
    return this->m_hwnd;
}

void CGxDeviceMetal::DeviceOverride(EGxOverride override, uint32_t value) {
    CGxDevice::DeviceOverride(override, value);

    if (override == GxOverride_PixelShader) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = value;
    } else if (override == GxOverride_Unk7) {
        this->m_caps.m_texNonPow2 = value != 0;
    }
}

void CGxDeviceMetal::PoolSizeSet(CGxPool* pool, uint32_t size) {
    this->IReleaseD3dPool(pool);
    pool->m_size = size;

    switch (pool->m_target) {
        case GxPoolTarget_Vertex:
            pool->m_apiSpecific = this->ICreateD3dVB(pool->m_usage, size);
            break;

        case GxPoolTarget_Index:
            pool->m_apiSpecific = this->ICreateD3dIB(pool->m_usage, size);
            break;

        default:
            break;
    }
}

void CGxDeviceMetal::ICursorDestroy() {
    CGxDevice::ICursorDestroy();

    if (this->m_hwCursorBitmap) {
        this->m_hwCursorBitmap->Release();
        this->m_hwCursorBitmap = nullptr;
    }

    if (this->m_hwCursorTexture) {
        this->m_hwCursorTexture->Release();
        this->m_hwCursorTexture = nullptr;
    }
}

float CGxDeviceMetal::StereoGetConvergence() {
    return this->m_d3dStereoConvergence;
}

float CGxDeviceMetal::StereoGetSeparation() {
    return this->m_d3dStereoSeparation;
}

bool CGxDeviceMetal::StereoEnabled() {
    return false;
}

void CGxDeviceMetal::QueryDestroy(CGxQuery*& query) {
    CGxQuery* gxQuery = query;

    if (gxQuery->m_apiSpecific) {
        static_cast<Query*>(gxQuery->m_apiSpecific)->Release();
    }

    gxQuery->m_apiSpecific = nullptr;
    CGxDevice::QueryDestroy(query);
}

bool CGxDeviceMetal::QueryEnd(CGxQuery* query) {
    if (!this->m_context || !query->m_apiSpecific) {
        return false;
    }

    HRESULT hr = D3D_OK;

    if (query->m_type == GxQueryType_Occlusion) {
        hr = static_cast<Query*>(query->m_apiSpecific)->Issue(D3DISSUE_END);
    }

    return hr == D3D_OK;
}

bool CGxDeviceMetal::QueryGetData(CGxQuery* query, uint32_t& data) {
    if (!this->m_context || !query->m_apiSpecific) {
        return false;
    }

    Query* d3dQuery = static_cast<Query*>(query->m_apiSpecific);
    HRESULT hr;

    do {
        hr = d3dQuery->GetData(&data, d3dQuery->GetDataSize(), D3DGETDATA_FLUSH);
    } while (hr == S_FALSE);

    return hr == D3D_OK;
}

void CGxDeviceMetal::ISceneEnd() {
    if (this->m_inScene) {
        this->m_d3dDevice->EndScene();
        this->m_inScene = 0;
    }
}

void CGxDeviceMetal::CapsWindowSize(CRect& rect) {
    rect = this->DeviceCurWindow();
}

void CGxDeviceMetal::IShaderCreate(CGxShader* shader) {
    if (shader->m_target == GxSh_Vertex) {
        this->IShaderCreateVertex(shader);
    } else if (shader->m_target == GxSh_Pixel) {
        this->IShaderCreatePixel(shader);
    }
}

void CGxDeviceMetal::CapsWindowSizeInScreenCoords(CRect& rect) {
    // Only the size is used; the client area's position on screen does not matter to the client
    rect = this->DeviceCurWindow();
}

void CGxDeviceMetal::XformSetProjection(const C44Matrix& matrix) {
    CGxDevice::XformSetProjection(matrix);

    C44Matrix projNative = matrix;

    if (!(fabs(projNative.c3 - 1.0f) < 0.00000023841858f) && !(fabs(projNative.c3) < 0.00000023841858f)) {
        projNative *= 1.0f / projNative.c3;
    }

    if (projNative.d3 == 0.0f) {
        float minZ = -(projNative.d2 / (projNative.c2 + 1.0f));
        float maxZ = -(projNative.d2 / (projNative.c2 - 1.0f));
        projNative.c2 = maxZ / (maxZ - minZ);
        projNative.d2 = maxZ * minZ / (minZ - maxZ);
    } else {
        float invC2 = 1.0f / projNative.c2;
        float minZ = (-1.0f - projNative.d2) * invC2;
        float maxZ = (1.0f - projNative.d2) * invC2;
        projNative.c2 = 1.0f / (maxZ - minZ);
        projNative.d2 = minZ / (minZ - maxZ);
    }

    if (!(this->m_appMasterEnables & (1 << GxMasterEnable_NormalProjection)) && projNative.d3 != 1.0f) {
        C44Matrix scale;
        scale.a0 = 0.2f;
        scale.b1 = 0.2f;
        scale.c2 = 0.2f;
        projNative = projNative * scale;
    }

    this->m_xforms[GxXform_Projection].m_dirty = 1;
    this->m_projNative = projNative;
}

void CGxDeviceMetal::XformSetView(const C44Matrix& matrix) {
    CGxDevice::XformSetView(matrix);
}

void CGxDeviceMetal::IShaderCreatePixel(CGxShader* shader) {
    shader->m_valid = 0;

    if (!this->m_context) {
        return;
    }

    shader->m_loaded = 1;

    PixelShader* pixelShader;

    if (shader->m_code.Count() && this->m_d3dDevice->CreatePixelShader(reinterpret_cast<const uint32_t*>(shader->m_code.m_data), &pixelShader, shader->m_code.Count()) == D3D_OK) {
        shader->m_apiSpecific = pixelShader;
        shader->m_valid = 1;
    }
}

void CGxDeviceMetal::IShaderCreateVertex(CGxShader* shader) {
    shader->m_valid = 0;

    if (!this->m_context) {
        return;
    }

    shader->m_loaded = 1;

    VertexShader* vertexShader;

    if (shader->m_code.Count() && this->m_d3dDevice->CreateVertexShader(reinterpret_cast<const uint32_t*>(shader->m_code.m_data), &vertexShader, shader->m_code.Count()) == D3D_OK) {
        shader->m_apiSpecific = vertexShader;
        shader->m_valid = 1;
    }
}

void CGxDeviceMetal::ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations) {
    CGxDevice::ShaderCreate(shaders, target, path, name, permutations);

    if (permutations == 1 && !shaders[0]->m_loaded) {
        if (target == GxSh_Vertex) {
            this->IShaderCreateVertex(shaders[0]);
        } else if (target == GxSh_Pixel) {
            this->IShaderCreatePixel(shaders[0]);
        }
    }
}

void CGxDeviceMetal::ShaderDestroy(CGxShader*& shader) {
    if (shader->m_target == GxSh_Vertex) {
        if (shader->m_refCount == 1) {
            if (shader->m_apiSpecific) {
                static_cast<VertexShader*>(shader->m_apiSpecific)->Release();
                shader->m_apiSpecific = nullptr;
            }

            shader->m_loaded = 0;
        }
    } else if (shader->m_target == GxSh_Pixel) {
        if (shader->m_refCount == 1) {
            if (shader->m_apiSpecific) {
                static_cast<PixelShader*>(shader->m_apiSpecific)->Release();
                shader->m_apiSpecific = nullptr;
            }

            shader->m_loaded = 0;
        }
    }

    CGxDevice::ShaderDestroy(shader);
}

void CGxDeviceMetal::DeviceUnk81(uint32_t a, uint32_t b) {
}

void CGxDeviceMetal::DeviceUnk82() {
}

void CGxDeviceMetal::DeviceUnk83(uint32_t a, uint32_t b) {
}

bool CGxDeviceMetal::GxVertexDecl::Compare(const D3DVERTEXELEMENT9* elements, uint32_t count) {
    if (count != this->m_count) {
        return false;
    }

    for (uint32_t i = 0; i < this->m_count; i++) {
        if (memcmp(&this->m_elements[i], &elements[i], sizeof(D3DVERTEXELEMENT9))) {
            return false;
        }
    }

    return true;
}

void CGxDeviceMetal::IStateSyncEnables() {
    if (this->m_appMasterEnables == this->m_hwMasterEnables) {
        return;
    }

    int32_t enable;

    if (this->NeedsUpdate(this->m_appMasterEnables, this->m_hwMasterEnables, 0, 0, GxMasterEnable_PolygonFill, enable)) {
        this->m_d3dDevice->SetRenderState(D3DRS_FILLMODE, enable ? D3DFILL_SOLID : D3DFILL_WIREFRAME);
    }

    this->m_hwMasterEnables = this->m_appMasterEnables;
}

void CGxDeviceMetal::IStateSyncClipPlanes() {
    if (!this->m_clipPlaneMask) {
        return;
    }

    for (uint32_t i = 0; i < 6; i++) {
        if (this->m_clipPlaneMask & (1 << i)) {
            this->m_d3dDevice->SetClipPlane(i, reinterpret_cast<const float*>(&this->m_clipPlane[i]));
        }
    }

    this->m_clipPlaneMask = 0;
}

void CGxDeviceMetal::IStateSyncScissorRect() {
    if (!this->m_scissorRectDirty) {
        return;
    }

    const CRect& window = this->DeviceCurWindow();
    Rect rect;

    rect.left = static_cast<int32_t>(this->m_scissorRect.minX * window.maxX + 0.5f);
    rect.right = static_cast<int32_t>(this->m_scissorRect.maxX * window.maxX + 1.0f);

    if (this->m_textureTarget[0].m_apiSpecific || this->m_textureTarget[1].m_apiSpecific) {
        rect.top = static_cast<int32_t>(this->m_scissorRect.minY * window.maxY + 0.5f);
        rect.bottom = static_cast<int32_t>(1.0f + this->m_scissorRect.maxY * window.maxY);
    } else {
        rect.top = static_cast<int32_t>((1.0f - this->m_scissorRect.maxY) * window.maxY + 0.5f);
        rect.bottom = static_cast<int32_t>(1.0f + (1.0f - this->m_scissorRect.minY) * window.maxY);
    }

    if (rect.left < 0) {
        rect.left = 0;
    }

    if (rect.top < 0) {
        rect.top = 0;
    }

    if (rect.right > window.maxX) {
        rect.right = static_cast<int32_t>(window.maxX);
    }

    if (rect.bottom > window.maxY) {
        rect.bottom = static_cast<int32_t>(window.maxY);
    }

    this->m_d3dDevice->SetScissorRect(&rect);
    this->m_scissorRectDirty = 0;
}

void CGxDeviceMetal::ISetVertexBuffer(uint32_t stream, VertexBuffer* buffer, uint32_t offset, uint32_t stride) {
    if (!this->m_caps.m_streamOffset) {
        offset = 0;
    }

    if (this->m_d3dVertexStreamBuf[stream] != buffer || this->m_d3dVertexStreamOfs[stream] != offset || this->m_d3dVertexStreamStride[stream] != stride) {
        this->m_d3dDevice->SetStreamSource(stream, buffer, offset, stride);
        this->m_d3dVertexStreamBuf[stream] = buffer;
        this->m_d3dVertexStreamOfs[stream] = offset;
        this->m_d3dVertexStreamStride[stream] = stride;
    }
}

void CGxDeviceMetal::DsSet(EDeviceState state, uint32_t value) {
    if (this->m_deviceStates[state] == value) {
        return;
    }

    switch (state) {
        case Ds_SrcBlend:
            this->m_d3dDevice->SetRenderState(D3DRS_SRCBLEND, value);
            break;

        case Ds_DstBlend:
            this->m_d3dDevice->SetRenderState(D3DRS_DESTBLEND, value);
            break;

        case Ds_TssMagFilter0:
        case Ds_TssMagFilter1:
        case Ds_TssMagFilter2:
        case Ds_TssMagFilter3:
        case Ds_TssMagFilter4:
        case Ds_TssMagFilter5:
        case Ds_TssMagFilter6:
        case Ds_TssMagFilter7:
        case Ds_TssMagFilter8:
        case Ds_TssMagFilter9:
        case Ds_TssMagFilter10:
        case Ds_TssMagFilter11:
        case Ds_TssMagFilter12:
        case Ds_TssMagFilter13:
        case Ds_TssMagFilter14:
        case Ds_TssMagFilter15:
            this->m_d3dDevice->SetSamplerState(state - Ds_TssMagFilter0, D3DSAMP_MAGFILTER, value);
            break;

        case Ds_TssMinFilter0:
        case Ds_TssMinFilter1:
        case Ds_TssMinFilter2:
        case Ds_TssMinFilter3:
        case Ds_TssMinFilter4:
        case Ds_TssMinFilter5:
        case Ds_TssMinFilter6:
        case Ds_TssMinFilter7:
        case Ds_TssMinFilter8:
        case Ds_TssMinFilter9:
        case Ds_TssMinFilter10:
        case Ds_TssMinFilter11:
        case Ds_TssMinFilter12:
        case Ds_TssMinFilter13:
        case Ds_TssMinFilter14:
        case Ds_TssMinFilter15:
            this->m_d3dDevice->SetSamplerState(state - Ds_TssMinFilter0, D3DSAMP_MINFILTER, value);
            break;

        case Ds_TssMipFilter0:
        case Ds_TssMipFilter1:
        case Ds_TssMipFilter2:
        case Ds_TssMipFilter3:
        case Ds_TssMipFilter4:
        case Ds_TssMipFilter5:
        case Ds_TssMipFilter6:
        case Ds_TssMipFilter7:
        case Ds_TssMipFilter8:
        case Ds_TssMipFilter9:
        case Ds_TssMipFilter10:
        case Ds_TssMipFilter11:
        case Ds_TssMipFilter12:
        case Ds_TssMipFilter13:
        case Ds_TssMipFilter14:
        case Ds_TssMipFilter15:
            this->m_d3dDevice->SetSamplerState(state - Ds_TssMipFilter0, D3DSAMP_MIPFILTER, value);
            break;

        case Ds_TssWrapU0:
        case Ds_TssWrapU1:
        case Ds_TssWrapU2:
        case Ds_TssWrapU3:
        case Ds_TssWrapU4:
        case Ds_TssWrapU5:
        case Ds_TssWrapU6:
        case Ds_TssWrapU7:
        case Ds_TssWrapU8:
        case Ds_TssWrapU9:
        case Ds_TssWrapU10:
        case Ds_TssWrapU11:
        case Ds_TssWrapU12:
        case Ds_TssWrapU13:
        case Ds_TssWrapU14:
        case Ds_TssWrapU15:
            this->m_d3dDevice->SetSamplerState(state - Ds_TssWrapU0, D3DSAMP_ADDRESSU, value);
            break;

        case Ds_TssWrapV0:
        case Ds_TssWrapV1:
        case Ds_TssWrapV2:
        case Ds_TssWrapV3:
        case Ds_TssWrapV4:
        case Ds_TssWrapV5:
        case Ds_TssWrapV6:
        case Ds_TssWrapV7:
        case Ds_TssWrapV8:
        case Ds_TssWrapV9:
        case Ds_TssWrapV10:
        case Ds_TssWrapV11:
        case Ds_TssWrapV12:
        case Ds_TssWrapV13:
        case Ds_TssWrapV14:
        case Ds_TssWrapV15:
            this->m_d3dDevice->SetSamplerState(state - Ds_TssWrapV0, D3DSAMP_ADDRESSV, value);
            break;

        case Ds_TssTexTransformFlags0:
        case Ds_TssTexTransformFlags1:
        case Ds_TssTexTransformFlags2:
        case Ds_TssTexTransformFlags3:
        case Ds_TssTexTransformFlags4:
        case Ds_TssTexTransformFlags5:
        case Ds_TssTexTransformFlags6:
        case Ds_TssTexTransformFlags7:
            this->m_d3dDevice->SetTextureStageState(state - Ds_TssTexTransformFlags0, D3DTSS_TEXTURETRANSFORMFLAGS, value);
            break;

        case Ds_TssMaxAnisotropy0:
        case Ds_TssMaxAnisotropy1:
        case Ds_TssMaxAnisotropy2:
        case Ds_TssMaxAnisotropy3:
        case Ds_TssMaxAnisotropy4:
        case Ds_TssMaxAnisotropy5:
        case Ds_TssMaxAnisotropy6:
        case Ds_TssMaxAnisotropy7:
        case Ds_TssMaxAnisotropy8:
        case Ds_TssMaxAnisotropy9:
        case Ds_TssMaxAnisotropy10:
        case Ds_TssMaxAnisotropy11:
        case Ds_TssMaxAnisotropy12:
        case Ds_TssMaxAnisotropy13:
        case Ds_TssMaxAnisotropy14:
        case Ds_TssMaxAnisotropy15:
            this->m_d3dDevice->SetSamplerState(state - Ds_TssMaxAnisotropy0, D3DSAMP_MAXANISOTROPY, value);
            break;

        case Ds_TssTexCoordIndex0:
        case Ds_TssTexCoordIndex1:
        case Ds_TssTexCoordIndex2:
        case Ds_TssTexCoordIndex3:
        case Ds_TssTexCoordIndex4:
        case Ds_TssTexCoordIndex5:
        case Ds_TssTexCoordIndex6:
        case Ds_TssTexCoordIndex7:
            this->m_d3dDevice->SetTextureStageState(state - Ds_TssTexCoordIndex0, D3DTSS_TEXCOORDINDEX, value);
            break;

        case Ds_TssColorOp0:
        case Ds_TssColorOp1:
        case Ds_TssColorOp2:
        case Ds_TssColorOp3:
        case Ds_TssColorOp4:
        case Ds_TssColorOp5:
        case Ds_TssColorOp6:
        case Ds_TssColorOp7:
            this->m_d3dDevice->SetTextureStageState(state - Ds_TssColorOp0, D3DTSS_COLOROP, value);
            break;

        case Ds_TssAlphaOp0:
        case Ds_TssAlphaOp1:
        case Ds_TssAlphaOp2:
        case Ds_TssAlphaOp3:
        case Ds_TssAlphaOp4:
        case Ds_TssAlphaOp5:
        case Ds_TssAlphaOp6:
        case Ds_TssAlphaOp7:
            this->m_d3dDevice->SetTextureStageState(state - Ds_TssAlphaOp0, D3DTSS_ALPHAOP, value);
            break;

        case Ds_TssColorArg10:
        case Ds_TssColorArg11:
        case Ds_TssColorArg12:
        case Ds_TssColorArg13:
        case Ds_TssColorArg14:
        case Ds_TssColorArg15:
        case Ds_TssColorArg16:
        case Ds_TssColorArg17:
            this->m_d3dDevice->SetTextureStageState(state - Ds_TssColorArg10, D3DTSS_COLORARG1, value);
            break;

        case Ds_TssColorArg20:
        case Ds_TssColorArg21:
        case Ds_TssColorArg22:
        case Ds_TssColorArg23:
        case Ds_TssColorArg24:
        case Ds_TssColorArg25:
        case Ds_TssColorArg26:
        case Ds_TssColorArg27:
            this->m_d3dDevice->SetTextureStageState(state - Ds_TssColorArg20, D3DTSS_COLORARG2, value);
            break;

        case Ds_TssAlphaArg10:
        case Ds_TssAlphaArg11:
        case Ds_TssAlphaArg12:
        case Ds_TssAlphaArg13:
        case Ds_TssAlphaArg14:
        case Ds_TssAlphaArg15:
        case Ds_TssAlphaArg16:
        case Ds_TssAlphaArg17:
            this->m_d3dDevice->SetTextureStageState(state - Ds_TssAlphaArg10, D3DTSS_ALPHAARG1, value);
            break;

        case Ds_TssAlphaArg20:
        case Ds_TssAlphaArg21:
        case Ds_TssAlphaArg22:
        case Ds_TssAlphaArg23:
        case Ds_TssAlphaArg24:
        case Ds_TssAlphaArg25:
        case Ds_TssAlphaArg26:
        case Ds_TssAlphaArg27:
            this->m_d3dDevice->SetTextureStageState(state - Ds_TssAlphaArg20, D3DTSS_ALPHAARG2, value);
            break;

        case Ds_AmbientMaterialSource:
            this->m_d3dDevice->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, value);
            break;

        case Ds_DiffuseMaterialSource:
            this->m_d3dDevice->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, value);
            break;

        case Ds_SpecularMaterialSource:
            this->m_d3dDevice->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, value);
            break;

        case Ds_EmissiveMaterialSource:
            this->m_d3dDevice->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, value);
            break;

        case Ds_Ambient:
            this->m_d3dDevice->SetRenderState(D3DRS_AMBIENT, value);
            break;

        case Ds_AlphaBlendEnable:
            this->m_d3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, value);
            break;

        case Ds_AlphaTestEnable:
            this->m_d3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, value);
            break;

        case Ds_AlphaRef:
            this->m_d3dDevice->SetRenderState(D3DRS_ALPHAREF, value);
            break;

        case Ds_FogEnable:
            this->m_d3dDevice->SetRenderState(D3DRS_FOGENABLE, value);
            break;

        case Ds_ZWriteEnable:
            this->m_d3dDevice->SetRenderState(D3DRS_ZWRITEENABLE, value);
            break;

        case Ds_ColorWriteEnable: {
            uint32_t d3dValue = (value & 0x1) != 0;

            if (value & 0x4) {
                d3dValue |= D3DCOLORWRITEENABLE_GREEN;
            }

            if (value & 0x2) {
                d3dValue |= D3DCOLORWRITEENABLE_BLUE;
            }

            if (value & 0x8) {
                d3dValue |= D3DCOLORWRITEENABLE_ALPHA;
            }

            this->m_d3dDevice->SetRenderState(D3DRS_COLORWRITEENABLE, d3dValue);
            break;
        }

        case Ds_Lighting:
            this->m_d3dDevice->SetRenderState(D3DRS_LIGHTING, value);
            break;

        case Ds_SpecularEnable:
            this->m_d3dDevice->SetRenderState(D3DRS_SPECULARENABLE, value);
            break;

        case Ds_CullMode:
            this->m_d3dDevice->SetRenderState(D3DRS_CULLMODE, value);
            break;

        case Ds_ClipPlaneEnable:
            this->m_d3dDevice->SetRenderState(D3DRS_CLIPPLANEENABLE, value);
            break;

        case Ds_ZFunc:
            this->m_d3dDevice->SetRenderState(D3DRS_ZFUNC, value);
            break;

        case Ds_PointScaleA:
            this->m_d3dDevice->SetRenderState(D3DRS_POINTSCALE_A, value);
            break;

        case Ds_PointScaleB:
            this->m_d3dDevice->SetRenderState(D3DRS_POINTSCALE_B, value);
            break;

        case Ds_PointScaleC:
            this->m_d3dDevice->SetRenderState(D3DRS_POINTSCALE_C, value);
            break;

        case Ds_PointScaleEnable:
            this->m_d3dDevice->SetRenderState(D3DRS_POINTSCALEENABLE, value);
            break;

        default:
            break;
    }

    this->m_deviceStates[state] = value;
}

void CGxDeviceMetal::ISetTexCoordIndex(uint32_t tmu, int32_t texGen, uint32_t coord) {
    EDeviceState state = static_cast<EDeviceState>(Ds_TssTexCoordIndex0 + tmu);

    switch (texGen) {
        case GxTexGen_Disabled:
            this->DsSet(state, coord);
            break;

        case GxTexGen_ObjectLinear:
        case GxTexGen_EyeLinear:
        case GxTexGen_Unk3:
            this->DsSet(state, tmu | D3DTSS_TCI_CAMERASPACEPOSITION);
            break;

        case GxTexGen_ReflectionMap:
        case GxTexGen_SphereMap:
            this->DsSet(state, tmu | D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
            break;

        case GxTexGen_Normal:
            this->DsSet(state, tmu | D3DTSS_TCI_CAMERASPACENORMAL);
            break;

        default:
            break;
    }
}

void CGxDeviceMetal::ISetColorOp(uint32_t tmu, uint32_t op) {
    if (tmu < static_cast<uint32_t>(this->m_caps.m_numTmus)) {
        this->DsSet(static_cast<EDeviceState>(Ds_TssColorOp0 + tmu), s_texOp[op]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssColorArg10 + tmu), s_texArgs[op][0]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssColorArg20 + tmu), s_texArgs[op][1]);
    }
}

void CGxDeviceMetal::ISetAlphaOp(uint32_t tmu, uint32_t op) {
    if (tmu < static_cast<uint32_t>(this->m_caps.m_numTmus)) {
        this->DsSet(static_cast<EDeviceState>(Ds_TssAlphaOp0 + tmu), s_texOp[op]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssAlphaArg10 + tmu), s_texArgs[op][0]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssAlphaArg20 + tmu), s_texArgs[op][1]);
    }
}

void CGxDeviceMetal::ISetMaterial(CImVector diffuse, CImVector emissive, CImVector specular, float specularExp) {
    s_d3dMaterial.Diffuse.r = diffuse.r * (1.0f / 255.0f);
    s_d3dMaterial.Diffuse.g = diffuse.g * (1.0f / 255.0f);
    s_d3dMaterial.Diffuse.b = diffuse.b * (1.0f / 255.0f);
    s_d3dMaterial.Diffuse.a = diffuse.a * (1.0f / 255.0f);
    s_d3dMaterial.Ambient = s_d3dMaterial.Diffuse;
    s_d3dMaterial.Emissive.r = emissive.r * (1.0f / 255.0f);
    s_d3dMaterial.Emissive.g = emissive.g * (1.0f / 255.0f);
    s_d3dMaterial.Emissive.b = emissive.b * (1.0f / 255.0f);
    s_d3dMaterial.Emissive.a = emissive.a * (1.0f / 255.0f);
    s_d3dMaterial.Specular.r = specular.r * (1.0f / 255.0f);
    s_d3dMaterial.Specular.g = specular.g * (1.0f / 255.0f);
    s_d3dMaterial.Specular.b = specular.b * (1.0f / 255.0f);
    s_d3dMaterial.Specular.a = specular.a * (1.0f / 255.0f);
    s_d3dMaterial.Power = specularExp;
    this->m_d3dDevice->SetMaterial(&s_d3dMaterial);
}

void CGxDeviceMetal::IStateSyncLights() {
    if (this->m_appRenderStates[GxRs_Lighting].m_value.m_data.i[0]) {
        if (this->m_ambientOnlyMode) {
            this->m_ambientOnlyMode = 0;
        }

        this->DsSet(Ds_Lighting, this->m_appMasterEnables & 0x1);
        this->DsSet(Ds_Ambient, 0);

        for (uint32_t i = 0; i < 4; i++) {
            CGxApiLight& light = this->m_lights[i];

            if (!light.m_flags) {
                continue;
            }

            if (light.m_flags & 0x1) {
                if (!light.m_enable) {
                    light.m_flags &= ~0x1;
                    this->m_d3dDevice->LightEnable(i, FALSE);
                    continue;
                }

                this->m_d3dDevice->LightEnable(i, TRUE);
            } else if (!light.m_enable) {
                continue;
            }

            if (light.m_dir.w == 1.0f) {
                s_d3dLight.Type = D3DLIGHT_POINT;
                s_d3dLight.Position.x = light.m_dir.x;
                s_d3dLight.Position.y = light.m_dir.y;
                s_d3dLight.Position.z = light.m_dir.z;
            } else {
                s_d3dLight.Type = D3DLIGHT_DIRECTIONAL;
                s_d3dLight.Direction.x = light.m_dir.x;
                s_d3dLight.Direction.y = light.m_dir.y;
                s_d3dLight.Direction.z = light.m_dir.z;
            }

            s_d3dLight.Diffuse.r = light.m_dirColor.x;
            s_d3dLight.Diffuse.g = light.m_dirColor.y;
            s_d3dLight.Diffuse.b = light.m_dirColor.z;
            s_d3dLight.Diffuse.a = 1.0f;
            s_d3dLight.Ambient.r = light.m_ambColor.x;
            s_d3dLight.Ambient.g = light.m_ambColor.y;
            s_d3dLight.Ambient.b = light.m_ambColor.z;
            s_d3dLight.Ambient.a = 0.0f;
            s_d3dLight.Specular.r = light.m_specColor.x;
            s_d3dLight.Specular.g = light.m_specColor.y;
            s_d3dLight.Specular.b = light.m_specColor.z;
            s_d3dLight.Specular.a = 0.0f;
            s_d3dLight.Range = 10000.0f;
            s_d3dLight.Falloff = 1.0f;
            s_d3dLight.Attenuation0 = light.m_constantAttenuation;
            s_d3dLight.Attenuation1 = light.m_linearAttenuation;
            s_d3dLight.Attenuation2 = light.m_quadraticAttenuation;
            this->m_d3dDevice->SetLight(i, &s_d3dLight);

            light.m_flags &= ~light.m_flags;
        }

        return;
    }

    if ((this->m_primVertexMask & (1 << GxVA_Color0)) || this->m_appRenderStates[GxRs_MatDiffuse].m_value.m_data.u[0] == 0xFFFFFFFF) {
        if (this->m_ambientOnlyMode) {
            this->m_ambientOnlyMode = 0;
        }

        this->DsSet(Ds_Lighting, 0);
        this->DsSet(Ds_Ambient, 0);
    } else {
        if (!this->m_ambientOnlyMode) {
            this->m_ambientOnlyMode = 1;

            for (uint32_t i = 0; i < 4; i++) {
                this->m_d3dDevice->LightEnable(i, FALSE);

                if (this->m_lights[i].m_enable) {
                    this->m_lights[i].m_flags |= 0x1;
                } else {
                    this->m_lights[i].m_flags &= ~0x1;
                }
            }
        }

        this->DsSet(Ds_Lighting, 1);
        this->DsSet(Ds_Ambient, 0xFFFFFFFF);
    }
}

void CGxDeviceMetal::IStateSyncMaterial() {
    uint32_t emissiveSource;

    if (this->m_primVertexMask & (1 << GxVA_Color0)) {
        int32_t colorMaterial = this->m_appRenderStates[GxRs_ColorMaterial].m_value.m_data.i[0];
        this->DsSet(Ds_AmbientMaterialSource, colorMaterial == 0);
        this->DsSet(Ds_DiffuseMaterialSource, colorMaterial == 0);
        this->DsSet(Ds_SpecularMaterialSource, colorMaterial == 1);
        emissiveSource = colorMaterial == 2;
    } else {
        emissiveSource = 0;
        this->DsSet(Ds_AmbientMaterialSource, 0);
        this->DsSet(Ds_DiffuseMaterialSource, 0);
        this->DsSet(Ds_SpecularMaterialSource, 0);
    }

    this->DsSet(Ds_EmissiveMaterialSource, emissiveSource);
}

void CGxDeviceMetal::IStateSyncXforms() {
    if (this->m_xforms[GxXform_Projection].m_dirty) {
        this->m_d3dDevice->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(&this->m_projNative));
        this->m_xforms[GxXform_Projection].m_dirty = 0;
    }

    if (this->m_xforms[GxXform_View].m_dirty) {
        CGxMatrixStack& view = this->m_xforms[GxXform_View];
        this->m_d3dDevice->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&view.m_mtx[view.m_level]));
        view.m_dirty = 0;
    }

    if (this->m_xforms[GxXform_World].m_dirty) {
        this->IXformSetWorld();
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(this->m_caps.m_numTmus); i++) {
        if (this->m_xforms[i].m_dirty || this->m_texGen[i].m_dirty) {
            this->IXformSetTex(i);
        }
    }
}

void CGxDeviceMetal::ISetTexture(uint32_t tmu, CGxTex* texId) {
    if (tmu >= 16) {
        return;
    }

    if (texId) {
        this->ITexMarkAsUpdated(texId);
        this->m_d3dDevice->SetTexture(tmu, static_cast<BaseTexture*>(texId->m_apiSpecificData));

        const D3DTEXTUREFILTERTYPE* filter = s_filterModes[texId->m_flags.m_filter];
        this->DsSet(static_cast<EDeviceState>(Ds_TssMinFilter0 + tmu), filter[0]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssMagFilter0 + tmu), filter[1]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssMipFilter0 + tmu), filter[2]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssWrapU0 + tmu), s_wrapModes[texId->m_flags.m_wrapU]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssWrapV0 + tmu), s_wrapModes[texId->m_flags.m_wrapV]);
        this->DsSet(static_cast<EDeviceState>(Ds_TssMaxAnisotropy0 + tmu), texId->m_flags.m_maxAnisotropy);

        if (tmu < 8) {
            if (!this->m_appRenderStates[GxRs_PixelShader].m_value.m_data.i[0] && !this->m_texEnabled[tmu]) {
                this->ISetColorOp(tmu, this->m_appRenderStates[GxRs_ColorOp0 + tmu].m_value.m_data.i[0]);
                this->ISetAlphaOp(tmu, this->m_appRenderStates[GxRs_AlphaOp0 + tmu].m_value.m_data.i[0]);
                this->m_appRenderStates[GxRs_ColorOp0 + tmu].m_dirty = 0;
                this->m_appRenderStates[GxRs_AlphaOp0 + tmu].m_dirty = 0;
            }

            this->m_texEnabled[tmu] = true;
        }
    } else {
        this->m_d3dDevice->SetTexture(tmu, nullptr);

        if (tmu < 8) {
            if (!this->m_appRenderStates[GxRs_PixelShader].m_value.m_data.i[0] && this->m_texEnabled[tmu]) {
                this->DsSet(static_cast<EDeviceState>(Ds_TssColorOp0 + tmu), D3DTOP_DISABLE);
                this->DsSet(static_cast<EDeviceState>(Ds_TssAlphaOp0 + tmu), D3DTOP_DISABLE);
            }

            this->m_texEnabled[tmu] = false;
        }
    }
}

void CGxDeviceMetal::ISetTexCoord(uint32_t tmu, uint32_t coord) {
    if (tmu < static_cast<uint32_t>(this->m_caps.m_numTmus)) {
        this->ISetTexCoordIndex(tmu, this->m_appRenderStates[GxRs_TexGen0 + tmu].m_value.m_data.i[0], coord);
    }
}

void CGxDeviceMetal::ISetTexGen(uint32_t tmu, int32_t texGen) {
    if (tmu >= static_cast<uint32_t>(this->m_caps.m_numTmus)) {
        return;
    }

    this->ISetTexCoordIndex(tmu, texGen, this->m_appRenderStates[GxRs_TextureCoord0 + tmu].m_value.m_data.i[0]);

    C44Matrix matrix;

    if (texGen == GxTexGen_ObjectLinear || texGen == GxTexGen_EyeLinear) {
        CGxMatrixStack& view = this->m_xforms[GxXform_View];
        matrix = view.m_mtx[view.m_level];
        matrix = matrix.AffineInverse();

        if (texGen == GxTexGen_ObjectLinear) {
            CGxMatrixStack& world = this->m_xforms[GxXform_World];
            matrix = matrix * world.m_mtx[world.m_level].Inverse();
        }
    } else if (texGen == GxTexGen_SphereMap) {
        matrix.a0 = 0.5f;
        matrix.b1 = 0.5f;
        matrix.d0 = 0.5f;
        matrix.d1 = 0.5f;
    } else {
        this->m_texGen[tmu].Identity();
        return;
    }

    CGxMatrixStack& stack = this->m_texGen[tmu];
    stack.m_dirty = 1;
    stack.m_flags[stack.m_level] &= ~CGxMatrixStack::F_Identity;
    stack.m_mtx[stack.m_level] = matrix;
}

void CGxDeviceMetal::IRsSendToHw(EGxRenderState which) {
    CGxStateBom& value = this->m_appRenderStates[which].m_value;

    switch (which) {
        case GxRs_PolygonOffset:
            if (this->m_caps.m_depthBias) {
                float bias = -value.m_data.f[0];
                this->m_d3dDevice->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<uint32_t*>(&bias));
            }

            break;

        case GxRs_MatDiffuse:
        case GxRs_MatEmissive:
        case GxRs_MatSpecular:
        case GxRs_MatSpecularExp: {
            CImVector diffuse;
            CImVector emissive;
            CImVector specular;
            *reinterpret_cast<uint32_t*>(&diffuse) = this->m_appRenderStates[GxRs_MatDiffuse].m_value.m_data.u[0];
            *reinterpret_cast<uint32_t*>(&emissive) = this->m_appRenderStates[GxRs_MatEmissive].m_value.m_data.u[0];
            *reinterpret_cast<uint32_t*>(&specular) = this->m_appRenderStates[GxRs_MatSpecular].m_value.m_data.u[0];
            this->ISetMaterial(diffuse, emissive, specular, this->m_appRenderStates[GxRs_MatSpecularExp].m_value.m_data.f[0]);

            this->m_appRenderStates[GxRs_MatDiffuse].m_dirty = 0;
            this->m_appRenderStates[GxRs_MatEmissive].m_dirty = 0;
            this->m_appRenderStates[GxRs_MatSpecular].m_dirty = 0;
            this->m_appRenderStates[GxRs_MatSpecularExp].m_dirty = 0;

            uint32_t specularEnable = 0;

            if (this->m_appRenderStates[GxRs_MatSpecularExp].m_value.m_data.f[0] > 0.0f && this->m_appRenderStates[GxRs_MatSpecular].m_value.m_data.i[0]) {
                specularEnable = 1;
            }

            this->DsSet(Ds_SpecularEnable, specularEnable);
            break;
        }

        case GxRs_NormalizeNormals:
            if (s_normalizeNormals != value.m_data.u[0]) {
                this->m_d3dDevice->SetRenderState(D3DRS_NORMALIZENORMALS, value.m_data.u[0]);
                s_normalizeNormals = value.m_data.u[0];
            }

            break;

        case GxRs_BlendingMode: {
            uint32_t mode = value.m_data.u[0];

            if (mode < 2) {
                this->DsSet(Ds_AlphaBlendEnable, 0);
            } else {
                this->DsSet(Ds_AlphaBlendEnable, 1);
                this->DsSet(Ds_SrcBlend, s_srcBlend[mode]);
                this->DsSet(Ds_DstBlend, s_dstBlend[mode]);
            }

            break;
        }

        case GxRs_AlphaRef: {
            int32_t alphaRef = value.m_data.i[0];

            if (alphaRef > 0) {
                this->DsSet(Ds_AlphaRef, alphaRef);
                this->DsSet(Ds_AlphaTestEnable, 1);
            } else {
                this->DsSet(Ds_AlphaTestEnable, 0);
            }

            break;
        }

        case GxRs_FogStart:
            this->m_d3dDevice->SetRenderState(D3DRS_FOGSTART, value.m_data.u[0]);
            break;

        case GxRs_FogEnd:
            this->m_d3dDevice->SetRenderState(D3DRS_FOGEND, value.m_data.u[0]);
            break;

        case GxRs_FogColor:
            this->m_d3dDevice->SetRenderState(D3DRS_FOGCOLOR, value.m_data.u[0]);
            break;

        case GxRs_Lighting: {
            uint32_t enable = 0;

            if (this->m_appMasterEnables & 0x1) {
                enable = value.m_data.i[0] != 0;
            }

            this->DsSet(Ds_Lighting, enable);
            break;
        }

        case GxRs_Fog: {
            uint32_t enable = 0;

            if (this->m_appMasterEnables & 0x2) {
                enable = value.m_data.i[0] != 0;
            }

            this->DsSet(Ds_FogEnable, enable);
            break;
        }

        case GxRs_DepthTest:
        case GxRs_DepthFunc: {
            uint32_t func;

            if ((this->m_appMasterEnables & 0x4) && this->m_appRenderStates[GxRs_DepthTest].m_value.m_data.i[0]) {
                func = s_cmpFunc[this->m_appRenderStates[GxRs_DepthFunc].m_value.m_data.i[0]];
            } else {
                func = D3DCMP_ALWAYS;
            }

            this->DsSet(Ds_ZFunc, func);
            this->m_appRenderStates[GxRs_DepthTest].m_dirty = 0;
            this->m_appRenderStates[GxRs_DepthFunc].m_dirty = 0;
            break;
        }

        case GxRs_DepthWrite: {
            uint32_t enable = 0;

            if (this->m_appMasterEnables & 0x8) {
                enable = value.m_data.i[0] != 0;
            }

            this->DsSet(Ds_ZWriteEnable, enable);
            break;
        }

        case GxRs_ColorWrite: {
            uint32_t mask = 0;

            if (this->m_appMasterEnables & 0x10) {
                mask = value.m_data.u[0];
            }

            this->DsSet(Ds_ColorWriteEnable, mask);
            break;
        }

        case GxRs_Culling: {
            uint32_t cull = 0;

            if (this->m_appMasterEnables & 0x20) {
                cull = value.m_data.i[0];

                if (value.m_data.i[0] > 2) {
                    cull = 2;
                }
            }

            this->DsSet(Ds_CullMode, s_cullMode[cull]);
            break;
        }

        case GxRs_ClipPlaneMask:
            this->DsSet(Ds_ClipPlaneEnable, value.m_data.u[0]);
            break;

        case GxRs_Multisample:
            this->m_d3dDevice->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, value.m_data.i[0] != 0);
            break;

        case GxRs_ScissorTest:
            this->m_d3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, value.m_data.i[0] != 0);
            break;

        case GxRs_Texture0:
        case GxRs_Texture1:
        case GxRs_Texture2:
        case GxRs_Texture3:
        case GxRs_Texture4:
        case GxRs_Texture5:
        case GxRs_Texture6:
        case GxRs_Texture7:
        case GxRs_Texture8:
        case GxRs_Texture9:
        case GxRs_Texture10:
        case GxRs_Texture11:
        case GxRs_Texture12:
        case GxRs_Texture13:
        case GxRs_Texture14:
        case GxRs_Texture15:
            this->ISetTexture(which - GxRs_Texture0, static_cast<CGxTex*>(value.m_data.p));
            break;

        case GxRs_ColorOp0:
        case GxRs_ColorOp1:
        case GxRs_ColorOp2:
        case GxRs_ColorOp3:
        case GxRs_ColorOp4:
        case GxRs_ColorOp5:
        case GxRs_ColorOp6:
        case GxRs_ColorOp7:
            this->ISetColorOp(which - GxRs_ColorOp0, value.m_data.u[0]);
            break;

        case GxRs_AlphaOp0:
        case GxRs_AlphaOp1:
        case GxRs_AlphaOp2:
        case GxRs_AlphaOp3:
        case GxRs_AlphaOp4:
        case GxRs_AlphaOp5:
        case GxRs_AlphaOp6:
        case GxRs_AlphaOp7:
            this->ISetAlphaOp(which - GxRs_AlphaOp0, value.m_data.u[0]);
            break;

        case GxRs_TexGen0:
        case GxRs_TexGen1:
        case GxRs_TexGen2:
        case GxRs_TexGen3:
        case GxRs_TexGen4:
        case GxRs_TexGen5:
        case GxRs_TexGen6:
        case GxRs_TexGen7:
            this->ISetTexGen(which - GxRs_TexGen0, value.m_data.i[0]);
            break;

        case GxRs_TextureCoord0:
        case GxRs_TextureCoord1:
        case GxRs_TextureCoord2:
        case GxRs_TextureCoord3:
        case GxRs_TextureCoord4:
        case GxRs_TextureCoord5:
        case GxRs_TextureCoord6:
        case GxRs_TextureCoord7:
            this->ISetTexCoord(which - GxRs_TextureCoord0, value.m_data.u[0]);
            break;

        case GxRs_VertexShader:
            this->IBindVertexShader(static_cast<CGxShader*>(value.m_data.p));
            break;

        case GxRs_PixelShader:
            this->IBindPixelShader(static_cast<CGxShader*>(value.m_data.p));
            break;

        case GxRs_PointScale: {
            float pointScale = value.m_data.f[0];
            const C3Vector& attenuation = *reinterpret_cast<const C3Vector*>(this->m_appRenderStates[GxRs_PointScaleAttenuation].m_value.m_data.f);

            if (attenuation != CGxDevice::s_pointScaleAttenuation) {
                const CRect& window = this->DeviceCurWindow();
                pointScale = pointScale / ((1.0f - this->m_viewport.m_y.m_l) * window.maxY - (1.0f - this->m_viewport.m_y.m_h) * window.maxY);
            }

            this->m_d3dDevice->SetRenderState(D3DRS_POINTSIZE, *reinterpret_cast<uint32_t*>(&pointScale));
            break;
        }

        case GxRs_PointScaleAttenuation: {
            const C3Vector& attenuation = *reinterpret_cast<const C3Vector*>(value.m_data.f);
            uint32_t enable = attenuation != CGxDevice::s_pointScaleAttenuation;

            this->DsSet(Ds_PointScaleEnable, enable);

            if (enable) {
                this->DsSet(Ds_PointScaleA, value.m_data.u[0]);
                this->DsSet(Ds_PointScaleB, value.m_data.u[1]);
                this->DsSet(Ds_PointScaleC, value.m_data.u[2]);
            }

            break;
        }

        case GxRs_PointScaleMin:
            this->m_d3dDevice->SetRenderState(D3DRS_POINTSIZE_MIN, value.m_data.u[0]);
            break;

        case GxRs_PointScaleMax:
            this->m_d3dDevice->SetRenderState(D3DRS_POINTSIZE_MAX, value.m_data.u[0]);
            break;

        case GxRs_PointSprite:
            this->m_d3dDevice->SetRenderState(D3DRS_POINTSPRITEENABLE, value.m_data.i[0] != 0);
            break;

        case Unk84: {
            uint32_t factor = static_cast<uint32_t>(value.m_data.f[0] * 255.0f);
            this->m_d3dDevice->SetRenderState(D3DRS_BLENDFACTOR, (((((factor << 8) | factor) << 8) | factor) << 8) | factor);
            break;
        }

        default:
            break;
    }
}

VertexDeclaration* CGxDeviceMetal::ICreateD3dVertexDecl(const D3DVERTEXELEMENT9* elements, uint32_t count) {
    if (this->m_primVertexFormat < GxVertexBufferFormats_Last) {
        if (!this->m_d3dVertexDecl[this->m_primVertexFormat]) {
            this->m_d3dDevice->CreateVertexDeclaration(elements, &this->m_d3dVertexDecl[this->m_primVertexFormat]);
        }

        return this->m_d3dVertexDecl[this->m_primVertexFormat];
    }

    for (uint32_t i = 0; i < this->m_gxVertexDecl.Count(); i++) {
        if (this->m_gxVertexDecl[i].Compare(elements, count)) {
            return this->m_gxVertexDecl[i].m_d3dDecl;
        }
    }

    GxVertexDecl* decl = this->m_gxVertexDecl.New();
    decl->m_count = count;
    memcpy(decl->m_elements, elements, sizeof(D3DVERTEXELEMENT9) * count);
    this->m_d3dDevice->CreateVertexDeclaration(elements, &decl->m_d3dDecl);

    return decl->m_d3dDecl;
}

void CGxDeviceMetal::IStateSyncVertexPtrs() {
    if (this->m_primVertexFormat < GxVertexBufferFormats_Last && this->m_d3dVertexDecl[this->m_primVertexFormat]) {
        VertexDeclaration* decl = this->m_d3dVertexDecl[this->m_primVertexFormat];
        CGxBuf* buf = this->m_primVertexBuf;

        if (this->m_d3dCurrentVertexDecl != decl) {
            this->m_d3dDevice->SetVertexDeclaration(decl);
            this->m_d3dCurrentVertexDecl = decl;
        }

        this->ISetVertexBuffer(0, static_cast<VertexBuffer*>(buf->m_pool->m_apiSpecific), buf->m_index, this->m_primVertexSize);

        return;
    }

    CGxBuf** streamBufs = static_cast<CGxBuf**>(_alloca(sizeof(CGxBuf*) * GxVAs_Last));
    uint32_t* streamStrides = static_cast<uint32_t*>(_alloca(sizeof(uint32_t) * GxVAs_Last));
    uint32_t streamCount = 0;

    memset(streamBufs, 0, sizeof(CGxBuf*) * GxVAs_Last);
    memset(streamStrides, 0, sizeof(uint32_t) * GxVAs_Last);

    D3DVERTEXELEMENT9 elements[GxVAs_Last + 1];
    uint32_t elementCount = 0;

    for (uint32_t i = 0; i < GxVAs_Last; i++) {
        if (!(this->m_primVertexMask & (1 << i))) {
            continue;
        }

        uint32_t stream;

        for (stream = 0; stream < streamCount; stream++) {
            if (streamBufs[stream] == this->m_primVertexFormatBuf[i]) {
                break;
            }
        }

        if (stream == streamCount) {
            streamBufs[streamCount++] = this->m_primVertexFormatBuf[i];
        }

        const CGxVertexAttrib& attrib = this->m_primVertexFormatAttrib[i];
        streamStrides[stream] += s_gxAttribToD3dAttribSize[attrib.m_type];

        D3DVERTEXELEMENT9& element = elements[elementCount++];
        element.Stream = static_cast<uint16_t>(stream);
        element.Offset = static_cast<uint16_t>(attrib.m_offset);
        element.Type = static_cast<uint8_t>(s_gxAttribToD3dAttribType[attrib.m_type]);
        element.Method = D3DDECLMETHOD_DEFAULT;
        element.Usage = static_cast<uint8_t>(s_gxAttribToD3dAttribUsage[attrib.m_attrib]);
        element.UsageIndex = static_cast<uint8_t>(s_gxAttribToD3dAttribUsageIndex[attrib.m_attrib]);
    }

    D3DVERTEXELEMENT9 end = D3DDECL_END();
    elements[elementCount] = end;

    VertexDeclaration* decl = this->ICreateD3dVertexDecl(elements, elementCount + 1);

    if (this->m_d3dCurrentVertexDecl != decl) {
        this->m_d3dDevice->SetVertexDeclaration(decl);
        this->m_d3dCurrentVertexDecl = decl;
    }

    for (uint32_t stream = 0; stream < streamCount; stream++) {
        CGxBuf* buf = streamBufs[stream];
        this->ISetVertexBuffer(stream, static_cast<VertexBuffer*>(buf->m_pool->m_apiSpecific), buf->m_index, streamStrides[stream]);
    }
}

void CGxDeviceMetal::IStateSync() {
    int32_t hwVertexShader = this->m_hwRenderStates[GxRs_VertexShader].m_data.i[0];

    this->IShaderConstantsFlush();
    this->IRsSync(0);

    if (!hwVertexShader || !this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.i[0]) {
        this->IStateSyncLights();
        this->IStateSyncMaterial();
        this->IStateSyncXforms();
    }

    this->IStateSyncEnables();
    this->IStateSyncClipPlanes();
    this->IStateSyncScissorRect();
    this->IStateSyncVertexPtrs();

    if (this->m_primIndexDirty) {
        this->m_primIndexDirty = 0;

        IndexBuffer* indexBuffer = static_cast<IndexBuffer*>(this->m_primIndexBuf->m_pool->m_apiSpecific);

        if (this->m_d3dCurrentIndexBuf != indexBuffer) {
            this->m_d3dDevice->SetIndices(indexBuffer);
            this->m_d3dCurrentIndexBuf = indexBuffer;
        }
    }

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }
}

void CGxDeviceMetal::IXformSetWorld() {
    CGxMatrixStack& world = this->m_xforms[GxXform_World];

    if (!s_isIdent || !(world.m_flags[world.m_level] & CGxMatrixStack::F_Identity)) {
        this->m_d3dDevice->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&world.m_mtx[world.m_level]));
    }

    int32_t isIdent = world.m_flags[world.m_level] & CGxMatrixStack::F_Identity;
    world.m_dirty = 0;
    s_isIdent = isIdent;
}

void CGxDeviceMetal::IXformSetTex(uint32_t tmu) {
    uint32_t flags = D3DTTFF_DISABLE;
    D3DTRANSFORMSTATETYPE state = static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + tmu);
    CGxMatrixStack& texGen = this->m_texGen[tmu];
    CGxMatrixStack& xform = this->m_xforms[tmu];

    switch (g_theGxDevicePtr->m_appRenderStates[GxRs_TextureShader0 + tmu].m_value.m_data.i[0]) {
        case 0:
            if (texGen.m_flags[texGen.m_level] & CGxMatrixStack::F_Identity) {
                this->m_d3dDevice->SetTransform(state, reinterpret_cast<const D3DMATRIX*>(&texGen.m_mtx[texGen.m_level]));
            } else {
                this->m_d3dDevice->SetTransform(state, reinterpret_cast<const D3DMATRIX*>(&texGen.m_mtx[texGen.m_level]));
                flags = D3DTTFF_COUNT3;
            }

            break;

        case 1: {
            C44Matrix product = texGen.m_mtx[texGen.m_level] * xform.m_mtx[xform.m_level];
            C44Matrix matrix = product;

            if (this->m_appRenderStates[GxRs_TexGen0 + tmu].m_value.m_data.i[0]) {
                flags = D3DTTFF_COUNT3;
            } else {
                flags = D3DTTFF_COUNT2;
                matrix.c0 = product.d0;
                matrix.c1 = product.d1;
            }

            this->m_d3dDevice->SetTransform(state, reinterpret_cast<const D3DMATRIX*>(&matrix));
            break;
        }

        case 2: {
            C44Matrix matrix = texGen.m_mtx[texGen.m_level] * xform.m_mtx[xform.m_level];
            this->m_d3dDevice->SetTransform(state, reinterpret_cast<const D3DMATRIX*>(&matrix));
            flags = D3DTTFF_COUNT3 | D3DTTFF_PROJECTED;
            break;
        }

        default:
            break;
    }

    this->DsSet(static_cast<EDeviceState>(Ds_TssTexTransformFlags0 + tmu), flags);
    xform.m_dirty = 0;
    texGen.m_dirty = 0;
}

void CGxDeviceMetal::IBindPixelShader(CGxShader* shader) {
    if (shader) {
        if (!shader->m_loaded) {
            this->IShaderCreatePixel(shader);
        }

        this->m_d3dDevice->SetPixelShader(static_cast<PixelShader*>(shader->m_apiSpecific));
        return;
    }

    this->m_d3dDevice->SetPixelShader(nullptr);

    for (uint32_t i = 0; i < 8; i++) {
        if (this->m_texEnabled[i]) {
            this->ISetColorOp(i, this->m_appRenderStates[GxRs_ColorOp0 + i].m_value.m_data.u[0]);
            this->ISetAlphaOp(i, this->m_appRenderStates[GxRs_AlphaOp0 + i].m_value.m_data.u[0]);
        } else {
            this->DsSet(static_cast<EDeviceState>(Ds_TssColorOp0 + i), D3DTOP_DISABLE);
            this->DsSet(static_cast<EDeviceState>(Ds_TssAlphaOp0 + i), D3DTOP_DISABLE);
        }

        this->m_appRenderStates[GxRs_ColorOp0 + i].m_dirty = 0;
        this->m_appRenderStates[GxRs_AlphaOp0 + i].m_dirty = 0;
    }
}

void CGxDeviceMetal::IXformSetViewport() {
    const CRect& window = this->DeviceCurWindow();
    D3DVIEWPORT9 viewport;

    viewport.X = static_cast<uint32_t>(this->m_viewport.m_x.m_l * window.maxX + 0.5f);
    viewport.Y = static_cast<uint32_t>((1.0f - this->m_viewport.m_y.m_h) * window.maxY + 0.5f);
    viewport.Width = static_cast<uint32_t>(this->m_viewport.m_x.m_h * window.maxX - viewport.X + 0.5f);
    viewport.Height = static_cast<uint32_t>((1.0f - this->m_viewport.m_y.m_l) * window.maxY - viewport.Y + 0.5f);
    viewport.MinZ = this->m_viewport.m_z.m_l;
    viewport.MaxZ = this->m_viewport.m_z.m_h;

    if (this->m_textureTarget[0].m_apiSpecific || this->m_textureTarget[1].m_apiSpecific) {
        viewport.Y = static_cast<uint32_t>(window.maxY * this->m_viewport.m_y.m_l);
    }

    this->m_d3dDevice->SetViewport(&viewport);
    this->m_viewportDirty = 0;
}

void CGxDeviceMetal::IShaderConstantsFlush() {
    ShadowConstants& vs = CGxDevice::s_shadowConstants[1];

    if (vs.dirtyMin <= vs.dirtyMax) {
        this->m_d3dDevice->SetVertexShaderConstantF(vs.dirtyMin, &vs.constants[vs.dirtyMin * 4], vs.dirtyMax - vs.dirtyMin + 1);
    }

    vs.dirtyMin = 255;
    vs.dirtyMax = 0;

    ShadowConstants& ps = CGxDevice::s_shadowConstants[0];

    if (ps.dirtyMin <= ps.dirtyMax) {
        this->m_d3dDevice->SetPixelShaderConstantF(ps.dirtyMin, &ps.constants[ps.dirtyMin * 4], ps.dirtyMax - ps.dirtyMin + 1);
    }

    ps.dirtyMax = 0;
    ps.dirtyMin = 255;
}

void CGxDeviceMetal::IBindVertexShader(CGxShader* shader) {
    if (shader) {
        if (!shader->m_loaded) {
            this->IShaderCreateVertex(shader);
        }

        this->m_d3dDevice->SetVertexShader(static_cast<VertexShader*>(shader->m_apiSpecific));
    } else {
        this->m_d3dDevice->SetVertexShader(nullptr);
    }
}

void CGxDeviceMetal::ISetPresentParms(PresentParameters& d3dpp, const CGxFormat& format) {
    memset(&d3dpp, 0, sizeof(d3dpp));

    // The back buffer is the window's client area in pixels, windowed or fullscreen: a fullscreen
    // window fixes its drawable size to the format's resolution (see ICreateWindow)
    const CRect& window = this->DeviceCurWindow();
    d3dpp.Windowed = format.m_window;
    d3dpp.BackBufferWidth = static_cast<uint32_t>(window.maxX);
    d3dpp.BackBufferHeight = static_cast<uint32_t>(window.maxY);
    d3dpp.BackBufferFormat = format.m_window ? D3DFMT_X8R8G8B8 : s_GxFormatToD3dFormat[format.m_colorFormat];
    d3dpp.BackBufferCount = format.m_vsync ? format.m_backBufferCount : 1;
    d3dpp.FullScreen_RefreshRateInHz = format.m_window ? 0 : format.m_refreshRate;
    d3dpp.Layer = OsMacGetMetalLayer(this->m_hwnd);
    d3dpp.EnableAutoDepthStencil = 1;
    d3dpp.AutoDepthStencilFormat = s_GxFormatToD3dFormat[format.m_depthFormat];

    // Presentation waits for vertical blanks only for vsync; compositing windows present once
    // per refresh like the windowed path of Direct3D 9
    d3dpp.PresentationInterval = format.m_vsync ? 1 : 0;

    if (format.m_multisampleCount <= 1) {
        return;
    }

    d3dpp.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(format.m_multisampleCount);

    uint32_t qualityLevels;

    if (FAILED(this->m_d3d->CheckDeviceMultiSampleType(0, d3dpp.BackBufferFormat, format.m_window, d3dpp.MultiSampleType, &qualityLevels))) {
        // Fall back to the largest supported count below the requested one
        uint32_t samples = format.m_multisampleCount;
        while (samples > 1 && FAILED(this->m_d3d->CheckDeviceMultiSampleType(0, d3dpp.BackBufferFormat, format.m_window, static_cast<D3DMULTISAMPLE_TYPE>(samples), &qualityLevels))) {
            samples >>= 1;
        }
        d3dpp.MultiSampleType = samples > 1 ? static_cast<D3DMULTISAMPLE_TYPE>(samples) : D3DMULTISAMPLE_NONE;
    }
}

int32_t CGxDeviceMetal::ICreateWindow(CGxFormat& format) {
    OsMacWindowParams params;
    params.m_width = format.m_size.x;
    params.m_height = format.m_size.y;
    params.m_fullscreen = !format.m_window;
    params.m_maximize = format.m_window && format.m_maximize == 1;

    this->m_hwnd = OsMacCreateGameWindow(params, "World of Warcraft");

    if (!this->m_hwnd) {
        return 0;
    }

    // Fullscreen renders at the format's resolution and the compositor scales it to the display;
    // a window renders at its own size in pixels
    if (!format.m_window && format.m_size.x > 0 && format.m_size.y > 0) {
        OsMacSetDrawableSize(this->m_hwnd, format.m_size.x, format.m_size.y);
    } else {
        OsMacSetDrawableSize(this->m_hwnd, 0, 0);
    }

    int32_t width;
    int32_t height;
    OsMacGetClientSize(this->m_hwnd, &width, &height);

    if (format.m_window) {
        format.m_size.x = width;
        format.m_size.y = height;
    }

    format.m_pos.x = 0;
    format.m_pos.y = 0;

    // Creating a window sends WM_SIZE on Windows, which sets the client area
    CRect rect;
    rect.minX = 0.0f;
    rect.minY = 0.0f;
    rect.maxX = static_cast<float>(width);
    rect.maxY = static_cast<float>(height);
    this->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 0);

    return 1;
}

void CGxDeviceMetal::ISetCaps(const CGxFormat& format) {
    this->m_caps.m_numTmus = this->m_d3dCaps.MaxSimultaneousTextures >= 8 ? 8 : this->m_d3dCaps.MaxSimultaneousTextures;
    this->m_caps.m_pixelCenterOnEdge = 0;
    this->m_caps.m_texelCenterOnEdge = 1;

    uint32_t maxTextureSize = this->m_d3dCaps.MaxTextureWidth;
    this->m_caps.m_texMaxSize[GxTex_2d] = maxTextureSize >= 256 ? maxTextureSize : 256;
    this->m_caps.m_texMaxSize[GxTex_CubeMap] = maxTextureSize >= 256 ? maxTextureSize : 256;
    this->m_caps.m_texMaxSize[GxTex_Rectangle] = maxTextureSize >= 256 ? maxTextureSize : 256;
    this->m_caps.m_texMaxSize[GxTex_NonPow2] = maxTextureSize >= 256 ? maxTextureSize : 256;
    this->m_caps.m_maxIndex = this->m_d3dCaps.MaxVertexIndex;

    uint32_t filterCaps = this->m_d3dCaps.TextureFilterCaps;
    this->m_caps.m_texFilterTrilinear = (filterCaps & D3DPTFILTERCAPS_MIPFLINEAR) != 0;
    this->m_caps.m_texFilterAnisotropic = (filterCaps & (D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MAGFANISOTROPIC)) != 0;

    if (filterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) {
        s_filterModes[GxTex_Anisotropic][0] = D3DTEXF_ANISOTROPIC;
    }

    if (this->m_d3dCaps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC) {
        s_filterModes[GxTex_Anisotropic][1] = D3DTEXF_ANISOTROPIC;
    }

    this->m_caps.m_maxTexAnisotropy = this->m_d3dCaps.MaxAnisotropy;

    if (this->m_caps.m_texFilterAnisotropic && this->m_d3dCaps.MaxAnisotropy < 2) {
        this->m_caps.m_texFilterAnisotropic = 0;
    }

    this->m_caps.m_depthBias = (this->m_d3dCaps.RasterCaps & D3DPRASTERCAPS_DEPTHBIAS) != 0;
    this->m_caps.m_numStreams = this->m_d3dCaps.MaxStreams;
    this->m_caps.m_streamOffset = this->m_d3dCaps.Caps2 & 0x1;

    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        this->m_caps.m_shaderTargets[i] = 0;
        this->m_caps.m_shaderConstants[i] = 0;
    }

    uint32_t pixelShaderVersion = this->m_d3dCaps.PixelShaderVersion;

    if (pixelShaderVersion >= D3DPS_VERSION(3, 0)) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = 4;
    } else if (pixelShaderVersion >= D3DPS_VERSION(2, 0)) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = 3;
    } else if (pixelShaderVersion >= D3DPS_VERSION(1, 4)) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = 2;
    } else if (pixelShaderVersion >= D3DPS_VERSION(1, 1)) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = 1;
    }

    if (this->m_caps.m_shaderTargets[GxSh_Pixel]) {
        uint32_t vertexShaderVersion = this->m_d3dCaps.VertexShaderVersion;

        if (vertexShaderVersion >= D3DVS_VERSION(3, 0)) {
            this->m_caps.m_shaderTargets[GxSh_Vertex] = 3;
        } else if (vertexShaderVersion >= D3DVS_VERSION(2, 0)) {
            this->m_caps.m_shaderTargets[GxSh_Vertex] = 2;
        } else if (vertexShaderVersion == D3DVS_VERSION(1, 1)) {
            this->m_caps.m_shaderTargets[GxSh_Vertex] = 1;
        }

        this->m_caps.m_shaderConstants[GxSh_Vertex] = this->m_d3dCaps.MaxVertexShaderConst;
    }

    if (format.m_fixedFunction[GxSh_Pixel] != -1 && format.m_fixedFunction[GxSh_Pixel] <= this->m_caps.m_shaderTargets[GxSh_Pixel]) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = format.m_fixedFunction[GxSh_Pixel];
    }

    if (format.m_fixedFunction[GxSh_Vertex] != -1 && format.m_fixedFunction[GxSh_Vertex] <= this->m_caps.m_shaderTargets[GxSh_Vertex]) {
        this->m_caps.m_shaderTargets[GxSh_Vertex] = format.m_fixedFunction[GxSh_Vertex];
    }

    if (this->m_caps.m_shaderTargets[GxSh_Pixel] == 4 && this->m_caps.m_shaderTargets[GxSh_Vertex] != 3) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = 3;
    }

    AdapterIdentifier identifier;
    this->m_d3d->GetAdapterIdentifier(0, 0, &identifier);

    if (identifier.VendorId == 0x10DE && identifier.DeviceId - 0x300 <= 0xFF && this->m_caps.m_shaderConstants[GxSh_Vertex] > 192) {
        this->m_caps.m_shaderConstants[GxSh_Vertex] = 192;
    }

    for (uint32_t i = 0; i < GxTexFormats_Last; i++) {
        if (i) {
            this->m_caps.m_texFmt[i] = this->m_d3d->CheckDeviceFormat(0, this->m_devAdapterFormat, 0, D3DRTYPE_TEXTURE, s_GxTexFmtToD3dFmt[i]) == D3D_OK;
        } else {
            this->m_caps.m_texFmt[0] = 0;
        }
    }

    this->m_caps.m_generateMipMaps = (this->m_d3dCaps.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) != 0;

    if (this->m_caps.m_shaderTargets[GxSh_Vertex] <= 1) {
        s_gxAttribToD3dAttribType[1] = D3DDECLTYPE_D3DCOLOR;
        s_gxAttribToD3dAttribType[2] = D3DDECLTYPE_D3DCOLOR;
    }

    this->m_caps.m_rttFormat[GxTex_Argb8888] = this->m_d3d->CheckDeviceFormat(0, this->m_devAdapterFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8) == D3D_OK;
    this->m_caps.m_rttFormat[GxTex_Rgb565] = this->m_d3d->CheckDeviceFormat(0, this->m_devAdapterFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_R5G6B5) == D3D_OK;
    this->m_caps.m_rttFormat[GxTex_Gr1616F] = this->m_d3d->CheckDeviceFormat(0, this->m_devAdapterFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_G16R16F) == D3D_OK;
    this->m_caps.m_rttFormat[GxTex_R32F] = this->m_d3d->CheckDeviceFormat(0, this->m_devAdapterFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_R32F) == D3D_OK;
    this->m_caps.m_rttFormat[GxTex_D24X8] = this->m_d3d->CheckDeviceFormat(0, this->m_devAdapterFormat, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24X8) == D3D_OK;

    this->m_caps.m_colorWrite = (this->m_d3dCaps.PrimitiveMiscCaps & D3DPMISCCAPS_COLORWRITEENABLE) != 0;
    this->m_caps.m_hwCursor = this->m_d3dCaps.CursorCaps & D3DCURSORCAPS_COLOR;
    this->m_caps.m_maxClipPlanes = this->m_d3dCaps.MaxUserClipPlanes;

    uint32_t textureCaps = this->m_d3dCaps.TextureCaps;
    this->m_caps.m_texTarget[GxTex_2d] = 1;
    this->m_caps.m_texTarget[GxTex_CubeMap] = (textureCaps & D3DPTEXTURECAPS_CUBEMAP) != 0;
    this->m_caps.m_texTarget[GxTex_Rectangle] = 0;
    this->m_caps.m_texTarget[GxTex_NonPow2] = (textureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) || !(textureCaps & D3DPTEXTURECAPS_POW2);
    this->m_caps.m_texNonPow2 = 1;

    if (this->m_caps.m_texTarget[GxTex_NonPow2]) {
        this->m_caps.m_texNonPow2 = (textureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0;

        if (identifier.VendorId == 0x10DE) {
            this->m_caps.m_texNonPow2 = 0;
        }
    }

    Query* query = nullptr;
    this->m_caps.m_occlusionQuery = this->m_d3dDevice->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query) == D3D_OK;

    if (query) {
        query->Release();
    }

    this->m_caps.m_pointParameters = this->m_d3dCaps.MaxPointSize > 1.0f;
    this->m_caps.m_pointScaleMax = this->m_d3dCaps.MaxPointSize;
    this->m_caps.m_pointSprite = this->m_d3dCaps.MaxPointSize > 1.0f;
    this->m_caps.m_blendFactor = (this->m_d3dCaps.SrcBlendCaps & D3DPBLENDCAPS_BLENDFACTOR) != 0;
    this->m_caps.m_shaderAlphaRef = 1;
    this->m_caps.unused_0114[1] = 0;
    this->m_caps.unused_0114[0] = 0;
    this->m_caps.unused_0114[2] = 0;
    this->m_caps.unused_0114[3] = 0;
    this->m_caps.unused_0114[4] = 0;
    this->m_caps.unk_0134 = this->m_caps.m_shaderTargets[GxSh_Pixel] != 4;
    this->m_caps.unk_0138 = this->m_caps.m_shaderTargets[GxSh_Pixel] != 4;
}

static void DefaultTexUserFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    static CImVector s_texels[64];

    switch (command) {
        case GxTex_Lock: {
            uint32_t color = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(userArg));

            for (uint32_t i = 0; i < 64; i++) {
                *reinterpret_cast<uint32_t*>(&s_texels[i]) = color;
            }

            break;
        }

        case GxTex_Latch:
            texelStrideInBytes = 4 * width;
            texels = s_texels;
            break;

        default:
            break;
    }
}

int32_t CGxDeviceMetal::ICreateD3dDevice(const CGxFormat& format) {
    bool hwTnL = format.m_hwTnL;

    if (hwTnL && !(this->m_d3dCaps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)) {
        hwTnL = false;
    }

    this->m_d3dIsHwDevice = hwTnL;

    PresentParameters d3dpp;
    this->ISetPresentParms(d3dpp, format);

    if (FAILED(this->m_d3d->CreateDevice(d3dpp, &this->m_d3dDevice))) {
        this->m_d3dDevice = nullptr;
        return 0;
    }

    this->m_devAdapterFormat = d3dpp.BackBufferFormat;
    this->m_context = 1;
    this->m_devDepthFormat = d3dpp.AutoDepthStencilFormat;

    this->ISetCaps(format);
    this->Log(this->m_caps);

    this->IStateSetD3DDefaults();
    this->DeviceSetGamma(this->m_gammaRamp);
    this->m_d3dDevice->SetCursorScale(s_cursorScale);
    this->ICursorCreate(format);
    static uint32_t s_defaultColor = 0xFF00FF00;

    GxTexCreate(8, 8, GxTex_Argb8888, CGxTexFlags(GxTex_Linear), reinterpret_cast<void*>(static_cast<uintptr_t>(s_defaultColor)), DefaultTexUserFunc, this->m_texture3B58);

    if (this->m_texture3B58) {
        GxTexUpdate(this->m_texture3B58, 0, 0, 0, 0, 1);
    }

    g_theGxDevicePtr->ShaderCreate(CGxDevice::s_uiVertexShader, GxSh_Vertex, "Shaders\\Vertex", "UI", 2);
    g_theGxDevicePtr->ShaderCreate(&CGxDevice::s_uiPixelShader, GxSh_Pixel, "Shaders\\Pixel", "UI", 1);

    return 1;
}

void CGxDeviceMetal::IReleaseD3dResources(int32_t freeTextures) {
    this->ITexForceRecreation(freeTextures);
    this->IReleaseD3dShaders(freeTextures);
    this->IReleaseD3dPools(freeTextures);
    memset(this->m_deviceStates, 0xFF, sizeof(this->m_deviceStates));

    if (this->m_defColorSurface) {
        this->m_defColorSurface->Release();
        this->m_defColorSurface = nullptr;
    }

    if (this->m_defDepthSurface) {
        this->m_defDepthSurface->Release();
        this->m_defDepthSurface = nullptr;
    }

    if (this->m_defDepthStencilSurface) {
        this->m_defDepthStencilSurface->Release();
        this->m_defDepthStencilSurface = nullptr;
    }

    if (this->m_surface3B44) {
        this->m_surface3B44->Release();
        this->m_surface3B44 = nullptr;
    }

    if (this->m_eventQuery) {
        this->m_eventQuery->Release();
        this->m_eventQuery = nullptr;
    }

    this->IReleaseD3dQueries();

    if (this->m_d3dDevice) {
        this->m_d3dDevice->ShowCursor(FALSE);
    }
}

int32_t CGxDeviceMetal::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size:
            if (param2 == 1 || param2 == 2) {
                this->m_windowVisible = 0;
                break;
            }

            this->m_windowVisible = 1;
            this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));

            if (this->m_d3dDevice && this->m_context) {
                this->IReleaseD3dResources(0);

                PresentParameters d3dpp;
                this->ISetPresentParms(d3dpp, this->m_format);

                if (this->m_d3dDevice->Reset(d3dpp) == D3D_OK) {
                    this->IStateSetD3DDefaults();
                    this->ISetWindowFocus(1);
                    this->m_context = 1;
                    this->m_intF5C = 0;
                    this->NotifyOnDeviceRestored();
                    this->m_viewportDirty = 1;
                    break;
                }

                this->m_context = 0;
            }

            this->m_viewportDirty = 1;
            break;

        case GxWM_DisplayChange:
            if (this->m_windowVisible) {
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_Destroy:
        case GxWM_KillFocus:
            this->m_windowFocus = 0;
            break;

        case GxWM_SetFocus:
            this->ISetWindowFocus(1);
            break;

        default:
            break;
    }

    return 0;
}

void CGxDeviceMetal::IDestroyD3d() {
    for (uint32_t i = 0; i < 2; i++) {
        if (CGxDevice::s_uiVertexShader[i]) {
            g_theGxDevicePtr->ShaderDestroy(CGxDevice::s_uiVertexShader[i]);
        }
    }

    if (CGxDevice::s_uiPixelShader) {
        g_theGxDevicePtr->ShaderDestroy(CGxDevice::s_uiPixelShader);
    }

    this->ICursorDestroy();
    this->IReleaseD3dResources(1);
    this->IReleaseD3dVertexDecl();

    if (this->m_eventQuery) {
        this->m_eventQuery->Release();
        this->m_eventQuery = nullptr;
    }

    if (this->m_texture3B58) {
        GxTexDestroy(this->m_texture3B58);
        this->m_texture3B58 = nullptr;
    }

    if (this->m_d3dDevice) {
        this->m_d3dDevice->Release();
        this->m_d3dDevice = nullptr;
    }
}

int32_t CGxDeviceMetal::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceMetal::DeviceSetFormat():");
    this->Log(format);

    this->IDestroyD3d();

    // The game window is kept and reconfigured for the new format rather than recreated
    this->m_format = format;

    CGxFormat createFormat = format;

    if (this->ICreateWindow(createFormat) && this->ICreateD3dDevice(createFormat) && CGxDevice::DeviceSetFormat(createFormat)) {
        OsGuiSetGxWindow(this->m_hwnd);
        this->ISetWindowFocus(1);
        return 1;
    }

    CGxDevice::Log("CGxDeviceMetal::DeviceSetFormat(): unable to set format!");
    this->IDestroyD3d();

    return 0;
}

void CGxDeviceMetal::DeviceDestroy() {
    CGxDevice::DeviceDestroy();

    this->IDestroyD3d();

    if (this->m_hwnd && this->m_ownhwnd) {
        OsMacDestroyGameWindow(this->m_hwnd);
        this->m_hwnd = nullptr;
    }

    if (this->m_d3d) {
        this->m_d3d->Release();
        this->m_d3d = nullptr;
    }
}

int32_t CGxDeviceMetal::ICreateD3d() {
    if (!this->m_d3d) {
        this->m_d3d = Direct3D::Create();
    }

    if (this->m_d3d && SUCCEEDED(this->m_d3d->GetDeviceCaps(0, &this->m_d3dCaps))) {
        if (this->m_desktopDisplayMode.Format) {
            return 1;
        }

        DisplayMode displayMode;

        if (SUCCEEDED(this->m_d3d->GetAdapterDisplayMode(0, &displayMode))) {
            this->m_desktopDisplayMode = displayMode;
            return 1;
        }
    }

    CGxDevice::Log("CGxDeviceMetal::ICreateD3d(): no Metal device");
    this->IDestroyD3d();

    if (this->m_d3d) {
        this->m_d3d->Release();
        this->m_d3d = nullptr;
    }

    return 0;
}

int32_t CGxDeviceMetal::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    // The display's gamma is the starting ramp, as Windows reads it from the desktop
    this->m_gammaRamp = this->m_systemGammaRamp;

    if (this->ICreateD3d() && CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

int32_t CGxDeviceMetal::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    CGxDevice::DeviceCreate(window, format);
    this->m_hwnd = window;

    if (this->ICreateD3d() && this->ICreateD3dDevice(format) && CGxDevice::DeviceCreate(window, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

void CGxDeviceMetal::IReleaseD3dQueries() {
    for (CGxQuery* query = this->m_queryList.Head(); query; query = this->m_queryList.Next(query)) {
        if (query->m_apiSpecific) {
            static_cast<Query*>(query->m_apiSpecific)->Release();
        }

        query->m_apiSpecific = nullptr;
    }
}

void CGxDeviceMetal::IReleaseD3dPools(int32_t freePools) {
    for (CGxPool* pool = this->m_poolList.Head(); pool; pool = this->m_poolList.Next(pool)) {
        if (freePools || pool->m_usage == GxPoolUsage_Stream || pool->m_usage == GxPoolUsage_Dynamic) {
            this->IReleaseD3dPool(pool);
        }
    }
}

void CGxDeviceMetal::ITexForceRecreation(int32_t all) {
    for (CGxTex* texId = this->m_textures.Head(); texId; texId = this->m_textures.Next(texId)) {
        if (!texId->m_apiSpecificData || !(all || texId->m_flags.m_renderTarget)) {
            continue;
        }
        static CiRect s_updateRect;

        if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
            static_cast<BaseTexture*>(texId->m_apiSpecificData)->Release();
        }

        texId->m_apiSpecificData = this->m_texture3B58;
        texId->m_needsCreation = 1;
        this->TexMarkForUpdate(texId, s_updateRect, 0);

        uint32_t texelStrideInBytes;
        const void* texels;
        texId->m_userFunc(GxTex_3, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
    }

    if (this->m_hwnd && this->m_d3dDevice) {
        this->NotifyOnTextureRecreation();
    }
}

void CGxDeviceMetal::TexDestroy(CGxTex* texId) {
    if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
        static_cast<BaseTexture*>(texId->m_apiSpecificData)->Release();
        texId->m_apiSpecificData = nullptr;
    }

    CGxDevice::TexDestroy(texId);
}

void CGxDeviceMetal::ITexCreate(CGxTex* texId) {
    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    texId->m_format = s_GxTexFormatToUse[texId->m_format];

    uint32_t usage = 0;
    D3DPOOL pool = D3DPOOL_MANAGED;

    if (texId->m_flags.m_renderTarget) {
        usage = D3DUSAGE_RENDERTARGET;
        pool = D3DPOOL_DEFAULT;
    }

    if (texId->m_flags.m_generateMipMaps) {
        usage |= D3DUSAGE_AUTOGENMIPMAP;
    }

    if (texId->m_target == GxTex_CubeMap) {
        CubeTexture* cubeTexture = nullptr;
        if (FAILED(this->m_d3dDevice->CreateCubeTexture(width, mipCount - baseMip, usage, s_GxTexFmtToD3dFmt[texId->m_format], pool, &cubeTexture))) {
            return;
        }

        texId->m_apiSpecificData = cubeTexture;
    } else {
        Texture* texture = nullptr;

        if (texId->m_format == GxTex_D24X8) {
            if (FAILED(this->m_d3dDevice->CreateTexture(width, height, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24X8, pool, &texture))) {
                return;
            }
        } else {
            uint32_t levels = mipCount - baseMip;

            if (FAILED(this->m_d3dDevice->CreateTexture(width, height, levels, usage, s_GxTexFmtToD3dFmt[texId->m_format], pool, &texture))) {
                if (texId->m_flags.m_renderTarget) {
                    return;
                }

                s_GxTexFormatToUse[texId->m_format] = s_tolerableTexFmtMapping[texId->m_format];
                texId->m_format = s_GxTexFormatToUse[texId->m_format];

                if (FAILED(this->m_d3dDevice->CreateTexture(width, height, levels, usage, s_GxTexFmtToD3dFmt[texId->m_format], pool, &texture))) {
                    return;
                }
            }
        }

        texId->m_apiSpecificData = texture;
    }

    texId->m_needsCreation = 0;
}

void CGxDeviceMetal::ITexUpload(CGxTex* texId) {
    uint32_t texelStrideInBytes;
    const void* texels = nullptr;
    texId->m_userFunc(GxTex_Lock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    int32_t compressed = texId->m_dataFormat >= GxTex_Dxt1 && texId->m_dataFormat <= GxTex_Dxt5;
    uint32_t faceCount = texId->m_target == GxTex_CubeMap ? 6 : 1;

    for (uint32_t face = 0; face < faceCount; face++) {
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        uint32_t level = 0;

        for (uint32_t mip = baseMip; mip != mipCount; mip++, level++) {
            texels = nullptr;
            texId->m_userFunc(GxTex_Latch, mipWidth, mipHeight, face, mip, texId->m_userArg, texelStrideInBytes, texels);

            if (texels && !texId->m_flags.m_renderTarget) {
                Surface* surface = nullptr;
                HRESULT result;

                if (texId->m_target == GxTex_CubeMap) {
                    result = static_cast<CubeTexture*>(texId->m_apiSpecificData)->GetCubeMapSurface(s_faceTypes[face], level, &surface);
                } else {
                    result = static_cast<Texture*>(texId->m_apiSpecificData)->GetSurfaceLevel(level, &surface);
                }

                if (FAILED(result)) {
                    goto done;
                }

                Rect rect;
                rect.left = texId->m_updateRect.minX >> mip;
                rect.top = texId->m_updateRect.minY >> mip;
                rect.bottom = rect.top + 1 > texId->m_updateRect.maxY >> mip ? rect.top + 1 : texId->m_updateRect.maxY >> mip;
                rect.right = rect.left + 1 > texId->m_updateRect.maxX >> mip ? rect.left + 1 : texId->m_updateRect.maxX >> mip;

                if (static_cast<uint32_t>(texId->m_format - GxTex_Dxt1) <= 2) {
                    rect.left &= ~3;
                    rect.top &= ~3;
                    rect.right = (rect.right + 3) & ~3;
                    rect.bottom = (rect.bottom + 3) & ~3;

                    if (rect.bottom > static_cast<int32_t>(mipHeight)) {
                        rect.bottom = mipHeight;
                    }

                    if (rect.right > static_cast<int32_t>(mipWidth)) {
                        rect.right = mipWidth;
                    }
                }

                D3DLOCKED_RECT lockedRect;
                if (FAILED(surface->LockRect(&lockedRect, &rect, 0))) {
                    surface->Release();
                    goto done;
                }

                const uint8_t* src = static_cast<const uint8_t*>(texels);

                if (!texId->m_flags.m_bit15) {
                    EGxTexFormat dataFormat = texId->m_dataFormat;

                    if (compressed) {
                        src += s_texFormatBytesPerBlock[dataFormat] * (rect.left >> 2) + texelStrideInBytes * (rect.top >> 2);
                    } else {
                        src += ((rect.left * s_texFormatBitDepth[dataFormat]) >> 3) + texelStrideInBytes * rect.top;
                    }
                }

                C2iVector size;
                size.x = rect.right - rect.left;
                size.y = rect.bottom - rect.top;
                Blit(size, BlitAlpha_0, src, texelStrideInBytes, GxGetBlitFormat(texId->m_dataFormat), lockedRect.pBits, lockedRect.Pitch, GxGetBlitFormat(texId->m_format));

                surface->UnlockRect();
                surface->Release();
            }

            mipHeight >>= 1;
            mipWidth >>= 1;

            if (!mipWidth) {
                mipWidth = 1;
            }

            if (!mipHeight) {
                mipHeight = 1;
            }
        }
    }

done:
    texels = nullptr;
    texId->m_userFunc(GxTex_Unlock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    if (!texId->m_flags.m_renderTarget) {
        static_cast<BaseTexture*>(texId->m_apiSpecificData)->PreLoad();
    }
}

void CGxDeviceMetal::ITexMarkAsUpdated(CGxTex* texId) {
    if (!texId->m_needsUpdate || !this->m_context) {
        return;
    }

    if (texId->m_needsCreation || (!texId->m_apiSpecificData && !texId->m_apiSpecificData2)) {
        this->ITexCreate(texId);
    }

    if (!texId->m_needsCreation && (texId->m_apiSpecificData || texId->m_apiSpecificData2)) {
        if (texId->m_userFunc) {
            this->ITexUpload(texId);
        }

        CGxDevice::ITexMarkAsUpdated(texId);
    }
}

void CGxDeviceMetal::ISceneBegin() {
    if (!this->m_context) {
        if (this->m_d3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
            this->IReleaseD3dResources(0);

            PresentParameters d3dpp;
            this->ISetPresentParms(d3dpp, this->m_format);

            if (this->m_d3dDevice->Reset(d3dpp) == D3D_OK) {
                this->IStateSetD3DDefaults();
                this->ISetWindowFocus(1);
                this->m_context = 1;
                this->m_intF5C = 0;
                this->NotifyOnDeviceRestored();
            }
        }

        if (!this->m_context) {
            return;
        }
    }

    this->ShaderConstantsClear();

    if (this->m_d3dDevice->BeginScene() == D3D_OK) {
        this->m_inScene = 1;
    }
}

void CGxDeviceMetal::ScenePresent() {
    if (this->m_context) {
        int32_t scrShotClick = this->m_scrShotClick;

        CGxDevice::ScenePresent();
        this->ISceneEnd();
        this->ICursorDraw();

        if (this->m_format.m_fixLag) {
            if (this->m_eventQuery) {
                this->m_eventQuery->Issue(D3DISSUE_END);

                while (this->m_eventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                    OsSleep(0);
                }
            } else {
                Surface* backBuffer;

                if (SUCCEEDED(this->m_d3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
                    D3DSURFACE_DESC desc;
                    backBuffer->GetDesc(&desc);

                    D3DLOCKED_RECT lockedRect;

                    if (SUCCEEDED(backBuffer->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY))) {
                        backBuffer->UnlockRect();
                    }

                    backBuffer->Release();
                }
            }
        }

        if (scrShotClick) {
            this->DeviceScreenShot();
        }

        this->WaitForFPSCap();

        if (FAILED(this->m_d3dDevice->Present())) {
            this->m_context = 0;
        }

    }

    // The window procedure of Windows reports size and focus changes; here they are polled
    this->IPollWindow();

    this->ISceneBegin();
}

void CGxDeviceMetal::Draw(CGxBatch* batch, int32_t count) {
    if (!this->m_context || this->m_intF5C) {
        return;
    }

    this->IStateSync();

    uint32_t baseVertexIndex = 0;

    if (!this->m_caps.m_streamOffset) {
        baseVertexIndex = this->m_primVertexFormatBuf[0]->m_index / this->m_primVertexFormatBuf[0]->m_itemSize;
    }

    if (count) {
        uint32_t primCount = this->PrimCalcCount(batch->m_primType, batch->m_count);
        this->m_d3dDevice->DrawIndexedPrimitive(s_primitiveConversion[batch->m_primType], baseVertexIndex, batch->m_minIndex, batch->m_maxIndex - batch->m_minIndex + 1, batch->m_start + (this->m_primIndexBuf->m_index >> 1), primCount);
    } else {
        uint32_t primCount = this->PrimCalcCount(batch->m_primType, batch->m_count);
        this->m_d3dDevice->DrawPrimitive(s_primitiveConversion[batch->m_primType], baseVertexIndex, primCount);
    }
}

void CGxDeviceMetal::IStateSetD3DDefaults() {
    this->m_d3dDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    this->m_d3dDevice->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
    this->m_d3dDevice->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    this->m_d3dDevice->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    this->m_d3dDevice->SetRenderState(D3DRS_FOGDENSITY, 0);

    for (uint32_t i = 0; i < 16; i++) {
        this->m_d3dDevice->SetSamplerState(i, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    }

    this->IRsForceUpdate();
    this->IRsSync(0);

    this->m_primVertexDirty = 0xFFFFFFFF;
    this->m_primIndexDirty = 0;
    this->m_d3dDevice->SetIndices(nullptr);
    this->m_d3dCurrentVertexDecl = nullptr;
    this->m_d3dCurrentIndexBuf = nullptr;

    for (uint32_t i = 0; i < 8; i++) {
        this->m_d3dVertexStreamBuf[i] = nullptr;
        this->m_d3dVertexStreamOfs[i] = 0xFFFFFFFF;
        this->m_d3dVertexStreamStride[i] = 0xFFFFFFFF;
    }

    this->m_d3dDevice->GetRenderTarget(0, &this->m_defColorSurface);
    this->m_d3dDevice->GetDepthStencilSurface(&this->m_defDepthSurface);
    this->InitLights();
    this->m_ambientOnlyMode = 0;

    if (!this->m_eventQuery) {
        this->m_d3dDevice->CreateQuery(D3DQUERYTYPE_EVENT, &this->m_eventQuery);
    }

    this->ISceneBegin();
}

void CGxDeviceMetal::IReleaseD3dVertexDecl() {
    for (uint32_t i = 0; i < GxVertexBufferFormats_Last; i++) {
        if (this->m_d3dVertexDecl[i]) {
            this->m_d3dVertexDecl[i]->Release();
            this->m_d3dVertexDecl[i] = nullptr;
        }
    }

    for (uint32_t i = 0; i < this->m_gxVertexDecl.Count(); i++) {
        this->m_gxVertexDecl[i].m_d3dDecl->Release();
    }

    this->m_gxVertexDecl.m_count = 0;
}

void CGxDeviceMetal::IReleaseD3dShaders(int32_t freeShaders) {
    if (!freeShaders) {
        return;
    }

    TSHashTable<CGxShader, HASHKEY_STRI>& pixelShaders = this->m_shaderList[GxSh_Pixel];

    for (CGxShader* shader = pixelShaders.m_fulllist.Head(); shader; shader = pixelShaders.m_fulllist.Next(shader)) {
        if (shader->m_apiSpecific) {
            static_cast<PixelShader*>(shader->m_apiSpecific)->Release();
            shader->m_apiSpecific = nullptr;
            shader->m_loaded = 0;
        }
    }

    TSHashTable<CGxShader, HASHKEY_STRI>& vertexShaders = this->m_shaderList[GxSh_Vertex];

    for (CGxShader* shader = vertexShaders.m_fulllist.Head(); shader; shader = vertexShaders.m_fulllist.Next(shader)) {
        if (shader->m_apiSpecific) {
            static_cast<VertexShader*>(shader->m_apiSpecific)->Release();
            shader->m_apiSpecific = nullptr;
            shader->m_loaded = 0;
        }
    }
}

void CGxDeviceMetal::SceneClear(uint32_t mask, CImVector color) {
    if (!this->m_context) {
        return;
    }

    uint32_t flags = (mask & 0x1) != 0;

    if (mask & 0x2) {
        flags |= D3DCLEAR_ZBUFFER;
    }

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }

    this->m_d3dDevice->Clear(0, nullptr, flags, D3DCOLOR_ARGB(color.a, color.r, color.g, color.b), 1.0f, 0);
}

void CGxDeviceMetal::DeviceSetBaseMipLevel(uint32_t level) {
    this->ITexForceRecreation(1);
    CGxDevice::DeviceSetBaseMipLevel(level);
}

void CGxDeviceMetal::DeviceSaveDepthSurface() {
    if (!this->m_context) {
        return;
    }

    this->m_d3dDevice->GetDepthStencilSurface(&s_savedDepthSurface);
    s_savedDepthSurface->Release();
}

void CGxDeviceMetal::DeviceRestoreDepthSurface() {
    if (!this->m_context) {
        return;
    }

    this->m_d3dDevice->SetDepthStencilSurface(s_savedDepthSurface);
}

void CGxDeviceMetal::LogCrashInfo(char* buffer, uint32_t size) {
    if (!this->m_d3d) {
        return;
    }

    uint32_t adapterCount = this->m_d3d->GetAdapterCount();

    char line[260];
    SStrPrintf(line, sizeof(line), "Adapter Count: %d\r\n", adapterCount);
    SStrPack(buffer, line, size);

    for (uint32_t i = 0; i < adapterCount; i++) {
        SStrPrintf(line, sizeof(line), "\r\nAdapter %d%s:\r\n", i, i == 0 ? " (primary)" : "");
        SStrPack(buffer, line, size);

        AdapterIdentifier identifier;
        this->m_d3d->GetAdapterIdentifier(i, 0, &identifier);

        SStrPrintf(line, sizeof(line), "  Driver: %s\r\n", identifier.Driver);
        SStrPack(buffer, line, size);
        SStrPrintf(line, sizeof(line), "  Version: %d.%d.%d\r\n", identifier.DriverVersionHigh >> 16, identifier.DriverVersionHigh & 0xFFFF, identifier.DriverVersionLow >> 16);
        SStrPack(buffer, line, size);
        SStrPrintf(line, sizeof(line), "  Description: %s\r\n", identifier.Description);
        SStrPack(buffer, line, size);
        SStrPrintf(line, sizeof(line), "  DeviceName: %s\r\n", identifier.DeviceName);
        SStrPack(buffer, line, size);
    }
}

void CGxDeviceMetal::ICreateReadbackSurface() {
    if (this->m_surface3B44) {
        return;
    }

    const CRect& window = this->DeviceDefWindow();
    D3DFORMAT format;

    if (this->m_format.m_window) {
        DisplayMode displayMode;

        if (SUCCEEDED(this->m_d3d->GetAdapterDisplayMode(0, &displayMode))) {
            format = displayMode.Format;
        } else {
            format = this->m_desktopDisplayMode.Format;
        }
    } else {
        format = s_GxFormatToD3dFormat[this->m_format.m_colorFormat];
    }

    this->m_d3dDevice->CreateRenderTarget(static_cast<uint32_t>(window.maxX - window.minX), static_cast<uint32_t>(window.maxY - window.minY), format, D3DMULTISAMPLE_NONE, 0, TRUE, &this->m_surface3B44);
}

void CGxDeviceMetal::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_context) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    if (target.m_texture == texture && target.m_plane == plane) {
        return;
    }

    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);

    if (target.m_apiSpecific) {
        static_cast<Surface*>(target.m_apiSpecific)->Release();
        target.m_apiSpecific = nullptr;
    }

    if (texture) {
        if (texture->m_needsCreation || (!texture->m_apiSpecificData && !texture->m_apiSpecificData2)) {
            this->ITexCreate(texture);
        }

        if (!texture->m_needsCreation && (texture->m_apiSpecificData || texture->m_apiSpecificData2)) {
            if (texture->m_target == GxTex_CubeMap) {
                static_cast<CubeTexture*>(texture->m_apiSpecificData)->GetCubeMapSurface(s_faceTypes[plane], 0, reinterpret_cast<Surface**>(&target.m_apiSpecific));
            } else {
                static_cast<Texture*>(texture->m_apiSpecificData)->GetSurfaceLevel(0, reinterpret_cast<Surface**>(&target.m_apiSpecific));
            }
        }
    }

    Surface* depthSurface = this->m_defDepthSurface;
    Surface* colorSurface = this->m_defColorSurface;

    if (this->m_textureTarget[GxBuffers_Color].m_apiSpecific) {
        colorSurface = static_cast<Surface*>(this->m_textureTarget[GxBuffers_Color].m_apiSpecific);

        if (!this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
            this->ICreateDepthStencilSurface(texture->m_width, texture->m_height);

            if (this->m_defDepthStencilSurface) {
                depthSurface = this->m_defDepthStencilSurface;
            }
        }
    }

    if (this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
        depthSurface = static_cast<Surface*>(this->m_textureTarget[GxBuffers_Depth].m_apiSpecific);
    }

    this->m_d3dDevice->SetRenderTarget(0, colorSurface);
    this->m_d3dDevice->SetDepthStencilSurface(depthSurface);

    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);

    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;
}

void CGxDeviceMetal::DeviceResolveDepthBuffer(CGxTex* texture) {
    if (!this->m_context) {
        return;
    }

    if (texture && (texture->m_needsCreation || (!texture->m_apiSpecificData && !texture->m_apiSpecificData2))) {
        this->ITexCreate(texture);
    }

    // The texture stands for its top-level surface
    this->m_d3dDevice->SetDepthStencilSurface(static_cast<BaseTexture*>(texture->m_apiSpecificData));
}

void CGxDeviceMetal::DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane) {
    Surface* sourceSurface = nullptr;
    Surface* destSurface = nullptr;

    if (source) {
        if (source->m_needsCreation || (!source->m_apiSpecificData && !source->m_apiSpecificData2)) {
            this->ITexCreate(source);
        }

        if (!source->m_needsCreation && (source->m_apiSpecificData || source->m_apiSpecificData2)) {
            if (source->m_target == GxTex_CubeMap) {
                static_cast<CubeTexture*>(source->m_apiSpecificData)->GetCubeMapSurface(s_faceTypes[sourcePlane], 0, &sourceSurface);
            } else {
                static_cast<Texture*>(source->m_apiSpecificData)->GetSurfaceLevel(0, &sourceSurface);
            }
        }
    }

    if (dest) {
        if (dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) {
            this->ITexCreate(dest);
        }

        if (!dest->m_needsCreation && (dest->m_apiSpecificData || dest->m_apiSpecificData2)) {
            if (dest->m_target == GxTex_CubeMap) {
                static_cast<CubeTexture*>(dest->m_apiSpecificData)->GetCubeMapSurface(s_faceTypes[destPlane], 0, &destSurface);
            } else {
                static_cast<Texture*>(dest->m_apiSpecificData)->GetSurfaceLevel(0, &destSurface);
            }
        }
    }

    if (sourceSurface) {
        if (destSurface) {
            this->m_d3dDevice->StretchRect(sourceSurface, nullptr, destSurface, nullptr, D3DTEXF_NONE);
        }

        if (sourceSurface) {
            sourceSurface->Release();
        }
    }

    if (destSurface) {
        destSurface->Release();
    }
}

CGxDeviceMetal::~CGxDeviceMetal() {
}

void CGxDeviceMetal::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    this->ClampRectToWindow(rect);

    uint32_t height = rect.maxY - rect.minY;
    uint32_t width = rect.maxX - rect.minX;
    uint32_t count = width * height;
    pixels.SetCount(count);

    Surface* surface = nullptr;
    HRESULT result;

    if (this->m_format.m_multisampleCount < 2) {
        result = this->m_d3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface);
    } else {
        Rect sourceRect = { rect.minX, rect.minY, static_cast<int32_t>(width), static_cast<int32_t>(height) };

        this->ICreateReadbackSurface();

        if (!this->m_surface3B44) {
            return;
        }

        result = this->m_d3dDevice->StretchRect(this->m_defColorSurface, &sourceRect, this->m_surface3B44, &sourceRect, D3DTEXF_NONE);
        surface = this->m_surface3B44;
        surface->AddRef();
    }

    if (FAILED(result)) {
        return;
    }

    D3DLOCKED_RECT lockedRect;

    if (SUCCEEDED(surface->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY))) {
        D3DSURFACE_DESC desc;
        surface->GetDesc(&desc);

        if (width <= desc.Width && height <= desc.Height) {
            const uint8_t* source = static_cast<const uint8_t*>(lockedRect.pBits);

            switch (desc.Format) {
                case D3DFMT_A8R8G8B8:
                case D3DFMT_X8R8G8B8: {
                    CImVector* dest = pixels.Ptr();

                    for (uint32_t y = height; y; y--) {
                        memcpy(dest, source, sizeof(CImVector) * width);
                        source += lockedRect.Pitch;
                        dest += width;
                    }

                    break;
                }

                case D3DFMT_R5G6B5: {
                    uint8_t* dest = reinterpret_cast<uint8_t*>(pixels.Ptr());

                    for (uint32_t y = height; y; y--) {
                        const uint8_t* row = source;

                        for (uint32_t x = width; x; x--) {
                            dest[3] = 0xFF;
                            dest[2] = row[1] & 0xF8;
                            dest[1] = (*reinterpret_cast<const uint16_t*>(row) >> 3) & 0xFC;
                            dest[0] = row[0] << 3;
                            dest += 4;
                            row += 2;
                        }

                        source += lockedRect.Pitch;
                    }

                    break;
                }

                case D3DFMT_X1R5G5B5:
                case D3DFMT_A1R5G5B5: {
                    uint8_t* dest = reinterpret_cast<uint8_t*>(pixels.Ptr());

                    for (uint32_t y = height; y; y--) {
                        const uint8_t* row = source;

                        for (uint32_t x = width; x; x--) {
                            dest[3] = 0xFF;
                            dest[2] = (*reinterpret_cast<const uint16_t*>(row) >> 7) & 0xF8;
                            dest[1] = (*reinterpret_cast<const uint16_t*>(row) >> 2) & 0xF8;
                            dest[0] = row[0] << 3;
                            dest += 4;
                            row += 2;
                        }

                        source += lockedRect.Pitch;
                    }

                    break;
                }

                default:
                    memset(pixels.Ptr(), 0xFF, sizeof(CImVector) * count);
                    break;
            }
        }

        surface->UnlockRect();
    }

    surface->Release();
}

int32_t CGxDeviceMetal::ICheckDepthStencilFormat(D3DFORMAT format) {
    return SUCCEEDED(this->m_d3d->CheckDeviceFormat(0, this->m_devAdapterFormat, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, format)) && SUCCEEDED(this->m_d3d->CheckDepthStencilMatch(0, this->m_devAdapterFormat, this->m_devAdapterFormat, format));
}

void CGxDeviceMetal::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    this->ClampRectToWindow(rect);

    uint32_t count = (rect.maxY - rect.minY) * (rect.maxX - rect.minX);
    depths.SetCount(count);
    memset(depths.Ptr(), 0, sizeof(float) * count);
}

int32_t CGxDeviceMetal::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    if (!this->m_context) {
        return 0;
    }

    if (dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) {
        this->ITexCreate(dest);
    }

    if (dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) {
        return 0;
    }

    Rect sourceRect = { sourcePos.x, sourcePos.y, sourcePos.x + size.x, sourcePos.y + size.y };
    Rect destRect = { destPos.x, destPos.y, destPos.x + size.x, destPos.y + size.y };

    Surface* surface;
    HRESULT result;

    if (dest->m_target == GxTex_CubeMap) {
        result = static_cast<CubeTexture*>(dest->m_apiSpecificData)->GetCubeMapSurface(s_faceTypes[plane], level, &surface);
    } else {
        result = static_cast<Texture*>(dest->m_apiSpecificData)->GetSurfaceLevel(level, &surface);
    }

    if (SUCCEEDED(result)) {
        result = this->m_d3dDevice->StretchRect(this->m_defColorSurface, &sourceRect, surface, &destRect, D3DTEXF_NONE);
        surface->Release();
    }

    return SUCCEEDED(result);
}

int32_t CGxDeviceMetal::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    if (!this->m_context) {
        return 0;
    }

    if (dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) {
        this->ITexCreate(dest);
    }

    if (dest->m_needsCreation || (!dest->m_apiSpecificData && !dest->m_apiSpecificData2)) {
        return 0;
    }

    Surface* surface;
    HRESULT result;

    if (dest->m_target == GxTex_CubeMap) {
        result = static_cast<CubeTexture*>(dest->m_apiSpecificData)->GetCubeMapSurface(s_faceTypes[face], level, &surface);
    } else {
        result = static_cast<Texture*>(dest->m_apiSpecificData)->GetSurfaceLevel(level, &surface);
    }

    if (SUCCEEDED(result)) {
        Rect d3dDestRect = { 0, 0, 0, 0 };

        if (destRect) {
            d3dDestRect.left = destRect->minX;
            d3dDestRect.top = destRect->minY;
            d3dDestRect.right = destRect->maxX;
            d3dDestRect.bottom = destRect->maxY;
        }

        Rect d3dSourceRect = { 0, 0, 0, 0 };

        if (sourceRect) {
            d3dSourceRect.left = sourceRect->minX;
            d3dSourceRect.top = sourceRect->minY;
            d3dSourceRect.right = sourceRect->maxX;
            d3dSourceRect.bottom = sourceRect->maxY;
        }

        D3DTEXTUREFILTERTYPE filter;

        if (destRect && sourceRect && d3dDestRect.right - d3dDestRect.left == d3dSourceRect.right - d3dSourceRect.left && d3dDestRect.bottom - d3dDestRect.top == d3dSourceRect.bottom - d3dSourceRect.top) {
            filter = D3DTEXF_NONE;
        } else {
            filter = D3DTEXF_LINEAR;
        }

        result = this->m_d3dDevice->StretchRect(this->m_defColorSurface, sourceRect ? &d3dSourceRect : nullptr, surface, destRect ? &d3dDestRect : nullptr, filter);
        surface->Release();
    }

    return SUCCEEDED(result);
}

void CGxDeviceMetal::IShaderReload(CGxShader* shader, const char* path, const char* name) {
    switch (shader->m_target) {
        case GxSh_Vertex:
            if (shader->m_refCount == 1) {
                if (shader->m_apiSpecific) {
                    static_cast<VertexShader*>(shader->m_apiSpecific)->Release();
                    shader->m_apiSpecific = nullptr;
                }

                shader->m_loaded = 0;
            }

            this->IShaderLoad(&shader, GxSh_Vertex, path, name, 1);
            this->IShaderCreateVertex(shader);
            break;

        case GxSh_Pixel:
            if (shader->m_refCount == 1) {
                if (shader->m_apiSpecific) {
                    static_cast<PixelShader*>(shader->m_apiSpecific)->Release();
                    shader->m_apiSpecific = nullptr;
                }

                shader->m_loaded = 0;
            }

            this->IShaderLoad(&shader, GxSh_Pixel, path, name, 1);
            this->IShaderCreatePixel(shader);
            break;

        default:
            break;
    }
}

void CGxDeviceMetal::ICreateDepthStencilSurface(uint32_t width, uint32_t height) {
    if (this->m_defDepthStencilSurface) {
        D3DSURFACE_DESC desc;

        if (SUCCEEDED(this->m_defDepthStencilSurface->GetDesc(&desc)) && desc.Width >= width && desc.Height >= height) {
            return;
        }

        this->m_defDepthStencilSurface->Release();
    }

    D3DFORMAT formats[3] = { D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D32 };

    for (uint32_t i = 0; i < 3; i++) {
        D3DFORMAT format = formats[i];

        if (this->ICheckDepthStencilFormat(format)) {
            this->m_d3dDevice->CreateDepthStencilSurface(width, height, format, D3DMULTISAMPLE_NONE, 0, FALSE, &this->m_defDepthStencilSurface);
        }

        if (this->m_defDepthStencilSurface) {
            break;
        }
    }
}

void CGxDeviceMetal::IPollWindow() {
    if (!this->m_hwnd || !this->m_ownhwnd) {
        return;
    }

    // Size: the client area in pixels, as WM_SIZE reports it
    int32_t width;
    int32_t height;
    OsMacGetClientSize(this->m_hwnd, &width, &height);

    const CRect& window = this->DeviceDefWindow();
    if (width > 0 && height > 0 && (static_cast<float>(width) != window.maxX || static_cast<float>(height) != window.maxY)) {
        CRect rect;
        rect.minX = 0.0f;
        rect.minY = 0.0f;
        rect.maxX = static_cast<float>(width);
        rect.maxY = static_cast<float>(height);
        this->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 0);
    }

    // The pointer over the game view, as WM_SETCURSOR keeps it on Windows
    this->IUpdateCursor();

    // Focus: WM_SETFOCUS and WM_KILLFOCUS
    int32_t focus = OsGuiGetWindow(1) == this->m_hwnd ? 1 : 0;
    if (focus != this->m_windowFocus) {
        this->DeviceWM(focus ? GxWM_SetFocus : GxWM_KillFocus, 0, 0);
    }
}

void CGxDeviceMetal::IUpdateCursor() {
    if (!this->m_d3dDevice) {
        return;
    }

    // The client's cursor becomes the pointer over the game view; a software cursor hides it.
    // Like WM_SETCURSOR on Windows, this follows the client's cursor state rather than the
    // device's, which starts out hidden whenever the device is created again (resolution changes)
    bool visible = this->m_hwCursor && this->m_cursorVisible;
    this->m_d3dDevice->ShowCursor(visible);
    OsMacSetGameCursor(this->m_d3dDevice->GetCursorImage(), visible);
}

void GxCursorSetScale(float scale) {
    s_cursorScale = scale;

    CGxDeviceMetal* device = dynamic_cast<CGxDeviceMetal*>(g_theGxDevicePtr);
    if (device && device->m_d3dDevice) {
        device->m_d3dDevice->SetCursorScale(scale);
        device->IUpdateCursor();
    }
}

void* GxMetalDevice() {
    CGxDeviceMetal* device = dynamic_cast<CGxDeviceMetal*>(g_theGxDevicePtr);
    return device && device->m_d3dDevice ? device->m_d3dDevice->GetMetalDevice() : nullptr;
}

int32_t GxMetalAcquireTargets(GxMetalTargets& targets) {
    CGxDeviceMetal* device = dynamic_cast<CGxDeviceMetal*>(g_theGxDevicePtr);

    if (!device || !device->m_d3dDevice) {
        return 0;
    }

    return device->m_d3dDevice->AcquireExternalTargets(&targets.commandBuffer, &targets.color, &targets.depth, &targets.width, &targets.height, &targets.sampleCount);
}

// The native device (CGxDeviceMetalNative.mm) is written slice by slice; WRATHMODERN_GX=native
// selects it for the frame-by-frame comparison with the device below, which stays the default.
CGxDevice* CreateCGxDeviceMetalNative();

CGxDevice* CreateCGxDeviceMetal() {
    const char* gx = getenv("WRATHMODERN_GX");

    if (gx && !strcmp(gx, "native")) {
        return CreateCGxDeviceMetalNative();
    }

    void* memory = SMemAlloc(sizeof(CGxDeviceMetal), __FILE__, __LINE__, 0);

    if (!memory) {
        return nullptr;
    }

    return new (memory) CGxDeviceMetal();
}
