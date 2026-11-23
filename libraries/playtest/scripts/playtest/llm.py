"""Optional sample: an LLM player policy over Claude (`python tools/playtest.py run --policy llm`).

Not an engine dependency: needs `pip install anthropic` and credentials (ANTHROPIC_API_KEY or
`ant auth login`); without either it plays the fallback policy and says so in its memory. The model
reads only the bounded observation and the numbered legal commands and answers with one index
(structured JSON output), so it can never issue a command outside the legal set, touch files or run
code. Its choices are recorded like any policy's: a replay never calls the model.

Environment: IYI_PLAYTEST_LLM_MODEL (default claude-opus-5-5), IYI_PLAYTEST_LLM_EVERY (ask the model
every N decisions, default 40 = one HR game day at the default cadence; the fallback plays between),
IYI_PLAYTEST_LLM_CALLS (per-run call budget, default 100), IYI_PLAYTEST_LLM_FALLBACK (default coverage),
IYI_PLAYTEST_LLM_TIMEOUT (seconds, default 60).
"""

import json
import os

SYSTEM = ("You are a QA playtester for a management game. Each turn you get the game's observation and a "
          "numbered list of legal commands. Pick the one command that best explores the game or exposes a bug, "
          "balance problem or dead end (unusual but legal play is welcome). Answer only with the JSON asked for.")
SCHEMA = {"type": "object", "properties": {"index": {"type": "integer"}, "why": {"type": "string"}},
          "required": ["index", "why"], "additionalProperties": False}
MAX_COMMANDS = 150


def make_policy(adapter):
    fallback = adapter.POLICIES[os.environ.get("IYI_PLAYTEST_LLM_FALLBACK", "coverage")]
    every = max(1, int(os.environ.get("IYI_PLAYTEST_LLM_EVERY", "40")))
    budget = int(os.environ.get("IYI_PLAYTEST_LLM_CALLS", "100"))
    model = os.environ.get("IYI_PLAYTEST_LLM_MODEL", "claude-opus-5-5")
    timeout = float(os.environ.get("IYI_PLAYTEST_LLM_TIMEOUT", "60"))

    def choose(obs, legal, rng, memory):
        n = memory["decisions"] = memory.get("decisions", 0) + 1
        out = list(fallback(obs, legal, rng, memory) or [])  # core accepts None from a policy
        if not legal or n % every or memory.get("llm_calls", 0) >= budget or memory.get("llm_off"):
            return out
        pick = _ask(memory, model, timeout, obs, legal[:MAX_COMMANDS])
        return ([pick] + out) if pick is not None else out

    choose.__name__ = "llm"
    return choose


def _ask(memory, model, timeout, obs, legal):
    """One Messages API call; any failure or invalid answer falls back (and is counted)."""
    try:
        import anthropic
    except ImportError:
        memory["llm_off"] = "anthropic SDK not installed (pip install anthropic)"
        return None
    client = memory.get("_client") or anthropic.Anthropic(timeout=timeout, max_retries=2)
    memory["_client"] = client
    memory["llm_calls"] = memory.get("llm_calls", 0) + 1
    numbered = "\n".join(f"{i}: {json.dumps(a, sort_keys=True)}" for i, a in enumerate(legal))
    prompt = (f"Observation:\n{json.dumps({k: v for k, v in obs.items() if k != 'scenario'}, sort_keys=True)}\n\n"
              f"Legal commands:\n{numbered}\n\nReply with the index of one command.")
    try:
        response = client.beta.messages.create(
            model=model, max_tokens=4000, system=SYSTEM,
            messages=[{"role": "user", "content": prompt}],
            output_config={"effort": "low", "format": {"type": "json_schema", "schema": SCHEMA}},
            betas=["server-side-fallback-2026-07-01"], fallbacks="default",
        )
    except anthropic.AuthenticationError:
        memory["llm_off"] = "no valid Claude credentials"
        return None
    except (anthropic.APIStatusError, anthropic.APIConnectionError) as exc:  # RateLimitError is an APIStatusError
        memory["llm_errors"] = memory.get("llm_errors", 0) + 1
        memory["llm_last_error"] = type(exc).__name__
        return None
    if response.stop_reason in ("refusal", "max_tokens"):
        memory["llm_invalid"] = memory.get("llm_invalid", 0) + 1
        return None
    try:
        index = json.loads(next(b.text for b in response.content if b.type == "text"))["index"]
    except (StopIteration, ValueError, KeyError, TypeError):
        index = -1
    if not isinstance(index, int) or not 0 <= index < len(legal):
        memory["llm_invalid"] = memory.get("llm_invalid", 0) + 1
        return None
    return legal[index]
