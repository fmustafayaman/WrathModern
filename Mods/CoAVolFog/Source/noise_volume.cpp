#include "noise_volume.h"

// The Windows build's noise_volume.cpp without its Direct3D 9 volume textures: the generators are unchanged;
// DensityNoiseVolume and AuthoredNoiseVolume return the texels for a Metal R8Unorm 3D texture, x fastest.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
constexpr uint32_t kAuthoredNoiseBaseCells = 4;
constexpr int kAuthoredNoiseDetailLayers = 3;
constexpr float kAuthoredNoiseDetailGain = 0.5f;
constexpr float kByteCentre = 127.5f;
constexpr int kEdgeGradientCount = 12;
constexpr float kEdgeGradients[kEdgeGradientCount][3] = {
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
    {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
};

BYTE DensityNoiseValue(UINT x, UINT y, UINT z)
{
    uint32_t value = x + kDensityNoiseSize * (y + kDensityNoiseSize * z);
    value = ((value ^ (value >> 7)) * 4051u + 12743u) & 0x7FFFu;
    value ^= value >> 9;
    value = (value * 109u + 583u) & 0x7FFFu;
    value ^= value >> 6;
    value = (value * 29317u + 9419u) & 0x7FFFu;
    value ^= value >> 7;
    return static_cast<BYTE>(value >> 7);
}

uint32_t LatticeHash(uint32_t x, uint32_t y, uint32_t z, uint32_t detailLayer)
{
    uint32_t h = (x * 0x8DA6B343u) ^ (y * 0xD8163841u) ^ (z * 0xCB1AB31Fu) ^ (detailLayer * 0x165667B1u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

float QuinticFade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

constexpr uint32_t kFinestLatticePeriod = kAuthoredNoiseBaseCells << (kAuthoredNoiseDetailLayers - 1);

class GradientLattice
{
public:
    GradientLattice(uint32_t period, uint32_t detailLayer) : m_period(period), m_gradients(period * period * period)
    {
        for (uint32_t z = 0; z < period; ++z)
            for (uint32_t y = 0; y < period; ++y)
                for (uint32_t x = 0; x < period; ++x)
                    m_gradients[x + period * (y + period * z)] =
                        kEdgeGradients[LatticeHash(x, y, z, detailLayer) % kEdgeGradientCount];
    }

    uint32_t Period() const { return m_period; }

    const float* const* Row(uint32_t y, uint32_t z) const
    {
        return &m_gradients[m_period * (y % m_period + m_period * (z % m_period))];
    }

private:
    uint32_t m_period;
    std::vector<const float*> m_gradients;
};

struct LatticeCoordinate
{
    uint32_t cell;
    uint32_t nextCell;
    float offset;
    float fade;
};

std::vector<LatticeCoordinate> TexelLatticeCoordinates(uint32_t period)
{
    std::vector<LatticeCoordinate> coordinates(kAuthoredNoiseSize);
    for (UINT texel = 0; texel < kAuthoredNoiseSize; ++texel)
    {
        const float position = (texel + 0.5f) / kAuthoredNoiseSize * period;
        const float floored = std::floor(position);
        const float offset = position - floored;
        const uint32_t cell = static_cast<uint32_t>(floored) % period;
        coordinates[texel] = {cell, (cell + 1) % period, offset, QuinticFade(offset)};
    }
    return coordinates;
}

struct RowGradientTerms
{
    float slope = 0.0f;
    float intercept = 0.0f;
};

void AddGradientNoiseRow(const GradientLattice& lattice, const std::vector<LatticeCoordinate>& coordinates, UINT y,
                         UINT z, float amplitude, float* row)
{
    const uint32_t period = lattice.Period();
    const LatticeCoordinate& along = coordinates[y];
    const LatticeCoordinate& across = coordinates[z];
    RowGradientTerms terms[kFinestLatticePeriod];
    for (uint32_t corner = 0; corner < 4; ++corner)
    {
        const uint32_t stepY = corner & 1u;
        const uint32_t stepZ = corner >> 1;
        const float weight = (stepY ? along.fade : 1.0f - along.fade) * (stepZ ? across.fade : 1.0f - across.fade);
        const float offsetY = along.offset - static_cast<float>(stepY);
        const float offsetZ = across.offset - static_cast<float>(stepZ);
        const float* const* gradients = lattice.Row(along.cell + stepY, across.cell + stepZ);
        for (uint32_t x = 0; x < period; ++x)
        {
            const float* g = gradients[x];
            terms[x].slope += weight * g[0];
            terms[x].intercept += weight * (g[1] * offsetY + g[2] * offsetZ);
        }
    }
    for (UINT x = 0; x < kAuthoredNoiseSize; ++x)
    {
        const LatticeCoordinate& c = coordinates[x];
        const RowGradientTerms& left = terms[c.cell];
        const RowGradientTerms& right = terms[c.nextCell];
        const float leftValue = left.slope * c.offset + left.intercept;
        const float rightValue = right.slope * (c.offset - 1.0f) + right.intercept;
        row[x] += amplitude * (leftValue + (rightValue - leftValue) * c.fade);
    }
}

std::vector<float> TileableGradientDetail()
{
    std::vector<float> values(kAuthoredNoiseSize * kAuthoredNoiseSize * kAuthoredNoiseSize, 0.0f);
    float amplitude = 1.0f;
    for (uint32_t layer = 0; layer < kAuthoredNoiseDetailLayers; ++layer)
    {
        const GradientLattice lattice(kAuthoredNoiseBaseCells << layer, layer);
        const std::vector<LatticeCoordinate> coordinates = TexelLatticeCoordinates(lattice.Period());
        for (UINT z = 0; z < kAuthoredNoiseSize; ++z)
            for (UINT y = 0; y < kAuthoredNoiseSize; ++y)
                AddGradientNoiseRow(lattice, coordinates, y, z, amplitude,
                                    &values[kAuthoredNoiseSize * (y + kAuthoredNoiseSize * z)]);
        amplitude *= kAuthoredNoiseDetailGain;
    }
    return values;
}

BYTE NearestByte(float nonNegative)
{
    return static_cast<BYTE>(std::clamp(static_cast<int>(nonNegative + 0.5f), 0, 255));
}

std::vector<BYTE> QuantizedAroundMedian(const std::vector<float>& values)
{
    std::vector<float> sorted = values;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float median = sorted[sorted.size() / 2];
    float extent = 0.0f;
    for (float value : values)
        extent = std::max(extent, std::fabs(value - median));
    std::vector<BYTE> bytes(values.size());
    for (size_t i = 0; i < values.size(); ++i)
    {
        const float centred = extent > 0.0f ? (values[i] - median) / extent : 0.0f;
        bytes[i] = NearestByte(kByteCentre + kByteCentre * centred);
    }
    return bytes;
}

const std::vector<BYTE>& AuthoredNoiseTexels()
{
    static const std::vector<BYTE> texels = QuantizedAroundMedian(TileableGradientDetail());
    return texels;
}

}

std::vector<BYTE> DensityNoiseVolume()
{
    std::vector<BYTE> texels(kDensityNoiseSize * kDensityNoiseSize * kDensityNoiseSize);
    for (UINT z = 0; z < kDensityNoiseSize; ++z)
        for (UINT y = 0; y < kDensityNoiseSize; ++y)
            for (UINT x = 0; x < kDensityNoiseSize; ++x)
                texels[x + kDensityNoiseSize * (y + kDensityNoiseSize * z)] = DensityNoiseValue(x, y, z);
    return texels;
}

void PrepareAuthoredNoise()
{
    AuthoredNoiseTexels();
}

BYTE AuthoredNoiseTexel(UINT x, UINT y, UINT z)
{
    const UINT size = kAuthoredNoiseSize;
    return AuthoredNoiseTexels()[x % size + size * (y % size + size * (z % size))];
}

const std::vector<BYTE>& AuthoredNoiseVolume()
{
    return AuthoredNoiseTexels();
}
