"""The net example's one actor: join or host, keep one square per player, move your own."""

import math

import engine
from engine import net

COLORS = [engine.RED, engine.BLUE, engine.GREEN, engine.ORANGE, engine.PURPLE, engine.GOLD]
SPEED = 160.0


def Start(actor):
    actor.state.update(waited=0.0, clock=0.0, logged=0.0)
    net.join("127.0.0.1:7777")


def Update(actor, dt):
    st = actor.state
    st["clock"] += dt
    for e in net.events():
        engine.log_info(f"event {e}", "net_example")

    # Nobody answered within a second: host here instead.
    if net.state() == "joining":
        st["waited"] += dt
        if st["waited"] > 1.0:
            net.leave()
            net.host(port=7777)

    if net.is_host():
        players = {p["id"] for p in net.players()}
        squares = net.objects("square")
        for sq in squares:
            if sq.owner not in players:
                sq.despawn()
        owned = {sq.owner for sq in squares}
        for p in players - owned:
            # Each player writes its own square (authority "owner"); the host relays it.
            net.spawn("square", owner=p, authority="owner", pos=(60 + 40 * p, 160), color=p)

    me = net.local_player()
    for sq in net.objects("square"):
        if sq.owner != me:
            continue
        x, y = sq.pos
        if engine.is_agent_run():
            t = st["clock"] + me
            dx, dy = math.cos(t), math.sin(t)
        else:
            dx = engine.is_key_down(engine.KEY_RIGHT) - engine.is_key_down(engine.KEY_LEFT)
            dy = engine.is_key_down(engine.KEY_DOWN) - engine.is_key_down(engine.KEY_UP)
        sq.pos = (min(max(x + dx * SPEED * dt, 12), 468), min(max(y + dy * SPEED * dt, 12), 308))

    engine.set_window_title(f"Net Example: {net.state()} as player {me}")
    if st["clock"] - st["logged"] >= 1.0:
        st["logged"] = st["clock"]
        seen = [(sq.owner, round(sq.pos[0]), round(sq.pos[1])) for sq in net.objects("square")]
        stats = net.stats()
        engine.log_info(f"state={net.state()} player={me} players={len(net.players())} squares={seen} "
                        f"snapshot_bytes={stats['last_snapshot_bytes']} out_per_sec={round(stats['bytes_out_per_sec'])}",
                        "net_example")


def Drawing(actor):
    engine.clear_background(engine.RAYWHITE)
    me = net.local_player()
    for sq in net.objects("square"):
        x, y = sq.pos if sq.owner == me else sq.sample("pos")
        engine.draw_rectangle(x - 12, y - 12, 24, 24, COLORS[sq.color % len(COLORS)])
    engine.draw_text(f"{net.state()}  player {me}  ({len(net.players())} playing)", 8, 8, 16, engine.DARKGRAY)
