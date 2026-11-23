"""The normalized event every connector produces, chat command parsing and viewer text cleaning.

Pure functions, no engine or network: a game checks everything here without a live provider.
"""

import re
import time
import unicodedata

SOURCES = ("twitch", "youtube", "kick", "streamelements", "streamlabs")
TYPES = ("chat_message", "donation", "gift_purchase", "message_deleted", "user_banned")
PAID = ("donation", "gift_purchase")  # never dropped because a queue or chat limit is full
MAX_TEXT, MAX_NAME, MAX_ID, MAX_AMOUNT = 500, 64, 128, 32  # raw bounds before a game's own limits

_COMMAND = re.compile(r"^!(mail|human)(?:\s+(.*))?$", re.IGNORECASE | re.DOTALL)
_LINK = re.compile(r"(https?://|www\.|\b[\w-]+\.(com|net|org|tv|gg|io|ly|me|co|xyz|ru|tr|link|app)\b)",
                   re.IGNORECASE)
_BAD_CATEGORIES = ("Cc", "Cf", "Co", "Cs", "Cn")  # control, bidi/zero-width format, private, surrogate, unassigned


def clean_text(value, limit: int) -> str:
    """Plain one-line text: NFC, no control/format/bidi/zero-width characters, at most two combining
    marks in a row, whitespace collapsed, cut to `limit` characters. Anything not a str gives ""."""
    if not isinstance(value, str):
        return ""
    out, marks = [], 0
    for ch in unicodedata.normalize("NFC", value[: limit * 4]):
        cat = unicodedata.category(ch)
        if ch in "\r\n\t":
            ch, cat = " ", "Zs"
        if cat in _BAD_CATEGORIES:
            continue
        marks = marks + 1 if cat.startswith("M") else 0
        if marks <= 2:
            out.append(ch)
    return " ".join("".join(out).split())[:limit]


def has_link(text: str) -> bool:
    return bool(_LINK.search(text))


def parse_command(text: str):
    """("mail", body) for "!mail <body>", ("human", "") for "!human", else None. Case-insensitive,
    only at the start of the message; an empty !mail is no command."""
    m = _COMMAND.match(text.strip()) if isinstance(text, str) else None
    if not m:
        return None
    cmd, arg = m.group(1).lower(), clean_text(m.group(2) or "", MAX_TEXT)
    if cmd == "mail":
        return ("mail", arg) if arg else None
    return ("human", "")


def event(source: str, type: str, provider_event_id, *, channel_id="", sender_id=None, sender_name="",
          message="", subscriber=None, evidence="none", amount_minor=None, currency=None,
          display_amount=None, gift_count=None, target_id=None, session_id="", synthetic=False):
    """A NormalizedEvent dict (plan §2.3), every field bounded; None when it is not valid.
    `subscriber` stays None when the provider gave no evidence (a game must treat that as no)."""
    event_id = clean_text(provider_event_id if isinstance(provider_event_id, str) else str(provider_event_id or ""), MAX_ID)
    if source not in SOURCES or type not in TYPES or not event_id:
        return None
    code = clean_text(currency, 3).upper() if currency else None
    return {
        "source": source,
        "type": type,
        "provider_channel_id": clean_text(str(channel_id or ""), MAX_ID),
        "provider_event_id": event_id,
        "sender_id": clean_text(str(sender_id), MAX_ID) or None if sender_id is not None else None,
        "sender_display_name": clean_text(sender_name, MAX_NAME),
        "message": clean_text(message, MAX_TEXT),
        "subscriber": subscriber if isinstance(subscriber, bool) else None,
        "subscriber_evidence": evidence if isinstance(subscriber, bool) else "none",
        "amount_minor": amount_minor if isinstance(amount_minor, int) and not isinstance(amount_minor, bool) else None,
        "currency": code if code and code.isalpha() and len(code) == 3 else None,
        "display_amount": clean_text(display_amount, MAX_AMOUNT) or None if display_amount else None,
        "gift_count": gift_count if isinstance(gift_count, int) and 0 < gift_count < 10000 else None,
        "target_id": clean_text(str(target_id), MAX_ID) or None if target_id else None,
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "session_id": session_id,
        "synthetic": bool(synthetic),  # local test events: a game labels them TEST
    }


def money(amount, currency):
    """(amount_minor, "50.00 TRY") from a provider's decimal amount; (None, None) when not a number.
    Two decimals for display; no conversion between currencies."""
    try:
        value = float(amount)
    except (TypeError, ValueError):
        return None, None
    if not (0 < value < 1e9):
        return None, None
    code = clean_text(currency, 3).upper() if isinstance(currency, str) else ""
    return round(value * 100), f"{value:.2f} {code}".strip()
