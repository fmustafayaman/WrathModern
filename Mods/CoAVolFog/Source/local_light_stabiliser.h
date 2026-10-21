#pragma once

#include "engine_lights.h"

#include <cstdint>

constexpr float kLocalLightFadeSeconds = 1.0f;
constexpr float kLocalLightPrioritySmoothingSeconds = 0.3f;
constexpr double kLocalLightSwapMargin = 1.5;
constexpr uint32_t kLocalLightSwapCaptures = 3;
constexpr uint32_t kLocalLightHoldCaptures = 8;
constexpr float kLocalLightIdentityYards = 2.0f;
constexpr float kLocalLightViewExitYards = 5.0f;
constexpr double kLocalLightResetSeconds = 1.0;
constexpr double kLocalLightActivityLogSeconds = 60.0;
constexpr uint32_t kLocalLightEntries = kLocalLightCandidatePool + kMaxLocalPointLights;

struct LocalLightActivity
{
    uint32_t admissions = 0;
    uint32_t evictions = 0;
    uint32_t fadeOuts = 0;
    uint32_t heldCaptures = 0;
};

class LocalLightStabiliser
{
public:
    void Step(const LocalLightCamera& camera, int mapId, uint32_t frame, double seconds, LocalLightInputs& lights);
    void Reset();
    const LocalLightActivity& Activity() const { return m_activity; }

private:
    struct Entry
    {
        LocalPointLight light;
        double priority = 0.0;
        double smoothedPriority = 0.0;
        float weight = 0.0f;
        uint32_t challengeCaptures = 0;
        bool slotted = false;
        bool evicted = false;
        bool heirToEvictedSlot = false;
        bool enteredAtViewEdge = false;
        bool present = false;
        bool outOfView = false;
    };

    void Replay(LocalLightInputs& lights) const;
    bool HoldOutput(LocalLightInputs& lights);
    void MatchCandidates(const LocalLightCamera& camera, const LocalLightInputs& lights, float dt);
    Entry* FindEntry(const LocalPointLight& light);
    Entry* AddEntry(const LocalPointLight& light, double priority);
    void AdvanceWeights(float dt);
    static bool TakesFreeSlotBefore(const Entry& entry, const Entry& other);
    void FillFreeSlots();
    void ChallengeWeakestSlot();
    void RemoveSpentEntries();
    void WriteOutput(LocalLightInputs& lights);
    void LogActivity(double seconds);

    Entry m_entries[kLocalLightEntries] = {};
    uint32_t m_count = 0;
    LocalPointLight m_held[kMaxLocalPointLights] = {};
    uint32_t m_heldCount = 0;
    uint32_t m_rejectedCaptures = 0;
    bool m_sceneStarting = true;
    int m_mapId = 0;
    uint32_t m_lastFrame = 0;
    double m_lastSeconds = 0.0;
    double m_lastLogSeconds = 0.0;
    bool m_started = false;
    LocalLightActivity m_activity;
    LocalLightActivity m_loggedActivity;
};
