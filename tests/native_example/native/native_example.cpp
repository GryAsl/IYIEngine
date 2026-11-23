// Implements the pure C++ core declared in native_example.h. Deliberately free of Python.h and of
// every engine header: this code has no idea it is being called from Python.
#include "native_example.h"

namespace native_example {

int Add(int a, int b)
{
    return a + b;
}

Accumulator::Accumulator(double stepSeconds) : m_step(stepSeconds) {}

void Accumulator::Step(double dt)
{
    m_remainder += dt;
    while (m_step > 0.0 && m_remainder >= m_step) {
        m_remainder -= m_step;
        ++m_ticks;
    }
}

void Accumulator::Reset()
{
    m_remainder = 0.0;
    m_ticks = 0;
}

std::tuple<int, double> Accumulate(double stepSeconds, const std::vector<double>& deltas)
{
    Accumulator accumulator(stepSeconds);
    for (double dt : deltas) {
        accumulator.Step(dt);
    }
    return { accumulator.Ticks(), accumulator.Remainder() };
}

} // namespace native_example
