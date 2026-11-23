// Declares NetSession, one multiplayer session (SPEC §25): definitions, peers, replicated objects, delta
// replication, messages, inputs, time sync and host migration. Net.cpp owns the game's one session; the
// native suite runs several in one process over the loopback link. Time is passed in (seconds), so tests
// step it by hand.
//
// Connected to:
// - Implemented in NetSession.cpp; Net.cpp wraps it as IYI::Api; tests/net_tests.cpp drives it directly.

#pragma once

#include "net/src/Net.h"
#include "net/src/NetLink.h"

#include <deque>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace IYI {

class NetBits;
class NetRead;

struct NetDef {   // an object kind, a message or the input: named fields, laid out as components and blobs
    std::string name;
    std::vector<NetField> fields;
    std::vector<int> start;   // per field: first component (numbers) or blob index (Text, Bytes)
    int comps = 0, blobs = 0;
};

struct NetObj {
    int id = 0;
    int kind = 0;
    int owner = 0;   // 0: whoever hosts
    bool ownerAuth = false;
    float priority = 1.0f;
    uint32_t hidden = 0;   // bit p: hidden from player p
    std::vector<uint64_t> q;   // quantized components
    std::vector<std::string> blobs;
    std::deque<std::pair<double, std::vector<double>>> history;   // client: (host tick, decoded components)
};

struct NetAck { std::vector<uint64_t> q; std::vector<std::string> blobs; float acc = 0.0f; };
struct NetSent { int id = 0; uint64_t mask = 0; std::vector<uint64_t> q; std::vector<std::string> blobs; };

struct NetPeer {
    uint64_t link = 0;
    int player = 0;
    bool welcomed = false;
    double lastHeard = 0.0, lastPing = 0.0;
    float rttMs = 0.0f, loss = 0.0f;
    uint16_t sendSeq = 0, recvSeq = 0;
    int recvGot = 0, recvExpected = 0;
    double lossAt = 0.0;
    int invalid = 0;
    double tokens = 120.0, tokensAt = 0.0;
    // Host side, per client: what it has acked, what is in flight, its inputs.
    std::unordered_map<int, NetAck> acked;
    std::deque<std::pair<uint32_t, std::vector<NetSent>>> pending;
    uint32_t inputSeqIn = 0, inputTaken = 0;
    std::vector<NetInput> inputs;
};

struct NetRosterEntry {
    int player = 0;
    uint32_t joinOrder = 0;
    std::string name, address;
    float rttMs = 0.0f, loss = 0.0f;
    bool connected = true;
    double expectUntil = 0.0;   // new host after a migration: when a player that did not come back is gone
};

class NetSession {
public:
    NetSession();
    ~NetSession();
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;

    bool localOnly = false;   // agent runs: ENet on 127.0.0.1, no Steam (§25.4)

    // Definitions (same on every peer).
    bool Define(const std::string& kind, const std::vector<NetField>& fields);
    bool DefineMessage(const std::string& name, const std::vector<NetField>& fields);
    bool DefineInput(const std::vector<NetField>& fields);
    const NetDef* Kind(const std::string& name) const;
    const NetDef* Message(const std::string& name) const;
    const NetDef* Input() const { return m_hasInput ? &m_input : nullptr; }
    const std::string& KindName(int index) const { return m_kinds[static_cast<size_t>(index)].name; }
    uint32_t SchemaHash() const;

    // Session.
    bool Host(const NetHostOptions& options, double now);
    bool Join(const std::string& target, const std::string& playerName, double now);
    void Find(const std::string& transport, double now);
    void Leave();
    void Simulate(float latencyMs, float jitterMs, float loss);
    void Receive(double now);
    void FixedStep60();
    void Send(double now);

    std::string State() const;
    bool IsHost() const { return m_isHost; }
    int LocalPlayer() const { return m_self; }
    int HostPlayer() const { return m_hostPlayer; }
    std::vector<NetPlayer> Players() const;
    uint32_t Tick() const;
    NetStats Stats() const;
    void SetLobbyData(const std::string& key, const std::string& value);
    std::vector<NetEvent> TakeEvents();

    // Objects.
    int Spawn(const std::string& kind, int owner, bool ownerAuthority, float priority, const NetValues& values);
    const NetObj* Object(int id) const;
    std::vector<int> Objects(const std::string& kind) const;
    bool CanWrite(int id) const;
    bool Set(int id, const std::string& field, const NetValue& value);
    std::optional<NetValue> Get(int id, const std::string& field) const;
    std::optional<NetValue> Sample(int id, const std::string& field) const;
    void SetPriority(int id, float priority);
    void SetVisible(int id, int player, bool visible);
    bool Despawn(int id);

    // Messages and inputs.
    bool SendMsg(const std::string& name, int to, bool reliable, const NetValues& values);
    std::vector<NetMessage> TakeMessages();
    uint32_t SendInput(const NetValues& values);
    std::vector<NetInput> TakeInputs(int player);
    uint32_t InputAck() const { return m_inputAck; }
    std::map<int, NetValues> Rewind(uint32_t tick) const;

private:
    enum class Mode : uint8_t { Offline, Hosting, Joining, Connected, Migrating };

    bool AddDef(std::vector<NetDef>& defs, NetDef def, const char* what);
    void Reset(bool keepLink);
    void Fail(const std::string& reason);
    void Event(NetEvent e) { m_events.push_back(std::move(e)); }
    void WriteHeader(NetBits& w, uint8_t type, NetPeer* unreliablePeer);
    void SendRaw(uint64_t link, bool reliable, const std::string& bytes);
    void FlushDelayed(double now, bool all);

    void OnLink(const NetLinkEvent& e);
    void OnPacket(NetPeer& peer, const std::string& bytes);
    void HostOnPacket(NetPeer& peer, uint8_t type, NetRead& r);
    void ClientOnPacket(NetPeer& peer, uint8_t type, NetRead& r);
    void HostHello(NetPeer& peer, NetRead& r);
    void DropPeer(uint64_t link, const std::string& reason);
    void SendHello(NetPeer& peer);
    void SendRoster();
    void SendSpawn(NetPeer& peer, const NetObj& obj);
    void SendSync(NetPeer& peer);
    void Replicate();
    void ClientSendState();
    void DeliverMessage(NetMessage msg, int to, bool reliable, int sender);
    void StartMigration(const std::string& reason);
    void TryNextCandidate();
    void BecomeHost();
    NetPeer* HostPeer();
    double EstimatedTick() const;
    void PushHistory(NetObj& obj, double tick);
    void PollFound(double now);

    std::vector<NetDef> m_kinds, m_messages;
    NetDef m_input;
    bool m_hasInput = false;

    Mode m_mode = Mode::Offline;
    NetLinkKind m_kindOfLink = NetLinkKind::None;
    NetLink m_link;
    NetDiscovery m_discovery;
    bool m_isHost = false;
    uint32_t m_session = 0;
    uint16_t m_epoch = 0;
    int m_self = 0, m_hostPlayer = 0;
    std::string m_playerName, m_lobbyName;
    std::vector<std::pair<std::string, std::string>> m_lobbyData;
    int m_maxPlayers = 8, m_tickRate = 30, m_port = 0;
    float m_timeout = 10.0f, m_migrationTimeout = 3.0f;
    bool m_public = true;
    uint32_t m_joinCounter = 0;
    std::vector<NetRosterEntry> m_roster;
    bool m_rosterSeen = false;
    std::map<uint64_t, NetPeer> m_peers;
    uint64_t m_hostLink = 0;
    double m_now = 0.0, m_joinDeadline = 0.0, m_rosterAt = 0.0;
    std::vector<NetRosterEntry> m_candidates;
    int m_oldHost = 0;
    bool m_steamWaitingLobby = false;

    std::map<int, NetObj> m_objects;
    int m_nextId = 1;
    uint32_t m_tick = 0;          // host: authoritative; client: last snapshot tick applied
    int m_fixedCount = 0;
    bool m_tickDue = false;
    double m_tickOffset = 0.0, m_lastSnapAt = 0.0;
    bool m_tickSynced = false;
    float m_jitterTicks = 0.0f;
    std::deque<std::pair<uint32_t, std::unordered_map<int, std::vector<uint64_t>>>> m_rewind;

    std::vector<NetMessage> m_inbox;
    std::vector<NetEvent> m_events;
    uint32_t m_inputSeq = 0, m_inputAck = 0;
    std::deque<NetInput> m_unacked;
    bool m_inputsNew = false;
    std::vector<NetInput> m_hostOwnInputs;

    struct Delayed { double at; uint64_t link; bool reliable; std::string bytes; };
    std::vector<Delayed> m_delayed;
    float m_simLatency = 0.0f, m_simJitter = 0.0f, m_simLoss = 0.0f;
    std::mt19937 m_rng;

    int64_t m_bytesIn = 0, m_bytesOut = 0, m_packetsIn = 0, m_packetsOut = 0, m_dropped = 0, m_lastSnapBytes = 0;
    double m_rateAt = 0.0, m_inPerSec = 0.0, m_outPerSec = 0.0;
    int64_t m_rateIn = 0, m_rateOut = 0;
    std::vector<std::string> m_loggedOnce;
    void LogOnce(const std::string& text);
};

} // namespace IYI
