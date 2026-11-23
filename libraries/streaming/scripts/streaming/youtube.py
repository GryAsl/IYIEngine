"""YouTube Live: the broadcaster's active broadcast's chat through liveChatMessages.list, polled at the
interval the server asks for (documented fallback to streamList; simpler and quota-visible).

Docs: developers.google.com/youtube/v3/live/docs/liveChatMessages, .../liveBroadcasts,
developers.google.com/identity/protocols/oauth2/native-app.
"""

import urllib.parse

from streaming import auth, events

SCOPE = "https://www.googleapis.com/auth/youtube.readonly"
API = "https://www.googleapis.com/youtube/v3"
TOKEN_URL = "https://oauth2.googleapis.com/token"
MIN_POLL, NO_LIVE_WAIT, QUOTA_WAIT = 2.0, 60.0, 900.0  # seconds


def normalize(item: dict, channel_id: str, session_id: str = ""):
    """One liveChatMessage resource -> a NormalizedEvent or None. Paid channel membership
    (`isChatSponsor`) is the subscriber evidence; free YouTube subscribers do not count."""
    snippet, author = item.get("snippet") or {}, item.get("authorDetails") or {}
    kind = snippet.get("type")
    sponsor = author.get("isChatSponsor")
    common = dict(channel_id=channel_id, session_id=session_id, sender_id=author.get("channelId"),
                  sender_name=author.get("displayName") or "")
    if kind == "textMessageEvent":
        return events.event("youtube", "chat_message", item.get("id"),
                            message=(snippet.get("textMessageDetails") or {}).get("messageText", ""),
                            subscriber=sponsor if isinstance(sponsor, bool) else None,
                            evidence="author_is_chat_sponsor", **common)
    if kind == "membershipGiftingEvent":  # the purchase; giftMembershipReceivedEvent is per recipient
        return events.event("youtube", "gift_purchase", item.get("id"),
                            gift_count=(snippet.get("membershipGiftingDetails") or {}).get("giftMembershipsCount"),
                            **common)
    if kind == "messageDeletedEvent":
        return events.event("youtube", "message_deleted", item.get("id"), channel_id=channel_id, session_id=session_id,
                            target_id=(snippet.get("messageDeletedDetails") or {}).get("deletedMessageId"))
    if kind == "userBannedEvent":
        banned = (snippet.get("userBannedDetails") or {}).get("bannedUserDetails") or {}
        return events.event("youtube", "user_banned", item.get("id"), channel_id=channel_id, session_id=session_id,
                            sender_id=banned.get("channelId"))
    return None


def run(w: dict, cfg: dict, put) -> None:
    client_id, secret, stop = cfg["client_id"], cfg.get("client_secret", ""), w["stop"]

    def login():
        w["status"] = "authorizing"
        return auth.google_loopback(client_id, secret, SCOPE, stop, lambda detail: w.update(detail=detail))

    def get(path, **params):
        url = f"{API}/{path}?{urllib.parse.urlencode(params)}"
        for force in (False, True):  # an expired access token: refresh once and ask again
            tok = auth.token(cfg["vault_key"], client_id, TOKEN_URL, login, secret, force_refresh=force)
            status, body = auth.request("GET", url, headers={"Authorization": f"Bearer {tok['access_token']}"})
            if status != 401:
                break
        reasons = {e.get("reason") for e in (body.get("error") or {}).get("errors") or [] if isinstance(e, dict)}
        return status, body, reasons

    while True:
        status, body, reasons = get("liveBroadcasts", part="snippet", broadcastStatus="active", broadcastType="all")
        if status == 403 and reasons & {"quotaExceeded", "rateLimitExceeded"}:
            w["status"] = "quota"
            auth.stoppable_sleep(stop, QUOTA_WAIT)
            continue
        if status in (401, 403):
            raise PermissionError(f"youtube broadcasts refused ({status})")
        live = [b for b in body.get("items") or [] if (b.get("snippet") or {}).get("liveChatId")]
        if status != 200 or not live:
            w["status"] = "no_live" if status == 200 else "reconnecting"
            auth.stoppable_sleep(stop, NO_LIVE_WAIT)
            continue
        chat_id, channel_id = live[0]["snippet"]["liveChatId"], live[0]["snippet"].get("channelId", "")
        w.update(status="connected", channel=live[0]["snippet"].get("title", "")[:40])
        page, first = None, True
        while True:
            params = dict(liveChatId=chat_id, part="id,snippet,authorDetails", maxResults=200)
            if page:
                params["pageToken"] = page
            status, body, reasons = get("liveChat/messages", **params)
            if status == 403 and reasons & {"quotaExceeded", "rateLimitExceeded"}:
                w["status"] = "quota"
                auth.stoppable_sleep(stop, QUOTA_WAIT)
                continue
            if status != 200:  # chat ended, disabled or the broadcast went offline: look again
                break
            w["status"] = "connected"
            if not first:  # the first page is the backlog from before connecting: skipped
                for item in body.get("items") or []:
                    ev = normalize(item, channel_id, cfg["session_id"])
                    if ev:
                        put(ev)
            first, page = False, body.get("nextPageToken")
            auth.stoppable_sleep(stop, max(MIN_POLL, float(body.get("pollingIntervalMillis") or 5000) / 1000.0))
        w["status"] = "reconnecting"
        auth.stoppable_sleep(stop, 10.0)
