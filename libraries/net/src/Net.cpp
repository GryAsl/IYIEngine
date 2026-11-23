// Implements the IYI::Api Net functions (SPEC §25) on the game's one NetSession, the field-type helpers
// and the frame hooks Application calls. Nothing runs until project.json lists libraries/net.
//
// Connected to:
// - Declared in Net.h; NetSession.cpp does the work; src/NetBindings.cpp and native games call it.

#include "net/src/Net.h"

#include "net/src/NetSession.h"
#include "IYI/Api.h"
#include "IYI/Log.h"

#include <chrono>

namespace IYI {

namespace {
NetSession s_session;   // offline until host or join; its definitions persist across sessions
bool s_listed = false;

double Now()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool Ready(const char* call)
{
    if (s_listed) {
        s_session.localOnly = Api::IsAgentRun();
        return true;
    }
    LogWrite(LogLevel::Error, "net", std::string(call) + ": list ../IYIEngine/libraries/net in project.json \"libraries\"");
    return false;
}
} // namespace

NetFieldType NetSimple(NetType type) { NetFieldType f; f.type = type; return f; }
NetFieldType NetFixed(double min, double max, double step) { NetFieldType f; f.type = NetType::Fixed; f.min = min; f.max = max; f.step = step; return f; }
NetFieldType NetAngle(int bits) { NetFieldType f; f.type = NetType::Angle; f.bits = bits; return f; }
NetFieldType NetVec2(double min, double max, double step) { NetFieldType f = NetFixed(min, max, step); f.type = NetType::Vec2; return f; }
NetFieldType NetVec3(double min, double max, double step) { NetFieldType f = NetFixed(min, max, step); f.type = NetType::Vec3; return f; }
NetFieldType NetText(int maxBytes) { NetFieldType f; f.type = NetType::Text; f.maxBytes = maxBytes; return f; }
NetFieldType NetBytes(int maxBytes) { NetFieldType f; f.type = NetType::Bytes; f.maxBytes = maxBytes; return f; }

void NetStartup(bool listed)
{
    s_listed = listed;
    if (!listed) return;
    if (!NetEnetStartup()) LogWrite(LogLevel::Warn, "net", "ENet could not start: no UDP play");
    SteamNetStart();   // false without Steam; invites then never arrive
}

bool NetListed() { return s_listed; }

void NetFrameBegin()
{
    if (s_listed) s_session.Receive(Now());
}

void NetFixedStep60()
{
    if (s_listed) s_session.FixedStep60();
}

void NetFrameEnd()
{
    if (s_listed) s_session.Send(Now());
}

void NetShutdown()
{
    if (!s_listed) return;
    s_session.Leave();
    SteamNetStop();
    NetEnetShutdown();
    s_listed = false;
}

namespace Api {

bool NetHost(const NetHostOptions& options) { return Ready("net.host") && s_session.Host(options, Now()); }
bool NetJoin(const std::string& target, const std::string& playerName) { return Ready("net.join") && s_session.Join(target, playerName, Now()); }
void NetFind(const std::string& transport) { if (Ready("net.find")) s_session.Find(transport, Now()); }
void NetLeave() { s_session.Leave(); }
void NetSimulate(float latencyMs, float jitterMs, float loss) { s_session.Simulate(latencyMs, jitterMs, loss); }
std::string NetState() { return s_session.State(); }
bool NetIsHost() { return s_session.IsHost(); }
int NetLocalPlayer() { return s_session.LocalPlayer(); }
std::vector<NetPlayer> NetPlayers() { return s_session.Players(); }
uint32_t NetTick() { return s_session.Tick(); }
NetStats NetGetStats() { return s_session.Stats(); }
void NetSetLobbyData(const std::string& key, const std::string& value) { s_session.SetLobbyData(key, value); }
std::vector<NetEvent> NetEvents() { return s_session.TakeEvents(); }

bool NetDefine(const std::string& kind, const std::vector<NetField>& fields) { return s_session.Define(kind, fields); }

std::optional<NetFieldType> NetKindField(const std::string& kind, const std::string& field)
{
    if (const NetDef* d = s_session.Kind(kind)) {
        for (const NetField& f : d->fields) if (f.name == field) return f.type;
    }
    return std::nullopt;
}

int NetSpawn(const std::string& kind, int owner, bool ownerAuthority, float priority, const NetValues& values)
{
    return s_session.Spawn(kind, owner, ownerAuthority, priority, values);
}

std::vector<int> NetObjects(const std::string& kind) { return s_session.Objects(kind); }
bool NetExists(int id) { return s_session.Object(id) != nullptr; }

std::string NetObjectKind(int id)
{
    const NetObj* o = s_session.Object(id);
    return o ? s_session.KindName(o->kind) : std::string();
}

int NetObjectOwner(int id) { const NetObj* o = s_session.Object(id); return o ? o->owner : 0; }
bool NetObjectOwnerAuthority(int id) { const NetObj* o = s_session.Object(id); return o && o->ownerAuth; }
bool NetCanWrite(int id) { return s_session.CanWrite(id); }
bool NetSet(int id, const std::string& field, const NetValue& value) { return s_session.Set(id, field, value); }
std::optional<NetValue> NetGet(int id, const std::string& field) { return s_session.Get(id, field); }
std::optional<NetValue> NetSample(int id, const std::string& field) { return s_session.Sample(id, field); }
void NetSetPriority(int id, float priority) { s_session.SetPriority(id, priority); }
void NetSetVisible(int id, int player, bool visible) { s_session.SetVisible(id, player, visible); }
bool NetDespawn(int id) { return s_session.Despawn(id); }

bool NetDefineMessage(const std::string& name, const std::vector<NetField>& fields) { return s_session.DefineMessage(name, fields); }

std::optional<NetFieldType> NetMessageField(const std::string& name, const std::string& field)
{
    if (const NetDef* d = s_session.Message(name)) {
        for (const NetField& f : d->fields) if (f.name == field) return f.type;
    }
    return std::nullopt;
}

bool NetSend(const std::string& name, int to, bool reliable, const NetValues& values) { return s_session.SendMsg(name, to, reliable, values); }
std::vector<NetMessage> NetReceive() { return s_session.TakeMessages(); }
bool NetDefineInput(const std::vector<NetField>& fields) { return s_session.DefineInput(fields); }

std::optional<NetFieldType> NetInputField(const std::string& field)
{
    if (const NetDef* d = s_session.Input()) {
        for (const NetField& f : d->fields) if (f.name == field) return f.type;
    }
    return std::nullopt;
}

uint32_t NetSendInput(const NetValues& values) { return s_session.SendInput(values); }
std::vector<NetInput> NetInputs(int player) { return s_session.TakeInputs(player); }
uint32_t NetInputAck() { return s_session.InputAck(); }
std::map<int, NetValues> NetRewind(uint32_t tick) { return s_session.Rewind(tick); }

} // namespace Api

} // namespace IYI
