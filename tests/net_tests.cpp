// Native checks for libraries/net (SPEC §25.12), run by IYIEngineTests when built with IYI_NET. Several
// NetSessions in one process over the loopback link, with time stepped by hand: join, delta replication,
// quantization, loss, the bandwidth budget, messages, inputs, owner authority, interest, refusal,
// host migration and bounded decoding of random bytes.

#include "net/src/NetSession.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace IYI;

static int s_failures = 0;

#define NCHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } } while (0)

namespace {

void Define(NetSession& s)
{
    s.Define("box", { { "pos", NetVec2(-1000, 1000, 0.01) }, { "hp", NetSimple(NetType::U8) }, { "name", NetText(16) },
                      { "ang", NetAngle(12) } });
    s.Define("avatar", { { "x", NetFixed(-100, 100, 0.01) } });
    s.DefineMessage("chat", { { "text", NetText(64) } });
    s.DefineInput({ { "dx", NetFixed(-1, 1, 0.01) } });
}

// One game step for every session: receive, the 60 Hz fixed step, send.
void Step(const std::vector<NetSession*>& all, double& now, int frames)
{
    for (int f = 0; f < frames; ++f) {
        now += 1.0 / 60.0;
        for (NetSession* s : all) s->Receive(now);
        for (NetSession* s : all) s->FixedStep60();
        for (NetSession* s : all) s->Send(now);
    }
}

int64_t Int(const std::optional<NetValue>& v) { return v ? std::get<int64_t>(*v) : -1; }
double Num(const std::optional<NetValue>& v) { return v ? std::get<double>(*v) : -1e9; }
Vector2 Vec(const std::optional<NetValue>& v) { return v ? std::get<Vector2>(*v) : Vector2{ -1e9f, -1e9f }; }

bool HasEvent(std::vector<NetEvent>& events, const std::string& type)
{
    for (const NetEvent& e : events) if (e.type == type) return true;
    return false;
}

} // namespace

int RunNetTests()
{
    s_failures = 0;
    NetSession host, a, b;
    Define(host);
    Define(a);
    Define(b);
    NetHostOptions o;
    o.transport = "loopback";
    o.port = 5001;
    double now = 0.0;
    NCHECK(host.Host(o, now));
    NCHECK(a.Join("loop:5001", "A", now));
    Step({ &host, &a }, now, 4);   // a joins first: it is next in line for host
    NCHECK(b.Join("loop:5001", "B", now));
    std::vector<NetSession*> all = { &host, &a, &b };
    Step(all, now, 10);
    NCHECK(a.State() == "connected" && b.State() == "connected");
    NCHECK(a.LocalPlayer() == 2 && b.LocalPlayer() == 3);
    NCHECK(host.Players().size() == 3 && b.Players().size() == 3);

    // Spawn and replicate; quantization; an unchanged object costs nothing.
    const int box = host.Spawn("box", 0, false, 1.0f, { { "pos", Vector2{ 1.5f, -2.25f } }, { "hp", int64_t(7) },
                                                     { "name", std::string("crate") }, { "ang", -90.0 } });
    NCHECK(box > 0);
    Step(all, now, 6);
    NCHECK(std::abs(Vec(a.Get(box, "pos")).x - 1.5f) < 0.006f && std::abs(Vec(a.Get(box, "pos")).y + 2.25f) < 0.006f);
    NCHECK(Int(b.Get(box, "hp")) == 7);
    NCHECK(a.Get(box, "name") && std::get<std::string>(*a.Get(box, "name")) == "crate");
    NCHECK(std::abs(Num(a.Get(box, "ang")) - 270.0) < 0.1);
    NCHECK(host.Stats().lastSnapshotBytes == 21);   // header 12 + tick 4 + input ack 4 + end bit: nothing changed
    NCHECK(host.Set(box, "hp", int64_t(300)));   // clamps to 255
    NCHECK(!a.Set(box, "hp", int64_t(1)));       // a client has no authority over host objects
    Step(all, now, 4);
    NCHECK(Int(b.Get(box, "hp")) == 255);

    // Half the snapshots lost: the next ones carry the same differences, so the client still converges.
    host.Simulate(0.0f, 0.0f, 0.5f);
    for (int i = 0; i < 30; ++i) {
        host.Set(box, "pos", Vector2{ static_cast<float>(i), 0.0f });
        Step(all, now, 2);
    }
    host.Simulate(0.0f, 0.0f, 0.0f);
    Step(all, now, 6);
    NCHECK(std::abs(Vec(a.Get(box, "pos")).x - 29.0f) < 0.006f && std::abs(Vec(b.Get(box, "pos")).x - 29.0f) < 0.006f);

    // 400 objects never overflow one packet and all arrive.
    std::vector<int> many;
    for (int i = 0; i < 400; ++i) many.push_back(host.Spawn("box", 0, false, 1.0f, { { "hp", int64_t(1) } }));
    Step(all, now, 4);
    for (int i = 0; i < 400; ++i) host.Set(many[static_cast<size_t>(i)], "pos", Vector2{ static_cast<float>(i), 1.0f });
    Step(all, now, 60);
    NCHECK(host.Stats().lastSnapshotBytes <= 1150);
    bool allThere = true;
    for (int i = 0; i < 400; ++i) allThere = allThere && std::abs(Vec(b.Get(many[static_cast<size_t>(i)], "pos")).x - i) < 0.006f;
    NCHECK(allThere);
    for (int id : many) host.Despawn(id);
    Step(all, now, 4);
    NCHECK(b.Objects("box").size() == 1);

    // Interpolation: a steadily moving object samples behind its latest value, between two received ones.
    for (int i = 0; i < 20; ++i) {
        host.Set(box, "pos", Vector2{ 100.0f + i * 3.0f, 0.0f });
        Step(all, now, 2);
    }
    const float latest = Vec(b.Get(box, "pos")).x, sampled = Vec(b.Sample(box, "pos")).x;
    NCHECK(latest > 150.0f && sampled < latest && sampled > latest - 12.0f);
    NCHECK(std::abs(Vec(host.Sample(box, "pos")).x - Vec(host.Get(box, "pos")).x) < 0.001f);   // the host has no delay

    // Messages: to everyone, then to one player.
    NCHECK(a.SendMsg("chat", kNetAll, true, { { "text", std::string("hi") } }));
    Step(all, now, 4);
    NCHECK(host.TakeMessages().size() == 1 && a.TakeMessages().size() == 1);
    std::vector<NetMessage> got = b.TakeMessages();
    NCHECK(got.size() == 1 && got[0].from == 2 && std::get<std::string>(got[0].values[0].second) == "hi");
    NCHECK(b.SendMsg("chat", 2, true, { { "text", std::string("only a") } }));
    Step(all, now, 4);
    NCHECK(a.TakeMessages().size() == 1 && host.TakeMessages().empty() && b.TakeMessages().empty());

    // Inputs reach the host in order and are acked once taken.
    const uint32_t seq = a.SendInput({ { "dx", 0.5 } });
    NCHECK(seq == 1);
    Step(all, now, 3);
    std::vector<NetInput> inputs = host.TakeInputs(2);
    NCHECK(inputs.size() == 1 && inputs[0].seq == 1 && std::abs(std::get<double>(inputs[0].values[0].second) - 0.5) < 0.006);
    Step(all, now, 4);
    NCHECK(a.InputAck() == 1);

    // Owner authority: player 2 writes its avatar, the host relays it, nobody else may write it.
    const int avatar = host.Spawn("avatar", 2, true, 1.0f, {});
    Step(all, now, 4);
    NCHECK(a.CanWrite(avatar) && !b.CanWrite(avatar) && !host.CanWrite(avatar));
    NCHECK(a.Set(avatar, "x", 5.0));
    Step(all, now, 6);
    NCHECK(std::abs(Num(host.Get(avatar, "x")) - 5.0) < 0.006 && std::abs(Num(b.Get(avatar, "x")) - 5.0) < 0.006);

    // Interest: hidden from player 3, then shown again.
    host.SetVisible(box, 3, false);
    Step(all, now, 4);
    NCHECK(b.Object(box) == nullptr && a.Object(box) != nullptr);
    host.SetVisible(box, 3, true);
    Step(all, now, 4);
    NCHECK(b.Object(box) != nullptr && Int(b.Get(box, "hp")) == 255);

    // A different game build is refused.
    NetSession other;
    other.Define("box", { { "pos", NetVec2(-10, 10, 0.1) } });
    NCHECK(other.Join("loop:5001", "", now));
    Step({ &host, &a, &b, &other }, now, 6);
    std::vector<NetEvent> otherEvents = other.TakeEvents();
    NCHECK(other.State() == "offline" && HasEvent(otherEvents, "disconnected"));

    // Random bytes: the host survives and drops that peer.
    {
        NetLink raw;
        NCHECK(LinkOpen(raw, NetLinkKind::Loopback, 0, 4, true));
        uint64_t peer = 0;
        NCHECK(LinkConnect(raw, "loop:5001", peer));
        std::mt19937 rng(7);
        for (int i = 0; i < 40; ++i) {
            std::string junk(static_cast<size_t>(rng() % 64), '\0');
            for (char& c : junk) c = static_cast<char>(rng());
            if (i % 2) junk = std::string("\x49\x4E\x01") + junk;   // a valid magic, then junk
            LinkSend(raw, peer, true, junk);
        }
        Step(all, now, 2);
        LinkClose(raw);
        NCHECK(host.State() == "hosting" && host.Players().size() == 3);
    }

    // Host migration: the host leaves; a (joined first) takes over with the replicated objects; b follows.
    a.TakeEvents();
    b.TakeEvents();
    const uint32_t tickBefore = a.Tick();
    host.Leave();
    Step({ &a, &b }, now, 30);
    std::vector<NetEvent> aEvents = a.TakeEvents(), bEvents = b.TakeEvents();
    NCHECK(a.IsHost() && a.State() == "hosting" && HasEvent(aEvents, "became_host"));
    NCHECK(b.State() == "connected" && b.HostPlayer() == 2 && HasEvent(bEvents, "host_changed"));
    NCHECK(a.Players().size() == 2 && b.Players().size() == 2);
    NCHECK(a.Object(box) != nullptr && Int(a.Get(box, "hp")) == 255 && b.Object(box) != nullptr);
    NCHECK(a.Tick() >= tickBefore);
    NCHECK(a.Set(box, "hp", int64_t(42)));
    Step({ &a, &b }, now, 6);
    NCHECK(Int(b.Get(box, "hp")) == 42);

    a.Leave();
    b.Leave();
    std::printf("net_tests: %s\n", s_failures == 0 ? "all passed" : "FAILED");
    return s_failures;
}
