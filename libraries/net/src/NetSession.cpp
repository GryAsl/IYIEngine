// Implements NetSession (SPEC §25): the packet format, the join handshake, delta replication with a
// per-client bandwidth queue, messages, inputs, time sync, interpolation history, lag-compensation
// history and host migration. Single-threaded: everything runs on the game thread (§25.3).
//
// Packet = 12-byte header (magic, version, type, session, epoch, unreliable sequence) + a bit-packed body.
// Delta rule (§25.6): the host keeps, per client, the field values that client has acked. Every snapshot
// carries each visible object's fields that differ from them, so a lost snapshot needs no resend logic:
// the next one carries the same differences. An ack marks only what that one snapshot carried.
//
// Connected to:
// - Declared in NetSession.h; links in NetLink.cpp / NetSteam.cpp; wrapped by Net.cpp.

#include "net/src/NetSession.h"

#include "IYI/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace IYI {

// --- bits ------------------------------------------------------------------------------------------

class NetBits {
public:
    std::string bytes;
    size_t nbits = 0;

    void Bits(uint64_t v, int bits)
    {
        if (bits < 64) v &= (uint64_t(1) << bits) - 1;
        while (bits > 0) {
            const int used = static_cast<int>(nbits & 7);
            if (used == 0) bytes.push_back('\0');
            const int take = std::min(8 - used, bits);
            bytes.back() = static_cast<char>(static_cast<uint8_t>(bytes.back()) | ((v & ((1u << take) - 1)) << used));
            v >>= take;
            bits -= take;
            nbits += static_cast<size_t>(take);
        }
    }
    void Bool(bool b) { Bits(b ? 1 : 0, 1); }
    void Str(const std::string& s, int maxBytes)
    {
        const size_t n = std::min(s.size(), static_cast<size_t>(maxBytes));
        Bits(n, 16);
        for (size_t i = 0; i < n; ++i) Bits(static_cast<uint8_t>(s[i]), 8);
    }
    size_t Mark() const { return nbits; }
    void Rollback(size_t mark)
    {
        nbits = mark;
        bytes.resize((mark + 7) / 8);
        if (mark & 7) bytes.back() = static_cast<char>(static_cast<uint8_t>(bytes.back()) & ((1u << (mark & 7)) - 1));
    }
};

class NetRead {
public:
    explicit NetRead(const std::string& b) : bytes(b) {}
    const std::string& bytes;
    size_t pos = 0;
    bool ok = true;

    uint64_t Bits(int bits)
    {
        if (!ok || pos + static_cast<size_t>(bits) > bytes.size() * 8) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        int got = 0;
        while (got < bits) {
            const int used = static_cast<int>(pos & 7);
            const int take = std::min(8 - used, bits - got);
            const uint64_t b = (static_cast<uint8_t>(bytes[pos >> 3]) >> used) & ((1u << take) - 1);
            v |= b << got;
            got += take;
            pos += static_cast<size_t>(take);
        }
        return v;
    }
    bool Bool() { return Bits(1) != 0; }
    std::string Str(int maxBytes)
    {
        const size_t n = static_cast<size_t>(Bits(16));
        if (!ok || n > static_cast<size_t>(maxBytes) || pos + n * 8 > bytes.size() * 8) {
            ok = false;
            return {};
        }
        std::string s(n, '\0');
        for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(Bits(8));
        return s;
    }
};

namespace {

constexpr uint16_t kMagic = 0x4E49;   // "IN"
constexpr uint8_t kVersion = 1;
constexpr size_t kSnapshotBudget = 1150;   // bytes: one packet under a 1200-byte MTU
constexpr int kMaxPlayers = 16;
constexpr int kKindBlobMax = 1024, kMessageBlobMax = 60000;
constexpr int kHistory = 32;

enum Packet : uint8_t { PHello = 1, PWelcome, PRefuse, PRoster, PSpawn, PDespawn, PSync, PSnap, PAck, POwner, PInput, PMsg, PPing, PPong, PBye };

int CompCount(NetType t)
{
    if (t == NetType::Vec2) return 2;
    if (t == NetType::Vec3) return 3;
    if (t == NetType::Text || t == NetType::Bytes) return 0;
    return 1;
}

uint64_t Steps(const NetFieldType& f) { return static_cast<uint64_t>(std::llround((f.max - f.min) / f.step)); }

int CompBits(const NetFieldType& f)
{
    switch (f.type) {
    case NetType::Bool: return 1;
    case NetType::U8: case NetType::I8: return 8;
    case NetType::U16: case NetType::I16: case NetType::Ref: return 16;
    case NetType::U32: case NetType::I32: case NetType::F32: return 32;
    case NetType::Player: return 5;
    case NetType::Angle: return f.bits;
    default: break;
    }
    const uint64_t steps = Steps(f);
    int bits = 1;
    while (bits < 33 && (uint64_t(1) << bits) <= steps) ++bits;
    return bits;
}

void IntRange(NetType t, double& lo, double& hi)
{
    switch (t) {
    case NetType::U8: lo = 0; hi = 255; break;
    case NetType::U16: case NetType::Ref: lo = 0; hi = 65535; break;
    case NetType::U32: lo = 0; hi = 4294967295.0; break;
    case NetType::I8: lo = -128; hi = 127; break;
    case NetType::I16: lo = -32768; hi = 32767; break;
    case NetType::I32: lo = -2147483648.0; hi = 2147483647.0; break;
    case NetType::Player: lo = 0; hi = kMaxPlayers; break;
    default: lo = 0; hi = 1; break;
    }
}

uint64_t Quantize(const NetFieldType& f, double v, bool& clamped)
{
    if (!std::isfinite(v)) {
        clamped = true;
        v = 0.0;
    }
    switch (f.type) {
    case NetType::Bool: return v != 0.0 ? 1 : 0;
    case NetType::F32: {
        const float x = static_cast<float>(v);
        uint32_t u;
        std::memcpy(&u, &x, 4);
        return u;
    }
    case NetType::Angle: {
        const uint64_t n = uint64_t(1) << f.bits;
        double a = std::fmod(v, 360.0);
        if (a < 0) a += 360.0;
        return static_cast<uint64_t>(std::llround(a / 360.0 * static_cast<double>(n))) % n;
    }
    case NetType::Fixed: case NetType::Vec2: case NetType::Vec3: {
        const double c = std::clamp(v, f.min, f.max);
        if (c != v) clamped = true;
        return std::min(static_cast<uint64_t>(std::llround((c - f.min) / f.step)), Steps(f));
    }
    default: {
        double lo, hi;
        IntRange(f.type, lo, hi);
        const double c = std::clamp(std::round(v), lo, hi);
        if (c != std::round(v)) clamped = true;
        return static_cast<uint64_t>(c - lo);
    }
    }
}

double Dequantize(const NetFieldType& f, uint64_t q)
{
    switch (f.type) {
    case NetType::Bool: return q ? 1.0 : 0.0;
    case NetType::F32: {
        const uint32_t u = static_cast<uint32_t>(q);
        float x;
        std::memcpy(&x, &u, 4);
        return std::isfinite(x) ? x : 0.0;
    }
    case NetType::Angle: return static_cast<double>(q) * 360.0 / static_cast<double>(uint64_t(1) << f.bits);
    case NetType::Fixed: case NetType::Vec2: case NetType::Vec3:
        return f.min + static_cast<double>(std::min(q, Steps(f))) * f.step;
    default: {
        double lo, hi;
        IntRange(f.type, lo, hi);
        return std::min(lo + static_cast<double>(q), hi);
    }
    }
}

bool ValidType(const NetFieldType& f, int blobMax)
{
    switch (f.type) {
    case NetType::Fixed: case NetType::Vec2: case NetType::Vec3:
        return std::isfinite(f.min) && std::isfinite(f.max) && f.max > f.min && f.step > 0 &&
               (f.max - f.min) / f.step >= 1.0 && (f.max - f.min) / f.step <= 4294967295.0;
    case NetType::Angle: return f.bits >= 1 && f.bits <= 16;
    case NetType::Text: case NetType::Bytes: return f.maxBytes >= 1 && f.maxBytes <= blobMax;
    default: return true;
    }
}

bool SameType(const NetFieldType& a, const NetFieldType& b)
{
    return a.type == b.type && a.min == b.min && a.max == b.max && a.step == b.step && a.bits == b.bits && a.maxBytes == b.maxBytes;
}

int FieldIndex(const NetDef& d, const std::string& name)
{
    for (size_t i = 0; i < d.fields.size(); ++i) {
        if (d.fields[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool ToDouble(const NetValue& v, double& out)
{
    if (const bool* b = std::get_if<bool>(&v)) out = *b ? 1.0 : 0.0;
    else if (const int64_t* i = std::get_if<int64_t>(&v)) out = static_cast<double>(*i);
    else if (const double* d = std::get_if<double>(&v)) out = *d;
    else return false;
    return true;
}

// Writes one value into a def's component/blob arrays. False: the value's type does not fit the field.
bool Encode(const NetDef& d, int fi, const NetValue& v, std::vector<uint64_t>& q, std::vector<std::string>& blobs, bool& clamped)
{
    const NetFieldType& f = d.fields[static_cast<size_t>(fi)].type;
    const int at = d.start[static_cast<size_t>(fi)];
    if (f.type == NetType::Text || f.type == NetType::Bytes) {
        const std::string* s = std::get_if<std::string>(&v);
        if (!s) return false;
        if (s->size() > static_cast<size_t>(f.maxBytes)) clamped = true;
        blobs[static_cast<size_t>(at)] = s->substr(0, static_cast<size_t>(f.maxBytes));
        return true;
    }
    if (f.type == NetType::Vec2) {
        const Vector2* p = std::get_if<Vector2>(&v);
        if (!p) return false;
        q[static_cast<size_t>(at)] = Quantize(f, p->x, clamped);
        q[static_cast<size_t>(at) + 1] = Quantize(f, p->y, clamped);
        return true;
    }
    if (f.type == NetType::Vec3) {
        const Vector3* p = std::get_if<Vector3>(&v);
        if (!p) return false;
        q[static_cast<size_t>(at)] = Quantize(f, p->x, clamped);
        q[static_cast<size_t>(at) + 1] = Quantize(f, p->y, clamped);
        q[static_cast<size_t>(at) + 2] = Quantize(f, p->z, clamped);
        return true;
    }
    double x;
    if (!ToDouble(v, x)) return false;
    q[static_cast<size_t>(at)] = Quantize(f, x, clamped);
    return true;
}

NetValue MakeValue(const NetFieldType& f, const double* c, const std::string* blob)
{
    switch (f.type) {
    case NetType::Bool: return c[0] != 0.0;
    case NetType::F32: case NetType::Fixed: case NetType::Angle: return c[0];
    case NetType::Vec2: return Vector2{ static_cast<float>(c[0]), static_cast<float>(c[1]) };
    case NetType::Vec3: return Vector3{ static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]) };
    case NetType::Text: case NetType::Bytes: return blob ? *blob : std::string();
    default: return static_cast<int64_t>(std::llround(c[0]));
    }
}

NetValue Decode(const NetDef& d, int fi, const std::vector<uint64_t>& q, const std::vector<std::string>& blobs)
{
    const NetFieldType& f = d.fields[static_cast<size_t>(fi)].type;
    const int at = d.start[static_cast<size_t>(fi)];
    const int n = CompCount(f.type);
    if (n == 0) return MakeValue(f, nullptr, &blobs[static_cast<size_t>(at)]);
    double c[3] = {};
    for (int i = 0; i < n; ++i) c[i] = Dequantize(f, q[static_cast<size_t>(at + i)]);
    return MakeValue(f, c, nullptr);
}

void WriteFields(NetBits& w, const NetDef& d, uint64_t mask, const std::vector<uint64_t>& q, const std::vector<std::string>& blobs)
{
    for (size_t fi = 0; fi < d.fields.size(); ++fi) {
        if (!((mask >> fi) & 1)) continue;
        const NetFieldType& f = d.fields[fi].type;
        const int n = CompCount(f.type);
        if (n == 0) {
            w.Str(blobs[static_cast<size_t>(d.start[fi])], f.maxBytes);
            continue;
        }
        const int bits = CompBits(f);
        for (int i = 0; i < n; ++i) w.Bits(q[static_cast<size_t>(d.start[fi] + i)], bits);
    }
}

bool ReadFields(NetRead& r, const NetDef& d, uint64_t mask, std::vector<uint64_t>& q, std::vector<std::string>& blobs)
{
    for (size_t fi = 0; fi < d.fields.size(); ++fi) {
        if (!((mask >> fi) & 1)) continue;
        const NetFieldType& f = d.fields[fi].type;
        const int n = CompCount(f.type);
        if (n == 0) {
            blobs[static_cast<size_t>(d.start[fi])] = r.Str(f.maxBytes);
            continue;
        }
        const int bits = CompBits(f);
        for (int i = 0; i < n; ++i) {
            uint64_t v = r.Bits(bits);
            if (f.type == NetType::Fixed || f.type == NetType::Vec2 || f.type == NetType::Vec3) v = std::min(v, Steps(f));
            q[static_cast<size_t>(d.start[fi] + i)] = v;
        }
    }
    return r.ok;
}

uint64_t AllFields(const NetDef& d) { return d.fields.size() >= 64 ? ~uint64_t(0) : (uint64_t(1) << d.fields.size()) - 1; }

uint64_t DiffMask(const NetDef& d, const NetObj& o, const NetAck& a)
{
    uint64_t mask = 0;
    for (size_t fi = 0; fi < d.fields.size(); ++fi) {
        const int n = CompCount(d.fields[fi].type.type);
        const size_t at = static_cast<size_t>(d.start[fi]);
        bool differs = false;
        if (n == 0) differs = o.blobs[at] != a.blobs[at];
        for (int i = 0; i < n && !differs; ++i) differs = o.q[at + static_cast<size_t>(i)] != a.q[at + static_cast<size_t>(i)];
        if (differs) mask |= uint64_t(1) << fi;
    }
    return mask;
}

uint8_t EncodeTarget(int to) { return to == kNetAll ? 255 : to == kNetOthers ? 254 : static_cast<uint8_t>(std::clamp(to, 0, kMaxPlayers)); }
int DecodeTarget(uint64_t v) { return v == 255 ? kNetAll : v == 254 ? kNetOthers : static_cast<int>(v); }

void Fnv(uint32_t& h, const std::string& s)
{
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    h = (h ^ 0xFF) * 16777619u;
}

void HashDef(uint32_t& h, const NetDef& d)
{
    Fnv(h, d.name);
    for (const NetField& f : d.fields) {
        char text[160];
        std::snprintf(text, sizeof(text), "%d|%.17g|%.17g|%.17g|%d|%d", static_cast<int>(f.type.type), f.type.min, f.type.max,
                      f.type.step, f.type.bits, f.type.maxBytes);
        Fnv(h, f.name);
        Fnv(h, text);
    }
}

} // namespace

// --- definitions -----------------------------------------------------------------------------------

NetSession::NetSession() : m_rng(std::random_device{}()) {}

NetSession::~NetSession() { Leave(); }

void NetSession::LogOnce(const std::string& text)
{
    if (std::find(m_loggedOnce.begin(), m_loggedOnce.end(), text) != m_loggedOnce.end()) return;
    m_loggedOnce.push_back(text);
    LogWrite(LogLevel::Warn, "net", text);
}

bool NetSession::AddDef(std::vector<NetDef>& defs, NetDef def, const char* what)
{
    // Names Python already uses on a net.Object, or as the keys receive() / inputs() add to each dict.
    static const char* objectNames[] = { "id", "kind", "owner", "authority", "priority", "exists", "sample", "hide_from", "show_to", "despawn" };
    static const char* dictNames[] = { "name", "from", "tick", "seq" };
    const bool isKind = std::strcmp(what, "kind") == 0;
    const int blobMax = std::strcmp(what, "message") == 0 ? kMessageBlobMax : kKindBlobMax;
    if (def.fields.size() > 64) {
        LogWrite(LogLevel::Error, "net", std::string(what) + " '" + def.name + "': more than 64 fields");
        return false;
    }
    for (size_t i = 0; i < def.fields.size(); ++i) {
        const NetField& f = def.fields[i];
        bool bad = f.name.empty() || !ValidType(f.type, blobMax);
        if (isKind) for (const char* r : objectNames) bad = bad || f.name == r;
        else for (const char* r : dictNames) bad = bad || f.name == r;
        for (size_t j = 0; j < i; ++j) bad = bad || def.fields[j].name == f.name;
        if (bad) {
            LogWrite(LogLevel::Error, "net", std::string(what) + " '" + def.name + "': field '" + f.name +
                                                 "' has a reserved or repeated name, or an invalid type");
            return false;
        }
        def.start.push_back(CompCount(f.type.type) ? def.comps : def.blobs);
        if (CompCount(f.type.type)) def.comps += CompCount(f.type.type);
        else ++def.blobs;
    }
    for (NetDef& old : defs) {
        if (old.name != def.name) continue;
        bool same = old.fields.size() == def.fields.size();
        for (size_t i = 0; same && i < def.fields.size(); ++i) {
            same = old.fields[i].name == def.fields[i].name && SameType(old.fields[i].type, def.fields[i].type);
        }
        if (same) return true;   // a hot reload defines it again
        if (m_mode != Mode::Offline) {
            LogWrite(LogLevel::Error, "net", std::string(what) + " '" + def.name + "' cannot change during a session");
            return false;
        }
        old = std::move(def);
        return true;
    }
    if (defs.size() >= 256) {
        LogWrite(LogLevel::Error, "net", std::string("more than 256 ") + what + " definitions");
        return false;
    }
    if (m_mode != Mode::Offline) {
        LogWrite(LogLevel::Error, "net", std::string(what) + " '" + def.name + "': define before host or join");
        return false;
    }
    defs.push_back(std::move(def));
    return true;
}

bool NetSession::Define(const std::string& kind, const std::vector<NetField>& fields)
{
    return !kind.empty() && AddDef(m_kinds, NetDef{ kind, fields, {}, 0, 0 }, "kind");
}

bool NetSession::DefineMessage(const std::string& name, const std::vector<NetField>& fields)
{
    return !name.empty() && AddDef(m_messages, NetDef{ name, fields, {}, 0, 0 }, "message");
}

bool NetSession::DefineInput(const std::vector<NetField>& fields)
{
    std::vector<NetDef> one;
    if (m_hasInput) one.push_back(m_input);
    if (!AddDef(one, NetDef{ "input", fields, {}, 0, 0 }, "input")) return false;
    m_input = one.front();
    m_hasInput = true;
    return true;
}

const NetDef* NetSession::Kind(const std::string& name) const
{
    for (const NetDef& d : m_kinds) if (d.name == name) return &d;
    return nullptr;
}

const NetDef* NetSession::Message(const std::string& name) const
{
    for (const NetDef& d : m_messages) if (d.name == name) return &d;
    return nullptr;
}

uint32_t NetSession::SchemaHash() const
{
    uint32_t h = 2166136261u;
    for (const NetDef& d : m_kinds) HashDef(h, d);
    Fnv(h, "|messages|");
    for (const NetDef& d : m_messages) HashDef(h, d);
    if (m_hasInput) HashDef(h, m_input);
    return h;
}

// --- sending ---------------------------------------------------------------------------------------

void NetSession::WriteHeader(NetBits& w, uint8_t type, NetPeer* unreliablePeer)
{
    w.Bits(kMagic, 16);
    w.Bits(kVersion, 8);
    w.Bits(type, 8);
    w.Bits(m_session, 32);
    w.Bits(m_epoch, 16);
    uint16_t seq = 0;
    if (unreliablePeer) {
        if (++unreliablePeer->sendSeq == 0) unreliablePeer->sendSeq = 1;
        seq = unreliablePeer->sendSeq;
    }
    w.Bits(seq, 16);
}

void NetSession::SendRaw(uint64_t link, bool reliable, const std::string& bytes)
{
    ++m_packetsOut;
    m_bytesOut += static_cast<int64_t>(bytes.size());
    m_rateOut += static_cast<int64_t>(bytes.size());
#ifndef IYI_SHIPPING_BUILD
    if (m_simLatency > 0.0f || m_simJitter > 0.0f || m_simLoss > 0.0f) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        if (!reliable && u(m_rng) < m_simLoss) return;
        double at = m_now + (m_simLatency + u(m_rng) * m_simJitter) / 1000.0;
        if (reliable) {   // reliable packets never overtake each other
            for (const Delayed& d : m_delayed) if (d.link == link && d.reliable) at = std::max(at, d.at);
        }
        m_delayed.push_back({ at, link, reliable, bytes });
        return;
    }
#endif
    LinkSend(m_link, link, reliable, bytes);
}

void NetSession::FlushDelayed(double now, bool all)
{
    if (m_delayed.empty()) return;
    std::stable_sort(m_delayed.begin(), m_delayed.end(), [](const Delayed& a, const Delayed& b) { return a.at < b.at; });
    size_t n = 0;
    while (n < m_delayed.size() && (all || m_delayed[n].at <= now)) {
        LinkSend(m_link, m_delayed[n].link, m_delayed[n].reliable, m_delayed[n].bytes);
        ++n;
    }
    m_delayed.erase(m_delayed.begin(), m_delayed.begin() + static_cast<std::ptrdiff_t>(n));
}

void NetSession::Simulate(float latencyMs, float jitterMs, float loss)
{
#ifdef IYI_SHIPPING_BUILD
    (void)latencyMs; (void)jitterMs; (void)loss;
#else
    m_simLatency = std::max(0.0f, latencyMs);
    m_simJitter = std::max(0.0f, jitterMs);
    m_simLoss = std::clamp(loss, 0.0f, 1.0f);
#endif
}

// --- session ---------------------------------------------------------------------------------------

std::string NetSession::State() const
{
    switch (m_mode) {
    case Mode::Hosting: return "hosting";
    case Mode::Joining: return "joining";
    case Mode::Connected: return "connected";
    case Mode::Migrating: return "migrating";
    default: return "offline";
    }
}

void NetSession::Reset(bool keepLink)
{
    if (!keepLink) {
        if (m_kindOfLink == NetLinkKind::Steam) {
            for (const auto& [link, peer] : m_peers) SteamNetClose(link);
            SteamLobbyLeave();
        }
        LinkClose(m_link);
        m_kindOfLink = NetLinkKind::None;
    }
    DiscoveryStop(m_discovery);
    m_mode = Mode::Offline;
    m_isHost = false;
    m_session = 0;
    m_epoch = 0;
    m_self = m_hostPlayer = 0;
    m_roster.clear();
    m_rosterSeen = false;
    m_peers.clear();
    m_hostLink = 0;
    m_candidates.clear();
    m_objects.clear();
    m_rewind.clear();
    m_unacked.clear();
    m_hostOwnInputs.clear();
    m_delayed.clear();
    m_inputSeq = m_inputAck = 0;
    m_tick = 0;
    m_tickSynced = false;
    m_tickDue = false;
    m_steamWaitingLobby = false;
}

void NetSession::Fail(const std::string& reason)
{
    LogWrite(LogLevel::Info, "net", "disconnected: " + reason);
    Reset(false);
    NetEvent e;
    e.type = "disconnected";
    e.reason = reason;
    Event(std::move(e));
}

bool NetSession::Host(const NetHostOptions& o, double now)
{
    m_now = now;
    if (m_mode != Mode::Offline) {
        LogWrite(LogLevel::Warn, "net", "host: already in a session (leave first)");
        return false;
    }
    if (o.tickRate != 60 && o.tickRate != 30 && o.tickRate != 20 && o.tickRate != 15 && o.tickRate != 10) {
        LogWrite(LogLevel::Error, "net", "host: tick_rate must be 60, 30, 20, 15 or 10");
        return false;
    }
    const NetLinkKind kind = o.transport == "enet" ? NetLinkKind::Enet : o.transport == "steam" ? NetLinkKind::Steam
                           : o.transport == "loopback" ? NetLinkKind::Loopback : NetLinkKind::None;
    if (kind == NetLinkKind::None || (kind == NetLinkKind::Steam && localOnly)) {
        LogWrite(LogLevel::Error, "net", "host: transport '" + o.transport + "' is not available here");
        return false;
    }
    const int maxPlayers = std::clamp(o.maxPlayers, 1, kMaxPlayers);
    if (!LinkOpen(m_link, kind, o.port, maxPlayers + 2, localOnly)) {
        LogWrite(LogLevel::Error, "net", "host: could not open " + o.transport + (kind == NetLinkKind::Enet ? " port " + std::to_string(o.port) : ""));
        return false;
    }
    m_kindOfLink = kind;
    m_maxPlayers = maxPlayers;
    m_tickRate = o.tickRate;
    m_timeout = std::max(1.0f, o.timeout);
    m_migrationTimeout = std::max(0.5f, o.migrationTimeout);
    m_public = o.isPublic;
    m_lobbyName = o.name;
    m_lobbyData = o.data;
    m_port = m_link.port;
    std::uniform_int_distribution<uint32_t> any(1, 0xFFFFFFFFu);
    m_session = any(m_rng);
    m_epoch = 1;
    m_self = m_hostPlayer = 1;
    m_isHost = true;
    m_mode = Mode::Hosting;
    m_tick = 0;
    m_nextId = 1;
    m_joinCounter = 1;
    m_playerName = !o.playerName.empty() ? o.playerName : kind == NetLinkKind::Steam ? SteamNetPersonaName() : "Player 1";
    m_roster = { NetRosterEntry{ 1, 1, m_playerName, "", 0.0f, 0.0f, true, 0.0 } };
    if (kind == NetLinkKind::Enet) DiscoveryAnswer(m_discovery, SchemaHash(), m_port, localOnly);
    if (kind == NetLinkKind::Steam) {
        char hash[16];
        std::snprintf(hash, sizeof(hash), "%08x", SchemaHash());
        SteamLobbySetData("iyinet", hash);
        SteamLobbySetData("name", m_lobbyName);
        for (const auto& [k, v] : m_lobbyData) SteamLobbySetData(k, v);
        SteamLobbyCreate(m_public, m_maxPlayers);
    }
    SendRoster();
    LogWrite(LogLevel::Info, "net", "hosting on " + o.transport + (kind == NetLinkKind::Enet ? " port " + std::to_string(m_port) : "") +
                                        ", tick rate " + std::to_string(m_tickRate));
    return true;
}

bool NetSession::Join(const std::string& target, const std::string& playerName, double now)
{
    m_now = now;
    if (m_mode != Mode::Offline) {
        LogWrite(LogLevel::Warn, "net", "join: already in a session (leave first)");
        return false;
    }
    const bool steam = target.rfind("steam:", 0) == 0, loop = target.rfind("loop:", 0) == 0;
    const NetLinkKind kind = steam ? NetLinkKind::Steam : loop ? NetLinkKind::Loopback : NetLinkKind::Enet;
    if ((steam && localOnly) || !LinkOpen(m_link, kind, 0, kMaxPlayers + 2, localOnly)) {
        LogWrite(LogLevel::Error, "net", "join: could not open a link for '" + target + "'");
        return false;
    }
    m_kindOfLink = kind;
    m_playerName = !playerName.empty() ? playerName : steam ? SteamNetPersonaName() : "";
    uint64_t link = 0;
    if (steam) {
        const uint64_t lobby = std::strtoull(target.c_str() + 6, nullptr, 10);
        if (lobby == 0) {
            Reset(false);
            return false;
        }
        SteamLobbyJoin(lobby);
        m_steamWaitingLobby = true;
    } else if (!LinkConnect(m_link, target, link)) {
        LogWrite(LogLevel::Error, "net", "join: bad address '" + target + "'");
        Reset(false);
        return false;
    } else {
        m_hostLink = link;
        NetPeer& p = m_peers[link];
        p.link = link;
        p.lastHeard = now;
    }
    m_mode = Mode::Joining;
    m_joinDeadline = now + m_timeout;
    LogWrite(LogLevel::Info, "net", "joining " + target);
    return true;
}

void NetSession::Find(const std::string& transport, double now)
{
    m_now = now;
    if (transport == "steam") {
        if (!localOnly && SteamNetStart()) SteamLobbyFind(SchemaHash());
    } else if (transport == "enet") {
        DiscoveryAsk(m_discovery, SchemaHash(), localOnly, now);
    }
}

void NetSession::Leave()
{
    if (m_mode == Mode::Offline) return;
    for (auto& [link, peer] : m_peers) {
        if (!m_isHost && link != m_hostLink) continue;
        NetBits w;
        WriteHeader(w, PBye, nullptr);
        SendRaw(link, true, w.bytes);
    }
    FlushDelayed(m_now, true);
    LinkFlush(m_link);
    LogWrite(LogLevel::Info, "net", "left the session");
    Reset(false);
}

void NetSession::SetLobbyData(const std::string& key, const std::string& value)
{
    if (!m_isHost) return;
    bool found = false;
    for (auto& [k, v] : m_lobbyData) {
        if (k == key) { v = value; found = true; }
    }
    if (!found) m_lobbyData.emplace_back(key, value);
    if (m_kindOfLink == NetLinkKind::Steam) SteamLobbySetData(key, value);
    SendRoster();   // refreshes the discovery reply too
}

std::vector<NetEvent> NetSession::TakeEvents()
{
    std::vector<NetEvent> out;
    out.swap(m_events);
    return out;
}

std::vector<NetPlayer> NetSession::Players() const
{
    std::vector<NetPlayer> out;
    for (const NetRosterEntry& e : m_roster) {
        if (!e.connected) continue;
        float rtt = e.rttMs, loss = e.loss;
        for (const auto& [link, p] : m_peers) {
            if (p.welcomed && (p.player == e.player || (!m_isHost && link == m_hostLink && e.player == m_hostPlayer))) {
                rtt = p.rttMs;
                loss = p.loss;
            }
        }
        out.push_back({ e.player, e.name, rtt, loss, e.player == m_hostPlayer });
    }
    return out;
}

double NetSession::EstimatedTick() const { return m_now * m_tickRate + m_tickOffset; }

uint32_t NetSession::Tick() const
{
    if (m_isHost || !m_tickSynced) return m_tick;
    const double t = EstimatedTick();
    return t <= 0.0 ? 0u : static_cast<uint32_t>(t);
}

NetStats NetSession::Stats() const
{
    NetStats s;
    s.bytesInPerSec = m_inPerSec;
    s.bytesOutPerSec = m_outPerSec;
    s.packetsIn = m_packetsIn;
    s.packetsOut = m_packetsOut;
    s.dropped = m_dropped;
    s.lastSnapshotBytes = m_lastSnapBytes;
    s.objects = static_cast<int>(m_objects.size());
    return s;
}

NetPeer* NetSession::HostPeer()
{
    auto it = m_peers.find(m_hostLink);
    return it == m_peers.end() ? nullptr : &it->second;
}

// --- receiving -------------------------------------------------------------------------------------

void NetSession::PollFound(double now)
{
    std::vector<std::pair<std::string, std::string>> found;
    DiscoveryPoll(m_discovery, now, found);
    for (const auto& [address, payload] : found) {
        NetRead r(payload);
        NetEvent e;
        e.type = "found";
        e.target = address;
        e.name = r.Str(256);
        e.players = static_cast<int>(r.Bits(8));
        e.maxPlayers = static_cast<int>(r.Bits(8));
        const int n = static_cast<int>(r.Bits(8));
        for (int i = 0; i < n && r.ok; ++i) {
            std::string k = r.Str(256);
            e.data.emplace_back(k, r.Str(256));
        }
        if (r.ok) Event(std::move(e));
    }
    std::vector<NetSteamEvent> steam;
    SteamLobbyPoll(steam);
    for (NetSteamEvent& s : steam) {
        if (s.type == NetSteamEvent::Found || s.type == NetSteamEvent::Invited) {
            NetEvent e;
            e.type = s.type == NetSteamEvent::Found ? "found" : "invited";
            e.target = "steam:" + std::to_string(s.lobby);
            e.name = s.name;
            e.players = s.players;
            e.maxPlayers = s.maxPlayers;
            e.data = std::move(s.data);
            Event(std::move(e));
        } else if (s.type == NetSteamEvent::Entered && m_steamWaitingLobby) {
            m_steamWaitingLobby = false;
            uint64_t link = 0;
            if (!LinkConnect(m_link, "steam-user:" + std::to_string(s.owner), link)) {
                Fail("the Steam lobby has no host");
                continue;
            }
            m_hostLink = link;
            NetPeer& p = m_peers[link];
            p.link = link;
            p.lastHeard = now;
        } else if (s.type == NetSteamEvent::Failed && m_mode == Mode::Joining) {
            Fail(s.reason);
        } else if (s.type == NetSteamEvent::Failed && m_isHost) {
            LogWrite(LogLevel::Warn, "net", s.reason);
        }
    }
}

void NetSession::Receive(double now)
{
    m_now = now;
    PollFound(now);
    if (m_mode == Mode::Offline) return;
    std::vector<NetLinkEvent> events;
    LinkPoll(m_link, events);
    for (const NetLinkEvent& e : events) {
        if (m_mode == Mode::Offline) break;   // a Refuse or Fail ended the session mid-poll
        OnLink(e);
    }
    FlushDelayed(now, false);
}

void NetSession::OnLink(const NetLinkEvent& e)
{
    if (e.type == NetLinkEvent::Connected) {
        NetPeer& p = m_peers[e.peer];
        p.link = e.peer;
        p.lastHeard = m_now;
        if (!m_isHost && e.peer == m_hostLink) SendHello(p);
        return;
    }
    if (e.type == NetLinkEvent::Disconnected) {
        if (m_isHost) {
            DropPeer(e.peer, "disconnected");
        } else if (e.peer == m_hostLink) {
            if (m_mode == Mode::Connected) StartMigration("the host disconnected");
            else if (m_mode == Mode::Migrating) TryNextCandidate();
            else Fail("could not reach the host");
        } else {
            m_peers.erase(e.peer);
        }
        return;
    }
    m_bytesIn += static_cast<int64_t>(e.bytes.size());
    m_rateIn += static_cast<int64_t>(e.bytes.size());
    ++m_packetsIn;
    NetPeer& p = m_peers[e.peer];
    if (p.link == 0) {
        p.link = e.peer;
        p.lastHeard = m_now;
    }
    OnPacket(p, e.bytes);
}

void NetSession::OnPacket(NetPeer& peer, const std::string& bytes)
{
    NetRead r(bytes);
    const bool valid = r.Bits(16) == kMagic && r.Bits(8) == kVersion;
    const uint8_t type = static_cast<uint8_t>(r.Bits(8));
    const uint32_t session = static_cast<uint32_t>(r.Bits(32));
    const uint16_t epoch = static_cast<uint16_t>(r.Bits(16));
    const uint16_t seq = static_cast<uint16_t>(r.Bits(16));
    if (!valid || !r.ok) {
        ++m_dropped;
        if (++peer.invalid > 10 && m_isHost) DropPeer(peer.link, "sent invalid packets");
        return;
    }
    if (type == PHello) {
        if (!m_isHost && (m_mode == Mode::Connected || m_mode == Mode::Migrating) && peer.link != m_hostLink) {
            StartMigration("another player is looking for a new host");   // they noticed first
        }
        if (m_isHost) HostHello(peer, r);
        return;
    }
    const bool fromNewHost = !m_isHost && peer.link == m_hostLink && (type == PWelcome || type == PRefuse);
    if (!fromNewHost && (session != m_session || epoch < m_epoch)) {
        ++m_dropped;
        return;
    }
    peer.lastHeard = m_now;
    if (seq != 0) {   // unreliable: count the gaps for the loss figure
        const int16_t gap = static_cast<int16_t>(seq - peer.recvSeq);
        if (peer.recvSeq == 0 || gap > 0) {
            peer.recvExpected += peer.recvSeq == 0 ? 1 : gap;
            peer.recvSeq = seq;
        }
        ++peer.recvGot;
        if (m_now - peer.lossAt >= 2.0) {
            if (peer.recvExpected > 0) peer.loss = std::clamp(1.0f - static_cast<float>(peer.recvGot) / peer.recvExpected, 0.0f, 1.0f);
            peer.recvGot = peer.recvExpected = 0;
            peer.lossAt = m_now;
        }
    }
    if (type == PPing) {
        NetBits w;
        WriteHeader(w, PPong, nullptr);
        w.Bits(r.Bits(32), 32);
        SendRaw(peer.link, false, w.bytes);
        return;
    }
    if (type == PPong) {
        const uint32_t sent = static_cast<uint32_t>(r.Bits(32));
        const float rtt = static_cast<float>(static_cast<uint32_t>(m_now * 1000.0) - sent);
        if (r.ok && rtt >= 0.0f && rtt < 60000.0f) peer.rttMs = peer.rttMs == 0.0f ? rtt : peer.rttMs * 0.8f + rtt * 0.2f;
        return;
    }
    if (m_isHost) HostOnPacket(peer, type, r);
    else if (peer.link == m_hostLink) ClientOnPacket(peer, type, r);
}

void NetSession::SendHello(NetPeer& peer)
{
    NetBits w;
    WriteHeader(w, PHello, nullptr);
    w.Bits(SchemaHash(), 32);
    w.Bits(static_cast<uint64_t>(m_self), 8);
    w.Bool(m_mode == Mode::Migrating);
    w.Str(m_playerName, 64);
    SendRaw(peer.link, true, w.bytes);
}

void NetSession::HostHello(NetPeer& peer, NetRead& r)
{
    const uint32_t hash = static_cast<uint32_t>(r.Bits(32));
    const int prev = static_cast<int>(r.Bits(8));
    r.Bool();
    const std::string name = r.Str(64);
    if (!r.ok || peer.welcomed) return;
    auto refuse = [&](const std::string& reason) {
        NetBits w;
        WriteHeader(w, PRefuse, nullptr);
        w.Str(reason, 200);
        SendRaw(peer.link, true, w.bytes);
        LogWrite(LogLevel::Info, "net", "refused a player: " + reason);
    };
    if (hash != SchemaHash()) return refuse("a different game build (its net definitions differ)");
    NetRosterEntry* back = nullptr;
    for (NetRosterEntry& e : m_roster) {
        if (prev > 0 && e.player == prev && !e.connected) back = &e;
    }
    int id = back ? prev : 0;
    if (!back) {
        int connected = 0;
        for (const NetRosterEntry& e : m_roster) connected += e.connected ? 1 : 0;
        if (connected >= m_maxPlayers) return refuse("the game is full");
        for (int candidate = 1; candidate <= kMaxPlayers && id == 0; ++candidate) {
            bool used = false;
            for (const NetRosterEntry& e : m_roster) used = used || e.player == candidate;
            if (!used) id = candidate;
        }
        if (id == 0) return refuse("the game is full");
    }
    peer.player = id;
    peer.welcomed = true;
    const std::string address = LinkPeerAddress(m_link, peer.link);
    if (back) {
        back->connected = true;
        back->address = address;
        if (!name.empty()) back->name = name;
    } else {
        m_roster.push_back({ id, ++m_joinCounter, name.empty() ? "Player " + std::to_string(id) : name, address, 0.0f, 0.0f, true, 0.0 });
        NetEvent e;
        e.type = "joined";
        e.player = id;
        e.name = m_roster.back().name;
        Event(std::move(e));
    }
    NetBits w;
    WriteHeader(w, PWelcome, nullptr);
    w.Bits(static_cast<uint64_t>(id), 8);
    w.Bits(static_cast<uint64_t>(m_self), 8);
    w.Bits(m_tick, 32);
    w.Bits(static_cast<uint64_t>(m_tickRate), 8);
    w.Bits(static_cast<uint64_t>(m_maxPlayers), 8);
    w.Bits(static_cast<uint64_t>(m_timeout * 10.0f), 16);
    w.Bits(static_cast<uint64_t>(m_migrationTimeout * 10.0f), 16);
    SendRaw(peer.link, true, w.bytes);
    SendRoster();
    for (const auto& [oid, obj] : m_objects) {
        if (!(obj.hidden & (1u << id))) SendSpawn(peer, obj);
    }
    SendSync(peer);
    LogWrite(LogLevel::Info, "net", std::string(back ? "player came back: " : "player joined: ") + std::to_string(id) + " (" + address + ")");
}

void NetSession::DropPeer(uint64_t link, const std::string& reason)
{
    auto it = m_peers.find(link);
    if (it == m_peers.end()) return;
    const int player = it->second.welcomed ? it->second.player : 0;
    LinkDisconnect(m_link, link);
    m_peers.erase(it);
    if (player == 0) return;
    m_roster.erase(std::remove_if(m_roster.begin(), m_roster.end(), [&](const NetRosterEntry& e) { return e.player == player; }), m_roster.end());
    NetEvent e;
    e.type = "left";
    e.player = player;
    e.reason = reason;
    Event(std::move(e));
    LogWrite(LogLevel::Info, "net", "player " + std::to_string(player) + " left: " + reason);
    SendRoster();
}

void NetSession::SendRoster()
{
    if (!m_isHost) return;
    NetBits w;
    WriteHeader(w, PRoster, nullptr);
    w.Bits(m_roster.size(), 8);
    int connected = 0;
    for (const NetRosterEntry& e : m_roster) {
        float rtt = e.rttMs, loss = e.loss;
        for (const auto& [link, p] : m_peers) {
            if (p.welcomed && p.player == e.player) { rtt = p.rttMs; loss = p.loss; }
        }
        w.Bits(static_cast<uint64_t>(e.player), 8);
        w.Bits(e.joinOrder, 32);
        w.Str(e.name, 64);
        w.Str(e.address, 128);
        w.Bits(static_cast<uint64_t>(std::clamp(rtt, 0.0f, 65535.0f)), 16);
        w.Bits(static_cast<uint64_t>(loss * 255.0f), 8);
        w.Bool(e.connected);
        connected += e.connected ? 1 : 0;
    }
    for (auto& [link, p] : m_peers) {
        if (p.welcomed) SendRaw(link, true, w.bytes);
    }
    // The LAN discovery answer: name, players, max players, lobby data.
    NetBits reply;
    reply.Str(m_lobbyName, 256);
    reply.Bits(static_cast<uint64_t>(connected), 8);
    reply.Bits(static_cast<uint64_t>(m_maxPlayers), 8);
    reply.Bits(std::min<size_t>(m_lobbyData.size(), 16), 8);
    for (size_t i = 0; i < m_lobbyData.size() && i < 16; ++i) {
        reply.Str(m_lobbyData[i].first, 256);
        reply.Str(m_lobbyData[i].second, 256);
    }
    m_discovery.reply = reply.bytes;
    m_rosterAt = m_now;
}

void NetSession::SendSpawn(NetPeer& peer, const NetObj& obj)
{
    const NetDef& d = m_kinds[static_cast<size_t>(obj.kind)];
    NetBits w;
    WriteHeader(w, PSpawn, nullptr);
    w.Bits(static_cast<uint64_t>(obj.id), 16);
    w.Bits(static_cast<uint64_t>(obj.kind), 8);
    w.Bits(static_cast<uint64_t>(obj.owner), 8);
    w.Bool(obj.ownerAuth);
    WriteFields(w, d, AllFields(d), obj.q, obj.blobs);
    SendRaw(peer.link, true, w.bytes);
    NetAck& a = peer.acked[obj.id];   // reliable: the client will have exactly these values
    a.q = obj.q;
    a.blobs = obj.blobs;
}

void NetSession::SendSync(NetPeer& peer)
{
    NetBits w;
    WriteHeader(w, PSync, nullptr);
    std::vector<int> ids;
    for (const auto& [id, obj] : m_objects) {
        if (!(obj.hidden & (1u << peer.player))) ids.push_back(id);
    }
    w.Bits(ids.size(), 16);
    for (int id : ids) w.Bits(static_cast<uint64_t>(id), 16);
    SendRaw(peer.link, true, w.bytes);
}

void NetSession::HostOnPacket(NetPeer& peer, uint8_t type, NetRead& r)
{
    if (!peer.welcomed) return;
    if (type == PBye) {
        DropPeer(peer.link, "left");
        return;
    }
    if (type == PAck) {
        const uint32_t tick = static_cast<uint32_t>(r.Bits(32));
        if (!r.ok) return;
        for (auto& [t, sent] : peer.pending) {
            if (t != tick) continue;
            for (const NetSent& s : sent) {
                auto a = peer.acked.find(s.id);
                auto o = m_objects.find(s.id);
                if (a == peer.acked.end() || o == m_objects.end()) continue;
                const NetDef& d = m_kinds[static_cast<size_t>(o->second.kind)];
                for (size_t fi = 0; fi < d.fields.size(); ++fi) {
                    if (!((s.mask >> fi) & 1)) continue;
                    const int n = CompCount(d.fields[fi].type.type);
                    const size_t at = static_cast<size_t>(d.start[fi]);
                    if (n == 0) a->second.blobs[at] = s.blobs[at];
                    for (int i = 0; i < n; ++i) a->second.q[at + static_cast<size_t>(i)] = s.q[at + static_cast<size_t>(i)];
                }
            }
        }
        while (!peer.pending.empty() && peer.pending.front().first <= tick) peer.pending.pop_front();
        return;
    }
    // Message and input packets are rate limited per peer (§25.11); owner state comes once per tick.
    if (type == PMsg || type == PInput) {
        peer.tokens = std::min(240.0, peer.tokens + (m_now - peer.tokensAt) * 240.0);
        peer.tokensAt = m_now;
        if (peer.tokens < 1.0) {
            ++m_dropped;
            return;
        }
        peer.tokens -= 1.0;
    }
    if (type == POwner) {
        while (r.Bool()) {
            const int id = static_cast<int>(r.Bits(16));
            const size_t kind = static_cast<size_t>(r.Bits(8));
            if (!r.ok || kind >= m_kinds.size()) break;
            const NetDef& d = m_kinds[kind];
            std::vector<uint64_t> q(static_cast<size_t>(d.comps));
            std::vector<std::string> blobs(static_cast<size_t>(d.blobs));
            if (!ReadFields(r, d, AllFields(d), q, blobs)) break;
            auto o = m_objects.find(id);
            if (o != m_objects.end() && o->second.ownerAuth && o->second.owner == peer.player && o->second.kind == static_cast<int>(kind)) {
                o->second.q = std::move(q);
                o->second.blobs = std::move(blobs);
            }
        }
    } else if (type == PInput && m_hasInput) {
        const int n = static_cast<int>(r.Bits(8));
        for (int i = 0; i < n && r.ok; ++i) {
            NetInput in;
            in.seq = static_cast<uint32_t>(r.Bits(32));
            in.tick = static_cast<uint32_t>(r.Bits(32));
            std::vector<uint64_t> q(static_cast<size_t>(m_input.comps));
            std::vector<std::string> blobs(static_cast<size_t>(m_input.blobs));
            if (!ReadFields(r, m_input, AllFields(m_input), q, blobs)) break;
            if (in.seq <= peer.inputSeqIn) continue;
            for (size_t fi = 0; fi < m_input.fields.size(); ++fi) {
                in.values.emplace_back(m_input.fields[fi].name, Decode(m_input, static_cast<int>(fi), q, blobs));
            }
            peer.inputSeqIn = in.seq;
            peer.inputs.push_back(std::move(in));
            if (peer.inputs.size() > 256) peer.inputs.erase(peer.inputs.begin());
        }
    } else if (type == PMsg) {
        const size_t index = static_cast<size_t>(r.Bits(8));
        r.Bits(8);   // the sender's claim is ignored: the host knows who sent it
        const int to = DecodeTarget(r.Bits(8));
        const uint32_t tick = static_cast<uint32_t>(r.Bits(32));
        const bool reliable = r.Bool();
        if (!r.ok || index >= m_messages.size()) return;
        const NetDef& d = m_messages[index];
        std::vector<uint64_t> q(static_cast<size_t>(d.comps));
        std::vector<std::string> blobs(static_cast<size_t>(d.blobs));
        if (!ReadFields(r, d, AllFields(d), q, blobs)) return;
        NetMessage msg{ d.name, peer.player, tick, {} };
        for (size_t fi = 0; fi < d.fields.size(); ++fi) msg.values.emplace_back(d.fields[fi].name, Decode(d, static_cast<int>(fi), q, blobs));
        DeliverMessage(std::move(msg), to, reliable, peer.player);
    }
}

void NetSession::ClientOnPacket(NetPeer& peer, uint8_t type, NetRead& r)
{
    if (type == PWelcome) {
        const int player = static_cast<int>(r.Bits(8));
        const int host = static_cast<int>(r.Bits(8));
        const uint32_t tick = static_cast<uint32_t>(r.Bits(32));
        const int rate = static_cast<int>(r.Bits(8));
        const int maxPlayers = static_cast<int>(r.Bits(8));
        const float timeout = static_cast<float>(r.Bits(16)) / 10.0f;
        const float migration = static_cast<float>(r.Bits(16)) / 10.0f;
        if (!r.ok || rate <= 0 || 60 % rate != 0) return;
        // The header of this packet carries the host's session and epoch: adopt them.
        NetRead h(r.bytes);
        h.Bits(32);
        m_session = static_cast<uint32_t>(h.Bits(32));
        m_epoch = static_cast<uint16_t>(h.Bits(16));
        const bool wasMigrating = m_mode == Mode::Migrating;
        m_self = player;
        m_hostPlayer = host;
        m_tick = tick;
        m_tickRate = rate;
        m_maxPlayers = maxPlayers;
        m_timeout = std::max(1.0f, timeout);
        m_migrationTimeout = std::max(0.5f, migration);
        m_tickSynced = false;
        m_mode = Mode::Connected;
        peer.welcomed = true;
        peer.player = host;
        NetEvent e;
        e.type = "connected";
        e.player = player;
        Event(std::move(e));
        if (wasMigrating) {
            NetEvent c;
            c.type = "host_changed";
            c.player = host;
            Event(std::move(c));
        }
        LogWrite(LogLevel::Info, "net", "connected as player " + std::to_string(player) + ", host is player " + std::to_string(host));
    } else if (type == PRefuse) {
        Fail("refused: " + r.Str(200));
    } else if (type == PBye) {
        StartMigration("the host left");
    } else if (type == PRoster) {
        std::vector<NetRosterEntry> roster;
        const int n = static_cast<int>(r.Bits(8));
        for (int i = 0; i < n && r.ok; ++i) {
            NetRosterEntry e;
            e.player = static_cast<int>(r.Bits(8));
            e.joinOrder = static_cast<uint32_t>(r.Bits(32));
            e.name = r.Str(64);
            e.address = r.Str(128);
            e.rttMs = static_cast<float>(r.Bits(16));
            e.loss = static_cast<float>(r.Bits(8)) / 255.0f;
            e.connected = r.Bool();
            roster.push_back(std::move(e));
        }
        if (!r.ok) return;
        if (m_rosterSeen) {
            for (const NetRosterEntry& now : roster) {
                bool known = now.player == m_self;
                for (const NetRosterEntry& old : m_roster) known = known || old.player == now.player;
                if (!known && now.connected) {
                    NetEvent e;
                    e.type = "joined";
                    e.player = now.player;
                    e.name = now.name;
                    Event(std::move(e));
                }
            }
            for (const NetRosterEntry& old : m_roster) {
                bool still = old.player == m_self;
                for (const NetRosterEntry& now : roster) still = still || now.player == old.player;
                if (!still) {
                    NetEvent e;
                    e.type = "left";
                    e.player = old.player;
                    Event(std::move(e));
                }
            }
        }
        m_roster = std::move(roster);
        m_rosterSeen = true;
    } else if (type == PSpawn) {
        NetObj obj;
        obj.id = static_cast<int>(r.Bits(16));
        obj.kind = static_cast<int>(r.Bits(8));
        obj.owner = static_cast<int>(r.Bits(8));
        obj.ownerAuth = r.Bool();
        if (!r.ok || obj.kind >= static_cast<int>(m_kinds.size())) return;
        const NetDef& d = m_kinds[static_cast<size_t>(obj.kind)];
        obj.q.assign(static_cast<size_t>(d.comps), 0);
        obj.blobs.assign(static_cast<size_t>(d.blobs), {});
        if (!ReadFields(r, d, AllFields(d), obj.q, obj.blobs)) return;
        PushHistory(obj, m_tick);
        m_objects[obj.id] = std::move(obj);
    } else if (type == PDespawn) {
        m_objects.erase(static_cast<int>(r.Bits(16)));
    } else if (type == PSync) {
        std::set<int> keep;
        const int n = static_cast<int>(r.Bits(16));
        for (int i = 0; i < n && r.ok; ++i) keep.insert(static_cast<int>(r.Bits(16)));
        if (!r.ok) return;
        for (auto it = m_objects.begin(); it != m_objects.end();) {
            it = keep.count(it->first) ? std::next(it) : m_objects.erase(it);
        }
    } else if (type == PSnap) {
        const uint32_t tick = static_cast<uint32_t>(r.Bits(32));
        const uint32_t inputAck = static_cast<uint32_t>(r.Bits(32));
        if (!r.ok || (m_tickSynced && tick <= m_tick)) return;   // older than what is applied
        bool complete = true;
        while (r.ok && r.Bool()) {
            const int id = static_cast<int>(r.Bits(16));
            const size_t kind = static_cast<size_t>(r.Bits(8));
            if (!r.ok || kind >= m_kinds.size()) return;
            const NetDef& d = m_kinds[kind];
            const uint64_t mask = r.Bits(static_cast<int>(d.fields.size()));
            auto it = m_objects.find(id);
            const bool known = it != m_objects.end() && it->second.kind == static_cast<int>(kind);
            std::vector<uint64_t> q = known ? it->second.q : std::vector<uint64_t>(static_cast<size_t>(d.comps));
            std::vector<std::string> blobs = known ? it->second.blobs : std::vector<std::string>(static_cast<size_t>(d.blobs));
            if (!ReadFields(r, d, mask, q, blobs)) return;
            if (!known) {   // its Spawn has not arrived yet: apply the rest, do not ack this snapshot
                complete = false;
                continue;
            }
            if (it->second.ownerAuth && it->second.owner == m_self) continue;
            it->second.q = std::move(q);
            it->second.blobs = std::move(blobs);
            PushHistory(it->second, tick);
        }
        if (!r.ok) return;
        // Time sync: the host's tick now is this tick plus half the round trip.
        const double target = tick + static_cast<double>(peer.rttMs) / 2000.0 * m_tickRate - m_now * m_tickRate;
        if (!m_tickSynced || std::abs(target - m_tickOffset) > 5.0) m_tickOffset = target;
        else m_tickOffset += (target - m_tickOffset) * 0.05;
        if (m_tickSynced) {
            const float interval = static_cast<float>((m_now - m_lastSnapAt) * m_tickRate) / static_cast<float>(std::max<uint32_t>(1, tick - m_tick));
            m_jitterTicks = m_jitterTicks * 0.9f + std::abs(interval - 1.0f) * 0.1f;
        }
        m_lastSnapAt = m_now;
        m_tick = tick;
        m_tickSynced = true;
        if (inputAck > m_inputAck) m_inputAck = inputAck;
        while (!m_unacked.empty() && m_unacked.front().seq <= m_inputAck) m_unacked.pop_front();
        if (complete) {
            NetBits w;
            WriteHeader(w, PAck, &peer);
            w.Bits(tick, 32);
            SendRaw(peer.link, false, w.bytes);
        }
    } else if (type == PMsg) {
        const size_t index = static_cast<size_t>(r.Bits(8));
        const int from = static_cast<int>(r.Bits(8));
        r.Bits(8);
        const uint32_t tick = static_cast<uint32_t>(r.Bits(32));
        r.Bool();
        if (!r.ok || index >= m_messages.size()) return;
        const NetDef& d = m_messages[index];
        std::vector<uint64_t> q(static_cast<size_t>(d.comps));
        std::vector<std::string> blobs(static_cast<size_t>(d.blobs));
        if (!ReadFields(r, d, AllFields(d), q, blobs)) return;
        NetMessage msg{ d.name, from, tick, {} };
        for (size_t fi = 0; fi < d.fields.size(); ++fi) msg.values.emplace_back(d.fields[fi].name, Decode(d, static_cast<int>(fi), q, blobs));
        m_inbox.push_back(std::move(msg));
    }
}

// --- per frame -------------------------------------------------------------------------------------

void NetSession::FixedStep60()
{
    if (m_mode == Mode::Offline || m_tickRate <= 0) return;
    if (++m_fixedCount % (60 / m_tickRate) != 0) return;
    m_tickDue = true;
    if (m_isHost) ++m_tick;
}

void NetSession::Send(double now)
{
    m_now = now;
    if (m_mode == Mode::Offline) {
        FlushDelayed(now, false);
        return;
    }
    // Pings, timeouts, joins and migrations that ran out of time.
    std::vector<uint64_t> drop;
    for (auto& [link, p] : m_peers) {
        const bool talking = m_isHost ? p.welcomed : link == m_hostLink;
        if (talking && now - p.lastPing >= 1.0) {
            p.lastPing = now;
            NetBits w;
            WriteHeader(w, PPing, nullptr);
            w.Bits(static_cast<uint32_t>(now * 1000.0), 32);
            SendRaw(link, false, w.bytes);
        }
        if (m_isHost && now - p.lastHeard > m_timeout) drop.push_back(link);
    }
    for (uint64_t link : drop) DropPeer(link, "timed out");
    if (m_isHost) {
        for (auto it = m_roster.begin(); it != m_roster.end();) {
            if (it->connected || it->expectUntil > now) {
                ++it;
                continue;
            }
            NetEvent e;
            e.type = "left";
            e.player = it->player;
            e.reason = "did not come back after the host changed";
            it = m_roster.erase(it);
            Event(std::move(e));
            SendRoster();
        }
        if (m_tickDue) Replicate();
        if (now - m_rosterAt >= 2.0) SendRoster();   // refreshes everyone's round-trip and loss figures
    } else if (m_mode == Mode::Joining && now > m_joinDeadline) {
        Fail("could not reach the host");
        return;
    } else if (m_mode == Mode::Migrating && now > m_joinDeadline) {
        TryNextCandidate();
    } else if (m_mode == Mode::Connected) {
        NetPeer* host = HostPeer();
        if (!host || now - host->lastHeard > m_migrationTimeout) {
            StartMigration("the host stopped answering");
        } else {
            ClientSendState();
        }
    }
    m_tickDue = false;
    FlushDelayed(now, false);
    LinkFlush(m_link);
    if (now - m_rateAt >= 1.0) {
        const double span = m_rateAt > 0.0 ? now - m_rateAt : 1.0;
        m_inPerSec = static_cast<double>(m_rateIn) / span;
        m_outPerSec = static_cast<double>(m_rateOut) / span;
        m_rateIn = m_rateOut = 0;
        m_rateAt = now;
    }
}

void NetSession::Replicate()
{
    // Lag-compensation history: one second of replicated components (§25.5 rewind).
    std::unordered_map<int, std::vector<uint64_t>> frame;
    for (const auto& [id, obj] : m_objects) frame[id] = obj.q;
    m_rewind.emplace_back(m_tick, std::move(frame));
    while (m_rewind.size() > static_cast<size_t>(m_tickRate) + 1) m_rewind.pop_front();

    struct Candidate { const NetObj* obj; NetAck* ack; uint64_t mask; };
    for (auto& [link, peer] : m_peers) {
        if (!peer.welcomed) continue;
        std::vector<Candidate> candidates;
        for (const auto& [id, obj] : m_objects) {
            if ((obj.hidden & (1u << peer.player)) || (obj.ownerAuth && obj.owner == peer.player)) continue;
            auto a = peer.acked.find(id);
            if (a == peer.acked.end()) continue;   // not spawned on that client
            const uint64_t mask = DiffMask(m_kinds[static_cast<size_t>(obj.kind)], obj, a->second);
            if (mask == 0) continue;
            // The client's own objects go first: prediction needs them every tick (§25.9).
            a->second.acc += obj.priority * (obj.owner == peer.player ? 1000.0f : 1.0f);
            candidates.push_back({ &obj, &a->second, mask });
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.ack->acc > y.ack->acc; });
        NetBits w;
        WriteHeader(w, PSnap, &peer);
        w.Bits(m_tick, 32);
        w.Bits(peer.inputTaken, 32);
        std::vector<NetSent> sent;
        for (const Candidate& c : candidates) {
            const NetDef& d = m_kinds[static_cast<size_t>(c.obj->kind)];
            const size_t mark = w.Mark();
            w.Bool(true);
            w.Bits(static_cast<uint64_t>(c.obj->id), 16);
            w.Bits(static_cast<uint64_t>(c.obj->kind), 8);
            w.Bits(c.mask, static_cast<int>(d.fields.size()));
            WriteFields(w, d, c.mask, c.obj->q, c.obj->blobs);
            if (w.bytes.size() + 1 > kSnapshotBudget) {   // waits for a later tick, its priority still growing
                w.Rollback(mark);
                continue;
            }
            c.ack->acc = 0.0f;
            sent.push_back({ c.obj->id, c.mask, c.obj->q, c.obj->blobs });
        }
        w.Bool(false);
        m_lastSnapBytes = static_cast<int64_t>(w.bytes.size());
        SendRaw(link, false, w.bytes);
        peer.pending.emplace_back(m_tick, std::move(sent));
        while (peer.pending.size() > 64) peer.pending.pop_front();
    }
}

void NetSession::ClientSendState()
{
    NetPeer* host = HostPeer();
    if (!host) return;
    if (m_tickDue) {   // owner-authoritative objects, every network tick (§25.6)
        NetBits w;
        WriteHeader(w, POwner, host);
        bool any = false;
        for (const auto& [id, obj] : m_objects) {
            if (!obj.ownerAuth || obj.owner != m_self) continue;
            const NetDef& d = m_kinds[static_cast<size_t>(obj.kind)];
            const size_t mark = w.Mark();
            w.Bool(true);
            w.Bits(static_cast<uint64_t>(id), 16);
            w.Bits(static_cast<uint64_t>(obj.kind), 8);
            WriteFields(w, d, AllFields(d), obj.q, obj.blobs);
            if (w.bytes.size() + 1 > kSnapshotBudget) {
                w.Rollback(mark);
                LogOnce("owner-authoritative objects of one player do not fit one packet; some wait");
                break;
            }
            any = true;
        }
        w.Bool(false);
        if (any) SendRaw(host->link, false, w.bytes);
    }
    if (!m_unacked.empty() && (m_inputsNew || m_tickDue)) {   // new inputs at once, resends each tick
        NetBits w;
        WriteHeader(w, PInput, host);
        const size_t first = m_unacked.size() > 16 ? m_unacked.size() - 16 : 0;
        w.Bits(m_unacked.size() - first, 8);
        for (size_t i = first; i < m_unacked.size(); ++i) {
            const NetInput& in = m_unacked[i];
            std::vector<uint64_t> q(static_cast<size_t>(m_input.comps));
            std::vector<std::string> blobs(static_cast<size_t>(m_input.blobs));
            bool clamped = false;
            for (const auto& [name, value] : in.values) {
                const int fi = FieldIndex(m_input, name);
                if (fi >= 0) Encode(m_input, fi, value, q, blobs, clamped);
            }
            w.Bits(in.seq, 32);
            w.Bits(in.tick, 32);
            WriteFields(w, m_input, AllFields(m_input), q, blobs);
        }
        SendRaw(host->link, false, w.bytes);
        m_inputsNew = false;
    }
}

// --- migration (§25.8) -----------------------------------------------------------------------------

void NetSession::StartMigration(const std::string& reason)
{
    if (m_isHost || m_mode == Mode::Migrating || m_mode == Mode::Offline) return;
    LogWrite(LogLevel::Info, "net", "host lost (" + reason + "): electing a new host");
    m_oldHost = m_hostPlayer;
    if (m_hostLink) LinkDisconnect(m_link, m_hostLink);
    m_peers.erase(m_hostLink);
    m_hostLink = 0;
    m_roster.erase(std::remove_if(m_roster.begin(), m_roster.end(), [&](const NetRosterEntry& e) { return e.player == m_oldHost; }), m_roster.end());
    NetEvent e;
    e.type = "left";
    e.player = m_oldHost;
    e.reason = reason;
    Event(std::move(e));
    m_candidates.clear();
    for (const NetRosterEntry& c : m_roster) if (c.connected) m_candidates.push_back(c);
    std::sort(m_candidates.begin(), m_candidates.end(), [](const NetRosterEntry& a, const NetRosterEntry& b) { return a.joinOrder < b.joinOrder; });
    if (m_kindOfLink == NetLinkKind::Steam) {   // whoever Steam made the lobby owner wins
        const uint64_t owner = SteamLobbyOwner();
        for (size_t i = 0; owner != 0 && i < m_candidates.size(); ++i) {
            const bool isOwner = m_candidates[i].player == m_self ? owner == SteamNetLocalId()
                                                                  : m_candidates[i].address == "steam-user:" + std::to_string(owner);
            if (isOwner) std::rotate(m_candidates.begin(), m_candidates.begin() + static_cast<std::ptrdiff_t>(i), m_candidates.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        }
    }
    m_mode = Mode::Migrating;
    TryNextCandidate();
}

void NetSession::TryNextCandidate()
{
    if (m_hostLink) {
        LinkDisconnect(m_link, m_hostLink);
        m_peers.erase(m_hostLink);
        m_hostLink = 0;
    }
    while (!m_candidates.empty()) {
        const NetRosterEntry c = m_candidates.front();
        m_candidates.erase(m_candidates.begin());
        if (c.player == m_self) {
            BecomeHost();
            return;
        }
        uint64_t link = 0;
        if (c.address.empty() || !LinkConnect(m_link, c.address, link)) continue;
        m_hostLink = link;
        NetPeer& p = m_peers[link];
        p.link = link;
        p.lastHeard = m_now;
        m_hostPlayer = c.player;
        m_joinDeadline = m_now + std::max(4.0, static_cast<double>(m_migrationTimeout) * 2.0);
        return;
    }
    Fail("the host left and no other player could take over");
}

void NetSession::BecomeHost()
{
    m_isHost = true;
    m_mode = Mode::Hosting;
    ++m_epoch;
    m_hostPlayer = m_self;
    if (m_tickSynced) m_tick = static_cast<uint32_t>(std::max(static_cast<double>(m_tick), EstimatedTick()));
    m_joinCounter = 0;
    for (NetRosterEntry& e : m_roster) {
        m_joinCounter = std::max(m_joinCounter, e.joinOrder);
        if (e.player != m_self) {
            e.connected = false;
            e.expectUntil = m_now + m_timeout;
        }
    }
    for (auto& [link, p] : m_peers) p = NetPeer{ link };   // anyone already here sends Hello again
    for (auto& [id, obj] : m_objects) {
        obj.history.clear();
        m_nextId = std::max(m_nextId, id + 1);
    }
    m_rewind.clear();
    m_unacked.clear();
    m_inputAck = 0;
    if (m_kindOfLink == NetLinkKind::Enet) DiscoveryAnswer(m_discovery, SchemaHash(), m_link.port, localOnly);
    if (m_kindOfLink == NetLinkKind::Steam) {
        SteamLobbySetData("name", m_lobbyName);
        for (const auto& [k, v] : m_lobbyData) SteamLobbySetData(k, v);
    }
    SendRoster();
    NetEvent e;
    e.type = "became_host";
    e.player = m_self;
    Event(std::move(e));
    LogWrite(LogLevel::Info, "net", "this player is the host now (epoch " + std::to_string(m_epoch) + ")");
}

// --- objects ---------------------------------------------------------------------------------------

int NetSession::Spawn(const std::string& kind, int owner, bool ownerAuthority, float priority, const NetValues& values)
{
    if (!m_isHost) {
        LogOnce("spawn: only the host spawns objects");
        return 0;
    }
    int kindIndex = -1;
    for (size_t i = 0; i < m_kinds.size(); ++i) if (m_kinds[i].name == kind) kindIndex = static_cast<int>(i);
    if (kindIndex < 0 || owner < 0 || owner > kMaxPlayers) {
        LogOnce("spawn: unknown kind '" + kind + "' or bad owner");
        return 0;
    }
    if (m_objects.size() >= 65535) {
        LogOnce("spawn: 65535 objects are live");
        return 0;
    }
    while (m_objects.count(m_nextId) || m_nextId <= 0 || m_nextId > 65535) m_nextId = m_nextId >= 65535 ? 1 : m_nextId + 1;
    const NetDef& d = m_kinds[static_cast<size_t>(kindIndex)];
    NetObj obj;
    obj.id = m_nextId++;
    obj.kind = kindIndex;
    obj.owner = owner;
    obj.ownerAuth = ownerAuthority;
    obj.priority = std::max(0.0f, priority);
    obj.q.assign(static_cast<size_t>(d.comps), 0);
    obj.blobs.assign(static_cast<size_t>(d.blobs), {});
    bool clamped = false;
    for (const auto& [name, value] : values) {
        const int fi = FieldIndex(d, name);
        if (fi < 0 || !Encode(d, fi, value, obj.q, obj.blobs, clamped)) {
            LogOnce("spawn '" + kind + "': field '" + name + "' is unknown or has the wrong type");
            return 0;
        }
    }
    if (clamped) LogOnce("spawn '" + kind + "': a value was outside its field's range and was clamped");
    const int id = obj.id;
    auto& stored = m_objects[id] = std::move(obj);
    for (auto& [link, peer] : m_peers) {
        if (peer.welcomed && !(stored.hidden & (1u << peer.player))) SendSpawn(peer, stored);
    }
    return id;
}

const NetObj* NetSession::Object(int id) const
{
    auto it = m_objects.find(id);
    return it == m_objects.end() ? nullptr : &it->second;
}

std::vector<int> NetSession::Objects(const std::string& kind) const
{
    std::vector<int> out;
    for (const auto& [id, obj] : m_objects) {
        if (kind.empty() || m_kinds[static_cast<size_t>(obj.kind)].name == kind) out.push_back(id);
    }
    return out;
}

bool NetSession::CanWrite(int id) const
{
    const NetObj* o = Object(id);
    if (!o || m_mode == Mode::Offline) return false;
    if (!o->ownerAuth) return m_isHost;
    return o->owner == m_self || (o->owner == 0 && m_isHost);
}

bool NetSession::Set(int id, const std::string& field, const NetValue& value)
{
    if (!CanWrite(id)) return false;
    NetObj& o = m_objects[id];
    const NetDef& d = m_kinds[static_cast<size_t>(o.kind)];
    const int fi = FieldIndex(d, field);
    bool clamped = false;
    if (fi < 0 || !Encode(d, fi, value, o.q, o.blobs, clamped)) return false;
    if (clamped) LogOnce("'" + d.name + "." + field + "': a value outside the field's range was clamped");
    return true;
}

std::optional<NetValue> NetSession::Get(int id, const std::string& field) const
{
    const NetObj* o = Object(id);
    if (!o) return std::nullopt;
    const NetDef& d = m_kinds[static_cast<size_t>(o->kind)];
    const int fi = FieldIndex(d, field);
    if (fi < 0) return std::nullopt;
    return Decode(d, fi, o->q, o->blobs);
}

void NetSession::PushHistory(NetObj& obj, double tick)
{
    const NetDef& d = m_kinds[static_cast<size_t>(obj.kind)];
    std::vector<double> values(obj.q.size());
    for (size_t fi = 0; fi < d.fields.size(); ++fi) {
        const int n = CompCount(d.fields[fi].type.type);
        for (int i = 0; i < n; ++i) {
            const size_t at = static_cast<size_t>(d.start[fi] + i);
            values[at] = Dequantize(d.fields[fi].type, obj.q[at]);
        }
    }
    // An object that did not change for a while holds still until the tick before this one, so its
    // move is not spread over the whole quiet stretch.
    if (!obj.history.empty() && obj.history.back().first < tick - 1.0) obj.history.emplace_back(tick - 1.0, obj.history.back().second);
    if (!obj.history.empty() && obj.history.back().first >= tick) obj.history.back() = { tick, std::move(values) };
    else obj.history.emplace_back(tick, std::move(values));
    while (obj.history.size() > static_cast<size_t>(kHistory)) obj.history.pop_front();
}

std::optional<NetValue> NetSession::Sample(int id, const std::string& field) const
{
    const NetObj* o = Object(id);
    if (!o) return std::nullopt;
    const NetDef& d = m_kinds[static_cast<size_t>(o->kind)];
    const int fi = FieldIndex(d, field);
    if (fi < 0) return std::nullopt;
    const NetFieldType& f = d.fields[static_cast<size_t>(fi)].type;
    const int n = CompCount(f.type);
    const bool smooth = n > 0 && f.type != NetType::Bool && f.type != NetType::Player && f.type != NetType::Ref;
    if (m_isHost || !smooth || o->history.size() < 2 || (o->ownerAuth && o->owner == m_self)) return Get(id, field);
    const double rt = EstimatedTick() - (2.0 + std::min(4.0, static_cast<double>(m_jitterTicks) * 2.0));
    const auto& h = o->history;
    size_t i = 0;
    while (i + 1 < h.size() && h[i + 1].first <= rt) ++i;
    const auto& a = h[i];
    const auto& b = i + 1 < h.size() ? h[i + 1] : h[i];
    const double t = b.first > a.first ? std::clamp((rt - a.first) / (b.first - a.first), 0.0, 1.0) : 0.0;
    double c[3] = {};
    for (int k = 0; k < n; ++k) {
        const size_t at = static_cast<size_t>(d.start[static_cast<size_t>(fi)] + k);
        const double x = a.second[at], y = b.second[at];
        c[k] = f.type == NetType::Angle ? x + (std::fmod(y - x + 540.0, 360.0) - 180.0) * t : x + (y - x) * t;
    }
    return MakeValue(f, c, nullptr);
}

void NetSession::SetPriority(int id, float priority)
{
    auto it = m_objects.find(id);
    if (it != m_objects.end() && m_isHost) it->second.priority = std::max(0.0f, priority);
}

void NetSession::SetVisible(int id, int player, bool visible)
{
    auto it = m_objects.find(id);
    if (it == m_objects.end() || !m_isHost || player < 1 || player > kMaxPlayers) return;
    const uint32_t bit = 1u << player;
    const bool hidden = (it->second.hidden & bit) != 0;
    if (hidden == !visible) return;
    if (visible) it->second.hidden &= ~bit;
    else it->second.hidden |= bit;
    for (auto& [link, peer] : m_peers) {
        if (!peer.welcomed || peer.player != player) continue;
        if (visible) {
            SendSpawn(peer, it->second);
        } else {
            NetBits w;
            WriteHeader(w, PDespawn, nullptr);
            w.Bits(static_cast<uint64_t>(id), 16);
            SendRaw(link, true, w.bytes);
            peer.acked.erase(id);
        }
    }
}

bool NetSession::Despawn(int id)
{
    if (!m_isHost || !m_objects.erase(id)) return false;
    for (auto& [link, peer] : m_peers) {
        if (!peer.welcomed || !peer.acked.erase(id)) continue;
        NetBits w;
        WriteHeader(w, PDespawn, nullptr);
        w.Bits(static_cast<uint64_t>(id), 16);
        SendRaw(link, true, w.bytes);
    }
    return true;
}

// --- messages and inputs ---------------------------------------------------------------------------

void NetSession::DeliverMessage(NetMessage msg, int to, bool reliable, int sender)
{
    const bool toHost = to == kNetHost || to == m_self || to == kNetAll || (to == kNetOthers && sender != m_self);
    size_t index = 0;
    while (index < m_messages.size() && m_messages[index].name != msg.name) ++index;
    const NetDef& d = m_messages[index];
    std::vector<uint64_t> q(static_cast<size_t>(d.comps));
    std::vector<std::string> blobs(static_cast<size_t>(d.blobs));
    bool clamped = false;
    for (const auto& [name, value] : msg.values) {
        const int fi = FieldIndex(d, name);
        if (fi >= 0) Encode(d, fi, value, q, blobs, clamped);
    }
    NetBits w;
    bool written = false;
    for (auto& [link, peer] : m_peers) {
        if (!peer.welcomed) continue;
        const bool send = to == kNetAll || (to == kNetOthers && peer.player != sender) || to == peer.player;
        if (!send) continue;
        if (!written) {
            WriteHeader(w, PMsg, nullptr);
            w.Bits(index, 8);
            w.Bits(static_cast<uint64_t>(msg.from), 8);
            w.Bits(EncodeTarget(to), 8);
            w.Bits(msg.tick, 32);
            w.Bool(reliable);
            WriteFields(w, d, AllFields(d), q, blobs);
            written = true;
        }
        SendRaw(link, reliable || w.bytes.size() > kSnapshotBudget, w.bytes);
    }
    if (toHost) m_inbox.push_back(std::move(msg));
}

bool NetSession::SendMsg(const std::string& name, int to, bool reliable, const NetValues& values)
{
    if (m_mode != Mode::Hosting && m_mode != Mode::Connected) return false;
    size_t index = 0;
    while (index < m_messages.size() && m_messages[index].name != name) ++index;
    if (index == m_messages.size()) {
        LogOnce("send: unknown message '" + name + "' (net.message it first)");
        return false;
    }
    const NetDef& d = m_messages[index];
    std::vector<uint64_t> q(static_cast<size_t>(d.comps));
    std::vector<std::string> blobs(static_cast<size_t>(d.blobs));
    bool clamped = false;
    for (const auto& [field, value] : values) {
        const int fi = FieldIndex(d, field);
        if (fi < 0 || !Encode(d, fi, value, q, blobs, clamped)) {
            LogOnce("send '" + name + "': field '" + field + "' is unknown or has the wrong type");
            return false;
        }
    }
    NetMessage msg{ name, m_self, Tick(), {} };
    for (size_t fi = 0; fi < d.fields.size(); ++fi) msg.values.emplace_back(d.fields[fi].name, Decode(d, static_cast<int>(fi), q, blobs));
    if (m_isHost) {
        DeliverMessage(std::move(msg), to, reliable, m_self);
        return true;
    }
    NetPeer* host = HostPeer();
    if (!host) return false;
    NetBits w;
    WriteHeader(w, PMsg, reliable ? nullptr : host);
    w.Bits(index, 8);
    w.Bits(static_cast<uint64_t>(m_self), 8);
    w.Bits(EncodeTarget(to), 8);
    w.Bits(msg.tick, 32);
    w.Bool(reliable);
    WriteFields(w, d, AllFields(d), q, blobs);
    if (!reliable && w.bytes.size() > kSnapshotBudget) {
        LogOnce("send '" + name + "': too big for an unreliable message, sent reliable");
        reliable = true;
    }
    SendRaw(host->link, reliable, w.bytes);
    return true;
}

std::vector<NetMessage> NetSession::TakeMessages()
{
    std::vector<NetMessage> out;
    out.swap(m_inbox);
    return out;
}

uint32_t NetSession::SendInput(const NetValues& values)
{
    if (!m_hasInput || (m_mode != Mode::Hosting && m_mode != Mode::Connected)) return 0;
    std::vector<uint64_t> q(static_cast<size_t>(m_input.comps));
    std::vector<std::string> blobs(static_cast<size_t>(m_input.blobs));
    bool clamped = false;
    for (const auto& [name, value] : values) {
        const int fi = FieldIndex(m_input, name);
        if (fi < 0 || !Encode(m_input, fi, value, q, blobs, clamped)) {
            LogOnce("send_input: field '" + name + "' is unknown or has the wrong type");
            return 0;
        }
    }
    NetInput in;
    in.seq = ++m_inputSeq;
    in.tick = Tick();
    for (size_t fi = 0; fi < m_input.fields.size(); ++fi) in.values.emplace_back(m_input.fields[fi].name, Decode(m_input, static_cast<int>(fi), q, blobs));
    if (m_isHost) {   // the host's own inputs take the same path for the game code
        m_hostOwnInputs.push_back(std::move(in));
        if (m_hostOwnInputs.size() > 256) m_hostOwnInputs.erase(m_hostOwnInputs.begin());
        return m_inputSeq;
    }
    m_unacked.push_back(std::move(in));
    while (m_unacked.size() > 32) m_unacked.pop_front();
    m_inputsNew = true;
    return m_inputSeq;
}

std::vector<NetInput> NetSession::TakeInputs(int player)
{
    std::vector<NetInput> out;
    if (!m_isHost) return out;
    if (player == m_self) {
        out.swap(m_hostOwnInputs);
        if (!out.empty()) m_inputAck = out.back().seq;
        return out;
    }
    for (auto& [link, peer] : m_peers) {
        if (!peer.welcomed || peer.player != player) continue;
        out.swap(peer.inputs);
        if (!out.empty()) peer.inputTaken = out.back().seq;
    }
    return out;
}

std::map<int, NetValues> NetSession::Rewind(uint32_t tick) const
{
    std::map<int, NetValues> out;
    if (!m_isHost || m_rewind.empty()) return out;
    size_t i = 0;
    while (i + 1 < m_rewind.size() && m_rewind[i + 1].first <= tick) ++i;
    for (const auto& [id, q] : m_rewind[i].second) {
        const NetObj* o = Object(id);
        if (!o) continue;
        const NetDef& d = m_kinds[static_cast<size_t>(o->kind)];
        NetValues values;
        for (size_t fi = 0; fi < d.fields.size(); ++fi) values.emplace_back(d.fields[fi].name, Decode(d, static_cast<int>(fi), q, o->blobs));
        out[id] = std::move(values);
    }
    return out;
}

} // namespace IYI
