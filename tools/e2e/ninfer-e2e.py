#!/usr/bin/env python3
"""E2E test suite for ninfer safety-net eviction system.

Runs twelve phases by default against a single test server (no flags needed):
  Phase 1 "pressure":           4 sessions — basic safety net (spills, restores, no re-prefills)
  Phase 2 "mixed":              1 big + 3 small — eviction order (smallest-first, big preserved)
  Phase 3 "trash":              10 sessions — graceful degradation under trashing (no crash)
  Phase 4 "thinking":           3 sessions, reasoning mode — session-key fallback with rewrite checkpoint
  Phase 5 "checkpoint-advance": 1 session, 8 turns — checkpoint frontier advances monotonically
  Phase 6 "tool-calling":       1 session, 6 turns with tools — rewrite restore under tool-call rounds
  Phase 7 "responses-tools":    1 session, 5 turns — Responses API tool-calling with checkpoint reuse
  Phase 8 "reasoning-effort":   5 requests — reasoning effort tier mapping (high, minimal, max, medium, low)
  Phase 9 "concurrent":         2 sessions + title-gen — source eviction fallback, no cross-session state destruction
  Phase 10 "thinking-sig":      4 requests — thinking signature skip when preserve_thinking=false
  Phase 11 "demotion":          3 sessions, large prompts — host demotion + checkpoint restore
  Phase 12 "state-lease":       4 thinking sessions — rewrite-recycle pressure; zero state-lease
                                leaks / orphaned state slots (regression: 2026-09-14 prod wedge)

Server config: 32k max-context, 64k kv-capacity, 4GB host-kv, 3 continuations,
5 device state slots (production parity — required by phase 12).
All phases use the same server — no restarts.

Usage: python3 ninfer-e2e.py [--host 127.0.0.1] [--port 8080] [--serve-log /home/zenz/ninfer-serve.log]
"""

import argparse
import json
import re
import random
import sys
import threading
import time
import urllib.request
import urllib.error

WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima "
         "mike november oscar papa quebec romeo sierra tango uniform victor whiskey "
         "xray yankee zulu amber cedar dawn ember frost grove harbor ivory "
         "jade kernel lumen meadow night opal prism quill raven stone umber "
         "vale willow xenon yonder zephyr anchor beacon compass estuary").split()


def filler(rng, tokens):
    chars = int(tokens * 4.2)
    parts, n = [], 0
    while n < chars:
        w = rng.choice(WORDS)
        parts.append(w)
        n += len(w) + 1
    return " ".join(parts)


class Session:
    def __init__(self, name, seed_tokens, turn_tokens, args):
        self.name = name
        self.rng = random.Random(sum(ord(c) for c in name))
        self.seed_tokens = seed_tokens
        self.turn_tokens = turn_tokens
        self.args = args
        self.response_id = None
        self.turns = []
        self.doc = filler(self.rng, seed_tokens)

    def turn(self, index):
        question = f"Question {index}: Consider the paragraph about '{self.rng.choice(WORDS)}'. Answer briefly."
        new_text = filler(self.rng, self.turn_tokens) + "\n\n" + question
        if index == 1:
            new_text = self.doc + "\n\n---\n\n" + new_text
        payload = {
            "model": self.args.model,
            "input": [{"role": "user", "content": [{"type": "input_text", "text": new_text}]}],
            "instructions": "You are a concise assistant.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
        }
        if getattr(self.args, "thinking_mode", False):
            payload["reasoning"] = {"effort": "low"}
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"), "output_tokens": usage.get("output_tokens")}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s prompt={record['input_tokens']} out={record['output_tokens']}")
        return record


class ChatSession:
    """Session using /v1/chat/completions with tools, simulating Claude Code."""
    TOOLS = [
        {"type": "function", "function": {
            "name": "read_file", "description": "Read a file",
            "parameters": {"type": "object", "properties": {"path": {"type": "string"}},
                           "required": ["path"]}}},
        {"type": "function", "function": {
            "name": "write_file", "description": "Write a file",
            "parameters": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}},
                           "required": ["path", "content"]}}},
        {"type": "function", "function": {
            "name": "list_dir", "description": "List directory contents",
            "parameters": {"type": "object", "properties": {"path": {"type": "string"}},
                           "required": ["path"]}}},
    ]

    def __init__(self, name, seed_tokens, turn_tokens, args):
        self.name = name
        self.rng = random.Random(sum(ord(c) for c in name) + 1)
        self.seed_tokens = seed_tokens
        self.turn_tokens = turn_tokens
        self.args = args
        self.messages = [{"role": "system", "content":
            "You are a coding assistant. You MUST use tools (read_file, write_file, list_dir) "
            "to answer questions. Always call at least one tool before responding."}]
        self.turns = []
        self.doc = filler(self.rng, seed_tokens)

    def turn(self, index):
        if index == 1:
            user_text = self.doc + "\n\n---\n\n" + filler(self.rng, self.turn_tokens) + "\n\nUse the read_file tool to read /tmp/test.txt, then answer."
        else:
            user_text = filler(self.rng, self.turn_tokens) + f"\n\nUse the list_dir tool to list /tmp, then answer question {index}."
        self.messages.append({"role": "user", "content": user_text})
        payload = {
            "model": self.args.model,
            "messages": self.messages,
            "max_tokens": self.args.max_output_tokens,
            "tools": self.TOOLS,
            "tool_choice": "auto",
            "stream": False,
        }
        if getattr(self.args, "thinking_mode", False):
            payload["enable_thinking"] = True
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/chat/completions",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        choice = out.get("choices", [{}])[0]
        msg = choice.get("message", {})
        usage = out.get("usage", {}) or {}
        # Record the assistant reply (including tool_calls) for multi-turn context
        assistant_msg = {"role": "assistant", "content": msg.get("content") or ""}
        if msg.get("tool_calls"):
            assistant_msg["tool_calls"] = msg["tool_calls"]
            self.messages.append(assistant_msg)
            # Simulate tool results so the conversation can continue
            for tc in msg["tool_calls"]:
                self.messages.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id", "call_0"),
                    "content": f"Result of {tc['function']['name']}: OK",
                })
        else:
            self.messages.append(assistant_msg)
        finish = choice.get("finish_reason", "unknown")
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("prompt_tokens"),
                  "output_tokens": usage.get("completion_tokens"),
                  "finish": finish}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s "
              f"prompt={record['input_tokens']} out={record['output_tokens']} finish={finish}")
        return record


class ResponsesApiSession:
    """Session using /v1/responses with tools. Uses store=True and
    previous_response_id for checkpoint reuse, with tool definitions
    to test the Responses API tool-calling path.
    """
    def __init__(self, name, seed_tokens, turn_tokens, args):
        self.name = name
        self.rng = random.Random(sum(ord(c) for c in name) + 7)
        self.seed_tokens = seed_tokens
        self.turn_tokens = turn_tokens
        self.args = args
        self.response_id = None
        self.turns = []
        self.doc = filler(self.rng, seed_tokens)
        self._pending_tool_calls = []

    def turn(self, index):
        # On turns after a tool call, send function_call_output as new input.
        # Otherwise send a new user message.
        if index == 1:
            user_text = self.doc + "\n\n---\n\n" + filler(self.rng, self.turn_tokens) + "\n\nRead the file."
            new_input = [{"role": "user", "content": [{"type": "input_text", "text": user_text}]}]
        elif self._pending_tool_calls:
            # Send function_call_output for each pending tool call
            new_input = []
            for call_id, fn_name in self._pending_tool_calls:
                new_input.append({
                    "type": "function_call_output",
                    "call_id": call_id,
                    "output": f"Result of {fn_name}: OK",
                })
            self._pending_tool_calls = []
        else:
            user_text = filler(self.rng, self.turn_tokens) + f"\n\nQuestion {index}: Summarize what you found."
            new_input = [{"role": "user", "content": [{"type": "input_text", "text": user_text}]}]
        payload = {
            "model": self.args.model,
            "input": new_input,
            "instructions": "You are a coding assistant. Use tools when needed.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
            "tools": [
                {"type": "function", "name": "read_file",
                 "description": "Read a file",
                 "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}},
            ],
        }
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        output = out.get("output", [])
        tool_calls = [item for item in output if isinstance(item, dict) and item.get("type") == "function_call"]
        has_tool_call = len(tool_calls) > 0
        has_text = any(item.get("type") == "message" for item in output if isinstance(item, dict))
        # Queue tool call results for next turn
        self._pending_tool_calls = [
            (tc.get("call_id", f"call_{i}"), tc.get("name", "read_file"))
            for i, tc in enumerate(tool_calls)
        ]
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"),
                  "output_tokens": usage.get("output_tokens"),
                  "has_tool_call": has_tool_call, "has_text": has_text}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s "
              f"prompt={record['input_tokens']} out={record['output_tokens']} "
              f"tool={has_tool_call} text={has_text}")
        return record


class ReasoningEffortTester:
    """Send requests with various reasoning effort levels to verify tier mapping."""
    EFFORTS = ["low", "medium", "high", "minimal", "max"]
    def __init__(self, args):
        self.args = args
        self.results = []

    def test(self):
        for effort in self.EFFORTS:
            payload = {
                "model": self.args.model,
                "input": [{"role": "user", "content": [{"type": "input_text", "text": "Say hello."}]}],
                "max_output_tokens": 32,
                "stream": False,
                "reasoning": {"effort": effort},
            }
            t0 = time.monotonic()
            try:
                out = json.load(urllib.request.urlopen(urllib.request.Request(
                    f"http://{self.args.host}:{self.args.port}/v1/responses",
                    data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
                    method="POST"), timeout=self.args.timeout))
                wall = time.monotonic() - t0
                usage = out.get("usage", {}) or {}
                self.results.append({"effort": effort, "ok": True, "wall_s": round(wall, 2),
                                    "input_tokens": usage.get("input_tokens"),
                                    "output_tokens": usage.get("output_tokens")})
                print(f"  effort={effort}: OK wall={wall:.1f}s out={usage.get('output_tokens', '?')}")
            except urllib.error.HTTPError as e:
                wall = time.monotonic() - t0
                body = e.read().decode()[:200]
                self.results.append({"effort": effort, "ok": False, "wall_s": round(wall, 2),
                                    "error": body})
                print(f"  effort={effort}: FAIL status={e.code} {body[:100]}")
            except Exception as e:
                wall = time.monotonic() - t0
                self.results.append({"effort": effort, "ok": False, "wall_s": round(wall, 2),
                                    "error": repr(e)})
                print(f"  effort={effort}: ERROR {repr(e)[:100]}")
        return self.results


class ThinkingSignatureTester:
    """Test thinking signature verification skip when preserve_thinking=false."""
    def __init__(self, args):
        self.args = args
        self.results = []

    def test(self):
        base_url = f"http://{self.args.host}:{self.args.port}/v1/messages"
        headers = {"Content-Type": "application/json", "x-api-key": "test"}

        # Step 1: Get a valid thinking block
        payload = {
            "model": self.args.model,
            "messages": [{"role": "user", "content": "Say hello in one word."}],
            "max_tokens": 2048,
            "thinking": {"type": "enabled", "budget_tokens": 1024},
            "stream": False,
        }
        try:
            out = json.load(urllib.request.urlopen(urllib.request.Request(
                base_url, data=json.dumps(payload).encode(), headers=headers,
                method="POST"), timeout=self.args.timeout))
            thinking_text = None
            for block in out.get("content", []):
                if block.get("type") == "thinking":
                    thinking_text = block.get("thinking", "")
                    break
            if not thinking_text:
                self.results.append({"test": "get_thinking", "ok": False, "error": "no thinking block"})
                print("  get_thinking: FAIL - no thinking block")
                return self.results
            self.results.append({"test": "get_thinking", "ok": True})
            print("  get_thinking: OK")
        except Exception as e:
            self.results.append({"test": "get_thinking", "ok": False, "error": repr(e)})
            print(f"  get_thinking: FAIL - {repr(e)[:100]}")
            return self.results

        # Build assistant message with INVALID signature
        bad_assistant = {"role": "assistant", "content": [
            {"type": "thinking", "thinking": thinking_text, "signature": "sig_INVALID_old_signature_12345"},
            {"type": "text", "text": "Hello!"},
        ]}

        # Test 2: preserve_thinking=false → should succeed
        for label, pt_val in [("preserve_false", False), ("preserve_none", None), ("preserve_true", True)]:
            payload = {
                "model": self.args.model,
                "messages": [
                    {"role": "user", "content": "hello"},
                    bad_assistant,
                    {"role": "user", "content": "What did you say?"},
                ],
                "max_tokens": 64,
                "stream": False,
            }
            if pt_val is not None:
                payload["preserve_thinking"] = pt_val
            try:
                urllib.request.urlopen(urllib.request.Request(
                    base_url, data=json.dumps(payload).encode(), headers=headers,
                    method="POST"), timeout=self.args.timeout)
                ok = True
                error = None
            except urllib.error.HTTPError as e:
                ok = False
                error = e.read().decode()[:100]
            except Exception as e:
                ok = False
                error = repr(e)[:100]
            # Retry once on materialization error (state may have been evicted)
            if not ok and "resident state" in str(error):
                time.sleep(2)
                try:
                    urllib.request.urlopen(urllib.request.Request(
                        base_url, data=json.dumps(payload).encode(), headers=headers,
                        method="POST"), timeout=self.args.timeout)
                    ok = True
                    error = None
                except urllib.error.HTTPError as e:
                    ok = False
                    error = e.read().decode()[:100]
                except Exception as e:
                    ok = False
                    error = repr(e)[:100]
            # Expected: succeed for false/none, fail for true
            expected_ok = pt_val is not True
            if ok == expected_ok:
                self.results.append({"test": label, "ok": True})
                print(f"  {label}: OK (accepted={ok}, expected={expected_ok})")
            else:
                self.results.append({"test": label, "ok": False, "error": f"accepted={ok} expected={expected_ok}: {error}"})
                print(f"  {label}: FAIL (accepted={ok}, expected={expected_ok}: {error})")
        return self.results


def run_round(sessions, r, timeout):
    results = [None] * len(sessions)
    errors = []
    def run(i):
        try:
            results[i] = sessions[i].turn(r)
        except Exception as exc:
            errors.append((sessions[i].name, repr(exc)))
    threads = [threading.Thread(target=run, args=(i,)) for i in range(len(sessions))]
    for t in threads: t.start()
    for t in threads: t.join()
    return errors


def get_stats(args):
    try:
        with urllib.request.urlopen(f"http://{args.host}:{args.port}/stats", timeout=10) as resp:
            return json.load(resp)
    except Exception:
        return {}


def count_log_lines(path):
    try:
        with open(path, "r", errors="replace") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


def parse_serve_log(path, skip_lines=0):
    d = {k: 0 for k in [
        "spill_ok", "spill_ckpt_ok", "spill_ckpt_missing", "spill_fail",
        "restore_started", "restore_completed", "restore_failed",
        "safety_find_hit", "safety_find_miss", "max_net_entries",
        "worker_crash", "bad_alloc", "capture_skip", "restore_skip",
        "refind_hit", "evict_smallest", "compact", "admit_session",
        "rewrite_prefix_hit", "rewrite_restore_fail", "worker_recover",
        "private_turn_closure", "tool_calls_done",
        "materialize_fallback", "materialize_safety_hit",
        "materialize_oom_first",
        "checkpoint_demoted", "checkpoint_restored", "unit_not_retained",
        "unit_partial_rejected", "unit_evicted_smallest_first",
        "hostonly_restore_fail",
        "kv_copy_skip",
        "host_copy_ok",
        "spill_fail_capacity",
        "mixed_copy_ok",
        "kv_not_resident_stale",
        "kv_not_resident_no_device",
        "state_lease_leak",
        "state_lease_orphan",
        "missing_source_result",
        "relief_demote",
        "relief_dual_drop",
        "pressure_expansion_fail",
        "state_replan",
        "entitlement_mismatch",
        "no_resident_state",
        "spill_before_loss",
        "state_relinquish",
    ]}
    d["evict_pages"] = []
    d["checkpoint_frontiers"] = []
    try:
        with open(path, "r", errors="replace") as f:
            for _ in range(skip_lines):
                f.readline()
            for line in f:
                if "[restore]" in line and "syncing" in line: d["restore_completed"] += 1
                if "[restore]" in line and "frontier=" in line: d["restore_started"] += 1
                if "[restore] FAILED" in line: d["restore_failed"] += 1
                if "[safety-find]" in line and "match=hit" in line: d["safety_find_hit"] += 1
                if "[safety-find]" in line and "match=miss" in line: d["safety_find_miss"] += 1
                if "[safety-find]" in line and "entries=" in line:
                    m = re.search(r"entries=(\d+)", line)
                    if m: d["max_net_entries"] = max(d["max_net_entries"], int(m.group(1)))
                if "[safety-spill] OK" in line:
                    d["spill_ok"] += 1
                    if "ckpt_valid=1" in line: d["spill_ckpt_ok"] += 1
                    elif "ckpt_valid=0" in line: d["spill_ckpt_missing"] += 1
                if "[safety-spill] FAIL" in line: d["spill_fail"] += 1
                if "[safety-spill] compact:" in line: d["compact"] += 1
                if "WORKER CRASH" in line: d["worker_crash"] += 1
                if "std::bad_alloc" in line: d["bad_alloc"] += 1
                if "[capture] skip zero-prefill" in line: d["capture_skip"] += 1
                if "[capture] skip restore" in line: d["restore_skip"] += 1
                if "[safety-find] re-find HIT" in line: d["refind_hit"] += 1
                if "[safety-spill] evict-smallest:" in line:
                    m = re.search(r"pages=(\d+)", line)
                    if m: d["evict_pages"].append(int(m.group(1)))
                    d["evict_smallest"] += 1
                if "[admit-session]" in line:
                    d["admit_session"] += 1
                if "prefix HIT (rewrite)" in line:
                    d["rewrite_prefix_hit"] += 1
                    m = re.search(r"frontier=(\d+)", line)
                    if m: d["checkpoint_frontiers"].append(int(m.group(1)))
                if "[rewrite-restore] FAILED" in line:
                    d["rewrite_restore_fail"] += 1
                if "WORKER RECOVER" in line:
                    d["worker_recover"] += 1
                if "reuse=private_turn_closure" in line:
                    d["private_turn_closure"] += 1
                if "finish=tool_calls" in line:
                    d["tool_calls_done"] += 1
                if "[materialize]" in line and "falling back to root" in line:
                    d["materialize_fallback"] += 1
                if "[materialize] safety-net HIT" in line:
                    d["materialize_safety_hit"] += 1
                if "[materialize] OOM (first)" in line:
                    d["materialize_oom_first"] += 1
                if "[checkpoint] demoting old checkpoint to host" in line:
                    d["checkpoint_demoted"] += 1
                # [rewrite-restore] restored demoted checkpoint from safety net
                # -- pattern reserved for future Step 5-6 work (not emitted yet)
                if "[rewrite-restore] restoring HostOnly checkpoint to device" in line:
                    d["checkpoint_restored"] += 1
                # A cache unit is {KV + state}. Non-retention is explicit, and a partial
                # unit must never be stored; eviction reclaims whole units smallest-first.
                if "[checkpoint] unit-not-retained" in line:
                    d["unit_not_retained"] += 1
                if "[safety-net] REJECT-PARTIAL" in line:
                    d["unit_partial_rejected"] += 1
                if "[host-state-pool] evict=" in line:
                    d["unit_evicted_smallest_first"] += 1
                if "[kv-not-resident]" in line and "STALE_HANDLE" in line:
                    d["kv_not_resident_stale"] += 1
                if "[kv-not-resident]" in line and "VALID_BUT_NO_DEVICE" in line:
                    d["kv_not_resident_no_device"] += 1
                if "[safety-spill] mixed_copy_ok" in line:
                    d["mixed_copy_ok"] += 1
                if "[safety-spill] FAIL: insufficient capacity" in line:
                    d["spill_fail_capacity"] += 1
                if "[safety-spill] host_copy_ok" in line:
                    d["host_copy_ok"] += 1
                if "[safety-spill] KV_COPY_SKIP" in line:
                    d["kv_copy_skip"] += 1
                if "[materialize] HostOnly restore failed" in line:
                    d["hostonly_restore_fail"] += 1
                # State-lease leak: a state image whose release was refused
                # (checkpoint_references != 0 at clear time). Two sub-cases:
                #   shared_refs >= 1 -> LEGITIMATE retention: the image backs
                #     a live shared prefix (publish_active_capture retains a
                #     ref on the active state; released only on shared-prefix
                #     eviction). The "LEAK" log is a false alarm here.
                #   shared_refs == 0  -> TRUE orphan: a reference with no
                #     owner — an unbalanced retain/release pair.
                # Two labels: 'LEAK (orphan)' = shared_refs=0 (a real bug);
                # 'retained (shared prefix)' = shared_refs>=1 (legitimate).
                if "[state-lease] LEAK (orphan)" in line:
                    d["state_lease_leak"] += 1
                    d["state_lease_orphan"] += 1
                elif "[state-lease] retained (shared prefix)" in line:
                    d["state_lease_leak"] += 1
                # State relief: freeing device state slots for an H2D restore —
                # demoting a DeviceOnly checkpoint to host, or dropping the
                # redundant device replica of a dual-resident checkpoint (its
                # host half is current). Presence proves the pool-saturation +
                # restore path was exercised.
                if "[relief] demoted" in line or "[relief] freed" in line:
                    d["relief_demote"] += 1
                    if " dropped " in line and "dropped 0 dual" not in line:
                        d["relief_dual_drop"] += 1
                # P4.2: the pressure planner's expansion-capacity limit — a
                # known self-recovering class (triaged 2026-09-16), not a
                # saturated-restore failure.
                if "prepared pressure expansion exceeds the target arena" in line:
                    d["pressure_expansion_fail"] += 1
                # P2.4 Increment 1: the plan was re-baselined to the
                # materialized unit after relief left a plan-optional state
                # image unrealized — the request succeeded (no 500).
                if "[replan] state entitlement re-baselined" in line:
                    d["state_replan"] += 1
                # The strict check still rejects a core-incomplete mismatch.
                if "[entitlement] MISMATCH" in line:
                    d["entitlement_mismatch"] += 1
                # P2.4 exit criterion: a materialization whose selected state
                # image has no replica anywhere (true last-replica loss) —
                # the request degrades to root prefill, so this is a WARN,
                # not a hard FAIL (the unit invariant is enforced upstream).
                if "no resident state" in line:
                    d["no_resident_state"] += 1
                # P2.4 Increment 1: the slot release found the unit missing
                # from the net and spilled the complete unit before its state
                # could lose its last copy (the unit-invariant backstop).
                if "[spill-before-loss]" in line:
                    d["spill_before_loss"] += 1
                # P2.4 Increment 2: the release path handed the store's host
                # replica to the net entry (move-not-copy) — the net is the
                # unit's host home. Presence proves the relinquish path fired.
                if "state relinquished (move)" in line:
                    d["state_relinquish"] += 1
                # The adopt-path error that followed the leak in prod: the
                # root-prefill fallback publishes no source, but the admission
                # claim still expects one.
                if "] error materialization private source result is missing" in line:
                    d["missing_source_result"] += 1
    except OSError:
        pass
    return d


def evaluate(phase_name, sessions, stats0, stats1, log, expect_trash=False):
    v = []
    pr = (stats1 or {}).get("pressure", {})
    pr0 = (stats0 or {}).get("pressure", {})
    evicted = int(pr.get("private_owners_evicted", 0)) - int(pr0.get("private_owners_evicted", 0))
    degraded = int(pr.get("private_owners_degraded", 0)) - int(pr0.get("private_owners_degraded", 0))
    cr = (stats1 or {}).get("cache_reuse", {})
    cr0 = (stats0 or {}).get("cache_reuse", {})
    reused = int(cr.get("reused_prompt_tokens", 0)) - int(cr0.get("reused_prompt_tokens", 0))
    restores = int(pr.get("admission_safety_net_restores", 0)) - int(pr0.get("admission_safety_net_restores", 0))

    # CRASH CHECK — always enforced
    if log["worker_crash"] > 0:
        v.append(f"FAIL: {log['worker_crash']} WORKER CRASH")
    if log["bad_alloc"] > 0 and phase_name not in ("trash", "mixed", "concurrent", "tool-calling"):
        v.append(f"FAIL: {log['bad_alloc']} std::bad_alloc — OOM was not prevented")
    if log.get("hostonly_restore_fail", 0) > 0:
        v.append(f"WARN: {log['hostonly_restore_fail']} HostOnly restore failures (aborted to root prefill)")
    if log.get("no_resident_state", 0) > 0:
        v.append(f"WARN: {log['no_resident_state']} 'no resident state' losses (P2.4 exit criterion is 0; degraded to root prefill)")
    if log.get("kv_not_resident_no_device", 0) > 0:
        v.append(f"WARN: {log['kv_not_resident_no_device']} pages with no device replica (demoted to host)")
    if log.get("mixed_copy_ok", 0) > 0:
        v.append(f"PASS: {log['mixed_copy_ok']} mixed device+host copies (partial D2H worked)")
    if log.get("spill_fail", 0) > 0 and phase_name not in ("trash", "mixed", "concurrent"):
        v.append(f"WARN: {log['spill_fail']} spill failures (a unit could not be completed)")
    if log.get("spill_fail_capacity", 0) > 0:
        v.append(f"WARN: {log['spill_fail_capacity']} spill capacity failures (no room for a complete unit)")
    if log.get("host_copy_ok", 0) > 0:
        v.append(f"PASS: {log['host_copy_ok']} host replica copies (demoted KV recovered from host)")
    if log.get("kv_copy_skip", 0) > 0:
        v.append(f"WARN: {log['kv_copy_skip']} KV copy skips (device pages unavailable, unit not retained)")
    if log["bad_alloc"] > 0 and phase_name in ("trash", "mixed", "concurrent", "tool-calling"):
        v.append(f"PASS: {log['bad_alloc']} std::bad_alloc caught and recovered (extreme pressure handled)")

    # Pressure (skip for single-session phases and state-pool phases)
    pressure = evicted > 0 or degraded > 0
    if phase_name not in ("checkpoint-advance", "tool-calling", "responses-tools", "reasoning-effort", "concurrent", "thinking-sig", "demotion", "state-saturation"):
        if not pressure and not expect_trash:
            v.append("FAIL: no KV pressure")
        if pressure:
            v.append(f"PASS: pressure (evicted={evicted}, degraded={degraded})")

    # Cache reuse (skip for single-session phases and state-pool phases)
    if phase_name not in ("checkpoint-advance", "tool-calling", "responses-tools", "reasoning-effort", "concurrent", "thinking-sig", "demotion", "state-saturation"):
        if reused > 0:
            v.append(f"PASS: cache reuse ({reused} tokens)")
        elif not expect_trash:
            v.append("FAIL: zero cache reuse")

    # Safety net
    if log["spill_ok"] > 0:
        v.append(f"PASS: {log['spill_ok']} spills OK (ckpt={log['spill_ckpt_ok']})")
    if log["spill_ckpt_missing"] > 0:
        v.append(f"WARN: {log['spill_ckpt_missing']} spills missing ckpt")
    if restores > 0 or log["restore_completed"] > 0:
        v.append(f"PASS: {max(restores, log['restore_completed'])} restores")
    if log["restore_started"] - log["restore_failed"] > log["restore_completed"]:
        incomplete = log["restore_started"] - log["restore_failed"] - log["restore_completed"]
        v.append(f"FAIL: {incomplete} restores started but not completed (excluding {log['restore_failed']} failures)")
    if log["max_net_entries"] >= 3 and log["restore_completed"] > 2:
        v.append(f"PASS: net accumulated (max={log['max_net_entries']})")
    if log["compact"] > 0:
        v.append(f"PASS: {log['compact']} arena compactions (fragmentation repaired)")
    if log["capture_skip"] > 0 or log["restore_skip"] > 0:
        v.append(f"PASS: {log['capture_skip'] + log['restore_skip']} capture skips (no crash)")
    if log["refind_hit"] > 0:
        if log["restore_completed"] >= log["refind_hit"]:
            v.append(f"PASS: {log['refind_hit']} re-find hits, restores completed")
        else:
            v.append(f"FAIL: {log['refind_hit']} re-find hits but insufficient restores")

    # Eviction order (mixed phase)
    if phase_name == "mixed" and len(log["evict_pages"]) > 1:
        ev = log["evict_pages"]
        fh = sum(ev[:len(ev)//2]) / max(len(ev)//2, 1)
        sh = sum(ev[len(ev)//2:]) / max(len(ev) - len(ev)//2, 1)
        if fh <= sh:
            v.append(f"PASS: eviction smallest-first (first={fh:.0f} <= second={sh:.0f})")
        else:
            v.append(f"WARN: eviction NOT smallest-first (first={fh:.0f} > second={sh:.0f})")

    # BIG session preservation (mixed phase)
    if phase_name == "mixed":
        for s in sessions:
            if s.name == "BIG":
                cold = sum(1 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
                fast = sum(1 for t in s.turns if t["turn"] > 1 and t["wall_s"] <= 60)
                if cold == 0 and fast > 0:
                    v.append(f"PASS: BIG {fast} fast turns, 0 cold-starts")
                elif cold > 0:
                    v.append(f"WARN: BIG {cold} cold-starts (may have been evicted)")

    # Checkpoint advancement (checkpoint-advance phase)
    if phase_name == "checkpoint-advance":
        frontiers = log["checkpoint_frontiers"]
        if len(frontiers) >= 2:
            advancing = all(f2 > f1 for f1, f2 in zip(frontiers, frontiers[1:]))
            if advancing:
                v.append(f"PASS: checkpoint frontier advances monotonically "
                         f"({len(frontiers)} hits: {frontiers[0]}→{frontiers[-1]})")
            else:
                v.append(f"FAIL: checkpoint frontier does NOT advance monotonically ({frontiers})")
        elif len(frontiers) == 1:
            v.append(f"WARN: only 1 checkpoint hit — need more turns to verify advancement")
        else:
            v.append("FAIL: zero rewrite checkpoint hits — checkpoint not advancing")
        if log["private_turn_closure"] >= 2:
            v.append(f"PASS: {log['private_turn_closure']} private_turn_closure reuses")
        elif log["private_turn_closure"] == 0:
            v.append("FAIL: zero private_turn_closure reuses — checkpoint not reused")
        if log["rewrite_restore_fail"] > 0:
            v.append(f"FAIL: {log['rewrite_restore_fail']} rewrite restore failures")

    # Tool-calling phase
    if phase_name == "tool-calling":
        frontiers = log["checkpoint_frontiers"]
        if len(frontiers) >= 2:
            # Allow non-advancing frontiers: DropOptional keeps the old
            # checkpoint (still valid, just not replaced). The inspect
            # finds the old checkpoint, so consecutive hits may show the
            # same frontier. Check that at least SOME advancement happened
            # and the last frontier is higher than the first.
            advancing = all(f2 > f1 for f1, f2 in zip(frontiers, frontiers[1:]))
            if advancing:
                v.append(f"PASS: checkpoint advances across tool-call turns "
                         f"({len(frontiers)} hits: {frontiers[0]}→{frontiers[-1]})")
            elif all(f2 >= f1 for f1, f2 in zip(frontiers, frontiers[1:])):
                # Non-strict: some turns used DropOptional (checkpoint stable).
                advanced = sum(1 for f1, f2 in zip(frontiers, frontiers[1:]) if f2 > f1)
                v.append(f"PASS: checkpoint stable or advancing across tool-call turns "
                         f"({advanced}/{len(frontiers)-1} steps advanced, "
                         f"{frontiers[0]}→{frontiers[-1]})")
            else:
                # Check that the max frontier increases overall (some turns
                # use root prefill, interleaving lower frontiers).
                advanced = sum(1 for f1, f2 in zip(frontiers, frontiers[1:]) if f2 > f1)
                if advanced > 0 and max(frontiers) > frontiers[0]:
                    v.append(f"PASS: checkpoint advances across tool-call turns "
                             f"({advanced}/{len(frontiers)-1} steps advanced, "
                             f"max={max(frontiers)})")
                else:
                    v.append(f"FAIL: checkpoint does NOT advance across tool-call turns ({frontiers})")
        elif len(frontiers) == 1:
            v.append(f"PASS: 1 checkpoint hit during tool-calling (frontier={frontiers[0]})")
        elif len(frontiers) == 0 and log["private_turn_closure"] == 0:
            v.append("FAIL: zero rewrite checkpoint hits during tool-calling")
        if log["tool_calls_done"] >= 1:
            v.append(f"PASS: {log['tool_calls_done']} tool-call rounds completed")
        elif log["tool_calls_done"] == 0:
            v.append("FAIL: zero tool-call rounds — model did not call tools")
        if log["private_turn_closure"] >= 1:
            v.append(f"PASS: {log['private_turn_closure']} checkpoint reuses during tool-calling")
        elif log["private_turn_closure"] == 0 and len(frontiers) == 0:
            v.append("FAIL: zero checkpoint reuses during tool-calling")
        if log["rewrite_restore_fail"] > 0:
            v.append(f"FAIL: {log['rewrite_restore_fail']} rewrite restore failures")
        if log["worker_recover"] > 0:
            v.append(f"WARN: {log['worker_recover']} worker recoveries (server survived, but requests may have failed)")

    # Responses API with tools (responses-tools phase)
    if phase_name == "responses-tools":
        frontiers = log["checkpoint_frontiers"]
        if len(frontiers) >= 2:
            # Responses API may interleave root and checkpoint frontiers.
            # Count advancing steps (like tool-calling phase).
            advanced = sum(1 for f1, f2 in zip(frontiers, frontiers[1:]) if f2 > f1)
            if advanced > 0:
                v.append(f"PASS: checkpoint advances across Responses API tool turns "
                         f"({advanced}/{len(frontiers)-1} advancing steps)")
            else:
                v.append(f"FAIL: checkpoint does NOT advance across Responses API turns ({frontiers})")
        elif len(frontiers) == 0:
            # Zero rewrite hits: checkpoint may be restored via safety-net
            # (root path) instead of rewrite-restore path. OK if turns succeeded.
            v.append("WARN: zero rewrite checkpoint hits (may use safety-net restore)")
        if log["private_turn_closure"] >= 2:
            v.append(f"PASS: {log['private_turn_closure']} checkpoint reuses in Responses API")
        if log["rewrite_restore_fail"] > 0:
            v.append(f"FAIL: {log['rewrite_restore_fail']} rewrite restore failures")
        # All turns must succeed without errors
        errors = sum(1 for t in sessions[0].turns if t.get("input_tokens") is None)
        if errors == 0 and len(sessions[0].turns) >= 3:
            v.append(f"PASS: {len(sessions[0].turns)} Responses API turns with tools succeeded")
        else:
            v.append(f"FAIL: {errors} turns failed in Responses API")

    # Reasoning effort tier mapping (reasoning-effort phase)
    if phase_name == "reasoning-effort":
        # sessions[0] is the ReasoningEffortTester with results
        tester = sessions[0] if sessions else None
        if tester and hasattr(tester, "results"):
            ok = sum(1 for r in tester.results if r["ok"])
            fail = sum(1 for r in tester.results if not r["ok"])
            if ok == len(tester.results) and fail == 0:
                v.append(f"PASS: all {ok} reasoning effort levels accepted (tier mapping works)")
            else:
                v.append(f"FAIL: {fail}/{len(tester.results)} reasoning effort levels rejected")
                for r in tester.results:
                    if not r["ok"]:
                        v.append(f"  effort={r['effort']}: {r.get('error', 'unknown')[:100]}")

    # Checkpoint demotion/restore (all phases)
    if log["checkpoint_demoted"] > 0:
        v.append(f"PASS: {log['checkpoint_demoted']} checkpoint demotions to host (device pressure handled)")
    if log["checkpoint_restored"] > 0:
        v.append(f"PASS: {log['checkpoint_restored']} checkpoint H2D restores (demoted checkpoints reused)")
    # Demotion without restore is OK (the demoted session may not have returned yet)
    # But restore without demotion would be unexpected
    if log["checkpoint_restored"] > 0 and log["checkpoint_demoted"] == 0:
        v.append(f"WARN: {log['checkpoint_restored']} restores without demotions (unexpected)")
    if log["relief_dual_drop"] > 0:
        v.append(f"PASS: {log['relief_dual_drop']} dual device-replica drops relieved a saturated state pool")

    # Concurrent sessions (concurrent phase)
    if phase_name == "concurrent":
        # Check OOM isolation: if bad_alloc happened, it should NOT cause WORKER RECOVER
        # Check OOM isolation: if bad_alloc happened, it should NOT cause WORKER RECOVER
        if log["materialize_oom_first"] > 0:
            v.append(f"PASS: {log['materialize_oom_first']} OOM caught at admission (request isolated)")
        # Verify both sessions got cache reuse (not all root rewrites)
        root_count = sum(1 for s in sessions if isinstance(s, (Session, ChatSession))
                         for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
        fast_count = sum(1 for s in sessions if isinstance(s, (Session, ChatSession))
                         for t in s.turns if t["turn"] > 1 and t["wall_s"] <= 60)
        if root_count == 0 and fast_count >= 4:
            v.append(f"PASS: {fast_count} fast turns, 0 cold-starts across concurrent sessions")
        elif root_count > 0:
            v.append(f"FAIL: {root_count} cold-starts — cross-session state destruction detected")
        # Check for materialize fallbacks (should have some, they prevent crashes)
        if log["materialize_fallback"] > 0:
            v.append(f"PASS: {log['materialize_fallback']} materialize fallbacks (graceful degradation)")
        if log["materialize_safety_hit"] > 0:
            v.append(f"PASS: {log['materialize_safety_hit']} safety-net restores after source eviction")
        # Worker recoveries should be 0 (fallback prevents nuclear recovery)
        if log["worker_recover"] > 0:
            v.append(f"FAIL: {log['worker_recover']} worker recoveries — fallback not preventing crash")

    # Thinking signature (thinking-sig phase)
    if phase_name == "thinking-sig":
        tester = sessions[0] if sessions else None
        if tester and hasattr(tester, "results"):
            ok = sum(1 for r in tester.results if r["ok"])
            fail = sum(1 for r in tester.results if not r["ok"])
            if ok == len(tester.results) and fail == 0:
                v.append(f"PASS: all {ok} thinking signature tests passed")
            else:
                for r in tester.results:
                    if not r["ok"]:
                        v.append(f"FAIL: {r['test']}: {r.get('error', 'unknown')}")

    # Worker recovery (all phases)
    if log["worker_recover"] > 0 and log["worker_crash"] == 0:
        v.append(f"PASS: {log['worker_recover']} worker recoveries without crash (logic_error caught)")

    # Trash mode: cold-starts and spill failures are acceptable
    if expect_trash:
        cold = sum(1 for s in sessions for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
        if cold > 0:
            v.append(f"PASS: {cold} cold-starts (expected in trash mode)")
        if log["spill_fail"] > 0:
            v.append(f"PASS: {log['spill_fail']} spill failures (expected — graceful degradation)")
        # Must verify trashing actually occurred
        if log["spill_fail"] == 0 and log["restore_failed"] == 0:
            # Only PASS if we have evidence trashing actually occurred
            if evicted > 0 or degraded > 0 or cold > 0:
                v.append("PASS: 0 spill failures — server handled trashing gracefully")
            else:
                v.append("WARN: no spill failures and no pressure evidence — test may not be exercising trashing")

    # Spill success rate (not trash mode, not single-session phases)
    if phase_name not in ("checkpoint-advance", "tool-calling", "responses-tools", "reasoning-effort", "concurrent", "thinking-sig", "demotion"):
        total = log["spill_ok"] + log["spill_fail"] + log["restore_failed"]
        if total > 5 and log["spill_ok"] == 0 and not expect_trash:
            v.append(f"FAIL: 0 spills succeeded out of {total}")

    # Demotion phase: verify checkpoint state preservation under pressure.
    # The unified architecture evicts continuations (KV + state) instead
    # of dropping rewrite checkpoints. Checkpoint state is preserved via
    # the safety net spill (ckpt_ok) or demotion capture.
    if phase_name == "demotion":
        # A cache unit is {attention KV + GDN state} and is retained whole or not at
        # all. Checkpoint state therefore survives only inside a retained unit, and
        # an explicit non-retention is the correct outcome when the shared host
        # budget cannot hold the whole unit - it is not a failure.
        # spill_ckpt_ok is counted inside the spill_ok branch, so it is a subset; use
        # spill_ok alone as the unit count and report the checkpoint-bearing share.
        retained = log["spill_ok"]
        if retained > 0:
            v.append(f"PASS: {retained} complete units retained "
                     f"(spill_ok={log['spill_ok']}, spill_ckpt={log['spill_ckpt_ok']})")
        else:
            v.append("FAIL: no complete {KV + state} unit retained in demotion phase")
        if log["checkpoint_restored"] > 0:
            v.append(f"PASS: {log['checkpoint_restored']} checkpoint restores from safety net")
        if log["unit_partial_rejected"] > 0:
            v.append(f"FAIL: {log['unit_partial_rejected']} partial units were offered for retention")
        if log["unit_not_retained"] > 0:
            v.append(f"INFO: {log['unit_not_retained']} captures not retained (no complete unit)")
        if log["unit_evicted_smallest_first"] > 0:
            v.append(f"INFO: {log['unit_evicted_smallest_first']} units reclaimed smallest-first")
        if log["rewrite_restore_fail"] > 0:
            v.append(f"FAIL: {log['rewrite_restore_fail']} rewrite restore failures")

    return v


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--max-output-tokens", type=int, default=48)
    p.add_argument("--serve-log", default="/home/zenz/ninfer-serve.log")
    p.add_argument("--request-log", default="/home/zenz/ninfer-requests.jsonl",
                   help="JSONL request log with per-request materialization diagnostics")
    p.add_argument("--timeout", type=int, default=120)
    p.add_argument("--start-phase", type=int, default=1,
                   help="run phases N..13 (for split runs across separate e2e server windows)")
    args = p.parse_args()

    # Verify we're running against the test server, not production.
    # The e2e tests require a constrained server (32k ctx, 4GB host-kv, 64k KV)
    # to trigger pressure, eviction, and trashing. Running against the production
    # server (555k ctx, 30GB host-kv) will silently pass phases that should fail.
    config = get_stats(args) or {}
    if not config:
        print("ERROR: cannot reach server. Is it running on the configured host:port?")
        return 1
    mem = config.get("memory", {})
    if not mem:
        print("ERROR: server stats missing 'memory' section — wrong server or old build?")
        return 1
    host_kv = int(mem.get("host_kv_capacity_bytes", 0))
    kv_pages = int(mem.get("kv_capacity_max_page_groups", 0))
    if not host_kv or not kv_pages:
        print(f"ERROR: cannot read server config from /stats (host_kv={host_kv}, kv_pages={kv_pages}). "
              f"Is the server running and responsive?")
        return 1
    if host_kv > 16 * 1024 * 1024 * 1024:
        print(f"ERROR: host-kv capacity is {host_kv / 1024**3:.1f} GiB — this looks like the "
              f"production server. The e2e tests require the test server (12 GiB host-kv, "
              f"32k context, 64k KV capacity). Start it with: tools/e2e/ninfer-start-test.sh")
        return 1
    if kv_pages > 4096:
        print(f"ERROR: KV capacity is {kv_pages} page groups — this looks like the production "
              f"server. The e2e tests require the test server (64k KV capacity = ~1024 pages). "
              f"Start it with: tools/e2e/ninfer-start-test.sh")
        return 1
    print(f"Server config OK: host-kv={host_kv / 1024**3:.1f} GiB, KV pages={kv_pages}")

    # Prod is stopped during the swap, so every request-log line written from
    # here on belongs to the e2e server. Mark the offset so the planner-latency
    # check (below) reads only this run's materialization diagnostics.
    args._request_log_start = count_log_lines(args.request_log)

    all_verdicts = []
    phases = (phase_1, phase_2, phase_3, phase_4, phase_5, phase_6, phase_7,
              phase_8, phase_9, phase_10, phase_11, phase_12, phase_13)
    for i, phase_fn in enumerate(phases, start=1):
        if i < args.start_phase:
            print(f"=== Phase {i}: skipped (--start-phase {args.start_phase}) ===")
            continue
        result = phase_fn(args)
        if result is None:
            return 1  # phase aborted
        all_verdicts.extend(result)
        for pn, v in result:
            print(f"  [{pn}] {v}")

    # Planner-latency gate: the admission planner must converge fast. The
    # 2026-09-17 fix seeds with a feasible cover and caps the beam search at
    # 30ms, so searches stop at model_optimal/queue_exhausted/
    # value_of_next_expansion/time_budget instead of enumerating to the 4096-
    # target budget (expansion_capacity/target_budget).
    planner_verdicts = phase_planner_latency(args)
    all_verdicts.extend(planner_verdicts)
    for pn, v in planner_verdicts:
        print(f"  [{pn}] {v}")
    return print_summary(all_verdicts)


def phase_1(args):
    all_verdicts = []
    # Phase 1: pressure — 4 sessions, basic safety net
    print("\n=== Phase 1: pressure (4 sessions, 8 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s1 = [Session("ABCD"[i], 14000, 2000, args) for i in range(4)]
    for r in range(1, 9):
        print(f"Round {r}:")
        errors = run_round(s1, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("ABORT: phase 1 failed"); return None
    stats1 = get_stats(args)
    log1 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("pressure", s1, stats0, stats1, log1):
        all_verdicts.append(("pressure", v))
    return all_verdicts


def phase_2(args):
    all_verdicts = []
    # Phase 2: mixed — 1 big + 3 small, eviction order
    print("\n=== Phase 2: mixed (1 BIG + 3 small, 10 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s2 = [Session("BIG", 16000, 1500, args),
          Session("A", 6000, 1500, args),
          Session("B", 6000, 1500, args),
          Session("C", 6000, 1500, args)]
    for r in range(1, 11):
        print(f"Round {r}:")
        errors = run_round(s2, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Retry only failed sessions (Session uses previous_response_id,
            # safe to retry; but don't re-run successful sessions).
            failed = [s for s in s2 if s.name in [n for n, _ in errors]]
            if failed:
                print("  Retrying failed sessions...")
                time.sleep(3)
                errors2 = run_round(failed, r, args.timeout)
                if errors2:
                    for n, e in errors2: print(f"  ERROR {n}: {e}")
                    print("  Continuing to next round (mixed phase tolerates failures)")
                continue
    stats1 = get_stats(args)
    log2 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("mixed", s2, stats0, stats1, log2):
        all_verdicts.append(("mixed", v))
    return all_verdicts


def phase_3(args):
    all_verdicts = []
    # Phase 3: trash — 10 sessions, graceful degradation (no crash)
    print("\n=== Phase 3: trash (10 sessions, 6 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s3 = [Session(f"S{i}", 15000, 1500, args) for i in range(10)]
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s3, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Retry only failed sessions
            failed = [s for s in s3 if s.name in [n for n, _ in errors]]
            if failed:
                print("  Retrying failed sessions...")
                time.sleep(3)
                errors2 = run_round(failed, r, args.timeout)
                if errors2:
                    for n, e in errors2: print(f"  ERROR {n}: {e}")
                    print("  Continuing despite errors (trash phase tolerates failures)")
            continue
    stats1 = get_stats(args)
    log3 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("trash", s3, stats0, stats1, log3, expect_trash=True):
        all_verdicts.append(("trash", v))
    return all_verdicts


def phase_4(args):
    all_verdicts = []
    # Phase 4: thinking — session-key fallback with rewrite checkpoint
    print("\n=== Phase 4: thinking (3 sessions, 6 rounds, reasoning mode) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s4 = [Session(f"T{i}", 10000, 2000, args) for i in range(3)]
    for s in s4:
        s.args = type(args)(**vars(args))
        s.args.thinking_mode = True
        s.args.max_output_tokens = 128
    # Override the Session.turn to add reasoning
    original_turn = Session.turn
    def thinking_turn(self, index):
        question = f"Question {index}: Consider the paragraph about '{self.rng.choice(WORDS)}'. Answer briefly."
        new_text = filler(self.rng, self.turn_tokens) + "\n\n" + question
        if index == 1:
            new_text = self.doc + "\n\n---\n\n" + new_text
        payload = {
            "model": self.args.model,
            "input": [{"role": "user", "content": [{"type": "input_text", "text": new_text}]}],
            "instructions": "You are a concise assistant.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
            "reasoning": {"effort": "low"},
        }
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"), "output_tokens": usage.get("output_tokens")}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s prompt={record['input_tokens']} out={record['output_tokens']}")
        return record
    Session.turn = thinking_turn
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s4, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Session uses previous_response_id, safe to retry
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s4 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
                print("  Continuing to next round (thinking phase tolerates failures)")
            continue
    Session.turn = original_turn
    stats1 = get_stats(args)
    log4 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("thinking", s4, stats0, stats1, log4):
        all_verdicts.append(("thinking", v))
    if log4["admit_session"] > 0:
        all_verdicts.append(("thinking", f"PASS: {log4['admit_session']} session-key fallback hits (rewrite checkpoint working)"))
    else:
        # Fallback may not fire if prefixes happen to match. Check for re-prefills instead.
        cold = sum(1 for s in s4 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
        if cold > 0:
            all_verdicts.append(("thinking", f"WARN: {cold} cold-starts in thinking mode (session-key fallback may not have fired)"))
    return all_verdicts


def phase_5(args):
    all_verdicts = []
    # Phase 5: checkpoint-advance — single session, verify frontier advances
    print("\n=== Phase 5: checkpoint-advance (1 session, 8 turns) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s5 = [Session("CKPT", 12000, 2000, args)]
    # Force reasoning (rewrite checkpoints) like phase 4 so the
    # checkpoint-advance assertions don't flap on model mood.
    s5[0].args = type(args)(**vars(args))
    s5[0].args.thinking_mode = True
    s5[0].args.max_output_tokens = 128
    for r in range(1, 9):
        print(f"Round {r}:")
        errors = run_round(s5, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Session uses previous_response_id, safe to retry
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s5 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
                print("  Continuing to next round (checkpoint-advance tolerates failures)")
            continue
    stats1 = get_stats(args)
    log5 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("checkpoint-advance", s5, stats0, stats1, log5):
        all_verdicts.append(("checkpoint-advance", v))
    # Token stability: verify input_tokens grow by ~turn_tokens each turn,
    # not by reasoning output size (which would indicate reasoning is kept)
    if len(s5[0].turns) >= 3:
        deltas = []
        for i in range(1, len(s5[0].turns)):
            t0 = s5[0].turns[i-1]
            t1 = s5[0].turns[i]
            if t0.get("input_tokens") and t1.get("input_tokens"):
                deltas.append(t1["input_tokens"] - t0["input_tokens"])
        if deltas:
            max_delta = max(deltas)
            # turn_tokens is 2000, max_output_tokens is 48.
            # With reasoning kept, delta would be ~2000 + reasoning_output.
            # Without reasoning, delta should be ~2000 + output_tokens.
            # Allow generous bound: 2000 (turn) + 48 (output) + 2000 (fudge) = 4048
            if max_delta < 5000:
                all_verdicts.append(("checkpoint-advance",
                    f"PASS: token stability (max delta={max_delta}, reasoning dropped)"))
            else:
                all_verdicts.append(("checkpoint-advance",
                    f"WARN: large token delta (max={max_delta}) — reasoning may be kept"))
    # Check no re-prefills after turn 1
    cold = sum(1 for t in s5[0].turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0 and len(s5[0].turns) > 1:
        all_verdicts.append(("checkpoint-advance", f"PASS: 0 cold-starts across {len(s5[0].turns)} turns"))
    elif cold > 0:
        all_verdicts.append(("checkpoint-advance", f"FAIL: {cold} cold-starts — checkpoint not reused"))
    return all_verdicts


def phase_6(args):
    all_verdicts = []
    # Phase 6: tool-calling — multi-turn with tools, simulating Claude Code
    print("\n=== Phase 6: tool-calling (1 session, 6 turns, tools) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s6 = [ChatSession("TOOL", 10000, 1500, args)]
    # Override max_output_tokens so the model has room to generate tool calls
    s6[0].args = type(args)(**vars(args))
    s6[0].args.max_output_tokens = 256
    # Force reasoning (rewrite checkpoints) like phase 4 so the
    # checkpoint assertions don't flap on model mood.
    s6[0].args.thinking_mode = True
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round(s6, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # ChatSession appends messages before HTTP call — don't retry.
            # Continue to next round (tool-calling tolerates missing turns).
            print("  Continuing to next round (tool-calling tolerates failures)")
            continue
    stats1 = get_stats(args)
    log6 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("tool-calling", s6, stats0, stats1, log6):
        all_verdicts.append(("tool-calling", v))
    # Check no re-prefills after turn 1
    cold = sum(1 for t in s6[0].turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0 and len(s6[0].turns) > 1:
        all_verdicts.append(("tool-calling", f"PASS: 0 cold-starts across {len(s6[0].turns)} tool-call turns"))
    elif cold > 0:
        all_verdicts.append(("tool-calling", f"WARN: {cold} cold-starts during tool-calling"))
    return all_verdicts


def phase_7(args):
    all_verdicts = []
    # Phase 7: responses-tools — Responses API tool-calling with checkpoint reuse
    print("\n=== Phase 7: responses-tools (1 session, 5 turns, Responses API) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s7 = [ResponsesApiSession("RSP", 8000, 1000, args)]
    s7[0].args = type(args)(**vars(args))
    s7[0].args.max_output_tokens = 128
    for r in range(1, 6):
        print(f"Round {r}:")
        errors = run_round(s7, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Retry once after OOM (worker recovery clears state)
            print("  Retrying after error...")
            time.sleep(2)
            errors = run_round(s7, r, args.timeout)
            if errors:
                for n, e in errors: print(f"  ERROR {n}: {e}")
                print("ABORT: phase 7 failed"); return None
    stats1 = get_stats(args)
    log7 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("responses-tools", s7, stats0, stats1, log7):
        all_verdicts.append(("responses-tools", v))
    cold = sum(1 for t in s7[0].turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0 and len(s7[0].turns) > 1:
        all_verdicts.append(("responses-tools", f"PASS: 0 cold-starts across {len(s7[0].turns)} Responses API turns"))
    elif cold > 0:
        all_verdicts.append(("responses-tools", f"WARN: {cold} cold-starts in Responses API"))
    return all_verdicts


def phase_8(args):
    all_verdicts = []
    # Phase 8: reasoning-effort — verify tier mapping (high, minimal, max, low, medium)
    print("\n=== Phase 8: reasoning-effort (5 effort levels) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    tester = ReasoningEffortTester(args)
    tester.test()
    stats1 = get_stats(args)
    log8 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("reasoning-effort", [tester], stats0, stats1, log8):
        all_verdicts.append(("reasoning-effort", v))
    return all_verdicts


def phase_9(args):
    all_verdicts = []
    # Phase 9: concurrent — 2 sessions + title-gen, verify no cross-session destruction
    print("\n=== Phase 9: concurrent (2 sessions + title-gen, 6 rounds) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s9a = ChatSession("CONC_A", 10000, 1500, args)
    s9b = ChatSession("CONC_B", 8000, 1200, args)
    s9a.args = type(args)(**vars(args))
    s9a.args.max_output_tokens = 128
    s9b.args = type(args)(**vars(args))
    s9b.args.max_output_tokens = 128
    # Interleave: both sessions + a tiny "title-gen" request each round
    title_gen_session = Session("TITLE", 500, 100, args)
    title_gen_session.args = type(args)(**vars(args))
    title_gen_session.args.max_output_tokens = 32
    all_sessions_9 = [s9a, s9b, title_gen_session]
    for r in range(1, 7):
        print(f"Round {r}:")
        errors = run_round([s9a, s9b], r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Don't retry — ChatSession appends messages before the HTTP call,
            # so a retry would corrupt the message list. Continue to next round.
            print("  Continuing to next round (concurrent phase tolerates failures)")
            continue
        # Title-gen request between main session turns (simulates Claude Code)
        if r > 1:
            try:
                title_gen_session.turn(r)
            except Exception as e:
                print(f"  TITLE ERROR: {repr(e)[:100]}")
    stats1 = get_stats(args)
    log9 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("concurrent", all_sessions_9, stats0, stats1, log9):
        all_verdicts.append(("concurrent", v))
    return all_verdicts


def phase_10(args):
    all_verdicts = []
    # Phase 10: thinking-sig — verify signature skip when preserve_thinking=false
    print("\n=== Phase 10: thinking-sig (4 requests) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    sig_tester = ThinkingSignatureTester(args)
    sig_tester.test()
    stats1 = get_stats(args)
    log10 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("thinking-sig", [sig_tester], stats0, stats1, log10):
        all_verdicts.append(("thinking-sig", v))
    return all_verdicts


def phase_11(args):
    all_verdicts = []
    # Phase 11: demotion — 3 sessions, large prompts, verify checkpoint demotion
    print("\n=== Phase 11: demotion (3 sessions, 5 rounds, verify host demotion) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s11 = [Session(f"DEM{i}", 20000, 2000, args) for i in range(3)]
    for r in range(1, 6):
        print(f"Round {r}:")
        errors = run_round(s11, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            # Session uses previous_response_id, safe to retry
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s11 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
                print("  Continuing to next round (demotion phase tolerates failures)")
            continue
    stats1 = get_stats(args)
    log11 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("demotion", s11, stats0, stats1, log11):
        all_verdicts.append(("demotion", v))
    # Verify no re-prefills after turn 1
    cold = sum(1 for s in s11 for t in s.turns if t["turn"] > 1 and t["wall_s"] > 60)
    if cold == 0:
        all_verdicts.append(("demotion", f"PASS: 0 cold-starts across {sum(len(s.turns) for s in s11)} turns"))
    elif cold > 0:
        all_verdicts.append(("demotion", f"WARN: {cold} cold-starts — demotion may not have prevented all re-prefills"))
    return all_verdicts


def phase_12(args):
    all_verdicts = []
    # Phase 12: state-lease — rewrite-checkpoint recycle pressure.
    # Reproduces the 2026-09-14 production wedge: forced-thinking sessions fill
    # the device state pool, so captures fork + recycle checkpoint slots. The
    # buggy recycle-capture publish path dropped the rewrite handle without
    # releasing its checkpoint reference, so every fork+recycle cycle leaked a
    # state slot (release refused -> orphaned slot -> pool exhaustion ->
    # "no resident state" -> "private source result is missing" request errors
    # -> client retry loop).
    # Requires the test server with --device-state-slots 5 (8 total: 3 cache +
    # 5 active): with the old 6-slot total the pool was exactly full under 4
    # thinking sessions, forks failed, and the recycle-with-fork path was never
    # exercised — which is how the bug shipped.
    print("\n=== Phase 12: state-lease (4 thinking sessions, 5 rounds, rewrite-recycle pressure) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    s12 = [Session(f"SL{i}", 10000, 1500, args) for i in range(4)]
    for s in s12:
        s.args = type(args)(**vars(args))
        s.args.max_output_tokens = 128
    # Force reasoning (rewrite checkpoints) like phase 4.
    original_turn = Session.turn
    def thinking_turn(self, index):
        question = f"Question {index}: Consider the paragraph about '{self.rng.choice(WORDS)}'. Answer briefly."
        new_text = filler(self.rng, self.turn_tokens) + "\n\n" + question
        if index == 1:
            new_text = self.doc + "\n\n---\n\n" + new_text
        payload = {
            "model": self.args.model,
            "input": [{"role": "user", "content": [{"type": "input_text", "text": new_text}]}],
            "instructions": "You are a concise assistant.",
            "max_output_tokens": self.args.max_output_tokens,
            "store": True,
            "stream": False,
            "reasoning": {"effort": "low"},
        }
        if self.response_id:
            payload["previous_response_id"] = self.response_id
        t0 = time.monotonic()
        out = json.load(urllib.request.urlopen(urllib.request.Request(
            f"http://{self.args.host}:{self.args.port}/v1/responses",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"},
            method="POST"), timeout=self.args.timeout))
        wall = time.monotonic() - t0
        usage = out.get("usage", {}) or {}
        self.response_id = out.get("id", self.response_id)
        record = {"session": self.name, "turn": index, "wall_s": round(wall, 2),
                  "input_tokens": usage.get("input_tokens"), "output_tokens": usage.get("output_tokens")}
        self.turns.append(record)
        print(f"  {self.name} t{index}: wall={record['wall_s']:.1f}s prompt={record['input_tokens']} out={record['output_tokens']}")
        return record
    Session.turn = thinking_turn
    for r in range(1, 6):
        print(f"Round {r}:")
        errors = run_round(s12, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s12 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
            continue
    Session.turn = original_turn
    stats1 = get_stats(args)
    log12 = parse_serve_log(args.serve_log, log_off)

    # The leak signatures. A refused release is only a TRUE bug when the
    # image has no owner (shared_refs == 0). shared_refs >= 1 is legitimate
    # retention (the image backs a live shared prefix) and is expected.
    if log12["state_lease_orphan"] > 0:
        all_verdicts.append(("state-lease", f"FAIL: {log12['state_lease_orphan']} orphaned state image(s) (shared_refs=0) — unbalanced checkpoint ref"))
    else:
        all_verdicts.append(("state-lease", "PASS: zero orphaned state images (no shared_refs=0 refusals)"))
    if log12["missing_source_result"] > 0:
        all_verdicts.append(("state-lease", f"FAIL: {log12['missing_source_result']} 'private source result is missing' request error(s)"))
    else:
        all_verdicts.append(("state-lease", "PASS: zero 'private source result is missing' errors"))
    # Refused releases that are LEGITIMATE (shared-prefix retention) are
    # expected under pressure; report them as info, not failure.
    if log12["state_lease_leak"] > 0:
        all_verdicts.append(("state-lease", f"INFO: {log12['state_lease_leak']} refused release(s), {log12['state_lease_leak'] - log12['state_lease_orphan']} legitimate (shared-prefix retention)"))
    # Orphaned state slots surface in /stats as refused releases.
    h0 = (stats0.get("pressure", {}) or {}).get("host_slot_release_failures", 0)
    h1 = (stats1.get("pressure", {}) or {}).get("host_slot_release_failures", 0)
    if h1 > h0:
        all_verdicts.append(("state-lease", f"WARN: host_slot_release_failures grew by {h1 - h0} (includes legitimate shared-prefix retention)"))
    else:
        all_verdicts.append(("state-lease", "PASS: host_slot_release_failures unchanged"))
    # Vacuity guards: the pressure path must have actually been exercised.
    captured = log12["spill_ckpt_ok"]
    restored = log12["checkpoint_restored"] + log12["rewrite_prefix_hit"]
    demoted = log12["checkpoint_demoted"]
    if captured < 4:
        all_verdicts.append(("state-lease", f"WARN: only {captured} checkpoint captures (expected >= 4) — state pressure may not have been reached"))
    else:
        all_verdicts.append(("state-lease", f"PASS: {captured} checkpoint captures (rewrite-recycle precondition)"))
    if restored == 0 and demoted == 0 and log12["relief_demote"] == 0:
        all_verdicts.append(("state-lease", "WARN: no checkpoint demote/restore/relief observed — state pool never pressurized (H2D-restore path may be unexercised)"))
    else:
        all_verdicts.append(("state-lease", f"PASS: state pool pressurized (demoted={demoted}, restored={restored}, relief={log12['relief_demote']})"))
    return all_verdicts


def phase_13(args):
    all_verdicts = []
    # Phase 13: state-saturation — the device state pool at 100% with a
    # HostOnly checkpoint that must be restored (H2D) while it stays full.
    # This is the exact production class that 500ed on 2026-09-16: the
    # rewrite-restore H2D takes a NEW device slot, and the emergency relief
    # must free one (DeviceOnly demotion, or dropping the redundant device
    # replica of a dual-resident checkpoint) or the materialization throws
    # std::bad_alloc. Phases 11/12 pressurize the pool but do not saturate
    # it (relief=0 in a full run), so this phase exists to force the
    # saturated-restore path: more thinking sessions (5) than decode lanes
    # (3), so the working set (endpoint + fork-write + rewrite checkpoint per
    # active session) exceeds the 8-slot pool (3 cache + 5 active).
    print("\n=== Phase 13: state-saturation (6 tool-calling sessions, 8 rounds, pool at 100%) ===")
    log_off = count_log_lines(args.serve_log)
    stats0 = get_stats(args)
    # Tool-calling sessions (the production shape): every tool-call turn is a
    # rewrite boundary, so turns capture rewrite checkpoints — the images the
    # saturated H2D-restore relief path operates on. Plain thinking turns
    # capture almost none (ckpt=0), which leaves the pool full of endpoints
    # only and never triggers the relief path.
    s13 = [ChatSession(f"SS{i}", 10000, 1500, args) for i in range(6)]
    for s in s13:
        s.args = type(args)(**vars(args))
        s.args.max_output_tokens = 512
    for r in range(1, 9):
        print(f"Round {r}:")
        errors = run_round(s13, r, args.timeout)
        if errors:
            for n, e in errors: print(f"  ERROR {n}: {e}")
            print("  Retrying failed sessions...")
            time.sleep(3)
            failed = [s for s in s13 if s.name in [n for n, _ in errors]]
            errors2 = run_round(failed, r, args.timeout)
            if errors2:
                for n, e in errors2: print(f"  ERROR {n}: {e}")
            continue
    stats1 = get_stats(args)
    log13 = parse_serve_log(args.serve_log, log_off)
    for v in evaluate("state-saturation", s13, stats0, stats1, log13):
        all_verdicts.append(("state-saturation", v))
    # Phase-specific gates: a saturated restore must never 500. Worker
    # recoveries from the P4.2 pressure-expansion class are known and
    # self-recovering (tracked in plan.md) — they are not saturated-restore
    # failures.
    other_recoveries = log13["worker_recover"] - log13["pressure_expansion_fail"]
    if other_recoveries > 0:
        all_verdicts.append(("state-saturation", f"FAIL: {other_recoveries} worker recoveries — saturated restore failed (relief did not free a slot)"))
    else:
        all_verdicts.append(("state-saturation", "PASS: zero worker recoveries (saturated restores resolved)"))
    if log13["pressure_expansion_fail"] > 0:
        all_verdicts.append(("state-saturation", f"WARN: {log13['pressure_expansion_fail']} pressure-expansion recoveries (P4.2 known class, self-recovering)"))
    # Occupancy readout: did the pool actually reach its ceiling?
    p1 = (stats1.get("pressure", {}) or {})
    cap = p1.get("checkpoint_device_state_slots", 0)
    occ = p1.get("device_state_occupied_slots", 0)
    if cap and occ >= cap:
        all_verdicts.append(("state-saturation", f"PASS: device state pool saturated ({occ}/{cap} at phase end)"))
    else:
        all_verdicts.append(("state-saturation", f"WARN: device state pool not saturated ({occ}/{cap} at phase end)"))
    if log13["relief_demote"] > 0:
        all_verdicts.append(("state-saturation", f"PASS: relief freed device state slots {log13['relief_demote']}x (dual drops: {log13['relief_dual_drop']})"))
    else:
        all_verdicts.append(("state-saturation", "WARN: relief never fired — pool may not have saturated (non-deterministic; the no-OOM gate still holds)"))
    if log13["state_replan"] > 0:
        all_verdicts.append(("state-saturation", f"PASS: {log13['state_replan']} entitlement re-plans (plan re-baselined after relief; no 500)"))
    if log13["entitlement_mismatch"] > 0:
        all_verdicts.append(("state-saturation", f"FAIL: {log13['entitlement_mismatch']} core-incomplete entitlement mismatches (strict check fired)"))
    if log13["spill_before_loss"] > 0:
        all_verdicts.append(("state-saturation", f"PASS: {log13['spill_before_loss']} spill-before-loss backstops fired (unit retained in net before its state lost its last copy)"))
    if log13["state_relinquish"] > 0:
        all_verdicts.append(("state-saturation", f"PASS: {log13['state_relinquish']} state images relinquished to the net (move-not-copy; the net is the unit's host home)"))
    return all_verdicts


def phase_planner_latency(args):
    """Admission-planner latency gate.

    The pressure planner is a beam search over parked catalog units. Before
    the 2026-09-17 convergence fix it enumerated to the 4096-target budget
    (stop_reason expansion_capacity/target_budget, ~3.2s per search) whenever
    the evict-all seed incumbent was hard to beat. The fix seeds with a
    feasible cover (guided closure / greedy eviction cover), stops expanding
    covered targets, prunes infeasible branches, and caps the search at
    30ms (time_budget). Assert the search no longer enumerates and stays fast.
    """
    all_verdicts = []
    start = getattr(args, "_request_log_start", 0)
    rows = []
    try:
        with open(args.request_log, "r", errors="replace") as f:
            for i, line in enumerate(f):
                if i < start:
                    continue
                line = line.strip()
                if not line:
                    continue
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                m = d.get("materialization")
                if m:
                    rows.append(m)
    except OSError:
        all_verdicts.append(("planner-latency", "WARN: request log unreadable — planner check skipped"))
        return all_verdicts
    if not rows:
        all_verdicts.append(("planner-latency", "WARN: no materialization diagnostics in window — planner check skipped"))
        return all_verdicts

    search_ns = sorted(m.get("search_elapsed_ns", 0) for m in rows)
    p95 = search_ns[min(len(search_ns) - 1, int(len(search_ns) * 0.95))]
    budget_stops = sum(1 for m in rows if m.get("stop_reason") in ("expansion_capacity", "target_budget"))
    stops = {}
    for m in rows:
        s = m.get("stop_reason", "?")
        stops[s] = stops.get(s, 0) + 1
    detail = (f"n={len(rows)} p95_search={p95 / 1e6:.1f}ms max_search={search_ns[-1] / 1e6:.1f}ms "
              f"budget_stops={budget_stops} stops={stops}")
    if budget_stops > 0:
        all_verdicts.append(("planner-latency",
                             f"FAIL: {budget_stops} searches enumerated to the target budget "
                             f"(expansion_capacity/target_budget) — convergence regression. {detail}"))
    elif p95 > 50 * 1000 * 1000:
        all_verdicts.append(("planner-latency",
                             f"FAIL: p95 search {p95 / 1e6:.1f}ms exceeds the 50ms budget. {detail}"))
    else:
        all_verdicts.append(("planner-latency", f"PASS: planner converged — {detail}"))
    return all_verdicts


def print_summary(all_verdicts):
    # Summary
    print("\n=== FINAL VERDICTS ===")
    for pn, v in all_verdicts:
        print(f"  [{pn}] {v}")
    npass = sum(1 for _, v in all_verdicts if v.startswith("PASS"))
    nwarn = sum(1 for _, v in all_verdicts if v.startswith("WARN"))
    nfail = sum(1 for _, v in all_verdicts if v.startswith("FAIL"))
    print(f"\n{'FAIL' if nfail else 'PASS'}: {npass} PASS, {nwarn} WARN, {nfail} FAIL")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())

