// Implements the Steam link of libraries/net (SPEC §25.4): Steam Networking Messages on channel 0 for
// packets, Steam lobbies for hosting, finding and invites. Without IYI_STEAM every call is a stub.
// Steam callbacks run inside SteamRunFrame on the game thread, before NetFrameBegin polls here.
//
// Connected to:
// - Declared in NetLink.h; NetLink.cpp routes Steam links here; NetSession.cpp calls the lobby functions.

#include "net/src/NetLink.h"

#include "IYI/SteamBridge.h"

#ifdef IYI_STEAM
#include "steam_api.h"
#include "isteamnetworkingmessages.h"

#include <cstdio>
#include <deque>
#include <memory>
#endif

namespace IYI {

#ifdef IYI_STEAM
namespace {

std::string HashHex(uint32_t hash)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", hash);
    return text;
}

struct SteamNet {
    std::deque<NetLinkEvent> links;
    std::vector<NetSteamEvent> lobbies;
    uint64_t lobby = 0;
    std::vector<std::pair<std::string, std::string>> pendingData;   // set before the lobby exists
    CCallResult<SteamNet, LobbyCreated_t> created;
    CCallResult<SteamNet, LobbyEnter_t> entered;
    CCallResult<SteamNet, LobbyMatchList_t> listed;

    STEAM_CALLBACK(SteamNet, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
    STEAM_CALLBACK(SteamNet, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);
    STEAM_CALLBACK(SteamNet, OnJoinRequested, GameLobbyJoinRequested_t);
    STEAM_CALLBACK(SteamNet, OnLobbyUpdate, LobbyChatUpdate_t);

    void OnCreated(LobbyCreated_t* r, bool ioFailure)
    {
        NetSteamEvent e;
        if (ioFailure || r->m_eResult != k_EResultOK) {
            e.type = NetSteamEvent::Failed;
            e.reason = "Steam could not create the lobby";
        } else {
            lobby = r->m_ulSteamIDLobby;
            for (const auto& [key, value] : pendingData) {
                SteamMatchmaking()->SetLobbyData(CSteamID(lobby), key.c_str(), value.c_str());
            }
            pendingData.clear();
            e.type = NetSteamEvent::Created;
            e.lobby = lobby;
        }
        lobbies.push_back(std::move(e));
    }

    void OnEntered(LobbyEnter_t* r, bool ioFailure)
    {
        NetSteamEvent e;
        if (ioFailure || r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
            e.type = NetSteamEvent::Failed;
            e.reason = "Steam could not join the lobby";
        } else {
            lobby = r->m_ulSteamIDLobby;
            e.type = NetSteamEvent::Entered;
            e.lobby = lobby;
            e.owner = SteamMatchmaking()->GetLobbyOwner(CSteamID(lobby)).ConvertToUint64();
        }
        lobbies.push_back(std::move(e));
    }

    void OnList(LobbyMatchList_t* r, bool ioFailure)
    {
        if (ioFailure) return;
        ISteamMatchmaking* mm = SteamMatchmaking();
        for (uint32 i = 0; i < r->m_nLobbiesMatching; ++i) {
            const CSteamID id = mm->GetLobbyByIndex(static_cast<int>(i));
            NetSteamEvent e;
            e.type = NetSteamEvent::Found;
            e.lobby = id.ConvertToUint64();
            e.name = mm->GetLobbyData(id, "name");
            e.players = mm->GetNumLobbyMembers(id);
            e.maxPlayers = mm->GetLobbyMemberLimit(id);
            const int count = mm->GetLobbyDataCount(id);
            for (int k = 0; k < count; ++k) {
                char key[256] = {}, value[1024] = {};
                if (mm->GetLobbyDataByIndex(id, k, key, sizeof(key), value, sizeof(value))) {
                    e.data.emplace_back(key, value);
                }
            }
            lobbies.push_back(std::move(e));
        }
    }
};

void SteamNet::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* r)
{
    SteamNetworkingMessages()->AcceptSessionWithUser(r->m_identityRemote);   // the session layer checks HELLO
}

void SteamNet::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t* r)
{
    links.push_back({ NetLinkEvent::Disconnected, r->m_info.m_identityRemote.GetSteamID64(), {} });
}

void SteamNet::OnJoinRequested(GameLobbyJoinRequested_t* r)
{
    NetSteamEvent e;
    e.type = NetSteamEvent::Invited;
    e.lobby = r->m_steamIDLobby.ConvertToUint64();
    lobbies.push_back(std::move(e));
}

void SteamNet::OnLobbyUpdate(LobbyChatUpdate_t* r)
{
    if (r->m_ulSteamIDLobby != lobby) return;
    NetSteamEvent e;
    e.type = NetSteamEvent::OwnerChanged;
    e.lobby = lobby;
    e.owner = SteamMatchmaking()->GetLobbyOwner(CSteamID(lobby)).ConvertToUint64();
    lobbies.push_back(std::move(e));
}

std::unique_ptr<SteamNet> s_steam;   // made by SteamNetStart (NetStartup), freed by SteamNetStop

SteamNetworkingIdentity IdentityOf(uint64_t steamId)
{
    SteamNetworkingIdentity id;
    id.SetSteamID64(steamId);
    return id;
}

} // namespace

bool SteamNetStart()
{
    if (!s_steam && Api::SteamAvailable() && SteamNetworkingMessages() && SteamMatchmaking()) {
        s_steam = std::make_unique<SteamNet>();
    }
    return s_steam != nullptr;
}

void SteamNetStop()
{
    SteamLobbyLeave();
    s_steam.reset();
}

uint64_t SteamNetLocalId() { return s_steam ? SteamUser()->GetSteamID().ConvertToUint64() : 0; }
std::string SteamNetPersonaName() { return s_steam ? SteamFriends()->GetPersonaName() : ""; }

void SteamNetConnect(uint64_t steamId)
{
    if (s_steam) s_steam->links.push_back({ NetLinkEvent::Connected, steamId, {} });
}

bool SteamNetSend(uint64_t steamId, bool reliable, const std::string& bytes)
{
    if (!s_steam) return false;
    const int flags = (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable) |
                      k_nSteamNetworkingSend_AutoRestartBrokenSession;
    const SteamNetworkingIdentity id = IdentityOf(steamId);
    return SteamNetworkingMessages()->SendMessageToUser(id, bytes.data(), static_cast<uint32>(bytes.size()), flags, 0) ==
           k_EResultOK;
}

void SteamNetPoll(std::vector<NetLinkEvent>& out)
{
    if (!s_steam) return;
    while (!s_steam->links.empty()) {
        out.push_back(std::move(s_steam->links.front()));
        s_steam->links.pop_front();
    }
    SteamNetworkingMessage_t* messages[64];
    int n;
    while ((n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, messages, 64)) > 0) {
        for (int i = 0; i < n; ++i) {
            out.push_back({ NetLinkEvent::Data, messages[i]->m_identityPeer.GetSteamID64(),
                            std::string(static_cast<const char*>(messages[i]->m_pData), static_cast<size_t>(messages[i]->m_cbSize)) });
            messages[i]->Release();
        }
    }
}

void SteamNetClose(uint64_t steamId)
{
    if (s_steam) SteamNetworkingMessages()->CloseSessionWithUser(IdentityOf(steamId));
}

void SteamLobbyCreate(bool isPublic, int maxPlayers)
{
    if (!s_steam) return;
    const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(isPublic ? k_ELobbyTypePublic : k_ELobbyTypeFriendsOnly, maxPlayers);
    s_steam->created.Set(call, s_steam.get(), &SteamNet::OnCreated);
}

void SteamLobbyJoin(uint64_t lobby)
{
    if (!s_steam) return;
    s_steam->entered.Set(SteamMatchmaking()->JoinLobby(CSteamID(lobby)), s_steam.get(), &SteamNet::OnEntered);
}

void SteamLobbyLeave()
{
    if (!s_steam || s_steam->lobby == 0) return;
    SteamMatchmaking()->LeaveLobby(CSteamID(s_steam->lobby));
    s_steam->lobby = 0;
}

void SteamLobbySetData(const std::string& key, const std::string& value)
{
    if (!s_steam) return;
    if (s_steam->lobby == 0) s_steam->pendingData.emplace_back(key, value);
    else SteamMatchmaking()->SetLobbyData(CSteamID(s_steam->lobby), key.c_str(), value.c_str());
}

void SteamLobbyFind(uint32_t hash)
{
    if (!s_steam) return;
    ISteamMatchmaking* mm = SteamMatchmaking();
    mm->AddRequestLobbyListStringFilter("iyinet", HashHex(hash).c_str(), k_ELobbyComparisonEqual);
    mm->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
    s_steam->listed.Set(mm->RequestLobbyList(), s_steam.get(), &SteamNet::OnList);
}

uint64_t SteamLobbyOwner()
{
    if (!s_steam || s_steam->lobby == 0) return 0;
    return SteamMatchmaking()->GetLobbyOwner(CSteamID(s_steam->lobby)).ConvertToUint64();
}

void SteamLobbyPoll(std::vector<NetSteamEvent>& out)
{
    if (!s_steam) return;
    for (auto& e : s_steam->lobbies) out.push_back(std::move(e));
    s_steam->lobbies.clear();
}

#else   // no Steamworks in this build

bool SteamNetStart() { return false; }
void SteamNetStop() {}
uint64_t SteamNetLocalId() { return 0; }
std::string SteamNetPersonaName() { return {}; }
void SteamNetConnect(uint64_t) {}
bool SteamNetSend(uint64_t, bool, const std::string&) { return false; }
void SteamNetPoll(std::vector<NetLinkEvent>&) {}
void SteamNetClose(uint64_t) {}
void SteamLobbyCreate(bool, int) {}
void SteamLobbyJoin(uint64_t) {}
void SteamLobbyLeave() {}
void SteamLobbySetData(const std::string&, const std::string&) {}
void SteamLobbyFind(uint32_t) {}
uint64_t SteamLobbyOwner() { return 0; }
void SteamLobbyPoll(std::vector<NetSteamEvent>&) {}

#endif

} // namespace IYI
