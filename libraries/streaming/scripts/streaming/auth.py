"""HTTPS requests, the OS credential vault and the two OAuth flows the connectors use.

No client secret is ever needed for Twitch (device code flow for public clients). Google's desktop
flow (loopback redirect + PKCE) takes an optional installed-app secret, which Google documents as
not confidential. Tokens live only in the vault, never in settings, saves or logs.
"""

import base64
import hashlib
import http.server
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

TIMEOUT = 15.0
MAX_BODY = 1 << 20


def request(method: str, url: str, *, form=None, body=None, headers=None):
    """(status, parsed JSON or {}). A network failure raises OSError; HTTP errors return their status."""
    data, head = None, dict(headers or {})
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        head["Content-Type"] = "application/x-www-form-urlencoded"
    elif body is not None:
        data = json.dumps(body).encode()
        head["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=head, method=method)
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            status, raw = r.status, r.read(MAX_BODY)
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read(MAX_BODY)
    try:
        parsed = json.loads(raw.decode("utf-8")) if raw else {}
    except ValueError:
        parsed = {}
    return status, parsed if isinstance(parsed, dict) else {}


# ---- vault -----------------------------------------------------------------------------------------

_memory_vault = {}  # shortcut: non-Windows keeps tokens for this run only, add Keychain/libsecret when shipped there


def _wincred():
    import ctypes
    from ctypes import wintypes

    class CREDENTIAL(ctypes.Structure):
        _fields_ = [("Flags", wintypes.DWORD), ("Type", wintypes.DWORD), ("TargetName", wintypes.LPWSTR),
                    ("Comment", wintypes.LPWSTR), ("LastWritten", wintypes.FILETIME),
                    ("CredentialBlobSize", wintypes.DWORD), ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)),
                    ("Persist", wintypes.DWORD), ("AttributeCount", wintypes.DWORD), ("Attributes", ctypes.c_void_p),
                    ("TargetAlias", wintypes.LPWSTR), ("UserName", wintypes.LPWSTR)]
    return ctypes, CREDENTIAL, ctypes.WinDLL("advapi32", use_last_error=True)


def vault_get(name: str):
    """The stored dict, or None."""
    if sys.platform != "win32":
        return _memory_vault.get(name)
    ctypes, CREDENTIAL, api = _wincred()
    ptr = ctypes.POINTER(CREDENTIAL)()
    if not api.CredReadW(name, 1, 0, ctypes.byref(ptr)):
        return None
    try:
        blob = ctypes.string_at(ptr.contents.CredentialBlob, ptr.contents.CredentialBlobSize)
    finally:
        api.CredFree(ptr)
    try:
        value = json.loads(blob.decode("utf-8"))
    except ValueError:
        return None
    return value if isinstance(value, dict) else None


def vault_put(name: str, value: dict) -> None:
    if sys.platform != "win32":
        _memory_vault[name] = dict(value)
        return
    ctypes, CREDENTIAL, api = _wincred()
    blob = json.dumps(value).encode("utf-8")
    buf = (ctypes.c_ubyte * len(blob)).from_buffer_copy(blob)
    cred = CREDENTIAL(Type=1, TargetName=name, CredentialBlobSize=len(blob), CredentialBlob=buf, Persist=2)
    if not api.CredWriteW(ctypes.byref(cred), 0):
        raise OSError(f"credential vault write failed ({ctypes.get_last_error()})")


def vault_delete(name: str) -> None:
    if sys.platform != "win32":
        _memory_vault.pop(name, None)
        return
    _ctypes, _cred, api = _wincred()
    api.CredDeleteW(name, 1, 0)


def read_clipboard() -> str:
    """The Windows clipboard text, so a pasted token is never shown on screen (or on stream)."""
    if sys.platform != "win32":
        return ""  # shortcut: Windows only, add the engine clipboard when another desktop OS ships
    import ctypes
    user32, kernel32 = ctypes.WinDLL("user32"), ctypes.WinDLL("kernel32")
    user32.GetClipboardData.restype = kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    if not user32.OpenClipboard(None):
        return ""
    try:
        handle = user32.GetClipboardData(13)  # CF_UNICODETEXT
        ptr = kernel32.GlobalLock(handle) if handle else None
        if not ptr:
            return ""
        try:
            return ctypes.wstring_at(ptr)[:8192]
        finally:
            kernel32.GlobalUnlock(handle)
    finally:
        user32.CloseClipboard()


# ---- OAuth -----------------------------------------------------------------------------------------

def _expiry(token: dict) -> dict:
    token["expires_at"] = time.time() + float(token.get("expires_in") or 3600)
    return token


def twitch_device(client_id: str, scopes: str, stop, show) -> dict:
    """Twitch device code flow: opens the activation page, shows the code via `show`, waits."""
    status, start = request("POST", "https://id.twitch.tv/oauth2/device", form={"client_id": client_id, "scopes": scopes})
    if status != 200 or "device_code" not in start:
        raise PermissionError(f"twitch device flow refused ({status})")
    show(f"code {start.get('user_code', '')}")
    webbrowser.open(start.get("verification_uri", "https://www.twitch.tv/activate"))
    end = time.monotonic() + float(start.get("expires_in", 1800))
    while time.monotonic() < end:
        if stop.wait(max(float(start.get("interval", 5)), 1.0)):
            raise InterruptedError()
        status, token = request("POST", "https://id.twitch.tv/oauth2/token", form={
            "client_id": client_id, "scopes": scopes, "device_code": start["device_code"],
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code"})
        if status == 200 and token.get("access_token"):
            return _expiry(token)
        if token.get("message") not in ("authorization_pending", "slow_down"):
            raise PermissionError(f"twitch authorization failed ({token.get('message', status)})")
    raise PermissionError("twitch authorization timed out")


def refresh(url: str, client_id: str, token: dict, secret: str = ""):
    """A refreshed token dict, or None when the refresh token no longer works."""
    form = {"client_id": client_id, "grant_type": "refresh_token", "refresh_token": token.get("refresh_token", "")}
    if secret:
        form["client_secret"] = secret
    status, new = request("POST", url, form=form)
    if status != 200 or not new.get("access_token"):
        return None
    new.setdefault("refresh_token", token.get("refresh_token"))
    return _expiry(new)


def google_loopback(client_id: str, secret: str, scope: str, stop, show) -> dict:
    """Google installed-app flow: browser consent, a one-shot 127.0.0.1 redirect, PKCE and state."""
    verifier, state, box = secrets.token_urlsafe(48), secrets.token_urlsafe(24), {}
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            if query.get("state", [""])[0] == state:
                box["code"], box["error"] = query.get("code", [""])[0], query.get("error", [""])[0]
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(b"You can close this tab and return to the game.")

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    server.timeout = 1.0
    redirect = f"http://127.0.0.1:{server.server_port}"
    show("browser")
    webbrowser.open("https://accounts.google.com/o/oauth2/v2/auth?" + urllib.parse.urlencode({
        "client_id": client_id, "redirect_uri": redirect, "response_type": "code", "scope": scope,
        "code_challenge": challenge, "code_challenge_method": "S256", "state": state, "access_type": "offline",
        "prompt": "consent"}))
    try:
        end = time.monotonic() + 300.0
        while not box and time.monotonic() < end:
            if stop.is_set():
                raise InterruptedError()
            server.handle_request()
    finally:
        server.server_close()
    if not box.get("code"):
        raise PermissionError(f"google authorization failed ({box.get('error') or 'timed out'})")
    form = {"client_id": client_id, "code": box["code"], "code_verifier": verifier,
            "grant_type": "authorization_code", "redirect_uri": redirect}
    if secret:
        form["client_secret"] = secret
    status, token = request("POST", "https://oauth2.googleapis.com/token", form=form)
    if status != 200 or not token.get("access_token"):
        raise PermissionError(f"google token exchange failed ({status})")
    return _expiry(token)


def new_id() -> str:
    return secrets.token_hex(8)


def stoppable_sleep(stop, seconds: float) -> None:
    if stop.wait(seconds):
        raise InterruptedError()


def backoff(attempt: int) -> float:
    """Reconnect wait with jitter: 2, 4, 8 ... 120 seconds."""
    return min(120.0, 2.0 ** min(attempt, 7)) * (0.75 + 0.5 * (int.from_bytes(os.urandom(1), "big") / 255))



def token(name: str, client_id: str, refresh_url: str, login, secret: str = "", force_refresh=False) -> dict:
    """A usable token from the vault: refreshed when it is near expiry (or `force_refresh`), from the
    `login()` browser flow when there is none or the refresh fails. Stored back in the vault."""
    tok = vault_get(name)
    if tok and (force_refresh or time.time() > float(tok.get("expires_at", 0)) - 120):
        tok = refresh(refresh_url, client_id, tok, secret) if tok.get("refresh_token") else None
        if tok:
            vault_put(name, tok)
    if not tok or not tok.get("access_token"):
        tok = login()
        vault_put(name, tok)
    return tok
