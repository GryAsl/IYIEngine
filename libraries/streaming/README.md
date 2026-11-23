# streaming

Desktop-only events from streaming platforms for a game to use as it likes. Pure Python, stdlib only.
A game lists it in `project.json` twice:

```json
"libraries": ["../IYIEngine/libraries/streaming"],
"desktop_libraries": ["../IYIEngine/libraries/streaming"]
```

and imports it guarded (`try: import streaming except ImportError`), since Android, iOS and web builds
do not contain it.

## Use

```python
session = streaming.new_session("MyGame")         # keep it in actor.state; never save it
streaming.start(session, "twitch", client_id=...)  # browser opens the first time
for ev in streaming.poll(session):                 # each frame, on the game thread
    ...                                            # ev: streaming.events.event() fields
streaming.stop(session)                            # all providers; on exit too
```

| Provider | How | Gives |
|---|---|---|
| `twitch` | Device code OAuth (public client, no secret), EventSub WebSocket | chat (subscriber/founder badge), gift subs (one per purchase), message deletions |
| `youtube` | Loopback OAuth + PKCE, `liveChatMessages.list` at the server's polling interval | chat (`isChatSponsor` = paid member), membership gifting, deletions, bans |
| `streamelements` | The streamer's JWT, read from the clipboard into the vault (`store_streamelements_jwt`), Astro `channel.tips` | completed, approved tips (amount, currency, name, text) |

Rules the library keeps: worker threads only append to the session queue (a `deque`); chat beyond
`MAX_QUEUE` is dropped and counted, paid events never are. Tokens live in the Windows Credential
Manager (`<app>/streaming/<provider>`), never in files or logs; logs name only error types.
`subscriber` is `None` when a provider gave no evidence. Super Chats, bits, channel points and
resubscriptions are ignored.

## Limits

- Fixtures in `selfcheck.py` are written from the documented payloads, not captured live.
- Non-Windows: tokens are kept for the run only and the clipboard is not read (`auth.py` shortcuts).
- Kick (needs a hosted HTTPS webhook relay) and Streamlabs (needs its app review) are not here yet.
