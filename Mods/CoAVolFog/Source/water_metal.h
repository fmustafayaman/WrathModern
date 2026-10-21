#pragma once

// The water of the Windows CoAVolFog (water_renderer.cpp) on Metal. Slice 7.1: the liquid draws are tagged
// with their class in the stencil plane of the window depth (GxMetalSetStencilTag), and WaterDebugView 5 shows
// the classes.

#include "config.h"
#include "water_types.h"

struct WMModHost;

class WaterRenderer
{
public:
    void LiquidsBegin(const Config& cfg);
    void LiquidDraw(const void* settings, bool begin);
    void LiquidsEnd(const WMModHost* host, const Config& cfg);
    void Release();

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_inLiquids = false;
    bool m_tagged = false;
    uint32_t m_classesDrawn = 0;
};
