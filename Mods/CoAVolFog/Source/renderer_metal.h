#pragma once

// The fog passes of the Windows CoAVolFog's Renderer::RenderPasses on Metal: the same inputs, constants
// (register numbers c0..c98) and pass order, drawn into the frame the client hands to the mod.

#include <cstdint>
#include "config.h"
#include "fog_data.h"
#include "fog_model.h"
#include "local_light_stabiliser.h"

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
    LocalLightStabiliser m_stabiliser;
    double m_noiseSeconds = 0.0;
    uint32_t m_frame = 0;

    // Temporal history (Renderer::RenderPasses)
    float m_prevWorldToView[16] = {};
    float m_prevProj[16] = {};
    float m_prevCam[3] = {};
    uint32_t m_prevScale = 0;
    uint32_t m_prevWidth = 0;
    uint32_t m_prevHeight = 0;
    Config m_prevConfig;
    int m_prevMap = -1;
    int m_prevLightSlot = -1;
    uint32_t m_prevLocalLightCount = 0;
    double m_prevSeconds = 0.0;
    bool m_historyValid = false;
    int m_historyIndex = 0;
    float m_loggedBlend = -1.0f;
    bool m_loggedFrame = false;
    bool m_loggedRays = false;
    bool m_loggedGrading = false;
};
