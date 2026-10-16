// Parses every permutation of the .bls shader files given on the command line and prints which D3D9
// shader features they use, per profile. Used to check the Metal translator covers the client's
// shaders: every opcode, register type and modifier listed must be handled.
//
//   shaderstat <file.bls>...

#include "../../Engine/Source/Gx/CGxDeviceMetal/D3D9/ShaderBytecode.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace D3D9Metal;

namespace {

const char* RegisterName(ShaderStage stage, uint32_t type) {
    switch (type) {
        case REG_TEMP: return "r";
        case REG_INPUT: return "v";
        case REG_CONST: return "c";
        case REG_ADDR: return stage == STAGE_VERTEX ? "a" : "t";
        case REG_RASTOUT: return "rast";
        case REG_ATTROUT: return "oD";
        case REG_OUTPUT: return stage == STAGE_VERTEX ? "oT/o" : "?6";
        case REG_CONSTINT: return "i";
        case REG_COLOROUT: return "oC";
        case REG_DEPTHOUT: return "oDepth";
        case REG_SAMPLER: return "s";
        case REG_CONSTBOOL: return "b";
        case REG_LOOP: return "aL";
        case REG_MISCTYPE: return "misc";
        case REG_LABEL: return "l";
        case REG_PREDICATE: return "p";
        default: return "?";
    }
}

struct Stats {
    uint32_t m_files = 0;
    uint32_t m_programs = 0;
    uint32_t m_failures = 0;
    std::map<std::string, uint32_t> m_opcodes;
    std::map<std::string, uint32_t> m_features;
    uint32_t m_maxConst = 0;
    uint32_t m_maxTemp = 0;
    uint32_t m_maxInstructions = 0;
};

void Count(std::map<std::string, uint32_t>& map, const std::string& key) {
    map[key]++;
}

void Analyze(const ShaderProgram& program, Stats& stats) {
    ShaderStage stage = program.m_stage;
    stats.m_maxInstructions = std::max<uint32_t>(stats.m_maxInstructions, program.m_instructions.size());

    for (const Instruction& instruction : program.m_instructions) {
        std::string name = OpcodeName(instruction.m_opcode);
        if (instruction.m_opcode == OP_TEX && program.m_major >= 2 && instruction.m_control) {
            name += instruction.m_control == TEXLD_PROJECT ? "(p)" : instruction.m_control == TEXLD_BIAS ? "(b)" : "(?)";
        }
        if (instruction.m_opcode == OP_IFC || instruction.m_opcode == OP_BREAKC || instruction.m_opcode == OP_SETP) {
            name += "(cmp" + std::to_string(instruction.m_control) + ")";
        }
        Count(stats.m_opcodes, name);

        if (instruction.m_coissue) Count(stats.m_features, "coissue");
        if (instruction.m_predicated) Count(stats.m_features, "predicated");

        if (instruction.m_opcode == OP_DCL) {
            const DestOperand& dest = instruction.m_dest;
            if (dest.m_type == REG_SAMPLER) {
                Count(stats.m_features, "dcl sampler type " + std::to_string(instruction.m_samplerType));
            } else {
                Count(stats.m_features, std::string("dcl ") + RegisterName(stage, dest.m_type) + " usage " + std::to_string(instruction.m_usage) + "/" + std::to_string(instruction.m_usageIndex)
                    + (dest.m_centroid ? " centroid" : "") + (dest.m_partialPrecision ? " pp" : ""));
            }
            continue;
        }
        if (instruction.m_opcode == OP_DEF || instruction.m_opcode == OP_DEFI || instruction.m_opcode == OP_DEFB) {
            continue;
        }

        if (instruction.m_hasDest) {
            const DestOperand& dest = instruction.m_dest;
            Count(stats.m_features, std::string("dst ") + RegisterName(stage, dest.m_type));
            if (dest.m_type != REG_TEMP) Count(stats.m_features, std::string("dst ") + RegisterName(stage, dest.m_type) + std::to_string(dest.m_index) + " mask " + std::to_string(dest.m_writeMask));
            if (dest.m_saturate) Count(stats.m_features, "dst _sat");
            if (dest.m_partialPrecision) Count(stats.m_features, "dst _pp");
            if (dest.m_centroid) Count(stats.m_features, "dst _centroid");
            if (dest.m_shift) Count(stats.m_features, "dst shift " + std::to_string(dest.m_shift));
            if (dest.m_relative) Count(stats.m_features, "dst relative");
            if (dest.m_writeMask != 0xF) Count(stats.m_features, "dst partial mask");
            if (dest.m_type == REG_TEMP) stats.m_maxTemp = std::max(stats.m_maxTemp, dest.m_index);
        }

        for (const SourceOperand& source : instruction.m_sources) {
            Count(stats.m_features, std::string("src ") + RegisterName(stage, source.m_type));
            if (source.m_type != REG_TEMP && source.m_type != REG_CONST) Count(stats.m_features, std::string("src ") + RegisterName(stage, source.m_type) + std::to_string(source.m_index));
            if (source.m_modifier) Count(stats.m_features, "src mod " + std::to_string(source.m_modifier));
            if (source.m_relative) {
                Count(stats.m_features, std::string("src relative ") + RegisterName(stage, source.m_type) + " via " + RegisterName(stage, source.m_address.m_type));
            }
            if (source.m_type == REG_CONST) stats.m_maxConst = std::max(stats.m_maxConst, source.m_index);
        }
    }
}

bool ReadFile(const char* path, std::vector<uint8_t>& data) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    data.resize(size);
    bool ok = fread(data.data(), 1, size, file) == static_cast<size_t>(size);
    fclose(file);
    return ok;
}

std::string ProfileOf(const char* path) {
    // .../{pixel,vertex}/<profile>/<name>.bls
    std::string p = path;
    size_t slash = p.rfind('/');
    if (slash == std::string::npos) return "?";
    size_t previous = p.rfind('/', slash - 1);
    return p.substr(previous + 1, slash - previous - 1);
}

}

int main(int argc, char** argv) {
    std::map<std::string, Stats> profiles;

    for (int arg = 1; arg < argc; arg++) {
        const char* path = argv[arg];
        std::string profile = ProfileOf(path);
        Stats& stats = profiles[profile];

        std::vector<uint8_t> data;
        if (!ReadFile(path, data) || data.size() < 12 || memcmp(data.data(), "HSXG", 4) != 0) {
            fprintf(stderr, "%s: not a GXSH file\n", path);
            stats.m_failures++;
            continue;
        }
        stats.m_files++;

        size_t offset = 12;
        while (offset + 16 <= data.size()) {
            uint32_t codeSize;
            memcpy(&codeSize, data.data() + offset + 12, 4);
            offset += 16;
            if (offset + codeSize > data.size()) {
                fprintf(stderr, "%s: truncated permutation\n", path);
                stats.m_failures++;
                break;
            }

            if (codeSize) {
                std::vector<uint32_t> tokens(codeSize / 4);
                memcpy(tokens.data(), data.data() + offset, tokens.size() * 4);

                ShaderProgram program;
                std::string error;
                stats.m_programs++;
                if (ParseShader(tokens.data(), tokens.size(), program, error)) {
                    Analyze(program, stats);
                    char version[16];
                    snprintf(version, sizeof(version), "%c%c_%u_%u", program.m_stage == STAGE_VERTEX ? 'v' : 'p', 's', program.m_major, program.m_minor);
                    if (profile != version) {
                        Count(stats.m_features, std::string("VERSION MISMATCH ") + version);
                    }
                } else {
                    fprintf(stderr, "%s: %s\n", path, error.c_str());
                    stats.m_failures++;
                }
            }

            offset += (codeSize + 3) & ~3u;
        }
    }

    for (auto& [profile, stats] : profiles) {
        printf("== %s: %u files, %u programs, %u failures, max c%u, max r%u, max %u instructions\n", profile.c_str(), stats.m_files, stats.m_programs, stats.m_failures, stats.m_maxConst, stats.m_maxTemp, stats.m_maxInstructions);
        printf("  opcodes:");
        for (auto& [name, count] : stats.m_opcodes) printf(" %s=%u", name.c_str(), count);
        printf("\n  features:\n");
        for (auto& [name, count] : stats.m_features) printf("    %s = %u\n", name.c_str(), count);
    }
    return 0;
}
