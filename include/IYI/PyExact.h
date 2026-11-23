// Declares arithmetic that gives the same bits as CPython 3.14, for C++ that replaces Python code
// whose results must not change (the navigation's seeded runs): random.Random (Mersenne Twister,
// str seeds through SHA-512), math.hypot, float // and %, sum() of floats (compensated), float ** and
// the iteration order of a small set of 2-tuples of ints. Everything else (+ - * /, sqrt, the libm
// functions) is already the same when evaluated in the same order with the same C runtime.
//
// Connected to:
// - NavSim*.cpp and Life.cpp: every computation the Python nav and life code did.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace IYI::Py {

// random.Random(seed) for a str or int seed: random(), uniform(a, b), getrandbits(k <= 32), randbelow(n).
class Random
{
public:
    explicit Random(const std::string& seed = "");
    explicit Random(uint64_t seed);
    double random();
    double uniform(double a, double b) { return a + (b - a) * random(); }
    uint32_t getrandbits(int k);
    int64_t randbelow(int64_t n);                 // random.choice's index: n > 0
    template <class T> const T& choice(const std::vector<T>& seq) { return seq[static_cast<size_t>(randbelow(static_cast<int64_t>(seq.size())))]; }

private:
    void InitByArray(const std::vector<uint32_t>& key);
    uint32_t Next();
    std::array<uint32_t, 624> m_mt{};
    int m_index = 625;
};

double hypot(double x, double y);                 // math.hypot(x, y)
double floordiv(double x, double y);              // x // y
double mod(double x, double y);                   // x % y
double pow(double x, double y);                   // x ** y (finite)
int64_t ifloor(double x);                         // math.floor(x)
int64_t iceil(double x);                          // math.ceil(x)
int64_t ifloordiv(double x, double y);            // int(x // y)
double radians(double deg);                       // math.radians
double degrees(double rad);                       // math.degrees

// sum() of floats: Neumaier compensation, as CPython 3.12+.
class Sum
{
public:
    void add(double x);
    double value() const;

private:
    bool m_any = false;
    double m_f = 0.0, m_c = 0.0;
};

// The order a set built from these 2-tuples of ints (added in this order, at most 4 distinct) iterates in.
std::vector<std::pair<int64_t, int64_t>> SetOrder(const std::vector<std::pair<int64_t, int64_t>>& added);

void Sha512(const uint8_t* data, size_t size, uint8_t out[64]);

} // namespace IYI::Py
