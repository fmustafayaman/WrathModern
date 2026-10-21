#pragma once

// noise_volume.h of the Windows CoAVolFog, with the volumes returned as texels instead of D3D9 textures

#include <cstdint>
#include <vector>

using BYTE = uint8_t;
using UINT = uint32_t;

constexpr UINT kDensityNoiseSize = 32;
constexpr UINT kAuthoredNoiseSize = 64;

std::vector<BYTE> DensityNoiseVolume();

void PrepareAuthoredNoise();
BYTE AuthoredNoiseTexel(UINT x, UINT y, UINT z);
const std::vector<BYTE>& AuthoredNoiseVolume();
