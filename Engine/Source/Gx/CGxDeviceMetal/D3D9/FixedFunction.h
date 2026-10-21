// Generates Metal Shading Language for the Direct3D 9 fixed-function pipeline: vertex
// transformation, lighting, texture coordinate generation and transforms, vertex fog and point
// size on the vertex side, and the texture stage cascade on the pixel side. Each distinct state
// combination becomes its own small function; the device caches them by key.

#ifndef ENGINE_SOURCE_GX_CGXDEVICEMETAL_D3D9_FIXEDFUNCTION_H
#define ENGINE_SOURCE_GX_CGXDEVICEMETAL_D3D9_FIXEDFUNCTION_H

#include "Engine/Source/Gx/CGxDeviceMetal/Shader/ShaderTranslator.h"
#include <cstdint>
#include <cstring>
#include <string>

namespace D3D9Metal {

using namespace GxMetal;

struct FixedVertexKey {
    uint8_t m_attributes[ATTRIB_COUNT];   // AttributeClass per attribute
    uint8_t m_texcoordComponents[8];      // components of each input texture coordinate set
    uint8_t m_lighting;
    uint8_t m_lightTypes[4];              // D3DLIGHTTYPE of enabled lights, 0 when disabled
    uint8_t m_diffuseSource;              // D3DMATERIALCOLORSOURCE, resolved against the vertex format
    uint8_t m_ambientSource;
    uint8_t m_specularSource;
    uint8_t m_emissiveSource;
    uint8_t m_localViewer;
    uint8_t m_normalizeNormals;
    uint8_t m_texcoordIndex[8];           // D3DTSS_TEXCOORDINDEX & 7
    uint8_t m_texgen[8];                  // D3DTSS_TEXCOORDINDEX >> 16
    uint8_t m_transformCount[8];          // D3DTSS_TEXTURETRANSFORMFLAGS & 7
    uint8_t m_fogMode;                    // D3DRS_FOGVERTEXMODE + 1, 0 when fog is disabled
    uint8_t m_rangeFog;
    uint8_t m_pointScale;
    uint8_t m_padding[3];

    FixedVertexKey() { memset(this, 0, sizeof(*this)); }
    bool operator==(const FixedVertexKey& other) const { return memcmp(this, &other, sizeof(*this)) == 0; }
};

struct FixedPixelStage {
    uint8_t m_colorOp;
    uint8_t m_colorArg[3];                // arg0, arg1, arg2
    uint8_t m_alphaOp;
    uint8_t m_alphaArg[3];
    uint8_t m_resultTemp;
    uint8_t m_samplerKind;                // SamplerKind of the bound texture
    uint8_t m_textureBound;
    uint8_t m_projected;                  // D3DTTFF count when D3DTTFF_PROJECTED is set, else 0
};

struct FixedPixelKey {
    FixedPixelStage m_stages[8];
    uint8_t m_specularEnable;
    uint8_t m_pointSprite;
    uint8_t m_padding[2];

    FixedPixelKey() { memset(this, 0, sizeof(*this)); }
    bool operator==(const FixedPixelKey& other) const { return memcmp(this, &other, sizeof(*this)) == 0; }
};

// Layout of D3DFixedVertexState in the generated source (vertex buffer VS_BUFFER_FIXED_FUNCTION).
// Matrices are Direct3D row-major matrices, which Metal reads as their transposes, so the
// generated code multiplies matrix * vector where Direct3D multiplies vector * matrix.
struct alignas(16) FixedLightBlock {
    float m_diffuse[4];
    float m_specular[4];
    float m_ambient[4];
    float m_position[4];                  // view space
    float m_direction[4];                 // view space
    float m_range;
    float m_falloff;
    float m_attenuation[3];
    float m_cosHalfTheta;
    float m_cosHalfPhi;
    uint32_t m_type;
};

struct alignas(16) FixedVertexStateBlock {
    float m_world[16];
    float m_worldView[16];
    float m_normal[16];                   // inverse transpose of world * view
    float m_projection[16];
    float m_texture[8][16];
    float m_globalAmbient[4];
    float m_materialDiffuse[4];
    float m_materialAmbient[4];
    float m_materialSpecular[4];
    float m_materialEmissive[4];
    float m_materialPower[4];
    FixedLightBlock m_lights[4];
    float m_fog[4];                       // x: start, y: end, z: 1 / (end - start), w: density
    float m_pointScale[4];                // x: A, y: B, z: C, w: viewport height
};

bool GenerateFixedVertexShader(const FixedVertexKey& key, std::string& source, std::string& error);
bool GenerateFixedPixelShader(const FixedPixelKey& key, std::string& source, std::string& error);

}

#endif
