#pragma once

// The fog passes of the Windows CoAVolFog's Renderer::RenderPasses on Metal: the same inputs, constants
// (register numbers c0..c98) and pass order, drawn into the frame the client hands to the mod.

#include <cstdint>
#include "config.h"
#include "fog_data.h"
#include "fog_model.h"

struct WMModHost;
struct WMWorldState;

struct SavedStockFog
{
    bool pushed = false;
    float start = 0.0f;
    float end = 0.0f;
};

class FogRenderer
{
public:
    // Draws the fog over the completed world (the hook before the screen effects). stock carries the client's
    // fog range of group 1 when it was pushed out of range for this frame. Returns whether fog was drawn.
    bool Render(const WMModHost* host, const Config& cfg, const FogData& data, const SavedStockFog& stock);
    void Release();

private:
    struct Impl;
    Impl* m_impl = nullptr;
    AuthoredNoiseScroll m_noiseScroll;
    double m_noiseSeconds = 0.0;
    uint32_t m_frame = 0;
    float m_loggedBlend = -1.0f;
    bool m_loggedFrame = false;
};
