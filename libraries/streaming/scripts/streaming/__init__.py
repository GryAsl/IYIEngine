"""Streaming platform events for a desktop game: Twitch, YouTube Live and StreamElements tips.

A game owns one session dict (`new_session`), starts and stops providers, and calls `poll` on its
own thread each frame; connectors run in daemon worker threads that only append NormalizedEvents
(`streaming.events`) to the session's queue, never touching game state, UI or the engine.
Everything a game does with an event (eligibility, moderation, limits, what it shows) is its own.
Overview: README.md beside this folder's library root.
"""

import collections
import threading

from streaming import auth, events, streamelements, twitch, ws, youtube

PROVIDERS = {"twitch": twitch, "youtube": youtube, "streamelements": streamelements}
MAX_QUEUE = 512        # chat events waiting for the game; paid events are never dropped
STATUSES = ("disconnected", "not_configured", "authorizing", "connecting", "connected", "reconnecting",
            "permission_missing", "no_live", "quota", "error")


def new_session(app: str) -> dict:
    """`app` names the vault entries ("HumanResources" -> "HumanResources/streaming/twitch")."""
    return {"app": app, "id": auth.new_id(), "queue": collections.deque(), "workers": {}, "dropped": 0}


def _vault_key(session: dict, provider: str) -> str:
    return f"{session['app']}/streaming/{provider}"


def _put(session: dict, ev: dict) -> None:
    if ev["type"] in events.PAID or len(session["queue"]) < MAX_QUEUE:
        session["queue"].append(ev)  # deque.append is thread-safe
    else:
        session["dropped"] += 1


def _work(session: dict, provider: str, w: dict, cfg: dict) -> None:
    attempt = 0
    while not w["stop"].is_set():
        try:
            PROVIDERS[provider].run(w, cfg, lambda ev: _put(session, ev))
        except (InterruptedError, ws.Stopped):
            break
        except PermissionError as e:
            print(f"[streaming] {provider}: permission ({type(e).__name__})")
            w.update(status="permission_missing", detail="")
            return
        except Exception as e:  # network, protocol or provider data: reconnect with backoff
            print(f"[streaming] {provider}: {type(e).__name__}, reconnecting")
            w["status"] = "reconnecting"
            attempt += 1
            if w["stop"].wait(auth.backoff(attempt)):
                break
        else:
            attempt = 0
    w.update(status="disconnected", detail="")


def start(session: dict, provider: str, client_id: str = "", client_secret: str = "") -> None:
    """Connects `provider` in a worker thread (a browser opens the first time for OAuth providers).
    Without a client id (Twitch, YouTube) or a stored JWT (StreamElements) it stays "not_configured"."""
    stop(session, provider)
    w = {"stop": threading.Event(), "status": "connecting", "detail": "", "channel": ""}
    session["workers"][provider] = w
    if not (client_id if provider != "streamelements" else has_token(session, provider)):
        w["status"] = "not_configured"
        return
    cfg = {"client_id": client_id, "client_secret": client_secret, "session_id": session["id"],
           "vault_key": _vault_key(session, provider)}
    threading.Thread(target=_work, args=(session, provider, w, cfg), daemon=True, name=f"streaming-{provider}").start()


def stop(session: dict, provider: str = "") -> None:
    """Stops one provider (or all with ""); its thread exits within about a second."""
    for name in [provider] if provider else list(session["workers"]):
        w = session["workers"].pop(name, None)
        if w:
            w["stop"].set()


def forget(session: dict, provider: str) -> None:
    """Disconnects and deletes the stored token (the streamer authorizes again next time)."""
    stop(session, provider)
    auth.vault_delete(_vault_key(session, provider))


def has_token(session: dict, provider: str) -> bool:
    return auth.vault_get(_vault_key(session, provider)) is not None


def store_streamelements_jwt(session: dict) -> bool:
    """Moves a StreamElements JWT from the clipboard into the vault without showing it. False when
    the clipboard holds no plausible token."""
    jwt = auth.read_clipboard().strip()
    if jwt.count(".") != 2 or not 40 <= len(jwt) <= 4096 or any(c.isspace() for c in jwt):
        return False
    auth.vault_put(_vault_key(session, "streamelements"), {"jwt": jwt})
    return True


def status(session: dict, provider: str) -> dict:
    """{"status", "detail", "channel"}; "disconnected" when not started. `detail` holds only what the
    streamer must see (a Twitch activation code), never a token."""
    w = session["workers"].get(provider)
    return {k: w[k] for k in ("status", "detail", "channel")} if w else {"status": "disconnected", "detail": "", "channel": ""}


def inject(session: dict, ev) -> None:
    """Adds a local event (a game's TEST buttons build them with events.event(..., synthetic=True))."""
    if ev:
        _put(session, ev)


def poll(session: dict, limit: int = 32) -> list:
    """Up to `limit` waiting events, oldest first. Call from the game thread."""
    out, queue = [], session["queue"]
    while queue and len(out) < limit:
        out.append(queue.popleft())
    return out
