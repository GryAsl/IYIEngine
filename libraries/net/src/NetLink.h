// Declares the network links of libraries/net (SPEC §25.4): ENet (UDP), loopback (sessions inside one
// process) and Steam Networking Messages behind one plain struct, plus ENet LAN discovery and the Steam
// lobby calls. No raylib, ENet or Steam header here: NetLink.cpp includes ENet (and so windows.h), which
// must never meet raylib.h in one translation unit.
//
// Connected to:
// - NetLink.cpp (ENet, loopback, discovery) and NetSteam.cpp (Steam); used only by NetSession.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace IYI {

enum class NetLinkKind : uint8_t { None, Loopback, Enet, Steam };

struct NetLinkEvent {
    enum Type : uint8_t { Connected, Disconnected, Data };
    Type type = Data;
    uint64_t peer = 0;   // ENet: address and port packed; Steam: the Steam id; loopback: the endpoint id
    std::string bytes;
};

struct NetLink {
    NetLinkKind kind = NetLinkKind::None;
    void* enet = nullptr;   // ENetHost*
    uint64_t self = 0;      // loopback: this endpoint's id; Steam: the local Steam id
    int port = 0;           // ENet: the bound UDP port
};

// Opens a link that both connects out and accepts peers. port 0: any free port (ENet), a new id (loopback).
// localOnly binds ENet to 127.0.0.1 (agent runs, §25.4).
bool LinkOpen(NetLink& link, NetLinkKind kind, int port, int maxPeers, bool localOnly);
bool LinkConnect(NetLink& link, const std::string& address, uint64_t& peer);   // false: bad address
void LinkSend(NetLink& link, uint64_t peer, bool reliable, const std::string& bytes);
void LinkPoll(NetLink& link, std::vector<NetLinkEvent>& out);
void LinkDisconnect(NetLink& link, uint64_t peer);
void LinkFlush(NetLink& link);
void LinkClose(NetLink& link);
std::string LinkPeerAddress(const NetLink& link, uint64_t peer);   // what another peer connects to

bool NetEnetStartup();   // enet_initialize once; false: no sockets
void NetEnetShutdown();

// ENet LAN discovery (UDP broadcast on kNetDiscoveryPort). A host answers queries carrying its schema
// hash with `reply`; a finder collects (address "ip:port", reply) pairs for two seconds.
inline constexpr int kNetDiscoveryPort = 47777;
struct NetDiscovery {
    intptr_t answer = -1, ask = -1;
    uint32_t hash = 0;
    int gamePort = 0;
    bool localOnly = false;
    std::string reply;
    double askUntil = 0.0;
};
void DiscoveryAnswer(NetDiscovery& d, uint32_t hash, int gamePort, bool localOnly);
void DiscoveryAsk(NetDiscovery& d, uint32_t hash, bool localOnly, double now);
void DiscoveryPoll(NetDiscovery& d, double now, std::vector<std::pair<std::string, std::string>>& found);
void DiscoveryStop(NetDiscovery& d);

// Steam (NetSteam.cpp). Without IYI_STEAM or a running Steam every call is false / 0 / empty.
struct NetSteamEvent {
    enum Type : uint8_t { Created, Entered, Failed, Found, Invited, OwnerChanged };
    Type type = Failed;
    uint64_t lobby = 0, owner = 0;
    std::string name, reason;
    int players = 0, maxPlayers = 0;
    std::vector<std::pair<std::string, std::string>> data;
};
bool SteamNetStart();   // callbacks for sessions, lobbies and invites; false: Steam is not running
void SteamNetStop();
uint64_t SteamNetLocalId();
std::string SteamNetPersonaName();
void SteamNetConnect(uint64_t steamId);   // queues the Connected event: Steam sessions open on first send
bool SteamNetSend(uint64_t steamId, bool reliable, const std::string& bytes);
void SteamNetPoll(std::vector<NetLinkEvent>& out);
void SteamNetClose(uint64_t steamId);
void SteamLobbyCreate(bool isPublic, int maxPlayers);
void SteamLobbyJoin(uint64_t lobby);
void SteamLobbyLeave();
void SteamLobbySetData(const std::string& key, const std::string& value);
void SteamLobbyFind(uint32_t hash);
uint64_t SteamLobbyOwner();
void SteamLobbyPoll(std::vector<NetSteamEvent>& out);

} // namespace IYI
