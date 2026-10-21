// Prints the Metal translation of one permutation of a .bls file.
//   shaderdump <file.bls> <permutation>
#include "../../Engine/Source/Gx/CGxDeviceMetal/Shader/ShaderTranslator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace GxMetal;
int main(int argc, char** argv) {
    if (argc < 3) return 1;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<uint8_t> data(1 << 22);
    data.resize(fread(data.data(), 1, data.size(), f));
    fclose(f);
    uint32_t want = atoi(argv[2]), index = 0;
    for (size_t offset = 12; offset + 16 <= data.size(); index++) {
        uint32_t size;
        memcpy(&size, data.data() + offset + 12, 4);
        offset += 16;
        if (index == want) {
            std::vector<uint32_t> tokens(size / 4);
            memcpy(tokens.data(), data.data() + offset, tokens.size() * 4);
            ShaderProgram program;
            ShaderInfo info;
            std::string error, source;
            if (!ParseShader(tokens.data(), tokens.size(), program, error) || !AnalyzeShader(program, info, error)) { puts(error.c_str()); return 1; }
            bool ok;
            if (program.m_stage == STAGE_VERTEX) {
                VertexInputSignature inputs;
                for (uint32_t i = 0; i < ATTRIB_COUNT; i++) if (info.m_attributeMask & (1u << i)) inputs.m_class[i] = i == ATTRIB_BLENDINDICES ? ATTRIBCLASS_UINT : ATTRIBCLASS_FLOAT;
                ok = TranslateVertexShader(program, inputs, source, error);
            } else {
                PixelShaderVariant variant;
                variant.m_samplerKinds = info.m_declaredKinds;
                ok = TranslatePixelShader(program, variant, source, error);
            }
            const char* body = strstr(source.c_str(), "\nstruct D3DVertexInput");
            if (!body) body = strstr(source.c_str(), "\nstatic float4 load_c");
            if (!body) body = strstr(source.c_str(), "\nstruct D3DFragmentInput");
            if (!body) body = strstr(source.c_str(), "\nstruct D3DFragmentOutput");
            puts(ok ? (body ? body : source.c_str()) : error.c_str());
            return ok ? 0 : 1;
        }
        offset += (size + 3) & ~3u;
    }
    return 1;
}
