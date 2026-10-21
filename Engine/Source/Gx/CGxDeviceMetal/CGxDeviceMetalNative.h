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
    struct HwStage {
        uint8_t m_colorOp;                // GxMetal::StageOp
        uint8_t m_alphaOp;
        uint8_t m_texcoordIndex;          // input texture coordinate set
        uint8_t m_texGen;                 // GxMetal::TexGen
        uint8_t m_transformCount;         // components the texture transform produces, 0: none
        uint8_t m_projected;              // the lookup divides by the last component
        float m_transform[16];
    };

    struct HwLight {
        bool m_enabled;
        uint8_t m_type;                   // GxMetal::LightType
        float m_position[4];              // world space
        float m_direction[4];
        float m_diffuse[4];
        float m_ambient[4];
        float m_specular[4];
        float m_attenuation[3];
    };

    struct HwStream {
        GxMetal::Buffer* m_buffer;
        uint32_t m_offset;
        uint32_t m_stride;
    };

    // The device state in Metal terms
    struct HwState {
        // Blending and output
        bool m_blendEnable;
        MTLBlendFactor m_srcBlend;
        MTLBlendFactor m_dstBlend;
        float m_blendColor[4];
        MTLColorWriteMask m_colorWrite;
        MTLCompareFunction m_depthCompare;
        bool m_depthWrite;
        bool m_alphaTest;
        uint32_t m_alphaRef;

        // Rasterizer
        MTLCullMode m_cullMode;
        MTLTriangleFillMode m_fillMode;
        float m_depthBias;
        bool m_scissorTest;
        GxMetal::Rect m_scissor;
        float m_viewport[6];              // x, y, width, height, min z, max z in target pixels
        uint32_t m_clipPlaneMask;
        float m_clipPlanes[6][4];
        float m_pointSize;
        float m_pointSizeMin;
        float m_pointSizeMax;
        bool m_pointScale;
        float m_pointScaleAttenuation[3];
        bool m_pointSprite;

        // Fog
        bool m_fog;
        float m_fogStart;
        float m_fogEnd;
        float m_fogColor[4];

        // Fixed-function vertex processing
        bool m_lighting;
        bool m_normalizeNormals;
        float m_globalAmbient[4];
        uint8_t m_diffuseSource;          // GxMetal::MaterialSource
        uint8_t m_ambientSource;
        uint8_t m_specularSource;
        uint8_t m_emissiveSource;
        float m_materialDiffuse[4];
        float m_materialAmbient[4];
        float m_materialSpecular[4];
        float m_materialEmissive[4];
        float m_materialPower;
        bool m_specularEnable;
        HwLight m_lights[GxMetal::kMaxLights];
        float m_world[16];
        float m_view[16];
        float m_projection[16];
        HwStage m_stages[8];

        // Textures, retained, and their samplers
        GxMetal::Texture* m_textures[16];
        GxMetal::SamplerKey m_samplers[16];

        // Shaders, retained, and their constants
        GxMetal::Shader* m_vertexShader;
        GxMetal::Shader* m_pixelShader;
        float m_vsConstants[256 * 4];
        float m_psConstants[256 * 4];

        // Geometry: streams and index buffer retained
        GxMetal::VertexLayout* m_layout;
        HwStream m_streams[16];
        GxMetal::Buffer* m_indices;
    };

    // What the open render encoder already has, valid while the context's pass id matches
    struct Bound {
        uint64_t m_passId;
        id<MTLRenderPipelineState> m_pipeline;
        id<MTLDepthStencilState> m_depthStencil;
        int32_t m_cull;
        int32_t m_fill;
        float m_depthBias;
        bool m_blendColorSet;
        float m_blendColor[4];
        MTLViewport m_viewport;
        MTLScissorRect m_scissor;
        bool m_viewportSet;
        id<MTLBuffer> m_vertexBuffers[16];
        uint32_t m_vertexOffsets[16];
        id<MTLTexture> m_textures[16];
        id<MTLSamplerState> m_samplers[16];
        uint64_t m_vertexFunction;
        uint64_t m_fragmentFunction;
    };

    void* m_hwnd;
    int32_t m_ownhwnd;
    GxMetal::Context* m_metal;
    GxMetal::PipelineCache* m_pipelines;
    HwState m_hw;
    Bound m_bound;
    bool m_vsConstantsDirty;
    bool m_psConstantsDirty;
    bool m_vertexStateDirty;
    bool m_pixelStateDirty;
    bool m_fixedVertexDirty;
    GxMetal::VertexLayout* m_formatLayouts [GxVertexBufferFormats_Last];
    TSGrowableArray<GxMetal::VertexLayout*> m_customLayouts;
    int32_t m_inScene;
    int32_t m_ambientOnlyMode;
    // The window's targets: back buffer, its depth buffer, and the depth buffer of texture
    // targets without their own (CGxDeviceD3d::m_defDepthStencilSurface)
    GxMetal::Texture* m_backBuffer;
    GxMetal::Texture* m_depthBuffer;
    // The vertex streams and the index buffer the client bound; retained while bound
    GxMetal::Buffer* m_streamBufs[16];
    uint32_t m_streamOffsets[16];
    uint32_t m_streamStrides[16];
    GxMetal::Buffer* m_indexBuf;
    // The encoder pass the stream bindings were applied for; a new pass invalidates them
    uint64_t m_streamPassId;
    // The opaque texture a CGxTex holds after a forced recreation (the shim's m_texture3B58)
    GxMetal::Texture* m_defaultTexture;
    GxMetal::Texture* m_textureDepthBuffer;
    GxMetal::Target m_savedDepthTarget;
    uint32_t m_samples;
    int32_t m_hwCursorDirty;
    void* m_hwCursorImage;
    CGxTex* m_texture3B58;
    bool m_texEnabled [8];
    float m_stereoConvergence;
    float m_stereoSeparation;

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
    virtual void DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane);
    virtual void DeviceSaveDepthSurface();
    virtual void DeviceRestoreDepthSurface();
    virtual void CapsWindowSize(CRect& rect);
    virtual void CapsWindowSizeInScreenCoords(CRect& rect);
    virtual void ScenePresent();
    virtual void SceneClear(uint32_t mask, CImVector color);
    virtual void DeviceSetGamma(float gamma);
    virtual void DeviceSetGamma(const CGxGammaRamp& ramp);
    virtual void PoolSizeSet(CGxPool* pool, uint32_t size);
    virtual void PoolDestroy(CGxPool* pool);
    virtual char* BufLock(CGxBuf* buf);
    virtual int32_t BufUnlock(CGxBuf* buf, uint32_t size);
    virtual void BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset);
    virtual void ITexMarkAsUpdated(CGxTex* texId);
    virtual void TexDestroy(CGxTex* texId);
    virtual void DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane);
    virtual void DeviceResolveDepthBuffer(CGxTex* texture);
    virtual void DeviceSetBaseMipLevel(uint32_t level);
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

    static EGxTexFormat s_GxTexFormatToUse[GxTexFormats_Last];
    static const EGxTexFormat s_tolerableTexFmtMapping[GxTexFormats_Last];

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
    // A CGxTex's texture as a render target (the face and level of the client's handle)
    GxMetal::Target ITexTarget(CGxTex* texId, uint32_t face, uint32_t level);
    void ITargetsFromState();
    // Pools and buffers (slice 3)
    GxMetal::Buffer* ICreateBuffer(CGxPool* pool);
    int32_t CreatePoolAPI(CGxPool* pool);
    void IReleasePool(CGxPool* pool);
    void IReleasePools(int32_t freePools);
    char* IBufLock(CGxBuf* buf);
    void IBufUnlock(CGxBuf* buf);
    void ISetVertexBuffer(uint32_t stream, GxMetal::Buffer* buffer, uint32_t offset, uint32_t stride);
    // Textures (slice 2)
    static GxMetal::Texture*& ITexData(CGxTex* texId);
    GxMetal::Texture* IDefaultTexture();
    void ITexCreate(CGxTex* texId);
    void ITexUpload(CGxTex* texId);
    void ITexForceRecreation(int32_t all);
    void IPollWindow();
    // The window's client size in pixels
    void IClientSize(int32_t* width, int32_t* height);
};

CGxDevice* CreateCGxDeviceMetalNative();

#endif
