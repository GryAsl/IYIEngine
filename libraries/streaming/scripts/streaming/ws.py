"""A minimal RFC 6455 WebSocket client (text messages) on the standard library's socket and ssl.

Used from connector worker threads only. Blocking reads wake every second to check `stop`, and a
read deadline (provider keepalive) raises TimeoutError, so a dead connection is noticed.
"""

import base64
import hashlib
import os
import socket
import ssl
import struct
import time
import urllib.parse

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_MESSAGE = 1 << 20  # bytes; larger frames end the connection


class Stopped(Exception):
    pass


def connect(url: str, stop, timeout: float = 10.0) -> dict:
    u = urllib.parse.urlsplit(url)
    secure = u.scheme == "wss"
    host = u.hostname or ""
    sock = socket.create_connection((host, u.port or (443 if secure else 80)), timeout)
    if secure:
        sock = ssl.create_default_context().wrap_socket(sock, server_hostname=host)
    key = base64.b64encode(os.urandom(16)).decode()
    path = (u.path or "/") + (f"?{u.query}" if u.query else "")
    sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(4096)
        if not chunk or len(head) > 16384:
            sock.close()
            raise ConnectionError("websocket handshake failed")
        head += chunk
    head, rest = head.split(b"\r\n\r\n", 1)
    accept = base64.b64encode(hashlib.sha1((key + _GUID).encode()).digest()).decode()
    lines = head.decode("latin-1").split("\r\n")
    headers = {k.strip().lower(): v.strip() for k, _, v in (line.partition(":") for line in lines[1:])}
    if " 101 " not in lines[0] + " " or headers.get("sec-websocket-accept") != accept:
        sock.close()
        raise ConnectionError(f"websocket upgrade refused: {lines[0][:60]}")
    sock.settimeout(1.0)
    return {"sock": sock, "buf": rest, "stop": stop, "deadline": None}


def _read(ws: dict, n: int) -> bytes:
    while len(ws["buf"]) < n:
        if ws["stop"].is_set():
            raise Stopped()
        if ws["deadline"] is not None and time.monotonic() > ws["deadline"]:
            raise TimeoutError("websocket keepalive missed")
        try:
            chunk = ws["sock"].recv(65536)
        except (socket.timeout, ssl.SSLWantReadError):
            continue
        if not chunk:
            raise ConnectionError("websocket closed by server")
        ws["buf"] += chunk
    data, ws["buf"] = ws["buf"][:n], ws["buf"][n:]
    return data


def _frame(opcode: int, payload: bytes) -> bytes:
    n = len(payload)
    head = bytes([0x80 | opcode]) + (bytes([0x80 | n]) if n < 126 else
                                      struct.pack("!BH", 0x80 | 126, n) if n < 65536 else
                                      struct.pack("!BQ", 0x80 | 127, n))
    mask = os.urandom(4)
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def send(ws: dict, text: str) -> None:
    ws["sock"].sendall(_frame(0x1, text.encode("utf-8")))


def recv(ws: dict) -> str:
    """The next text message. Answers pings; raises ConnectionError on close, Stopped on stop."""
    parts = []
    while True:
        b0, b1 = _read(ws, 2)
        opcode, n = b0 & 0x0F, b1 & 0x7F
        if n == 126:
            n = struct.unpack("!H", _read(ws, 2))[0]
        elif n == 127:
            n = struct.unpack("!Q", _read(ws, 8))[0]
        if n > MAX_MESSAGE or sum(map(len, parts)) + n > MAX_MESSAGE:
            raise ConnectionError("websocket message too large")
        mask = _read(ws, 4) if b1 & 0x80 else None
        payload = _read(ws, n)
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if opcode == 0x8:
            raise ConnectionError("websocket closed by server")
        if opcode == 0x9:
            ws["sock"].sendall(_frame(0xA, payload))
            continue
        if opcode in (0x0, 0x1, 0x2):
            parts.append(payload)
            if b0 & 0x80:
                return b"".join(parts).decode("utf-8", "replace")


def close(ws) -> None:
    if ws:
        try:
            ws["sock"].sendall(_frame(0x8, b""))
        except OSError:
            pass
        ws["sock"].close()
