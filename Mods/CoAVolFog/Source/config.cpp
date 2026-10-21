#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <vector>

namespace
{

constexpr const char* kSection = "CoAVolFog";

struct IntSetting
{
    const char* key;
    int Config::*value;
    int lo;
    int hi;
};

struct FloatSetting
{
    const char* key;
    float Config::*value;
    float lo;
    float hi;
};

struct BoolSetting
{
    const char* key;
    bool Config::*value;
};

// The tables and ranges of the Windows build's config.cpp
const IntSetting kIntSettings[] = {
    {"Quality", &Config::quality, 1, 3},     {"StockFog", &Config::stockFog, 0, 1},
    {"DataMode", &Config::dataMode, 0, 1},   {"ColorSpace", &Config::colorSpace, 0, 1},
    {"DebugView", &Config::debugView, 0, 3}, {"LogLevel", &Config::logLevel, 0, 2},
    {"ClassicPhase", &Config::classicPhase, 0, 1}, {"ForeverGlow", &Config::foreverGlow, 0, 1},
    {"WaterQuality", &Config::waterQuality, 1, 3}, {"WaterDebugView", &Config::waterDebugView, 0, 6},
};

const FloatSetting kFloatSettings[] = {
    {"Density", &Config::density, 0.0f, 10.0f},
    {"Haze", &Config::haze, 0.0f, 10.0f},
    {"GroundFog", &Config::groundFog, 0.0f, 10.0f},
    {"FarFog", &Config::farFog, 0.0f, 10.0f},
    {"SunScatter", &Config::sunScatter, 0.0f, 10.0f},
    {"Ambient", &Config::ambient, 0.0f, 10.0f},
    {"Exposure", &Config::exposure, 0.0f, 10.0f},
    {"ClassicExposure", &Config::classicExposure, 0.0f, 10.0f},
    {"LocalLightIntensity", &Config::localLightIntensity, 0.0f, 8.0f},
    {"LocalLightPhase", &Config::localLightPhase, -0.9f, 0.9f},
    {"InteriorDensity", &Config::interiorDensity, 0.0f, 1.0f},
    {"InteriorFarFog", &Config::interiorFarFog, 0.0f, 1.0f},
    {"NoiseAmount", &Config::noiseAmount, 0.0f, 1.0f},
    {"NoiseScale", &Config::noiseScale, 0.001f, 1.0f},
    {"NoiseWindSpeed", &Config::noiseWindSpeed, 0.0f, 10.0f},
    {"GodRays", &Config::godRays, 0.0f, 4.0f},
    {"FarClipMax", &Config::farClipMax, 0.0f, kEngineFarClipMax},
    {"MaxDistance", &Config::maxDistance, 200.0f, 5000.0f},
    {"Temporal", &Config::temporal, 0.0f, 0.97f},
    {"ColorGrading", &Config::colorGrading, 0.0f, 1.0f},
    {"WaterWaves", &Config::waterWaves, 0.0f, 2.0f},
    {"WaterWind", &Config::waterWind, 0.5f, 10.0f},
    {"WaterFoam", &Config::waterFoam, 0.0f, 2.0f},
    {"WaterReflections", &Config::waterReflections, 0.0f, 2.0f},
    {"WaterSpecular", &Config::waterSpecular, 0.0f, 4.0f},
    {"WaterClarity", &Config::waterClarity, 0.25f, 4.0f},
    {"WaterZoneColors", &Config::waterZoneColors, 0.0f, 1.0f},
    {"WaterRipples", &Config::waterRipples, 0.0f, 2.0f},
};

const BoolSetting kBoolSettings[] = {
    {"Enable", &Config::enable},
    {"EngineHooks", &Config::hooks},
    {"LightStay", &Config::lightStay},
    {"Overlay", &Config::overlay},
    {"GlowCompensation", &Config::glowCompensation},
    {"LocalLights", &Config::localLights},
    {"InteriorAware", &Config::interiorAware},
    {"Underwater", &Config::underwater},
    {"LiquidDepth", &Config::liquidDepth},
    {"SunMarker", &Config::sunMarker},
    {"Water", &Config::water},
    {"Multisampling", &Config::multisampling},
    {"ClassicNoise", &Config::classicNoise},
    {"TransparentFog", &Config::transparentFog},
    {"WaterClientSplashes", &Config::waterClientSplashes},
};

std::string Trim(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();

    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        begin++;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        end--;

    return text.substr(begin, end - begin);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

// The [CoAVolFog] keys, case-insensitive as GetPrivateProfileString reads them; ';' and '#' start comments
bool ReadSection(const std::string& path, std::map<std::string, std::string>& values)
{
    FILE* file = std::fopen(path.c_str(), "rb");

    if (!file)
        return false;

    bool inSection = false;
    char line[1024];

    while (std::fgets(line, sizeof(line), file))
    {
        std::string text = Trim(line);

        if (text.empty() || text[0] == ';' || text[0] == '#')
            continue;

        if (text[0] == '[')
        {
            size_t close = text.find(']');
            inSection = close != std::string::npos && Lower(Trim(text.substr(1, close - 1))) == Lower(kSection);
            continue;
        }

        size_t equals = text.find('=');

        if (!inSection || equals == std::string::npos)
            continue;

        values[Lower(Trim(text.substr(0, equals)))] = Trim(text.substr(equals + 1));
    }

    std::fclose(file);
    return true;
}

const std::string* Find(const std::map<std::string, std::string>& values, const char* key)
{
    auto found = values.find(Lower(key));
    return found != values.end() && !found->second.empty() ? &found->second : nullptr;
}

}

bool LoadConfig(const std::string& path, Config& out)
{
    Config config;
    std::map<std::string, std::string> values;
    bool read = ReadSection(path, values);

    for (const IntSetting& s : kIntSettings)
    {
        if (const std::string* text = Find(values, s.key))
            config.*s.value = std::clamp(std::atoi(text->c_str()), s.lo, s.hi);
    }

    for (const FloatSetting& s : kFloatSettings)
    {
        const std::string* text = Find(values, s.key);
        char* end = nullptr;
        float v = text ? std::strtof(text->c_str(), &end) : 0.0f;

        if (text && end != text->c_str() && v == v)
            config.*s.value = std::clamp(v, s.lo, s.hi);
    }

    for (const BoolSetting& s : kBoolSettings)
    {
        if (const std::string* text = Find(values, s.key))
            config.*s.value = std::clamp(std::atoi(text->c_str()), 0, 1) != 0;
    }

    out = config;
    return read;
}

std::string DescribeConfig(const Config& config)
{
    std::string text;
    char line[96];

    for (const IntSetting& s : kIntSettings)
    {
        std::snprintf(line, sizeof(line), "%s=%d ", s.key, config.*s.value);
        text += line;
    }

    for (const FloatSetting& s : kFloatSettings)
    {
        std::snprintf(line, sizeof(line), "%s=%g ", s.key, config.*s.value);
        text += line;
    }

    for (const BoolSetting& s : kBoolSettings)
    {
        std::snprintf(line, sizeof(line), "%s=%d ", s.key, config.*s.value ? 1 : 0);
        text += line;
    }

    return text;
}

bool SameFogSettings(const Config& a, const Config& b)
{
    for (const IntSetting& s : kIntSettings)
        if (a.*s.value != b.*s.value)
            return false;
    for (const FloatSetting& s : kFloatSettings)
        if (a.*s.value != b.*s.value)
            return false;
    for (const BoolSetting& s : kBoolSettings)
        if (a.*s.value != b.*s.value)
            return false;
    return true;
}

namespace
{

std::string ValueText(const Config& config, const char* key)
{
    char kind;
    float lo, hi, value;
    char text[32] = "";
    if (GetSetting(config, key, kind, lo, hi, value))
    {
        if (kind == 'f')
            std::snprintf(text, sizeof(text), "%g", value);
        else
            std::snprintf(text, sizeof(text), "%d", static_cast<int>(value));
    }
    return text;
}

template <typename Visit>
void ForEachKey(Visit visit)
{
    for (const IntSetting& s : kIntSettings)
        visit(s.key);
    for (const FloatSetting& s : kFloatSettings)
        visit(s.key);
    for (const BoolSetting& s : kBoolSettings)
        visit(s.key);
}

}

bool GetSetting(const Config& config, const char* key, char& kind, float& lo, float& hi, float& value)
{
    std::string wanted = Lower(key);
    for (const IntSetting& s : kIntSettings)
        if (Lower(s.key) == wanted)
        {
            kind = 'i', lo = static_cast<float>(s.lo), hi = static_cast<float>(s.hi), value = static_cast<float>(config.*s.value);
            return true;
        }
    for (const FloatSetting& s : kFloatSettings)
        if (Lower(s.key) == wanted)
        {
            kind = 'f', lo = s.lo, hi = s.hi, value = config.*s.value;
            return true;
        }
    for (const BoolSetting& s : kBoolSettings)
        if (Lower(s.key) == wanted)
        {
            kind = 'b', lo = 0.0f, hi = 1.0f, value = config.*s.value ? 1.0f : 0.0f;
            return true;
        }
    return false;
}

bool SetSetting(Config& config, const char* key, float value, float& applied)
{
    if (value != value)
        return false;
    std::string wanted = Lower(key);
    for (const IntSetting& s : kIntSettings)
        if (Lower(s.key) == wanted)
        {
            config.*s.value = std::clamp(static_cast<int>(std::lround(value)), s.lo, s.hi);
            applied = static_cast<float>(config.*s.value);
            return true;
        }
    for (const FloatSetting& s : kFloatSettings)
        if (Lower(s.key) == wanted)
        {
            config.*s.value = std::clamp(value, s.lo, s.hi);
            applied = config.*s.value;
            return true;
        }
    for (const BoolSetting& s : kBoolSettings)
        if (Lower(s.key) == wanted)
        {
            config.*s.value = value >= 0.5f;
            applied = config.*s.value ? 1.0f : 0.0f;
            return true;
        }
    return false;
}

bool SaveConfig(const std::string& path, const Config& config)
{
    std::vector<std::string> lines;
    if (FILE* file = std::fopen(path.c_str(), "rb"))
    {
        char line[1024];
        while (std::fgets(line, sizeof(line), file))
        {
            std::string text = line;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            lines.push_back(text);
        }
        std::fclose(file);
    }

    std::map<std::string, bool> written;
    bool inSection = false;
    size_t sectionEnd = std::string::npos;

    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string text = Trim(lines[i]);
        if (!text.empty() && text[0] == '[')
        {
            if (inSection && sectionEnd == std::string::npos)
                sectionEnd = i;
            size_t close = text.find(']');
            inSection = close != std::string::npos && Lower(Trim(text.substr(1, close - 1))) == Lower(kSection);
            continue;
        }
        size_t equals = text.find('=');
        if (!inSection || text.empty() || text[0] == ';' || text[0] == '#' || equals == std::string::npos)
            continue;
        std::string key = Trim(text.substr(0, equals));
        char kind;
        float lo, hi, value;
        if (!GetSetting(config, key.c_str(), kind, lo, hi, value))
            continue;
        // A line whose value reads the same keeps its own spelling (1.0 stays 1.0)
        std::string current = Trim(text.substr(equals + 1));
        char* end = nullptr;
        float existing = std::strtof(current.c_str(), &end);
        bool same = end != current.c_str() &&
                    (kind == 'f' ? existing == value : static_cast<int>(existing) == static_cast<int>(value));
        if (!same)
            lines[i] = key + "=" + ValueText(config, key.c_str());
        written[Lower(key)] = true;
    }

    // Keys the file lacks go at the end of the section, or into a new one
    std::vector<std::string> missing;
    ForEachKey([&](const char* key) {
        if (!written.count(Lower(key)))
            missing.push_back(std::string(key) + "=" + ValueText(config, key));
    });
    bool hasSection = inSection || sectionEnd != std::string::npos ||
                      std::any_of(lines.begin(), lines.end(), [](const std::string& l) { return Lower(Trim(l)) == "[coavolfog]"; });
    if (!hasSection)
        lines.push_back("[CoAVolFog]");
    size_t insertAt = sectionEnd != std::string::npos ? sectionEnd : lines.size();
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), missing.begin(), missing.end());

    std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    for (const std::string& line : lines)
        std::fprintf(file, "%s\r\n", line.c_str());
    bool ok = std::fclose(file) == 0;
    return ok && std::rename(temporary.c_str(), path.c_str()) == 0;
}
