"""StreamElements: tips from the Astro WebSocket's `channel.tips` topic.

Docs: docs.streamelements.com/websockets and .../topics/channel-tips. Auth: the streamer's JWT
(StreamElements dashboard), pasted from the clipboard into the OS vault, never shown. OAuth needs an
app StreamElements must approve first (docs/streaming.md, Next).
"""

import json

from streaming import auth, events, ws

ASTRO = "wss://astro.streamelements.com"


def normalize(message: dict, session_id: str = ""):
    """One Astro `message` on channel.tips -> a donation NormalizedEvent, or None. Only a tip the
    provider reports as completed (status "success") and not held or rejected by tip moderation;
    a test alert or anything else is not a donation. Donor e-mail and payment data are never read."""
    if message.get("type") != "message" or message.get("topic") != "channel.tips":
        return None
    tip = message.get("data") or {}
    if tip.get("status") != "success" or tip.get("approved", "allowed") != "allowed" or tip.get("deleted"):
        return None
    donation = tip.get("donation") or {}
    amount_minor, display = events.money(donation.get("amount"), donation.get("currency"))
    if amount_minor is None:
        return None
    return events.event("streamelements", "donation", tip.get("_id") or tip.get("id"),
                        channel_id=message.get("room") or tip.get("channel") or "",
                        sender_name=(donation.get("user") or {}).get("username") or "",
                        message=donation.get("message") or "", amount_minor=amount_minor,
                        currency=donation.get("currency"), display_amount=display, session_id=session_id)


def run(w: dict, cfg: dict, put) -> None:
    jwt = (auth.vault_get(cfg["vault_key"]) or {}).get("jwt")
    if not jwt:
        raise PermissionError("streamelements token missing")
    stop, sock = w["stop"], None
    try:
        sock = ws.connect(ASTRO, stop)
        nonce = auth.new_id()
        ws.send(sock, json.dumps({"type": "subscribe", "nonce": nonce, "data": {
            "topic": "channel.tips", "token": jwt, "token_type": "jwt"}}))
        while True:
            message = json.loads(ws.recv(sock))
            if message.get("type") == "response" and message.get("nonce") == nonce:
                if message.get("error"):
                    raise PermissionError("streamelements subscription refused")
                w.update(status="connected", channel=str((message.get("data") or {}).get("room") or "")[:40])
                continue
            ev = normalize(message, cfg["session_id"])
            if ev:
                put(ev)
    finally:
        ws.close(sock)
