// Pure fixed-timestep helpers used by Application::RunFixedUpdates (and the native tests).
// The frame delta is clamped ONCE, then fed to every accumulator, so all fixed timelines drop
// exactly the same stalled-frame time. Step values stay constant for determinism.

#pragma once

namespace IYI {

inline constexpr float kFixedStep60 = 1.0f / 60.0f;
inline constexpr float kFixedStep30 = 1.0f / 30.0f;
inline constexpr int   kFixedMaxCatchUpSteps = 4;
// Longest frame time the fixed clocks will ever see: the 60 Hz catch-up window. Anything beyond it
// (breakpoint, window drag, multi-second stall) is dropped rather than replayed.
inline constexpr float kFixedMaxFrameDelta = kFixedStep60 * kFixedMaxCatchUpSteps;

inline float ClampFrameDelta(float dt)
{
    if (!(dt > 0.0f)) return 0.0f; // also rejects NaN
    return dt > kFixedMaxFrameDelta ? kFixedMaxFrameDelta : dt;
}

// Adds the (already clamped) dt to acc and returns how many fixed steps to run, consuming them.
inline int AdvanceFixedClock(float& acc, float dt, float step)
{
    acc += dt;
    int steps = 0;
    while (acc >= step && steps < kFixedMaxCatchUpSteps) {
        acc -= step;
        ++steps;
    }
    return steps;
}

} // namespace IYI
