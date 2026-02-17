#include "Timer.hpp"

namespace core {

Timer::Timer()
    : m_startTime(Clock::now())
    , m_lastTime(Clock::now())
{}

void Timer::update() {
    TimePoint now = Clock::now();

    m_deltaTime = std::chrono::duration<float>(now - m_lastTime).count();
    m_lastTime  = now;

    // Clamp delta to avoid spiral-of-death on breakpoints / lag spikes
    if (m_deltaTime > 0.25f) m_deltaTime = 0.25f;

    // Smooth FPS: recalculate every 0.5 s
    m_fpsAccumulator += m_deltaTime;
    ++m_fpsCounter;

    if (m_fpsAccumulator >= 0.5f) {
        m_fps            = static_cast<float>(m_fpsCounter) / m_fpsAccumulator;
        m_fpsAccumulator = 0.0f;
        m_fpsCounter     = 0;
    }
}

double Timer::getTotalTime() const {
    return std::chrono::duration<double>(Clock::now() - m_startTime).count();
}

} // namespace core
