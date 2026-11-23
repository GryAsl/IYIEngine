"""Twitch: EventSub over WebSocket (chat messages, message deletions, gift subscriptions).

Docs: dev.twitch.tv/docs/eventsub (WebSocket transport), .../reference/chat, .../reference/channel.
"""

import json
import time

from streaming import auth, events, ws

SCOPES = "user:read:chat channel:read:subscriptions"
EVENTSUB = "wss://eventsub.wss.twitch.tv/ws"
HELIX = "https://api.twitch.tv/helix"
TOKEN_URL = "https://id.twitch.tv/oauth2/token"
SUBSCRIBER_BADGES = ("subscriber", "founder")  # a founder is a subscriber whose badge is "founder"
# (type, version, needs user_id in the condition, required)
SUBSCRIPTIONS = (("channel.chat.message", "1", True, True),
                 ("channel.subscription.gift", "1", False, True),
                 ("channel.chat.message_delete", "1", True, False))


def normalize(message: dict, broadcaster_id: str, session_id: str = ""):
    """One EventSub `notification` message -> a NormalizedEvent or None (other channel or type)."""
    meta, payload = message.get("metadata") or {}, message.get("payload") or {}
    kind = (payload.get("subscription") or {}).get("type")
    e = payload.get("event") or {}
    if str(e.get("broadcaster_user_id", "")) != broadcaster_id:
        return None
    common = dict(channel_id=broadcaster_id, session_id=session_id)
    if kind == "channel.chat.message":
        badges = {b.get("set_id") for b in e.get("badges") or [] if isinstance(b, dict)}
        return events.event("twitch", "chat_message", e.get("message_id"), sender_id=e.get("chatter_user_id"),
                            sender_name=e.get("chatter_user_name") or e.get("chatter_user_login") or "",
                            message=(e.get("message") or {}).get("text", ""),
                            subscriber=bool(badges & set(SUBSCRIBER_BADGES)), evidence="badge", **common)
    if kind == "channel.chat.message_delete":
        return events.event("twitch", "message_deleted", meta.get("message_id"), target_id=e.get("message_id"),
                            sender_id=e.get("target_user_id"), **common)
    if kind == "channel.subscription.gift":
        anonymous = bool(e.get("is_anonymous"))
        return events.event("twitch", "gift_purchase", meta.get("message_id"),
                            sender_id=None if anonymous else e.get("user_id"),
                            sender_name="" if anonymous else (e.get("user_name") or ""),
                            gift_count=e.get("total"), **common)
    return None


def run(w: dict, cfg: dict, put) -> None:
    client_id, stop = cfg["client_id"], w["stop"]
    vault = cfg["vault_key"]

    def login():
        w["status"] = "authorizing"
        return auth.twitch_device(client_id, SCOPES, stop, lambda detail: w.update(detail=detail))

    tok = auth.token(vault, client_id, TOKEN_URL, login)
    status, me = auth.request("GET", "https://id.twitch.tv/oauth2/validate",
                              headers={"Authorization": f"OAuth {tok['access_token']}"})
    if status == 401:
        tok = auth.token(vault, client_id, TOKEN_URL, login, force_refresh=True)
        status, me = auth.request("GET", "https://id.twitch.tv/oauth2/validate",
                                  headers={"Authorization": f"OAuth {tok['access_token']}"})
    if status != 200 or not me.get("user_id"):
        raise PermissionError("twitch token invalid")
    if not set(SCOPES.split()) <= set(me.get("scopes") or []):
        auth.vault_delete(vault)  # authorize again with every scope next time
        raise PermissionError("twitch scopes missing")
    me_id = str(me["user_id"])
    w.update(channel=me.get("login", ""), detail="")
    headers = {"Authorization": f"Bearer {tok['access_token']}", "Client-Id": client_id}
    url, sock = EVENTSUB, None
    try:
        while True:
            new = ws.connect(url, stop)  # connect before closing the old one: the subscriptions move over
            welcome = json.loads(ws.recv(new))
            ws.close(sock)
            sock = new
            session = (welcome.get("payload") or {}).get("session") or {}
            keepalive = float(session.get("keepalive_timeout_seconds") or 10) + 5.0
            if url == EVENTSUB:  # a reconnect URL keeps the existing subscriptions
                for kind, version, with_user, required in SUBSCRIPTIONS:
                    condition = {"broadcaster_user_id": me_id} | ({"user_id": me_id} if with_user else {})
                    code, _ = auth.request("POST", f"{HELIX}/eventsub/subscriptions", headers=headers, body={
                        "type": kind, "version": version, "condition": condition,
                        "transport": {"method": "websocket", "session_id": session.get("id")}})
                    if code not in (200, 202) and required:
                        raise PermissionError(f"twitch subscription {kind} refused ({code})")
            w["status"] = "connected"
            while True:
                sock["deadline"] = time.monotonic() + keepalive
                message = json.loads(ws.recv(sock))
                kind = (message.get("metadata") or {}).get("message_type")
                if kind == "notification":
                    ev = normalize(message, me_id, cfg["session_id"])
                    if ev:
                        put(ev)
                elif kind == "session_reconnect":
                    url = ((message.get("payload") or {}).get("session") or {}).get("reconnect_url") or EVENTSUB
                    break
                elif kind == "revocation":
                    raise PermissionError("twitch subscription revoked")
    finally:
        ws.close(sock)
