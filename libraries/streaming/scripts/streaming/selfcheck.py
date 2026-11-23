"""Self-check without network or engine: python libraries/streaming/scripts/streaming/selfcheck.py

Provider fixtures are hand-made from the documented payload shapes (ids redacted), not live captures;
replace them with real redacted captures once developer apps exist (docs: README.md).
"""

import base64
import hashlib
import os
import socket
import struct
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import streaming  # noqa: E402
from streaming import events, limits, streamelements, twitch, ws, youtube  # noqa: E402


def check_text():
    assert events.clean_text("a‮b​c\nd\x00e", 50) == "abc de"
    assert events.clean_text("x" + "́" * 9, 50) == "x́́"
    assert events.clean_text("  many   spaces ", 50) == "many spaces"
    assert events.clean_text(5, 50) == "" and len(events.clean_text("y" * 999, 280)) == 280
    for text, want in (("!mail hello CEO", ("mail", "hello CEO")), ("!MAIL  hi", ("mail", "hi")),
                       ("!mail", None), ("!mail   ", None), ("!human", ("human", "")), ("!Human please", ("human", "")),
                       ("hi !mail x", None), ("!mailbox x", None), ("!humans", None)):
        assert events.parse_command(text) == want, (text, events.parse_command(text))
    assert events.has_link("see www.x.com") and events.has_link("bit.ly/a") and not events.has_link("good job")
    assert events.money("50", "try") == (5000, "50.00 TRY") and events.money("x", "USD") == (None, None)
    assert events.event("twitch", "chat_message", "") is None and events.event("bad", "chat_message", "1") is None


def check_limits():
    s = limits.new()
    assert not limits.seen(s, "a") and limits.seen(s, "a")
    m = limits.new()
    assert limits.allow(m, "u1", 0.0, 600.0, 3, 30)
    assert not limits.allow(m, "u1", 10.0, 600.0, 3, 30)        # cooldown
    assert limits.allow(m, "u2", 11.0, 600.0, 3, 30) and limits.allow(m, "u3", 12.0, 600.0, 3, 30)
    assert not limits.allow(m, "u4", 13.0, 600.0, 3, 30)        # 3 per minute
    assert limits.allow(m, "u4", 61.0, 600.0, 3, 30)            # a minute later
    h = limits.new()
    assert limits.allow(h, "v", 0.0, float("inf"), 99, 50) and not limits.allow(h, "v", 9e9, float("inf"), 99, 50)


def check_twitch():
    def chat(badges, broadcaster="111"):
        return {"metadata": {"message_id": "m1", "message_type": "notification"},
                "payload": {"subscription": {"type": "channel.chat.message"}, "event": {
                    "broadcaster_user_id": broadcaster, "chatter_user_id": "222", "chatter_user_name": "Viewer‮",
                    "message_id": "chat-1", "message": {"text": "!mail hello"},
                    "badges": [{"set_id": b, "id": "1", "info": ""} for b in badges]}}}
    ev = twitch.normalize(chat(["subscriber"]), "111", "s")
    assert ev["subscriber"] is True and ev["sender_display_name"] == "Viewer" and ev["provider_event_id"] == "chat-1"
    assert twitch.normalize(chat(["founder"]), "111")["subscriber"] is True
    assert twitch.normalize(chat(["sub_gifter", "vip"]), "111")["subscriber"] is False
    assert twitch.normalize(chat(["subscriber"], broadcaster="999"), "111") is None   # another channel
    gift = {"metadata": {"message_id": "g1"}, "payload": {"subscription": {"type": "channel.subscription.gift"},
            "event": {"broadcaster_user_id": "111", "user_id": None, "user_name": None, "total": 5, "is_anonymous": True}}}
    ev = twitch.normalize(gift, "111")
    assert ev["type"] == "gift_purchase" and ev["gift_count"] == 5 and ev["sender_id"] is None and ev["sender_display_name"] == ""


def check_youtube():
    def item(kind, sponsor, **snippet):
        return {"id": "yt-" + kind, "snippet": dict(type=kind, **snippet),
                "authorDetails": {"channelId": "UCv", "displayName": "Member", **({} if sponsor is None else {"isChatSponsor": sponsor})}}
    text = {"textMessageDetails": {"messageText": "!human"}}
    assert youtube.normalize(item("textMessageEvent", True, **text), "UCc")["subscriber"] is True
    assert youtube.normalize(item("textMessageEvent", False, **text), "UCc")["subscriber"] is False
    assert youtube.normalize(item("textMessageEvent", None, **text), "UCc")["subscriber"] is None   # fails closed
    gift = youtube.normalize(item("membershipGiftingEvent", True, membershipGiftingDetails={"giftMembershipsCount": 5}), "UCc")
    assert gift["type"] == "gift_purchase" and gift["gift_count"] == 5
    assert youtube.normalize(item("giftMembershipReceivedEvent", False), "UCc") is None
    assert youtube.normalize(item("superChatEvent", True), "UCc") is None
    gone = youtube.normalize(item("messageDeletedEvent", None, messageDeletedDetails={"deletedMessageId": "yt-x"}), "UCc")
    assert gone["type"] == "message_deleted" and gone["target_id"] == "yt-x"


def check_streamelements():
    def tip(**over):
        data = {"_id": "tip1", "channel": "c1", "status": "success", "approved": "allowed", "deleted": False,
                "donation": {"user": {"username": "Donor", "email": "never@read"}, "message": "for the CEO",
                             "amount": 50, "currency": "TRY"}}
        data.update(over)
        return {"type": "message", "topic": "channel.tips", "room": "c1", "data": data}
    ev = streamelements.normalize(tip())
    assert ev["amount_minor"] == 5000 and ev["display_amount"] == "50.00 TRY" and ev["currency"] == "TRY"
    assert "never@read" not in repr(ev)
    for over in ({"status": "pending"}, {"approved": "pending"}, {"deleted": True}, {"status": None}):
        assert streamelements.normalize(tip(**over)) is None, over


def check_websocket():
    """The client against a tiny local server: handshake, a masked send, a ping, a 70 KB message."""
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)

    def serve():
        conn, _ = server.accept()
        head = b""
        while b"\r\n\r\n" not in head:
            head += conn.recv(4096)
        key = [l.split(b":", 1)[1].strip() for l in head.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")][0]
        accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        b0, b1 = conn.recv(2)
        mask, n = conn.recv(4), b1 & 0x7F
        got = bytes(b ^ mask[i % 4] for i, b in enumerate(conn.recv(n)))
        conn.sendall(bytes([0x89, 0]))                       # ping: the client must answer and go on
        big = got * 7000
        conn.sendall(bytes([0x81, 127]) + struct.pack("!Q", len(big)) + big)
        conn.recv(16)
        conn.close()

    threading.Thread(target=serve, daemon=True).start()
    sock = ws.connect(f"ws://127.0.0.1:{server.getsockname()[1]}/", threading.Event())
    ws.send(sock, "ping!!!!!!")
    assert ws.recv(sock) == "ping!!!!!!" * 7000
    ws.close(sock)
    server.close()


def check_session():
    s = streaming.new_session("SelfCheck")
    streaming.start(s, "twitch")
    assert streaming.status(s, "twitch")["status"] == "not_configured"
    for i in range(streaming.MAX_QUEUE + 5):
        streaming.inject(s, events.event("twitch", "chat_message", f"c{i}", synthetic=True))
    streaming.inject(s, events.event("streamelements", "donation", "d1", amount_minor=100, currency="EUR"))
    assert s["dropped"] == 5 and len(s["queue"]) == streaming.MAX_QUEUE + 1   # the paid event got in
    assert len(streaming.poll(s, 32)) == 32
    streaming.stop(s)
    assert streaming.status(s, "twitch")["status"] == "disconnected"


if __name__ == "__main__":
    for check in (check_text, check_limits, check_twitch, check_youtube, check_streamelements, check_websocket, check_session):
        check()
    print("streaming selfcheck: ok")
