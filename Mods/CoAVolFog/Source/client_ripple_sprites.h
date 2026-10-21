#pragma once

// ClientRippleSprites of the Windows CoAVolFog (src/client_ripple_sprites.cpp) on macOS: while the water's
// ripple rings are drawn, the client's own splash and wake sprites are turned off through its waterRipples
// value (0xADF7F0: CWorld::s_waterRipples), and the player's value comes back a second after the water stops
// being shaded. The Windows build checks the code that reads the value before binding to it; here the client's
// own variable is the gate.

#include <cstdint>

constexpr double kClientSpriteGraceSeconds = 1.0;

class ClientRippleSprites
{
public:
    void Update(bool allowed, bool shaded, double seconds);
    void Restore();
    bool Holding() const { return m_holding; }

private:
    void Hold();
    void LogTransition(const char* text, int32_t value);

    bool m_holding = false;
    int32_t m_remembered = 0;
    double m_lastShaded = 0.0;
    uint32_t m_transitions = 0;
};
