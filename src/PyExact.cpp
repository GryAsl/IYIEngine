// CPython 3.14 arithmetic in C++ (PyExact.h). Each function follows the CPython source it names.

#include "IYI/PyExact.h"
#include <cfloat>
#include <cmath>
#include <cstring>

namespace IYI::Py {

namespace {

constexpr int N = 624, M = 397;

struct DoubleLength { double hi, lo; };

DoubleLength dl_mul(double x, double y)            // mathmodule.c: exact product
{
    const double z = x * y;
    return { z, std::fma(x, y, -z) };
}

DoubleLength dl_fast_sum(double a, double b)
{
    const double x = a + b;
    return { x, (a - x) + b };
}

double vector_norm(int n, double* vec, double max)  // mathmodule.c vector_norm (3.12+)
{
    if (std::isinf(max)) return max;
    if (max == 0.0 || n <= 1) return max;
    int max_e;
    std::frexp(max, &max_e);
    if (max_e < -1023) {
        for (int i = 0; i < n; ++i) vec[i] /= DBL_MIN;
        return DBL_MIN * vector_norm(n, vec, max / DBL_MIN);
    }
    const double scale = std::ldexp(1.0, -max_e);
    double csum = 1.0, frac1 = 0.0, frac2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = vec[i] * scale;
        const DoubleLength pr = dl_mul(x, x);
        const DoubleLength sm = dl_fast_sum(csum, pr.hi);
        csum = sm.hi;
        frac1 += pr.lo;
        frac2 += sm.lo;
    }
    double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
    const DoubleLength pr = dl_mul(-h, h);
    const DoubleLength sm = dl_fast_sum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    const double x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);
    return h / scale;
}

uint64_t IntHash(int64_t v)                         // hash(int) for |v| < 2**61
{
    return static_cast<uint64_t>(v == -1 ? -2 : v);
}

uint64_t TupleHash(int64_t a, int64_t b)            // tupleobject.c tuple_hash (xxHash lanes)
{
    constexpr uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL, P5 = 2870177450012600261ULL;
    uint64_t acc = P5;
    for (uint64_t lane : { IntHash(a), IntHash(b) }) {
        acc += lane * P2;
        acc = (acc << 31) | (acc >> 33);
        acc *= P1;
    }
    acc += 2ULL ^ (P5 ^ 3527539ULL);
    return acc == ~0ULL ? 1546275796ULL : acc;
}

} // namespace

// --- random.Random (_randommodule.c) --------------------------------------------------------

Random::Random(const std::string& seed)
{
    // random.py, version 2: int.from_bytes(s + sha512(s).digest()) (big-endian), then init_by_array
    // with its 32-bit words least significant first.
    std::vector<uint8_t> bytes(seed.begin(), seed.end());
    uint8_t digest[64];
    Sha512(bytes.data(), bytes.size(), digest);
    bytes.insert(bytes.end(), digest, digest + 64);
    size_t first = 0;
    while (first < bytes.size() && bytes[first] == 0) ++first;
    size_t bits = 0;
    if (first < bytes.size()) {
        int top = 0;
        for (uint8_t b = bytes[first]; b; b >>= 1) ++top;
        bits = (bytes.size() - first - 1) * 8 + top;
    }
    const size_t used = bits == 0 ? 1 : (bits - 1) / 32 + 1;
    std::vector<uint32_t> key(used, 0);
    for (size_t k = 0; k < used * 4; ++k) {            // little-endian byte k of the integer
        if (k >= bytes.size()) break;
        key[k / 4] |= static_cast<uint32_t>(bytes[bytes.size() - 1 - k]) << (8 * (k % 4));
    }
    InitByArray(key);
}

Random::Random(uint64_t seed)
{
    std::vector<uint32_t> key;
    do { key.push_back(static_cast<uint32_t>(seed)); seed >>= 32; } while (seed);
    InitByArray(key);
}

void Random::InitByArray(const std::vector<uint32_t>& key)
{
    m_mt[0] = 19650218U;
    for (int i = 1; i < N; ++i) m_mt[i] = 1812433253U * (m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) + static_cast<uint32_t>(i);
    size_t i = 1, j = 0;
    for (size_t k = N > key.size() ? N : key.size(); k; --k) {
        m_mt[i] = (m_mt[i] ^ ((m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) * 1664525U)) + key[j] + static_cast<uint32_t>(j);
        ++i; ++j;
        if (i >= N) { m_mt[0] = m_mt[N - 1]; i = 1; }
        if (j >= key.size()) j = 0;
    }
    for (size_t k = N - 1; k; --k) {
        m_mt[i] = (m_mt[i] ^ ((m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) * 1566083941U)) - static_cast<uint32_t>(i);
        ++i;
        if (i >= N) { m_mt[0] = m_mt[N - 1]; i = 1; }
    }
    m_mt[0] = 0x80000000U;
    m_index = N;
}

uint32_t Random::Next()
{
    static constexpr uint32_t mag01[2] = { 0x0U, 0x9908b0dfU };
    if (m_index >= N) {
        int kk = 0;
        for (; kk < N - M; ++kk) {
            const uint32_t y = (m_mt[kk] & 0x80000000U) | (m_mt[kk + 1] & 0x7fffffffU);
            m_mt[kk] = m_mt[kk + M] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        for (; kk < N - 1; ++kk) {
            const uint32_t y = (m_mt[kk] & 0x80000000U) | (m_mt[kk + 1] & 0x7fffffffU);
            m_mt[kk] = m_mt[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        const uint32_t y = (m_mt[N - 1] & 0x80000000U) | (m_mt[0] & 0x7fffffffU);
        m_mt[N - 1] = m_mt[M - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
        m_index = 0;
    }
    uint32_t y = m_mt[m_index++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= (y >> 18);
    return y;
}

double Random::random()
{
    const uint32_t a = Next() >> 5, b = Next() >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

uint32_t Random::getrandbits(int k)
{
    if (k <= 0) return 0;
    return Next() >> (32 - k);
}

int64_t Random::randbelow(int64_t n)
{
    int k = 0;
    for (int64_t v = n; v; v >>= 1) ++k;
    int64_t r = getrandbits(k);
    while (r >= n) r = getrandbits(k);
    return r;
}

// --- math and float operators ----------------------------------------------------------------

double hypot(double x, double y)
{
    double v[2] = { std::fabs(x), std::fabs(y) };
    if (std::isnan(v[0]) || std::isnan(v[1])) return std::isinf(v[0]) || std::isinf(v[1]) ? INFINITY : NAN;
    return vector_norm(2, v, v[0] > v[1] ? v[0] : v[1]);
}

double floordiv(double vx, double wx)               // floatobject.c _float_div_mod
{
    double mod = std::fmod(vx, wx);
    double div = (vx - mod) / wx;
    if (mod) {
        if ((wx < 0) != (mod < 0)) div -= 1.0;
    }
    if (div) {
        double fd = std::floor(div);
        if (div - fd > 0.5) fd += 1.0;
        return fd;
    }
    return std::copysign(0.0, vx / wx);
}

double mod(double vx, double wx)                    // floatobject.c float_rem
{
    double m = std::fmod(vx, wx);
    if (m) {
        if ((wx < 0) != (m < 0)) m += wx;
    }
    else {
        m = std::copysign(0.0, wx);
    }
    return m;
}

double pow(double iv, double iw)                    // floatobject.c float_pow, finite arguments
{
    if (iw == 0) return 1.0;
    if (iv == 1.0) return 1.0;
    bool negate = false;
    if (iv < 0.0) {
        negate = std::fmod(std::fabs(iw), 2.0) == 1.0;
        iv = -iv;
    }
    if (iv == 0.0) return iw < 0.0 ? INFINITY : (negate ? -0.0 : 0.0);
    // CPython's ** gets the C runtime's pow without FMA3 (its cos, sin, atan2 and acos are the default ones).
    static double (*volatile crtPow)(double, double) = &::pow;
#if defined(_MSC_VER) && defined(_M_X64)
    const int was = _get_FMA3_enable();
    _set_FMA3_enable(0);
    const double r = crtPow(iv, iw);
    _set_FMA3_enable(was);
#else
    const double r = crtPow(iv, iw);
#endif
    return negate ? -r : r;
}

int64_t ifloor(double x) { return static_cast<int64_t>(std::floor(x)); }
int64_t iceil(double x) { return static_cast<int64_t>(std::ceil(x)); }
int64_t ifloordiv(double x, double y) { return static_cast<int64_t>(floordiv(x, y)); }

double radians(double deg) { return deg * (3.14159265358979323846 / 180.0); }
double degrees(double rad) { return rad * (180.0 / 3.14159265358979323846); }

void Sum::add(double x)
{
    if (!m_any) { m_any = true; m_f = x == 0.0 ? 0.0 : x; return; }   // 0 + x: the int start
    const double t = m_f + x;
    if (std::fabs(m_f) >= std::fabs(x)) m_c += (m_f - t) + x;
    else m_c += (x - t) + m_f;
    m_f = t;
}

double Sum::value() const
{
    return m_c && std::isfinite(m_c) ? m_f + m_c : m_f;
}

std::vector<std::pair<int64_t, int64_t>> SetOrder(const std::vector<std::pair<int64_t, int64_t>>& added)
{
    // setobject.c set_add_entry on the 8-slot table (mask 7: no linear probes; resizes only at a 5th entry).
    std::array<int, 8> slot;
    slot.fill(-1);
    std::array<uint64_t, 8> hashes{};
    for (size_t k = 0; k < added.size(); ++k) {
        const uint64_t hash = TupleHash(added[k].first, added[k].second);
        size_t i = hash & 7, perturb = hash;
        while (true) {
            if (slot[i] < 0) { slot[i] = static_cast<int>(k); hashes[i] = hash; break; }
            if (hashes[i] == hash && added[slot[i]] == added[k]) break;
            perturb >>= 5;
            i = (i * 5 + 1 + perturb) & 7;
        }
    }
    std::vector<std::pair<int64_t, int64_t>> out;
    for (int s : slot)
        if (s >= 0) out.push_back(added[s]);
    return out;
}

// --- SHA-512 (FIPS 180-4) -------------------------------------------------------------------

void Sha512(const uint8_t* data, size_t size, uint8_t out[64])
{
    static constexpr uint64_t K[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
        0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
        0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
        0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
        0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
        0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
        0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
        0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
        0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
        0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
        0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
        0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
        0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };
    uint64_t h[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                      0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    std::vector<uint8_t> msg(data, data + size);
    msg.push_back(0x80);
    while (msg.size() % 128 != 112) msg.push_back(0);
    for (int k = 0; k < 8; ++k) msg.push_back(0);                // the high 64 bits of the 128-bit length
    const uint64_t bits = static_cast<uint64_t>(size) * 8;
    for (int k = 7; k >= 0; --k) msg.push_back(static_cast<uint8_t>(bits >> (8 * k)));
    auto rotr = [](uint64_t x, int n) { return (x >> n) | (x << (64 - n)); };
    for (size_t off = 0; off < msg.size(); off += 128) {
        uint64_t w[80];
        for (int t = 0; t < 16; ++t) {
            w[t] = 0;
            for (int k = 0; k < 8; ++k) w[t] = (w[t] << 8) | msg[off + t * 8 + k];
        }
        for (int t = 16; t < 80; ++t) {
            const uint64_t s0 = rotr(w[t - 15], 1) ^ rotr(w[t - 15], 8) ^ (w[t - 15] >> 7);
            const uint64_t s1 = rotr(w[t - 2], 19) ^ rotr(w[t - 2], 61) ^ (w[t - 2] >> 6);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int t = 0; t < 80; ++t) {
            const uint64_t t1 = hh + (rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
            const uint64_t t2 = (rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 8; ++k) out[i * 8 + k] = static_cast<uint8_t>(h[i] >> (56 - 8 * k));
}

} // namespace IYI::Py
