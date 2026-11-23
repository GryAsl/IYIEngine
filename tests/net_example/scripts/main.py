"""Net example: the smallest game on libraries/net (SPEC §25.12).

Every player is a coloured square moved with the arrow keys. The game first tries to join
127.0.0.1:7777 and hosts there itself when nobody answers within a second, so two copies started on
one machine play together with no setup. Close the host's window and the other copy takes over
(host migration). In an agent run the square circles by itself, and every second the game logs what
it sees (category "net_example").
"""

import engine
from engine import net

# Definitions are the same on every peer and come before host/join.
net.define("square", pos=net.vec2(0, 480, 0.1), color=net.u8)


def GameStart():
    engine.spawn_actor("Game", "scripts/game.py")
