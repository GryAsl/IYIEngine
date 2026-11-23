// Pure C++ core of the native_example fixture. It includes no Python.h and no engine header, so the
// same translation unit also builds into a plain C++ tool or test; bindings.cpp is the only file that
// knows about Python. Everything across the boundary is plain data.
#pragma once

#include <tuple>
#include <vector>

namespace native_example {

int Add(int a, int b);

// A fixed-step accumulator: Step(dt) turns a stream of frame deltas into whole ticks and keeps the
// remainder for the next call. Plain data in, plain data out.
class Accumulator
{
public:
    explicit Accumulator(double stepSeconds);

    void Step(double dt);
    void Reset();
    int Ticks() const { return m_ticks; }
    double Remainder() const { return m_remainder; }

private:
    double m_step;
    double m_remainder = 0.0;
    int m_ticks = 0;
};

// The whole accumulator as one pass over the deltas: (ticks, remainder).
std::tuple<int, double> Accumulate(double stepSeconds, const std::vector<double>& deltas);

} // namespace native_example
