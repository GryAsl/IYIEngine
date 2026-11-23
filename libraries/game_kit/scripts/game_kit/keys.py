"""Player-rebindable keyboard shortcuts over an action table the game owns.

`actions` maps action -> (default engine.KEY_* name, group). Keys are unique only within a group,
because groups never fire in the same situation: "plain" keys fire alone, "ctrl" keys with Ctrl
held (Ctrl+S), "held" keys are modifiers held while clicking (Shift+Click). Esc is never bindable:
it always backs out of a menu and cancels a key capture. The game stores the bindings dict
(action -> key code) wherever it keeps settings; resolve() turns whatever is stored into a full,
valid set.

Key names and labels ("Ctrl+S", "Shift+Click", "Space") stay English in every language.
"""

import functools

try:  # headless tests import this without the engine
    import engine
except ImportError:
    engine = None

_RESERVED = ("KEY_NULL", "KEY_ESCAPE")
_CTRL_KEYS = ("KEY_LEFT_CONTROL", "KEY_RIGHT_CONTROL")
# A held modifier accepts its other-side twin too.
_TWINS = (
    ("KEY_LEFT_SHIFT", "KEY_RIGHT_SHIFT"),
    ("KEY_LEFT_CONTROL", "KEY_RIGHT_CONTROL"),
    ("KEY_LEFT_ALT", "KEY_RIGHT_ALT"),
    ("KEY_LEFT_SUPER", "KEY_RIGHT_SUPER"),
)
_DIGIT_WORDS = {
    "ZERO": "0", "ONE": "1", "TWO": "2", "THREE": "3", "FOUR": "4",
    "FIVE": "5", "SIX": "6", "SEVEN": "7", "EIGHT": "8", "NINE": "9",
}


def code(name: str):
    value = getattr(engine, name, None)
    return value if isinstance(value, int) and not isinstance(value, bool) else None


@functools.lru_cache(maxsize=1)  # engine constants never change at runtime
def _key_names() -> dict:
    """engine key code -> constant name (first name wins for aliased codes)."""
    names = {}
    for name in sorted(dir(engine)):
        if name.startswith("KEY_") and name not in _RESERVED:
            c = code(name)
            if c is not None and c not in names:
                names[c] = name
    return names


def default_key(actions: dict, action: str) -> int:
    return code(actions[action][0])


def is_valid_key(actions: dict, action: str, key) -> bool:
    if action not in actions or not isinstance(key, int) or isinstance(key, bool) or key not in _key_names():
        return False
    return not (actions[action][1] == "ctrl" and key in (code(n) for n in _CTRL_KEYS))  # Ctrl+Ctrl is not a combo


def resolve(actions: dict, stored) -> dict:
    """action -> key code, falling back to the default for any missing or invalid entry."""
    if not isinstance(stored, dict):
        stored = {}
    taken = {(actions[a][1], k) for a, k in stored.items() if is_valid_key(actions, a, k)}
    result = {}
    for action, (_name, group) in actions.items():
        key = stored.get(action)
        if not is_valid_key(actions, action, key):
            key = default_key(actions, action)
            # A newer action whose default the player gave another one: unbound ("?") until rebound.
            key = 0 if (group, key) in taken else key
        result[action] = key
    return result


def rebind(actions: dict, current: dict, action: str, key: int) -> dict | None:
    """The bindings with `key` on `action` (a same-group action using it takes the old key), or
    None when the key cannot be bound there."""
    if not is_valid_key(actions, action, key):
        return None
    current = dict(current)
    group = actions[action][1]
    for other, other_key in current.items():
        if other != action and other_key == key and actions[other][1] == group:
            current[other] = current[action]
    current[action] = key
    return current


def key_name(key: int) -> str:
    name = _key_names().get(key)
    if name is None:
        return "?"
    word = name[len("KEY_"):]
    if word in _DIGIT_WORDS:
        return _DIGIT_WORDS[word]
    if len(word) == 1 or (word[0] == "F" and word[1:].isdigit()):
        return word  # letters and F-keys stay upper-case: "O", "F5"
    return " ".join(part.capitalize() for part in word.split("_"))


def label(actions: dict, action: str, key: int) -> str:
    """The key as the player types it: "O", "Ctrl+S", "Shift+Click"."""
    name = key_name(key)
    group = actions[action][1]
    if group == "ctrl":
        return f"Ctrl+{name}"
    if group == "held":
        for prefix in ("Left ", "Right "):  # either side's twin works
            if name.startswith(prefix):
                name = name[len(prefix):]
        return f"{name}+Click"
    return name


def held(key: int) -> bool:
    keys = [key]
    for left, right in _TWINS:
        pair = (code(left), code(right))
        if key in pair:
            keys = [k for k in pair if k is not None]
    return any(engine.is_key_down(k) for k in keys)
