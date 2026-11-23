// Declares multiplayer for C++ (SPEC §25): the IYI::Api Net functions that engine.net and native games
// both call, the value and field types they use, and the frame hooks Application calls. Built only with
// IYI_NET (libraries/net); no ENet, Steam, Python or JSON header here.
//
// Connected to:
// - Implemented in Net.cpp (one NetSession, NetSession.h), links in NetLink.cpp and NetSteam.cpp.
// - src/NetBindings.cpp exposes it as engine.net; Application calls the frame hooks (GameStep order, §25.3).

#pragma once

#include "raylib.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace IYI {

enum class NetType : uint8_t { Bool, U8, U16, U32, I8, I16, I32, F32, Fixed, Angle, Vec2, Vec3, Text, Bytes, Player, Ref };

// A field's wire type. Fixed/Vec2/Vec3 quantize to `step` inside [min, max]; Angle (degrees) to `bits`;
// Text/Bytes carry up to `maxBytes`.
struct NetFieldType {
    NetType type = NetType::F32;
    double min = 0.0, max = 0.0, step = 0.0;
    int bits = 0;
    int maxBytes = 0;
};
NetFieldType NetSimple(NetType type);   // Bool, U8 ... F32, Player, Ref
NetFieldType NetFixed(double min, double max, double step);
NetFieldType NetAngle(int bits);
NetFieldType NetVec2(double min, double max, double step);
NetFieldType NetVec3(double min, double max, double step);
NetFieldType NetText(int maxBytes);
NetFieldType NetBytes(int maxBytes);

// Integers (and Player/Ref) are int64, F32/Fixed/Angle double, Text and Bytes std::string.
using NetValue = std::variant<bool, int64_t, double, Vector2, Vector3, std::string>;
using NetValues = std::vector<std::pair<std::string, NetValue>>;
struct NetField { std::string name; NetFieldType type; };

inline constexpr int kNetHost = 0;     // net.HOST: whoever hosts now
inline constexpr int kNetAll = -1;     // net.ALL: everyone, the sender too
inline constexpr int kNetOthers = -2;  // net.OTHERS: everyone but the sender

struct NetHostOptions {
    std::string transport = "enet";   // "enet", "steam" or "loopback"
    int port = 7777;                  // enet: UDP port; loopback: the endpoint id
    int maxPlayers = 8;               // up to 16
    int tickRate = 30;                // 60, 30, 20, 15 or 10
    std::string name;                 // lobby name shown by find()
    std::string playerName;
    bool isPublic = true;
    std::vector<std::pair<std::string, std::string>> data;   // lobby data shown by find()
    float timeout = 10.0f;            // seconds without a packet before a peer is dropped
    float migrationTimeout = 3.0f;    // seconds without the host before clients migrate
};

struct NetEvent {
    std::string type;   // joined, left, connected, found, invited, host_changed, became_host, disconnected
    int player = 0;
    std::string name, target, reason;
    int players = 0, maxPlayers = 0;
    std::vector<std::pair<std::string, std::string>> data;
};
struct NetPlayer { int id = 0; std::string name; float rttMs = 0.0f; float loss = 0.0f; bool isHost = false; };
struct NetMessage { std::string name; int from = 0; uint32_t tick = 0; NetValues values; };
struct NetInput { uint32_t seq = 0; uint32_t tick = 0; NetValues values; };
struct NetStats {
    double bytesInPerSec = 0.0, bytesOutPerSec = 0.0;
    int64_t packetsIn = 0, packetsOut = 0, dropped = 0, lastSnapshotBytes = 0;
    int objects = 0;
};

// Frame hooks (Application, game thread; no-ops until a session starts).
void NetStartup(bool listed);   // after SteamStartup; `listed`: project.json lists libraries/net
bool NetListed();
void NetFrameBegin();           // after SteamRunFrame: receive and apply
void NetFixedStep60();          // after each FixedUpdate60: counts network ticks
void NetFrameEnd();             // after Update: replicate, flush messages and inputs
void NetShutdown();             // before SteamShutdown

namespace Api {
// Session (§25.5). Host/Join return false (and log) when they cannot start.
bool NetHost(const NetHostOptions& options);
bool NetJoin(const std::string& target, const std::string& playerName = "");   // "ip:port", "steam:<lobby>", "loop:<id>"
void NetFind(const std::string& transport);   // results arrive as "found" events
void NetLeave();
void NetSimulate(float latencyMs, float jitterMs, float loss);   // outgoing, development builds only
std::string NetState();   // offline, hosting, joining, connected, migrating
bool NetIsHost();
int NetLocalPlayer();
std::vector<NetPlayer> NetPlayers();
uint32_t NetTick();
NetStats NetGetStats();
void NetSetLobbyData(const std::string& key, const std::string& value);
std::vector<NetEvent> NetEvents();

// Replicated objects (§25.6). Definitions are the same on every peer, before host/join.
bool NetDefine(const std::string& kind, const std::vector<NetField>& fields);
std::optional<NetFieldType> NetKindField(const std::string& kind, const std::string& field);
int NetSpawn(const std::string& kind, int owner, bool ownerAuthority, float priority, const NetValues& values);   // 0: failed
std::vector<int> NetObjects(const std::string& kind = "");
bool NetExists(int id);
std::string NetObjectKind(int id);
int NetObjectOwner(int id);
bool NetObjectOwnerAuthority(int id);
bool NetCanWrite(int id);
bool NetSet(int id, const std::string& field, const NetValue& value);
std::optional<NetValue> NetGet(int id, const std::string& field);
std::optional<NetValue> NetSample(int id, const std::string& field);   // interpolated (§25.7)
void NetSetPriority(int id, float priority);
void NetSetVisible(int id, int player, bool visible);
bool NetDespawn(int id);

// Messages (RPC) and inputs.
bool NetDefineMessage(const std::string& name, const std::vector<NetField>& fields);
std::optional<NetFieldType> NetMessageField(const std::string& name, const std::string& field);
bool NetSend(const std::string& name, int to, bool reliable, const NetValues& values);
std::vector<NetMessage> NetReceive();
bool NetDefineInput(const std::vector<NetField>& fields);
std::optional<NetFieldType> NetInputField(const std::string& field);
uint32_t NetSendInput(const NetValues& values);   // the input's seq; 0: not a connected client
std::vector<NetInput> NetInputs(int player);      // host: inputs since the last call, in seq order
uint32_t NetInputAck();                           // client: the last seq the host has taken
std::map<int, NetValues> NetRewind(uint32_t tick);   // host: replicated state at that tick (1 s kept)
} // namespace Api

} // namespace IYI
