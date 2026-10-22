#include "water_fft_metal.h"

#include <algorithm>
#include <cstring>
#include "gpu_profile.h"
#include "log.h"
#include "water_spectrum.h"

namespace
{
constexpr float kLargestFoamStepSeconds = 0.1f;

struct Float4
{
    float x, y, z, w;
};

// vw_water.metal Butterfly
struct ButterflyEntry
{
    float twiddle[2];
    uint32_t first;
    uint32_t second;
};

float FoamStepSeconds(float deltaSeconds)
{
    return deltaSeconds > 0.0f ? std::min(deltaSeconds, kLargestFoamStepSeconds) : 0.0f;
}

id<MTLComputePipelineState> Kernel(id<MTLDevice> device, id<MTLLibrary> library, NSString* name)
{
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (!function)
        return nil;
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline)
        VF_LOG_ERROR("water wave kernel %s failed: %s", name.UTF8String, error.localizedDescription.UTF8String);
    return pipeline;
}
}

WaterFft::WaterFft(id<MTLDevice> device, id<MTLLibrary> library) : m_device(device)
{
    m_evolve = Kernel(device, library, @"vw_fft_evolve");
    m_rows = Kernel(device, library, @"vw_fft_rows");
    m_columns = Kernel(device, library, @"vw_fft_columns");
    m_surface = Kernel(device, library, @"vw_fft_surface");
    m_mip = Kernel(device, library, @"vw_fft_mip");
    m_unsupported = !m_evolve || !m_rows || !m_columns || !m_surface || !m_mip ||
                    m_rows.maxTotalThreadsPerThreadgroup < static_cast<NSUInteger>(kWaterFftHighResolution) ||
                    m_columns.maxTotalThreadsPerThreadgroup < static_cast<NSUInteger>(kWaterFftHighResolution);
}

bool WaterFft::Fail(const char* reason)
{
    m_failure = reason;
    return false;
}

id<MTLTexture> WaterFft::MakeTexture(MTLPixelFormat format, uint32_t side, uint32_t levels, MTLTextureUsage usage)
{
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:side
                                                                                         height:side mipmapped:levels > 1];
    descriptor.mipmapLevelCount = levels;
    descriptor.usage = usage;
    descriptor.storageMode = MTLStorageModePrivate;
    return [m_device newTextureWithDescriptor:descriptor];
}

// WaterFftPasses::Prepare and EnsureTables: the noise and butterfly tables of the resolution, the spectrum
// scratch pair
bool WaterFft::Prepare(int resolution)
{
    if (m_unsupported)
        return Fail("wave kernels unsupported");
    if (!IsWaterFftResolution(resolution))
        return Fail("wave resolution is not 128 or 256");
    if (resolution == m_resolution && m_noise && m_butterflies && m_spectrum[0] && m_spectrum[1])
        return true;

    m_resolution = 0;
    for (TileMaps& maps : m_tiles)
        maps = TileMaps{maps.params};
    const uint32_t side = static_cast<uint32_t>(resolution);

    const std::vector<float> noise = WaterSpectrumNoise(resolution);
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                         width:side height:side mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    m_noise = [m_device newTextureWithDescriptor:descriptor];
    if (!m_noise)
        return Fail("wave noise upload failed");
    [m_noise replaceRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0 withBytes:noise.data()
               bytesPerRow:side * kWaterFftTexelChannels * sizeof(float)];

    const std::vector<WaterButterfly> butterflies = WaterButterflies(resolution);
    std::vector<ButterflyEntry> entries(butterflies.size());
    for (size_t i = 0; i < butterflies.size(); ++i)
        entries[i] = {{static_cast<float>(butterflies[i].twiddle.real()), static_cast<float>(butterflies[i].twiddle.imag())},
                      static_cast<uint32_t>(butterflies[i].first), static_cast<uint32_t>(butterflies[i].second)};
    m_butterflies = [m_device newBufferWithBytes:entries.data() length:entries.size() * sizeof(ButterflyEntry)
                                         options:MTLResourceStorageModeShared];
    if (!m_butterflies)
        return Fail("wave butterfly table upload failed");

    for (int i = 0; i < 2; ++i)
    {
        m_spectrum[i] = MakeTexture(MTLPixelFormatRGBA32Float, side, 1, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
        if (!m_spectrum[i])
            return Fail("wave spectrum creation failed");
    }
    m_resolution = resolution;
    return true;
}

void WaterFft::ForgetChangedTiles(const std::vector<WaterFftTile>& tiles)
{
    m_tiles.resize(tiles.size());
    for (size_t i = 0; i < tiles.size(); ++i)
        if (std::memcmp(&m_tiles[i].params, &tiles[i], sizeof(WaterFftTile)) != 0)
            m_tiles[i] = TileMaps{tiles[i]};
}

// EnsureTileMaps and ClearFoam: a new tile starts without foam
bool WaterFft::EnsureTileMaps(id<MTLCommandBuffer> commands, TileMaps& maps)
{
    if (maps.surface && maps.foam[0] && maps.foam[1])
        return true;
    const uint32_t side = static_cast<uint32_t>(m_resolution);
    const uint32_t levels = static_cast<uint32_t>(WaterFftStages(m_resolution) + 1);
    const MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    maps.surface = MakeTexture(MTLPixelFormatRGBA16Float, side, levels, usage);
    maps.foam[0] = MakeTexture(MTLPixelFormatRGBA16Float, side, 1, usage | MTLTextureUsageRenderTarget);
    maps.foam[1] = MakeTexture(MTLPixelFormatRGBA16Float, side, 1, usage | MTLTextureUsageRenderTarget);
    if (!maps.surface || !maps.foam[0] || !maps.foam[1])
    {
        maps = TileMaps{maps.params};
        return false;
    }
    NSMutableArray<id<MTLTexture>>* views = [NSMutableArray arrayWithCapacity:levels];
    for (uint32_t level = 0; level < levels; ++level)
        [views addObject:[maps.surface newTextureViewWithPixelFormat:MTLPixelFormatRGBA16Float textureType:MTLTextureType2D
                                                              levels:NSMakeRange(level, 1) slices:NSMakeRange(0, 1)]];
    maps.surfaceLevels = views;
    for (id<MTLTexture> foam : maps.foam)
    {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = foam;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = ProfiledRenderEncoder(commands, pass, "CoAVolFog foam clear");
        [encoder endEncoding];
    }
    maps.latestFoam = 0;
    maps.simulated = false;
    return true;
}

void WaterFft::Dispatch(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline, uint32_t width,
                        uint32_t height)
{
    [encoder setComputePipelineState:pipeline];
    const NSUInteger w = pipeline.threadExecutionWidth;
    const NSUInteger h = std::max<NSUInteger>(1, pipeline.maxTotalThreadsPerThreadgroup / w);
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
       threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(w, width), std::min<NSUInteger>(h, height), 1)];
    ++m_dispatches;
}

// WaterFft::Simulate: Prepare, then per tile Evolve, Transform and UpdateMaps (surface, foam, mips)
bool WaterFft::Simulate(id<MTLCommandBuffer> commands, const WaterFftSettings& settings,
                        const std::vector<WaterFftTile>& tiles, uint32_t tileMask, double seconds, float deltaSeconds)
{
    m_failure = "";
    m_dispatches = 0;
    if (settings.referenceResolution <= 0)
        return Fail("wave reference resolution is not positive");
    if (tiles.size() > static_cast<size_t>(kWaterMaxTiles))
        return Fail("more wave tiles than the simulation holds");
    if (!Prepare(settings.resolution))
        return false;
    ForgetChangedTiles(tiles);
    for (size_t i = 0; i < tiles.size(); ++i)
        if ((tileMask & (1u << i)) && !EnsureTileMaps(commands, m_tiles[i]))
            return Fail("wave map creation failed");

    const int resolution = m_resolution;
    const uint32_t side = static_cast<uint32_t>(resolution);
    const int stages = WaterFftStages(resolution);
    const float sideF = static_cast<float>(resolution);
    const float foamStep = FoamStepSeconds(deltaSeconds);
    const WaterLoopClock clock = MakeWaterLoopClock(seconds);
    const float inverseReferenceArea = static_cast<float>(
        1.0 / (static_cast<double>(settings.referenceResolution) * settings.referenceResolution));

    id<MTLComputeCommandEncoder> encoder = ProfiledComputeEncoder(commands, "CoAVolFog water waves");
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        if (!(tileMask & (1u << i)))
            continue;
        const WaterFftTile& tile = tiles[i];
        TileMaps& maps = m_tiles[i];

        // Evolve
        const WaterSpectrumShape shape = MakeWaterSpectrumShape(tile, settings.windSpeed, settings.windDirection);
        const Float4 evolve[4] = {
            {sideF, 1.0f / sideF, 0.0f, 0.5f * sideF},
            {static_cast<float>(kWaterTwoPi / shape.length), static_cast<float>(shape.amplitude * settings.amplitudeScale),
             static_cast<float>(shape.alignment), static_cast<float>(shape.inversePeakLengthSquared)},
            {static_cast<float>(shape.wind[0]), static_cast<float>(shape.wind[1]), 0.0f, 0.0f},
            {clock.coarse, clock.fine, WaterLoopFrequenciesPerRootRadius(shape.length), 0.0f},
        };
        [encoder setBytes:evolve length:sizeof(evolve) atIndex:0];
        [encoder setTexture:m_noise atIndex:0];
        [encoder setTexture:m_spectrum[0] atIndex:1];
        Dispatch(encoder, m_evolve, side, side);

        // Transform: every row, then every column with the displacement assembled
        for (const bool columns : {false, true})
        {
            const Float4 transform = {sideF, static_cast<float>(stages), inverseReferenceArea, columns ? 1.0f : 0.0f};
            const int source = columns ? 1 : 0;
            [encoder setComputePipelineState:columns ? m_columns : m_rows];
            [encoder setBytes:&transform length:sizeof(transform) atIndex:0];
            [encoder setBuffer:m_butterflies offset:0 atIndex:1];
            [encoder setTexture:m_spectrum[source] atIndex:0];
            [encoder setTexture:m_spectrum[1 - source] atIndex:1];
            [encoder setThreadgroupMemoryLength:side * sizeof(Float4) atIndex:0];
            [encoder setThreadgroupMemoryLength:side * sizeof(Float4) atIndex:1];
            [encoder dispatchThreadgroups:MTLSizeMake(side, 1, 1) threadsPerThreadgroup:MTLSizeMake(side, 1, 1)];
            ++m_dispatches;
        }

        // UpdateMaps: the surface and the next foam from the displacement (in spectrum 0), then the mips
        const WaterSurfaceStencil stencil = MakeWaterSurfaceStencil(resolution, WaterTileLength(tile));
        const Float4 surface[4] = {
            {sideF, 1.0f / sideF, 0.0f, 1.0f / sideF},
            {static_cast<float>(stencil.halfTexelsPerUnit), static_cast<float>(stencil.wideRadius),
             static_cast<float>(stencil.inverseWideBaseline), foamStep},
            {tile.foam[0], tile.foam[1], tile.foam[2], 0.0f},
            {tile.oxygen[0], tile.oxygen[1], tile.oxygen[2], 0.0f},
        };
        const int next = 1 - maps.latestFoam;
        [encoder setBytes:surface length:sizeof(surface) atIndex:0];
        [encoder setTexture:m_spectrum[0] atIndex:0];
        [encoder setTexture:maps.foam[maps.latestFoam] atIndex:1];
        [encoder setTexture:maps.surfaceLevels[0] atIndex:2];
        [encoder setTexture:maps.foam[next] atIndex:3];
        Dispatch(encoder, m_surface, side, side);
        for (int level = 1; level <= stages; ++level)
        {
            [encoder setTexture:maps.surfaceLevels[level - 1] atIndex:0];
            [encoder setTexture:maps.surfaceLevels[level] atIndex:1];
            Dispatch(encoder, m_mip, side >> level, side >> level);
        }
        maps.latestFoam = next;
        maps.simulated = true;
    }
    [encoder endEncoding];
    return true;
}

id<MTLTexture> WaterFft::Surface(int tile) const
{
    if (tile < 0 || static_cast<size_t>(tile) >= m_tiles.size() || !m_tiles[tile].simulated)
        return nil;
    return m_tiles[tile].surface;
}

id<MTLTexture> WaterFft::Foam(int tile) const
{
    if (tile < 0 || static_cast<size_t>(tile) >= m_tiles.size() || !m_tiles[tile].simulated)
        return nil;
    return m_tiles[tile].foam[m_tiles[tile].latestFoam];
}
