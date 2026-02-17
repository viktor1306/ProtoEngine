#pragma once
#include <chrono>

namespace core {

// Frame timer — call update() once per frame.
// Provides deltaTime (seconds/ms) and smoothed FPS.
class Timer {
public:
    Timer();

    // Must be called at the very start of each frame.
    void update();

    // Time elapsed since the previous frame (seconds).
    float getDeltaTime()   const { return m_deltaTime; }

    // Time elapsed since the previous frame (milliseconds).
    float getDeltaTimeMs() const { return m_deltaTime * 1000.0f; }

    // Smoothed FPS (updated every 0.5 s).
    float getFPS()         const { return m_fps; }

    // Total time elapsed since Timer construction (seconds).
    double getTotalTime()  const;

private:
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    TimePoint m_startTime;
    TimePoint m_lastTime;

    float m_deltaTime      = 0.0f;
    float m_fps            = 0.0f;

    // FPS smoothing state
    float m_fpsAccumulator = 0.0f;
    int   m_fpsCounter     = 0;
};

} // namespace core
