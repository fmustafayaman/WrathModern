// Checks the Metal wave simulation against the double-precision reference of water_spectrum.cpp, with the
// tolerances of the Windows CoAVolFog's tests/water_fft_checks.h. Runs without the game:
//
//   M=Mods/CoAVolFog; clang++ -std=c++20 -O2 -fobjc-arc -I$M/Source -DSHADER_PATH="\"$PWD/$M/Shaders/vw_water.metal\"" \
//       $M/Tests/water_fft_check.mm $M/Source/water_fft_metal.mm $M/Source/water_spectrum.cpp \
//       -framework Metal -framework Foundation -o /tmp/water_fft_check && /tmp/water_fft_check
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
#include "log.h"
#include "water_fft_metal.h"
#include "water_spectrum.h"

void LogWrite(LogLevel, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static const WaterFftTile kLakeTiles[] = {
    {20, 32.0f, 7689.0f, 3.0f, 4.0f, {-1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}},
    {24, 64.0f, 50000.0f, 3.0f, 5.0f, {-0.16f, 12.0f, 3.0f}, {-0.16f, 2.0f, 2.0f}},
    {122, 16.0f, 30000.0f, 1.8f, 4.0f, {-1.0f, 0.0f, 1.0f}, {-0.158f, 1.0f, 0.506f}},
};
static const WaterFftTile kOceanTiles[] = {
    {44, 128.0f, 42000.0f, 1.8f, 2.0f, {-1.0f, 0.0f, 1.0f}, {-0.163f, 0.445f, 0.3f}},
    {45, 256.0f, 36000.0f, 1.8f, 5.0f, {-0.29f, 12.0f, 0.9f}, {-0.36f, 100.0f, 0.7f}},
    {46, 512.0f, 16000.0f, 1.8f, 6.0f, {-0.2f, 20.0f, 1.5f}, {-0.35f, 200.0f, 0.7f}},
    {96, 32.0f, 200000.0f, 4.0f, 6.0f, {-1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}},
};
constexpr int kReferenceResolution = 256;
constexpr float kWindSpeed = 2.0f;
constexpr double kSimulatedSeconds = 12.345;
constexpr float kFrameSeconds = 0.05f;
constexpr double kFloatTolerance = 1.0e-5;
constexpr double kHalfRelativeTolerance = 2.0e-3;
constexpr double kHalfAbsoluteTolerance = 1.0e-3;
constexpr double kFoamStateTolerance = 1.0e-5;
constexpr double kSmallestHalfStep = 5.9604644775390625e-8;

static int g_failures = 0;
static void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

// Every texel of a level as doubles
static std::vector<double> Read(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture, int level)
{
    const NSUInteger side = std::max<NSUInteger>(1, texture.width >> level);
    const bool half = texture.pixelFormat == MTLPixelFormatRGBA16Float;
    const NSUInteger texelBytes = half ? 8 : 16;
    id<MTLBuffer> buffer = [device newBufferWithLength:side * side * texelBytes options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(side, side, 1) toBuffer:buffer destinationOffset:0
      destinationBytesPerRow:side * texelBytes destinationBytesPerImage:side * side * texelBytes];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    std::vector<double> out(side * side * 4);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = half ? static_cast<double>(static_cast<const __fp16*>(buffer.contents)[i])
                      : static_cast<double>(static_cast<const float*>(buffer.contents)[i]);
    return out;
}

int main()
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        std::ifstream file(SHADER_PATH);
        std::stringstream text;
        text << file.rdbuf();
        // As water_metal.mm compiles the water library
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(text.str().c_str()) options:options error:&error];
        if (!library)
        {
            std::printf("compile failed: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }

        const float wind[2] = {1.0f, 0.0f};
        double worst = 0.0, residual = 0.0, parseval = 0.0, mean = 0.0;
        double interior = 0.0, boundary = 0.0, foamError = 0.0, mipError = 0.0;
        bool ran = true;
        for (int n : {kWaterFftLowResolution, kWaterFftHighResolution})
            for (int set = 0; set < 2; ++set)
            {
                std::vector<WaterFftTile> tiles = set == 0 ? std::vector<WaterFftTile>(std::begin(kLakeTiles), std::end(kLakeTiles))
                                                           : std::vector<WaterFftTile>(std::begin(kOceanTiles), std::end(kOceanTiles));
                for (size_t slot = 0; slot < tiles.size(); ++slot)
                {
                    WaterFft fft(device, library);
                    WaterFftSettings settings = {n, kReferenceResolution, kWindSpeed, {wind[0], wind[1]}};
                    id<MTLCommandBuffer> commands = [queue commandBuffer];
                    if (!fft.Simulate(commands, settings, tiles, 1u << slot, kSimulatedSeconds, kFrameSeconds))
                    {
                        std::printf("simulate failed: %s\n", fft.LastFailure());
                        ran = false;
                        continue;
                    }
                    [commands commit];
                    [commands waitUntilCompleted];
                    if (commands.error)
                    {
                        std::printf("gpu error: %s\n", commands.error.localizedDescription.UTF8String);
                        ran = false;
                        continue;
                    }

                    // Displacement vs WaterReferenceDisplacement
                    const std::vector<double> d = Read(device, queue, fft.Displacement(), 0);
                    const std::vector<WaterDisplacement> reference = WaterReferenceDisplacement(
                        tiles[slot], n, kReferenceResolution, kWindSpeed, wind, kSimulatedSeconds);
                    double scale = 0.0, error = 0.0, largestResidual = 0.0, squares = 0.0, sum = 0.0;
                    std::vector<WaterDisplacement> gpu(reference.size());
                    for (size_t i = 0; i < reference.size(); ++i)
                    {
                        gpu[i] = {d[i * 4], d[i * 4 + 1], d[i * 4 + 2], d[i * 4 + 3]};
                        scale = std::max({scale, std::fabs(reference[i].x), std::fabs(reference[i].y), std::fabs(reference[i].up)});
                        error = std::max({error, std::fabs(gpu[i].x - reference[i].x), std::fabs(gpu[i].y - reference[i].y),
                                          std::fabs(gpu[i].up - reference[i].up)});
                        largestResidual = std::max(largestResidual, std::fabs(gpu[i].residual));
                        squares += gpu[i].up * gpu[i].up;
                        sum += gpu[i].up;
                    }
                    double refSquares = 0.0;
                    for (const WaterDisplacement& r : reference)
                        refSquares += r.up * r.up;
                    worst = std::max(worst, error / scale);
                    residual = std::max(residual, largestResidual / scale);
                    parseval = std::max(parseval, std::fabs(squares - refSquares) / refSquares);
                    mean = std::max(mean, std::fabs(sum / reference.size()) / scale);
                    std::printf("  tile %3u N %d: error %.2e of %.3f yd, h rms %.4f yd\n", tiles[slot].foreverTileId, n,
                                error / scale, scale, std::sqrt(squares / reference.size()));

                    // Surface and foam vs WaterReferenceSurface of the GPU displacement
                    const std::vector<double> surface = Read(device, queue, fft.Surface(static_cast<int>(slot)), 0);
                    const std::vector<double> foam = Read(device, queue, fft.Foam(static_cast<int>(slot)), 0);
                    const std::vector<WaterSurfaceTexel> expected = WaterReferenceSurface(gpu, n, WaterTileLength(tiles[slot]));
                    double channelScale[4] = {};
                    for (const WaterSurfaceTexel& t : expected)
                    {
                        channelScale[0] = std::max(channelScale[0], std::fabs(t.slope[0]));
                        channelScale[1] = std::max(channelScale[1], std::fabs(t.slope[1]));
                        channelScale[2] = std::max(channelScale[2], t.slopeSquared);
                        channelScale[3] = std::max(channelScale[3], t.wideSlopeSquared);
                    }
                    for (int y = 0; y < n; ++y)
                        for (int x = 0; x < n; ++x)
                        {
                            const size_t i = static_cast<size_t>(y) * n + x;
                            const WaterSurfaceTexel& t = expected[i];
                            const double wanted[4] = {t.slope[0], t.slope[1], t.slopeSquared, t.wideSlopeSquared};
                            double e = 0.0;
                            for (int c = 0; c < 4; ++c)
                                e = std::max(e, std::fabs(surface[i * 4 + c] - wanted[c]) /
                                                    (kHalfRelativeTolerance * std::fabs(wanted[c]) +
                                                     kHalfAbsoluteTolerance * std::max(channelScale[c], 1.0e-6)));
                            double& region = (x == 0 || y == 0 || x == n - 1 || y == n - 1) ? boundary : interior;
                            region = std::max(region, e);
                            const double foamWanted = WaterFoamStep(0.0, t.jacobian, tiles[slot].foam, kFrameSeconds);
                            const double oxygenWanted = WaterFoamStep(0.0, t.jacobian, tiles[slot].oxygen, kFrameSeconds);
                            foamError = std::max({foamError, std::fabs(foam[i * 4] + foam[i * 4 + 2] - foamWanted),
                                                  std::fabs(foam[i * 4 + 1] + foam[i * 4 + 3] - oxygenWanted)});
                        }

                    // Each mip level is the 2x2 box average of the level above
                    std::vector<double> finer = surface;
                    for (int level = 1; level <= WaterFftStages(n); ++level)
                    {
                        const std::vector<double> coarser = Read(device, queue, fft.Surface(static_cast<int>(slot)), level);
                        const int side = n >> level;
                        for (int y = 0; y < side; ++y)
                            for (int x = 0; x < side; ++x)
                                for (int c = 0; c < 4; ++c)
                                {
                                    auto at = [&](int fx, int fy) { return finer[(static_cast<size_t>(fy) * side * 2 + fx) * 4 + c]; };
                                    const double box = 0.25 * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                                                               at(2 * x + 1, 2 * y + 1));
                                    const double got = coarser[(static_cast<size_t>(y) * side + x) * 4 + c];
                                    mipError = std::max(mipError, std::fabs(got - box) /
                                                                      (kHalfRelativeTolerance * std::fabs(box) + kSmallestHalfStep));
                                }
                        finer = coarser;
                    }
                }
            }
        // Every tile in one encoder (the spectrum scratch is shared) equals each tile alone, over two frames
        double batchError = 0.0;
        {
            std::vector<WaterFftTile> tiles(std::begin(kOceanTiles), std::end(kOceanTiles));
            WaterFftSettings settings = {256, kReferenceResolution, kWindSpeed, {wind[0], wind[1]}};
            WaterFft together(device, library);
            for (int frame = 0; frame < 2; ++frame)
            {
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                together.Simulate(commands, settings, tiles, 0xF, kSimulatedSeconds + frame * kFrameSeconds, kFrameSeconds);
                [commands commit];
                [commands waitUntilCompleted];
            }
            for (size_t slot = 0; slot < tiles.size(); ++slot)
            {
                WaterFft alone(device, library);
                for (int frame = 0; frame < 2; ++frame)
                {
                    id<MTLCommandBuffer> commands = [queue commandBuffer];
                    alone.Simulate(commands, settings, tiles, 1u << slot, kSimulatedSeconds + frame * kFrameSeconds, kFrameSeconds);
                    [commands commit];
                    [commands waitUntilCompleted];
                }
                for (bool foam : {false, true})
                {
                    const std::vector<double> a = Read(device, queue, foam ? together.Foam(slot) : together.Surface(slot), 0);
                    const std::vector<double> b = Read(device, queue, foam ? alone.Foam(slot) : alone.Surface(slot), 0);
                    for (size_t i = 0; i < a.size(); ++i)
                        batchError = std::max(batchError, std::fabs(a[i] - b[i]));
                }
            }
        }
        std::printf("  batched vs single tiles, two frames: largest difference %.3e\n", batchError);
        Check(batchError == 0.0, "tiles simulated together equal tiles simulated alone");
        std::printf("  GPU vs double reference: worst %.2e, residual %.2e, Parseval %.2e, mean %.2e\n", worst, residual,
                    parseval, mean);
        std::printf("  moments (fraction of fp16 tolerance): interior %.3f, edges %.3f; foam error %.2e; mips %.3f\n",
                    interior, boundary, foamError, mipError);
        Check(ran && worst <= kFloatTolerance && residual <= kFloatTolerance,
              "GPU evolution, fold and transforms match the double reference (lake and ocean tiles, N 128 and 256)");
        Check(ran && parseval <= 1.0e-4 && mean <= kFloatTolerance, "GPU heights keep Parseval's energy and a zero mean");
        Check(ran && interior <= 1.0 && boundary <= 1.0, "surface map holds the gradient moments, wrapping periodically");
        Check(ran && foamError <= kFoamStateTolerance, "foam and oxygen take one Euler step of the Jacobian ODE");
        Check(ran && mipError <= 1.0, "surface mips are 2x2 box averages");
    }
    return g_failures ? 1 : 0;
}
