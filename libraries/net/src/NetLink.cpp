// Implements the ENet and loopback links and ENet LAN discovery (SPEC §25.4). Steam is NetSteam.cpp.
// This file includes ENet (windows.h on Windows) and must not include raylib.h or Net.h.
//
// Connected to:
// - Declared in NetLink.h; called only by NetSession.cpp.

#include "net/src/NetLink.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <set>
#include <unordered_map>

namespace IYI {

namespace {

// Loopback: endpoints inside this process, each with its event queue and the peers it is linked to.
struct LoopEndpoint {
    std::deque<NetLinkEvent> events;
    std::set<uint64_t> peers;
};
std::unordered_map<uint64_t, LoopEndpoint> s_loop;
uint64_t s_nextLoopId = 1000000;
bool s_enetUp = false;

uint64_t KeyOf(const ENetAddress& a) { return (static_cast<uint64_t>(a.host) << 16) | a.port; }

std::string IpOf(const ENetAddress& a)
{
    char ip[64] = {};
    enet_address_get_host_ip(&a, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(a.port);
}

bool ParseAddress(const std::string& text, ENetAddress& out)
{
    const size_t colon = text.rfind(':');
    const std::string host = colon == std::string::npos ? text : text.substr(0, colon);
    const int port = colon == std::string::npos ? 7777 : std::atoi(text.c_str() + colon + 1);
    if (host.empty() || port <= 0 || port > 65535) return false;
    if (enet_address_set_host(&out, host.c_str()) != 0) return false;
    out.port = static_cast<enet_uint16>(port);
    return true;
}

ENetPeer* FindPeer(NetLink& link, uint64_t key)
{
    auto* host = static_cast<ENetHost*>(link.enet);
    for (size_t i = 0; host && i < host->peerCount; ++i) {
        ENetPeer& p = host->peers[i];
        if (p.state != ENET_PEER_STATE_DISCONNECTED && KeyOf(p.address) == key) return &p;
    }
    return nullptr;
}

} // namespace

bool NetEnetStartup()
{
    if (!s_enetUp) s_enetUp = enet_initialize() == 0;
    return s_enetUp;
}

void NetEnetShutdown()
{
    if (s_enetUp) enet_deinitialize();
    s_enetUp = false;
}

bool LinkOpen(NetLink& link, NetLinkKind kind, int port, int maxPeers, bool localOnly)
{
    LinkClose(link);
    if (kind == NetLinkKind::Loopback) {
        const uint64_t id = port > 0 ? static_cast<uint64_t>(port) : s_nextLoopId++;
        if (s_loop.count(id)) return false;
        s_loop[id];
        link.kind = kind;
        link.self = id;
        return true;
    }
    if (kind == NetLinkKind::Enet) {
        if (!NetEnetStartup()) return false;
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        if (localOnly) enet_address_set_host(&address, "127.0.0.1");
        address.port = static_cast<enet_uint16>(port);
        // Every peer can accept connections: a client becomes the host when the host leaves (§25.8).
        ENetHost* host = enet_host_create(&address, static_cast<size_t>(maxPeers), 2, 0, 0);
        if (!host) return false;
        link.kind = kind;
        link.enet = host;
        link.port = host->address.port;
        return true;
    }
    if (kind == NetLinkKind::Steam) {   // no socket of its own: Steam Networking Messages
        if (!SteamNetStart()) return false;
        link.kind = kind;
        link.self = SteamNetLocalId();
        return link.self != 0;
    }
    return false;
}

bool LinkConnect(NetLink& link, const std::string& address, uint64_t& peer)
{
    if (link.kind == NetLinkKind::Loopback) {
        if (address.rfind("loop:", 0) != 0) return false;
        const uint64_t id = std::strtoull(address.c_str() + 5, nullptr, 10);
        auto it = s_loop.find(id);
        if (it == s_loop.end() || id == link.self) return false;
        it->second.peers.insert(link.self);
        it->second.events.push_back({ NetLinkEvent::Connected, link.self, {} });
        s_loop[link.self].peers.insert(id);
        s_loop[link.self].events.push_back({ NetLinkEvent::Connected, id, {} });
        peer = id;
        return true;
    }
    if (link.kind == NetLinkKind::Enet) {
        ENetAddress a{};
        if (!ParseAddress(address, a)) return false;
        if (!enet_host_connect(static_cast<ENetHost*>(link.enet), &a, 2, 0)) return false;
        peer = KeyOf(a);
        return true;
    }
    if (link.kind == NetLinkKind::Steam) {
        if (address.rfind("steam-user:", 0) != 0) return false;
        peer = std::strtoull(address.c_str() + 11, nullptr, 10);
        if (peer == 0) return false;
        SteamNetConnect(peer);
        return true;
    }
    return false;
}

void LinkSend(NetLink& link, uint64_t peer, bool reliable, const std::string& bytes)
{
    if (link.kind == NetLinkKind::Loopback) {
        auto it = s_loop.find(peer);
        if (it != s_loop.end() && it->second.peers.count(link.self)) {
            it->second.events.push_back({ NetLinkEvent::Data, link.self, bytes });
        }
    } else if (link.kind == NetLinkKind::Enet) {
        ENetPeer* p = FindPeer(link, peer);
        if (!p || p->state != ENET_PEER_STATE_CONNECTED) return;
        // Snapshots stay under one MTU (§25.6), so unreliable packets are never fragmented.
        ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (enet_peer_send(p, reliable ? 0 : 1, packet) < 0) enet_packet_destroy(packet);
    } else if (link.kind == NetLinkKind::Steam) {
        SteamNetSend(peer, reliable, bytes);
    }
}

void LinkPoll(NetLink& link, std::vector<NetLinkEvent>& out)
{
    if (link.kind == NetLinkKind::Loopback) {
        auto it = s_loop.find(link.self);
        if (it == s_loop.end()) return;
        while (!it->second.events.empty()) {
            out.push_back(std::move(it->second.events.front()));
            it->second.events.pop_front();
        }
    } else if (link.kind == NetLinkKind::Enet) {
        auto* host = static_cast<ENetHost*>(link.enet);
        ENetEvent e;
        while (host && enet_host_service(host, &e, 0) > 0) {
            if (e.type == ENET_EVENT_TYPE_CONNECT) {
                enet_peer_timeout(e.peer, 0, 5000, 15000);   // the session's own timeouts act first
                out.push_back({ NetLinkEvent::Connected, KeyOf(e.peer->address), {} });
            } else if (e.type == ENET_EVENT_TYPE_DISCONNECT) {
                out.push_back({ NetLinkEvent::Disconnected, KeyOf(e.peer->address), {} });
            } else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
                out.push_back({ NetLinkEvent::Data, KeyOf(e.peer->address),
                                std::string(reinterpret_cast<const char*>(e.packet->data), e.packet->dataLength) });
                enet_packet_destroy(e.packet);
            }
        }
    } else if (link.kind == NetLinkKind::Steam) {
        SteamNetPoll(out);
    }
}

void LinkDisconnect(NetLink& link, uint64_t peer)
{
    if (link.kind == NetLinkKind::Loopback) {
        auto it = s_loop.find(peer);
        if (it != s_loop.end() && it->second.peers.erase(link.self)) {
            it->second.events.push_back({ NetLinkEvent::Disconnected, link.self, {} });
        }
        s_loop[link.self].peers.erase(peer);
    } else if (link.kind == NetLinkKind::Enet) {
        if (ENetPeer* p = FindPeer(link, peer)) enet_peer_disconnect(p, 0);
    } else if (link.kind == NetLinkKind::Steam) {
        SteamNetClose(peer);
    }
}

void LinkFlush(NetLink& link)
{
    if (link.kind == NetLinkKind::Enet && link.enet) enet_host_flush(static_cast<ENetHost*>(link.enet));
}

void LinkClose(NetLink& link)
{
    if (link.kind == NetLinkKind::Loopback) {
        auto it = s_loop.find(link.self);
        if (it != s_loop.end()) {
            for (uint64_t peer : it->second.peers) {
                auto other = s_loop.find(peer);
                if (other == s_loop.end()) continue;
                other->second.peers.erase(link.self);
                other->second.events.push_back({ NetLinkEvent::Disconnected, link.self, {} });
            }
            s_loop.erase(it);
        }
    } else if (link.kind == NetLinkKind::Enet && link.enet) {
        auto* host = static_cast<ENetHost*>(link.enet);
        for (size_t i = 0; i < host->peerCount; ++i) {
            if (host->peers[i].state == ENET_PEER_STATE_CONNECTED) enet_peer_disconnect_now(&host->peers[i], 0);
        }
        enet_host_flush(host);
        enet_host_destroy(host);
    }
    link = NetLink{};
}

std::string LinkPeerAddress(const NetLink& link, uint64_t peer)
{
    if (link.kind == NetLinkKind::Loopback) return "loop:" + std::to_string(peer);
    if (link.kind == NetLinkKind::Steam) return "steam-user:" + std::to_string(peer);
    ENetAddress a{};
    a.host = static_cast<enet_uint32>(peer >> 16);
    a.port = static_cast<enet_uint16>(peer & 0xFFFF);
    return IpOf(a);
}

// --- LAN discovery ---------------------------------------------------------------------------------

namespace {
constexpr char kAsk[] = "IYINET?";
constexpr char kAnswer[] = "IYINET!";

ENetSocket OpenDatagram(int port, bool broadcast, bool localOnly)
{
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) return s;
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);
    if (broadcast) enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
    ENetAddress a{};
    a.host = ENET_HOST_ANY;
    if (localOnly) enet_address_set_host(&a, "127.0.0.1");
    a.port = static_cast<enet_uint16>(port);
    if (enet_socket_bind(s, &a) < 0) {
        enet_socket_destroy(s);
        return ENET_SOCKET_NULL;
    }
    return s;
}

std::string HashBytes(uint32_t hash)
{
    std::string out(4, '\0');
    for (int i = 0; i < 4; ++i) out[i] = static_cast<char>((hash >> (8 * i)) & 0xFF);
    return out;
}

int Receive(ENetSocket s, ENetAddress& from, char* data, size_t size)
{
    ENetBuffer buffer;
    buffer.data = data;
    buffer.dataLength = size;
    return enet_socket_receive(s, &from, &buffer, 1);
}

void SendTo(ENetSocket s, const ENetAddress& to, const std::string& bytes)
{
    ENetBuffer buffer;
    buffer.data = const_cast<char*>(bytes.data());
    buffer.dataLength = bytes.size();
    enet_socket_send(s, &to, &buffer, 1);
}
} // namespace

void DiscoveryAnswer(NetDiscovery& d, uint32_t hash, int gamePort, bool localOnly)
{
    if (!NetEnetStartup()) return;
    if (d.answer < 0) {
        ENetSocket s = OpenDatagram(kNetDiscoveryPort, false, localOnly);
        d.answer = s == ENET_SOCKET_NULL ? -1 : static_cast<intptr_t>(s);
    }
    d.hash = hash;
    d.gamePort = gamePort;
    d.localOnly = localOnly;
}

void DiscoveryAsk(NetDiscovery& d, uint32_t hash, bool localOnly, double now)
{
    if (!NetEnetStartup()) return;
    if (d.ask < 0) {
        ENetSocket s = OpenDatagram(0, true, localOnly);
        if (s == ENET_SOCKET_NULL) return;
        d.ask = static_cast<intptr_t>(s);
    }
    const std::string query = std::string(kAsk, 7) + HashBytes(hash);
    ENetAddress to{};
    to.port = kNetDiscoveryPort;
    enet_address_set_host(&to, "127.0.0.1");   // a broadcast may not come back to this machine
    SendTo(static_cast<ENetSocket>(d.ask), to, query);
    if (!localOnly) {
        to.host = ENET_HOST_BROADCAST;
        SendTo(static_cast<ENetSocket>(d.ask), to, query);
    }
    d.hash = hash;
    d.askUntil = now + 2.0;
}

void DiscoveryPoll(NetDiscovery& d, double now, std::vector<std::pair<std::string, std::string>>& found)
{
    char data[1400];
    ENetAddress from{};
    if (d.answer >= 0) {
        const std::string mine = std::string(kAsk, 7) + HashBytes(d.hash);
        int n;
        while ((n = Receive(static_cast<ENetSocket>(d.answer), from, data, sizeof(data))) > 0) {
            if (static_cast<size_t>(n) != mine.size() || mine.compare(0, mine.size(), data, n) != 0) continue;
            std::string reply = std::string(kAnswer, 7) + HashBytes(d.hash);
            reply += static_cast<char>(d.gamePort & 0xFF);
            reply += static_cast<char>((d.gamePort >> 8) & 0xFF);
            SendTo(static_cast<ENetSocket>(d.answer), from, reply + d.reply);
        }
    }
    if (d.ask >= 0) {
        const std::string head = std::string(kAnswer, 7) + HashBytes(d.hash);
        int n;
        while ((n = Receive(static_cast<ENetSocket>(d.ask), from, data, sizeof(data))) > 0) {
            if (n < 13 || head.compare(0, 11, data, 11) != 0) continue;
            from.port = static_cast<enet_uint16>(static_cast<uint8_t>(data[11]) | (static_cast<uint8_t>(data[12]) << 8));
            found.emplace_back(IpOf(from), std::string(data + 13, static_cast<size_t>(n - 13)));
        }
        if (now > d.askUntil) {
            enet_socket_destroy(static_cast<ENetSocket>(d.ask));
            d.ask = -1;
        }
    }
}

void DiscoveryStop(NetDiscovery& d)
{
    if (d.answer >= 0) enet_socket_destroy(static_cast<ENetSocket>(d.answer));
    if (d.ask >= 0) enet_socket_destroy(static_cast<ENetSocket>(d.ask));
    d = NetDiscovery{};
}

} // namespace IYI
