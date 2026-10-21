#include "client_ripple_sprites.h"

#include "log.h"
#include "WoW/Source/WorldClient/World.h"

namespace
{
constexpr uint32_t kInfoLoggedTransitions = 2;
}

void ClientRippleSprites::Update(bool allowed, bool shaded, double seconds)
{
    if (!allowed)
    {
        Restore();
        return;
    }
    if (shaded || seconds < m_lastShaded)
        m_lastShaded = seconds;
    if (shaded)
        Hold();
    else if (m_holding && seconds - m_lastShaded > kClientSpriteGraceSeconds)
        Restore();
    else if (m_holding)
        Hold();
}

void ClientRippleSprites::Hold()
{
    const int32_t current = CWorld::s_waterRipples;
    if (!m_holding)
        LogTransition("hidden while the ripples run", current);
    if (!m_holding || current != 0)
        m_remembered = current;
    m_holding = true;
    if (current != 0)
        CWorld::s_waterRipples = 0;
}

void ClientRippleSprites::Restore()
{
    if (!m_holding)
        return;
    m_holding = false;
    const int32_t current = CWorld::s_waterRipples;
    if (current == 0)
        CWorld::s_waterRipples = m_remembered;
    LogTransition("shown again", current != 0 ? current : m_remembered);
}

void ClientRippleSprites::LogTransition(const char* text, int32_t value)
{
    const LogLevel level = m_transitions < kInfoLoggedTransitions ? LogLevel::Info : LogLevel::Debug;
    ++m_transitions;
    LogWrite(level, "water: the client's splash and wake sprites are %s (waterRipples %d)", text, static_cast<int>(value));
}
