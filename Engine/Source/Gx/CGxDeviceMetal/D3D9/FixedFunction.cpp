#include "FixedFunction.h"
#include "D3D9Types.h"

#include <cstdarg>
#include <cstdio>

namespace D3D9Metal {

namespace {

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));

std::string Format(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

const char kComponents[] = "xyzw";

const char* const kFixedVertexStructs = R"MSL(
struct D3DFixedLight {
    float4 diffuse;
    float4 specular;
    float4 ambient;
    float4 position;
    float4 direction;
    float range;
    float falloff;
    float attenuation0;
    float attenuation1;
    float attenuation2;
    float cosHalfTheta;
    float cosHalfPhi;
    uint type;
};

struct D3DFixedVertexState {
    float4x4 world;
    float4x4 worldView;
    float4x4 normal;
    float4x4 projection;
    float4x4 texture[8];
    float4 globalAmbient;
    float4 materialDiffuse;
    float4 materialAmbient;
    float4 materialSpecular;
    float4 materialEmissive;
    float4 materialPower;
    D3DFixedLight lights[4];
    float4 fog;
    float4 pointScale;
};

)MSL";

const char* AttributeType(uint8_t attributeClass) {
    switch (attributeClass) {
        case ATTRIBCLASS_UINT: return "uint4";
        case ATTRIBCLASS_SINT: return "int4";
        default: return "float4";
    }
}

const char* MaterialSource(uint8_t source, const char* material) {
    switch (source) {
        case D3DMCS_COLOR1: return "color0";
        case D3DMCS_COLOR2: return "color1";
        default: return material;
    }
}

}

bool GenerateFixedVertexShader(const FixedVertexKey& key, std::string& source, std::string& error) {
    source = kShaderPrelude;
    source += kFixedVertexStructs;

    bool hasInputs = false;
    std::string inputs = "struct D3DVertexInput {\n";
    for (uint32_t i = 0; i < ATTRIB_COUNT; i++) {
        if (key.m_attributes[i]) {
            inputs += Format("    %s a%u [[attribute(%u)]];\n", AttributeType(key.m_attributes[i]), i, i);
            hasInputs = true;
        }
    }
    inputs += "};\n\n";

    if (hasInputs) {
        source += inputs;
    }

    source += Format("vertex D3DVaryings %s(%sconstant D3DVertexState& state [[buffer(%d)]], constant D3DFixedVertexState& ff [[buffer(%d)]]) {\n", kVertexFunctionName, hasInputs ? "D3DVertexInput in [[stage_in]], " : "", VS_BUFFER_STATE, VS_BUFFER_FIXED_FUNCTION);

    auto input = [&](uint32_t attribute, const char* fallback) -> std::string {
        if (!key.m_attributes[attribute]) {
            return fallback;
        }
        if (key.m_attributes[attribute] == ATTRIBCLASS_FLOAT) {
            return Format("in.a%u", attribute);
        }
        return Format("float4(in.a%u)", attribute);
    };

    source += "    float4 position = " + input(ATTRIB_POSITION, "float4(0.0f, 0.0f, 0.0f, 1.0f)") + ";\n";
    source += "    float3 normal = (" + input(ATTRIB_NORMAL, "float4(0.0f)") + ").xyz;\n";
    source += "    float4 color0 = " + input(ATTRIB_COLOR0, "float4(1.0f)") + ";\n";
    source += "    float4 color1 = " + input(ATTRIB_COLOR1, "float4(0.0f)") + ";\n";
    source += "    float4 worldPos = ff.world * position;\n";
    source += "    float4 vtx = ff.worldView * position;\n";
    source += "    float3 n = (ff.normal * float4(normal, 0.0f)).xyz;\n";

    if (key.m_normalizeNormals) {
        source += "    n = all(n == 0.0f) ? float3(0.0f) : normalize(n);\n";
    }

    source += "    float4 oPos = ff.projection * vtx;\n";
    source += "    D3DVaryings out;\n";

    // Texture coordinates: generation, then the texture transform
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t inputIndex = key.m_texcoordIndex[i] & 7;
        uint32_t flags = key.m_transformCount[i] & 7;
        uint32_t texcoordCount = key.m_texcoordComponents[inputIndex];
        uint32_t count = 4;
        std::string value;

        switch (key.m_texgen[i]) {
            case D3DTSS_TCI_CAMERASPACENORMAL >> 16:
                value = "float4(n, 1.0f)";
                break;

            case D3DTSS_TCI_CAMERASPACEPOSITION >> 16:
                value = "float4(vtx.xyz, 1.0f)";
                break;

            case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR >> 16:
                value = "float4(reflect(normalize(vtx.xyz), n), 1.0f)";
                break;

            case D3DTSS_TCI_SPHEREMAP >> 16:
                source += Format("    float3 reflection%u = reflect(normalize(vtx.xyz), n);\n", i);
                source += Format("    float sphere%u = 2.0f * length(reflection%u + float3(0.0f, 0.0f, 1.0f));\n", i, i);
                value = Format("float4(reflection%u.xy / sphere%u + 0.5f, 0.0f, 1.0f)", i, i);
                break;

            default:
                value = input(ATTRIB_TEXCOORD0 + inputIndex, "float4(0.0f, 0.0f, 0.0f, 1.0f)");
                // The transform flags count the components passed on, at most those of the input
                count = flags;
                if (texcoordCount) {
                    if (!count || count > texcoordCount) {
                        count = texcoordCount;
                    }
                } else {
                    flags = D3DTTFF_DISABLE;
                }
                break;
        }

        source += Format("    float4 tc%u = %s;\n", i, value.c_str());

        if (flags != D3DTTFF_DISABLE) {
            // Before the transform the first missing component reads as 1 and the rest as 0
            for (uint32_t j = count; j < 4; j++) {
                source += Format("    tc%u.%c = %s;\n", i, kComponents[j], j > texcoordCount ? "0.0f" : "1.0f");
            }

            source += Format("    tc%u = ff.texture[%u] * tc%u;\n", i, i, i);

            // Components past the count repeat the last one, the divisor of projected lookups
            for (uint32_t j = count; j < 4; j++) {
                source += Format("    tc%u.%c = tc%u.%c;\n", i, kComponents[j], i, kComponents[count - 1]);
            }
        }

        source += Format("    out.texcoord%u = tc%u;\n", i, i);
    }

    // Lighting
    if (key.m_lighting) {
        source += "    float4 ambientLight = float4(0.0f);\n";
        source += "    float4 diffuseLight = float4(0.0f);\n";
        source += "    float4 specularLight = float4(0.0f);\n";

        for (uint32_t i = 0; i < 4; i++) {
            uint32_t type = key.m_lightTypes[i];
            if (!type) {
                continue;
            }

            source += "    {\n";
            source += Format("        constant D3DFixedLight& light = ff.lights[%u];\n", i);

            if (type == D3DLIGHT_DIRECTIONAL) {
                source += "        float3 hitDir = normalize(-light.direction.xyz);\n";
                source += "        float atten = 1.0f;\n";
            } else {
                source += "        float3 delta = light.position.xyz - vtx.xyz;\n";
                source += "        float d = length(delta);\n";
                source += "        float3 hitDir = normalize(delta);\n";
                source += "        float atten = d > light.range ? 0.0f : min(1.0f / (light.attenuation0 + d * (light.attenuation1 + d * light.attenuation2)), FLT_MAX);\n";

                if (type == D3DLIGHT_SPOT) {
                    source += "        float rho = dot(-hitDir, normalize(light.direction.xyz));\n";
                    source += "        float spot = rho > light.cosHalfTheta ? 1.0f : rho <= light.cosHalfPhi ? 0.0f : pow(saturate((rho - light.cosHalfPhi) / (light.cosHalfTheta - light.cosHalfPhi)), light.falloff);\n";
                    source += "        atten *= spot;\n";
                }
            }

            source += "        float hitDot = saturate(dot(n, hitDir));\n";

            if (key.m_localViewer) {
                source += "        float3 mid = normalize(hitDir - normalize(vtx.xyz));\n";
            } else {
                source += "        float3 mid = normalize(hitDir - float3(0.0f, 0.0f, 1.0f));\n";
            }

            source += "        float midDot = saturate(dot(n, mid));\n";
            source += "        float specularness = midDot > 0.0f ? d3d_pow(midDot, ff.materialPower.x) * atten : 0.0f;\n";
            source += "        ambientLight += light.ambient * atten;\n";
            source += "        diffuseLight += light.diffuse * (hitDot * atten);\n";
            source += "        specularLight += light.specular * specularness;\n";
            source += "    }\n";
        }

        const char* diffuse = MaterialSource(key.m_diffuseSource, "ff.materialDiffuse");
        const char* ambient = MaterialSource(key.m_ambientSource, "ff.materialAmbient");
        const char* emissive = MaterialSource(key.m_emissiveSource, "ff.materialEmissive");
        const char* specular = MaterialSource(key.m_specularSource, "ff.materialSpecular");

        source += Format("    float4 lit0 = %s * ff.globalAmbient + %s;\n", ambient, emissive);
        source += Format("    lit0 = %s * ambientLight + lit0;\n", ambient);
        source += Format("    lit0 = %s * diffuseLight + lit0;\n", diffuse);
        source += Format("    lit0.w = %s.w;\n", diffuse);
        source += "    out.color0 = saturate(lit0);\n";
        source += Format("    out.color1 = saturate(%s * specularLight);\n", specular);
    } else {
        source += "    out.color0 = saturate(color0);\n";
        source += "    out.color1 = saturate(color1);\n";
    }

    // Vertex fog (the key holds D3DRS_FOGVERTEXMODE + 1, or 0 when fog is disabled)
    switch (key.m_fogMode) {
        case 0:
            source += "    out.fog = 1.0f;\n";
            break;

        case D3DFOG_NONE + 1:
            // Without a vertex fog mode the fog factor is the specular color's alpha
            source += Format("    out.fog = %s;\n", key.m_attributes[ATTRIB_COLOR1] ? "saturate(color1.w)" : "1.0f");
            break;

        case D3DFOG_LINEAR + 1:
        case D3DFOG_EXP + 1:
        case D3DFOG_EXP2 + 1:
            source += Format("    float fogDepth = %s;\n", key.m_rangeFog ? "length(vtx.xyz)" : "abs(vtx.z)");
            if (key.m_fogMode == D3DFOG_LINEAR + 1) {
                source += "    out.fog = saturate((ff.fog.y - fogDepth) * ff.fog.z);\n";
            } else if (key.m_fogMode == D3DFOG_EXP + 1) {
                source += "    out.fog = exp(-(fogDepth * ff.fog.w));\n";
            } else {
                source += "    out.fog = exp(-(fogDepth * ff.fog.w) * (fogDepth * ff.fog.w));\n";
            }
            break;

        default:
            error = "unknown fog mode";
            return false;
    }

    // Point size, scaled by distance when point scaling is on
    source += "    float pointSize = state.pointSize.x;\n";
    if (key.m_pointScale) {
        source += "    float eyeDistance = length(vtx.xyz);\n";
        source += "    pointSize *= ff.pointScale.w * sqrt(1.0f / (ff.pointScale.x + eyeDistance * (ff.pointScale.y + eyeDistance * ff.pointScale.z)));\n";
    }
    source += "    out.pointSize = clamp(pointSize, state.pointSize.y, state.pointSize.z);\n";

    // User clip planes are in world space for fixed-function vertex processing
    for (uint32_t i = 0; i < 6; i++) {
        source += Format("    out.clipDistance[%u] = (state.flags.x & %uu) ? dot(worldPos, state.clipPlanes[%u]) : 1.0f;\n", i, 1u << i, i);
    }

    source += "    out.position = oPos;\n";
    source += "    out.position.xy += state.posFixup.xy * oPos.w;\n";
    source += "    out.position.z += state.posFixup.z * oPos.w;\n";
    source += "    return out;\n}\n";

    return true;
}

bool GenerateFixedPixelShader(const FixedPixelKey& key, std::string& source, std::string& error) {
    // Stages in use: up to the first one whose color operation is disabled
    uint32_t stageCount = 0;
    while (stageCount < 8 && key.m_stages[stageCount].m_colorOp != D3DTOP_DISABLE && key.m_stages[stageCount].m_colorOp != 0) {
        stageCount++;
    }

    auto stageReadsTexture = [&](uint32_t i) {
        const FixedPixelStage& stage = key.m_stages[i];
        if (!stage.m_textureBound) {
            return false;
        }

        auto argReads = [](uint8_t arg) { return (arg & D3DTA_SELECTMASK) == D3DTA_TEXTURE; };
        auto opReads = [](uint8_t op) { return op == D3DTOP_BLENDTEXTUREALPHA || op == D3DTOP_BLENDTEXTUREALPHAPM; };

        for (uint32_t a = 0; a < 3; a++) {
            if (argReads(stage.m_colorArg[a]) || (stage.m_alphaOp != D3DTOP_DISABLE && argReads(stage.m_alphaArg[a]))) {
                return true;
            }
        }

        return opReads(stage.m_colorOp) || opReads(stage.m_alphaOp);
    };

    std::string body;
    std::string params;
    bool usesTexcoord[8] = {};
    bool usesColor1 = key.m_specularEnable != 0;

    body += "    float4 diffuse = in.color0;\n";
    body += "    float4 current = diffuse;\n";
    body += "    float4 temp = float4(0.0f);\n";

    for (uint32_t i = 0; i < stageCount; i++) {
        const FixedPixelStage& stage = key.m_stages[i];
        bool sampled = stageReadsTexture(i);

        body += Format("    // Stage %u\n", i);

        if (sampled) {
            SamplerKind kind = static_cast<SamplerKind>(stage.m_samplerKind);
            uint32_t components = kind == SAMPLERKIND_2D ? 2 : 3;
            const char* type;
            switch (kind) {
                case SAMPLERKIND_CUBE: type = "texturecube<float>"; break;
                case SAMPLERKIND_VOLUME: type = "texture3d<float>"; break;
                case SAMPLERKIND_DEPTH: type = "depth2d<float>"; break;
                default: type = "texture2d<float>"; break;
            }
            params += Format(", %s tex%u [[texture(%u)]], sampler smp%u [[sampler(%u)]]", type, i, i, i, i);

            if (key.m_pointSprite) {
                body += Format("    float4 coord%u = float4(pointCoord, 0.0f, 1.0f);\n", i);
            } else {
                usesTexcoord[i] = true;
                body += Format("    float4 coord%u = in.texcoord%u;\n", i, i);
            }

            if (stage.m_projected) {
                // The divisor is the last counted component, or w
                uint32_t divisor = stage.m_projected >= 2 && stage.m_projected <= 4 ? stage.m_projected - 1 : 3;
                body += Format("    coord%u /= coord%u.%c;\n", i, i, kComponents[divisor]);
            }

            switch (kind) {
                case SAMPLERKIND_DEPTH:
                    body += Format("    float4 texture%u = float4(tex%u.sample_compare(smp%u, coord%u.xy, coord%u.z, level(0)));\n", i, i, i, i, i);
                    break;
                default:
                    body += Format("    float4 texture%u = tex%u.sample(smp%u, coord%u.%s);\n", i, i, i, i, components == 2 ? "xy" : "xyz");
                    break;
            }
        }

        auto arg = [&](uint8_t value) -> std::string {
            std::string reg;
            switch (value & D3DTA_SELECTMASK) {
                case D3DTA_DIFFUSE: reg = "diffuse"; break;
                case D3DTA_CURRENT: reg = "current"; break;
                case D3DTA_TEXTURE: reg = sampled ? Format("texture%u", i) : "float4(0.0f, 0.0f, 0.0f, 1.0f)"; break;
                case D3DTA_TFACTOR: reg = "state.textureFactor"; break;
                case D3DTA_SPECULAR: reg = "in.color1"; usesColor1 = true; break;
                case D3DTA_TEMP: reg = "temp"; break;
                default: reg = "float4(1.0f)"; break;
            }
            if (value & D3DTA_COMPLEMENT) {
                reg = "(1.0f - " + reg + ")";
            }
            if (value & D3DTA_ALPHAREPLICATE) {
                reg = "(" + reg + ").wwww";
            }
            return reg;
        };

        auto op = [&](uint8_t operation, const uint8_t* args, std::string& out) -> bool {
            std::string a0 = arg(args[0]);
            std::string a1 = arg(args[1]);
            std::string a2 = arg(args[2]);
            std::string textureAlpha = sampled ? Format("texture%u.wwww", i) : "float4(1.0f)";

            switch (operation) {
                case D3DTOP_SELECTARG1: out = a1; return true;
                case D3DTOP_SELECTARG2: out = a2; return true;
                case D3DTOP_MODULATE: out = "(" + a1 + " * " + a2 + ")"; return true;
                case D3DTOP_MODULATE2X: out = "saturate(" + a1 + " * " + a2 + " * 2.0f)"; return true;
                case D3DTOP_MODULATE4X: out = "saturate(" + a1 + " * " + a2 + " * 4.0f)"; return true;
                case D3DTOP_ADD: out = "saturate(" + a1 + " + " + a2 + ")"; return true;
                case D3DTOP_ADDSIGNED: out = "saturate(" + a1 + " + " + a2 + " - 0.5f)"; return true;
                case D3DTOP_ADDSIGNED2X: out = "saturate((" + a1 + " + " + a2 + " - 0.5f) * 2.0f)"; return true;
                case D3DTOP_SUBTRACT: out = "saturate(" + a1 + " - " + a2 + ")"; return true;
                case D3DTOP_ADDSMOOTH: out = "saturate((1.0f - " + a1 + ") * " + a2 + " + " + a1 + ")"; return true;
                case D3DTOP_BLENDDIFFUSEALPHA: out = "mix(" + a2 + ", " + a1 + ", diffuse.wwww)"; return true;
                case D3DTOP_BLENDTEXTUREALPHA: out = "mix(" + a2 + ", " + a1 + ", " + textureAlpha + ")"; return true;
                case D3DTOP_BLENDFACTORALPHA: out = "mix(" + a2 + ", " + a1 + ", state.textureFactor.wwww)"; return true;
                case D3DTOP_BLENDTEXTUREALPHAPM: out = "saturate(" + a2 + " * (1.0f - " + textureAlpha + ") + " + a1 + ")"; return true;
                case D3DTOP_BLENDCURRENTALPHA: out = "mix(" + a2 + ", " + a1 + ", current.wwww)"; return true;
                case D3DTOP_MODULATEALPHA_ADDCOLOR: out = "saturate((" + a1 + ").wwww * " + a2 + " + " + a1 + ")"; return true;
                case D3DTOP_MODULATECOLOR_ADDALPHA: out = "saturate(" + a1 + " * " + a2 + " + (" + a1 + ").wwww)"; return true;
                case D3DTOP_MODULATEINVALPHA_ADDCOLOR: out = "saturate((1.0f - (" + a1 + ").wwww) * " + a2 + " + " + a1 + ")"; return true;
                case D3DTOP_MODULATEINVCOLOR_ADDALPHA: out = "saturate((1.0f - " + a1 + ") * " + a2 + " + (" + a1 + ").wwww)"; return true;
                case D3DTOP_DOTPRODUCT3: out = "float4(saturate(dot((" + a1 + ").xyz - 0.5f, (" + a2 + ").xyz - 0.5f) * 4.0f))"; return true;
                case D3DTOP_MULTIPLYADD: out = "saturate(" + a1 + " * " + a2 + " + " + a0 + ")"; return true;
                case D3DTOP_LERP: out = "mix(" + a2 + ", " + a1 + ", " + a0 + ")"; return true;
                default:
                    error = Format("unsupported texture operation %u", operation);
                    return false;
            }
        };

        const char* dst = stage.m_resultTemp ? "temp" : "current";
        std::string color;
        if (!op(stage.m_colorOp, stage.m_colorArg, color)) {
            return false;
        }

        bool sameAlpha = stage.m_colorOp == stage.m_alphaOp && memcmp(stage.m_colorArg, stage.m_alphaArg, 3) == 0;

        if (sameAlpha || stage.m_colorOp == D3DTOP_DOTPRODUCT3) {
            body += Format("    %s = %s;\n", dst, color.c_str());
        } else {
            body += Format("    float4 color%u = %s;\n", i, color.c_str());

            if (stage.m_alphaOp != D3DTOP_DISABLE) {
                std::string alpha;
                if (!op(stage.m_alphaOp, stage.m_alphaArg, alpha)) {
                    return false;
                }
                body += Format("    float alpha%u = (%s).w;\n", i, alpha.c_str());
                body += Format("    %s = float4(color%u.xyz, alpha%u);\n", dst, i, i);
            } else {
                body += Format("    %s.xyz = color%u.xyz;\n", dst, i);
            }
        }
    }

    if (key.m_specularEnable) {
        body += "    current.xyz += in.color1.xyz;\n";
    }

    source = kShaderPrelude;
    source += "\nstruct D3DFragmentInput {\n";
    source += "    float4 color0 [[user(d3d_color0)]];\n";
    if (usesColor1) {
        source += "    float4 color1 [[user(d3d_color1)]];\n";
    }
    for (uint32_t i = 0; i < 8; i++) {
        if (usesTexcoord[i]) {
            source += Format("    float4 texcoord%u [[user(d3d_texcoord%u)]];\n", i, i);
        }
    }
    source += "    float fog [[user(d3d_fog)]];\n};\n\n";
    source += "struct D3DFragmentOutput {\n    float4 color [[color(0)]];\n};\n\n";

    source += Format("fragment D3DFragmentOutput %s(D3DFragmentInput in [[stage_in]], constant D3DPixelState& state [[buffer(%d)]]%s%s) {\n", kFragmentFunctionName, PS_BUFFER_STATE, params.c_str(), key.m_pointSprite ? ", float2 pointCoord [[point_coord]]" : "");
    source += body;
    source += "    D3DFragmentOutput out;\n";
    source += "    out.color = d3d_fog(state, current, in.fog);\n";
    source += "    if (!d3d_alpha_test(state, out.color.a)) discard_fragment();\n";
    source += "    return out;\n}\n";

    return true;
}

}
