"""Client-side prediction for the local player's host-authoritative object (SPEC §25.9).

    from engine import net
    from net_predict import Predictor

    def step(state, inp, dt):          # pure: the same function the host runs on net.inputs()
        x, y = state["pos"]
        return {"pos": (x + inp["dx"] * 160 * dt, y + inp["dy"] * 160 * dt)}

    pred = Predictor(step)
    # every frame on the client:
    state = pred.apply({"dx": dx, "dy": dy}, dt, host_state={"pos": obj.pos})
    draw(pred.shown(state))

`apply` sends the input (`net.send_input`), forgets inputs the host has applied (`net.input_ack()`),
replays the rest over the latest host state and returns the predicted state. `shown` adds a small
correction offset that fades out, so a misprediction under `snap_distance` glides instead of jumping.
Only numbers and tuples of numbers are smoothed; other fields are taken as predicted.
"""

from engine import net


class Predictor:
    def __init__(self, step, snap_distance=64.0, smoothing=0.15):
        self.step = step
        self.snap_distance = snap_distance
        self.smoothing = smoothing          # fraction of the correction kept per frame
        self.pending = []                   # [(seq, input, dt)] the host has not applied yet
        self.predicted = None
        self.offset = {}

    def apply(self, inp, dt, host_state):
        """Send this frame's input and return the state predicted from `host_state` plus every unacked input."""
        seq = net.send_input(**inp)
        if seq:
            self.pending.append((seq, dict(inp), dt))
        ack = net.input_ack()
        self.pending = [p for p in self.pending if p[0] > ack]
        state = dict(host_state)
        for _, pending_input, pending_dt in self.pending:
            state = self.step(state, pending_input, pending_dt)
        if self.predicted is not None:
            # Where last frame's prediction would be now, against where the replay says: the difference
            # is the misprediction. Shown positions keep it as an offset that fades, unless it is too big.
            expected = self.step(dict(self.predicted), inp, dt)
            for key, value in state.items():
                before = expected.get(key)
                if not (_numeric(value) and _numeric(before)):
                    continue
                error = _add(_sub(before, value), self.offset[key]) if key in self.offset else _sub(before, value)
                if _length(error) > self.snap_distance:
                    self.offset.pop(key, None)
                elif _length(error) > 1e-3:
                    self.offset[key] = error
        self.predicted = state
        return state

    def shown(self, state):
        """The state to draw: `state` with the fading correction added."""
        out = dict(state)
        for key, off in self.offset.items():
            if key in out:
                out[key] = _add(out[key], off)
        self.offset = {k: _scale(v, self.smoothing) for k, v in self.offset.items() if _length(v) > 1e-3}
        return out

    def reset(self):
        self.pending.clear()
        self.predicted = None
        self.offset = {}


def _numeric(v):
    return isinstance(v, (int, float)) or (isinstance(v, (tuple, list)) and len(v) > 0 and all(isinstance(c, (int, float)) for c in v))


def _add(a, b):
    return a + b if not isinstance(a, (tuple, list)) else tuple(x + y for x, y in zip(a, b))


def _sub(a, b):
    return a - b if not isinstance(a, (tuple, list)) else tuple(x - y for x, y in zip(a, b))


def _scale(a, s):
    return a * s if not isinstance(a, (tuple, list)) else tuple(x * s for x in a)


def _length(a):
    return abs(a) if not isinstance(a, (tuple, list)) else sum(x * x for x in a) ** 0.5
