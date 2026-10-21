#pragma once

// The water of the Windows CoAVolFog (src/water_renderer.cpp) on Metal. Before the liquids are drawn the world
// is copied (colour, resolved when multisampled, and linear depth) and the stencil plane of the window depth
// cleared; each liquid draw tags its pixels with its water class (GxMetalSetStencilTag) and writes depth
// (GxMetalForceDepthWrite); after the liquids, the waves are simulated (water_fft_metal) and each tagged class
// is shaded over the client's water with its preset (vw_water.metal).
//
// Not yet here: the ripples of the water contacts (slice 8: the ripple constants keep them off).

#include <cstdint>
#include "config.h"
#include "water_types.h"

struct WMModHost;
class FogData;
class WaterData;

class WaterRenderer
{
public:
    // fogDrewLastFrame and stockFogPushed: hooks.cpp's g_renderedLastFrame and g_stockFogPushed
    void LiquidsBegin(const WMModHost* host, const Config& cfg, const WaterData& data, bool fogDrewLastFrame,
                      bool stockFogPushed);
    void LiquidDraw(const void* settings, bool begin);
    void LiquidsEnd(const WMModHost* host, const FogData& fog);
    void Release();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
