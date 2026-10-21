#pragma once

// WaterFft of the Windows CoAVolFog (src/water_fft.cpp) on Metal: per frame, each wave tile the drawn water
// uses is evolved from its spectrum, transformed to displacement, and turned into a surface map (slopes and
// their moments, mipmapped) and a foam state map that carries over between frames. The tables and settings
// come from water_spectrum.cpp unchanged; the passes are compute dispatches (vw_water.metal). Objective-C++.

#import <Metal/Metal.h>

#include <cstdint>
#include <vector>
#include "water_types.h"

struct WaterFftSettings
{
    int resolution;
    int referenceResolution;
    float windSpeed;
    float windDirection[2];
    float amplitudeScale = 1.0f;
};

class WaterFft
{
public:
    // The kernels come from library (vw_water.metal)
    WaterFft(id<MTLDevice> device, id<MTLLibrary> library);

    // Simulates the tiles of tileMask into their maps; false (with LastFailure) when it cannot
    bool Simulate(id<MTLCommandBuffer> commands, const WaterFftSettings& settings, const std::vector<WaterFftTile>& tiles,
                  uint32_t tileMask, double seconds, float deltaSeconds);

    // RGBA16F, mipmapped: slope x, slope y, slope squared, wide slope squared
    id<MTLTexture> Surface(int tile) const;
    // RGBA16F: foam and oxygen, each split in a coarse part and a residual for half-precision storage
    id<MTLTexture> Foam(int tile) const;
    // RGBA32F: the displacement of the last simulated tile (x, y, up, residual), for the tests
    id<MTLTexture> Displacement() const { return m_spectrum[0]; }
    const char* LastFailure() const { return m_failure; }
    int Resolution() const { return m_resolution; }
    unsigned Dispatches() const { return m_dispatches; }

private:
    struct TileMaps
    {
        WaterFftTile params = {};
        id<MTLTexture> surface = nil;
        NSArray<id<MTLTexture>>* surfaceLevels = nil;
        id<MTLTexture> foam[2] = {nil, nil};
        int latestFoam = 0;
        bool simulated = false;
    };

    bool Fail(const char* reason);
    bool Prepare(int resolution);
    void ForgetChangedTiles(const std::vector<WaterFftTile>& tiles);
    bool EnsureTileMaps(id<MTLCommandBuffer> commands, TileMaps& maps);
    void Dispatch(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline, uint32_t width, uint32_t height);
    id<MTLTexture> MakeTexture(MTLPixelFormat format, uint32_t side, uint32_t levels, MTLTextureUsage usage);

    id<MTLDevice> m_device;
    id<MTLComputePipelineState> m_evolve = nil;
    id<MTLComputePipelineState> m_rows = nil;
    id<MTLComputePipelineState> m_columns = nil;
    id<MTLComputePipelineState> m_surface = nil;
    id<MTLComputePipelineState> m_mip = nil;
    id<MTLTexture> m_noise = nil;
    id<MTLBuffer> m_butterflies = nil;
    id<MTLTexture> m_spectrum[2] = {nil, nil};
    std::vector<TileMaps> m_tiles;
    int m_resolution = 0;
    unsigned m_dispatches = 0;
    const char* m_failure = "";
    bool m_unsupported = false;
};
