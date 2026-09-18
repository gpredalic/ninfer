#!/usr/bin/env python3
"""P1.9 long-context recall probe (needle-in-haystack, cold vs warm).

Plants unique recall codes at four depth zones of a ~300k-token prompt and
asks the model to report all four. Runs twice with the IDENTICAL prompt:

  COLD — first request: full prefill from scratch.
  WARM — second request: the prefix is served from the KV cache.

Interpretation (plan.md P1.9):
  - recall fails in BOTH cold and warm -> intrinsic long-context / RoPE-scaling
    quality (hypothesis H-A).
  - recall fails ONLY warm            -> restore-path position/attention bug
    (hypothesis H-C): the cached-prefix path corrupts deep positions.
  - recall passes in both             -> the attention machinery is intact at
    this length; the macro-stuck symptom is behavioral (task-state tracking),
    not attention recall.

Zone placement (fractions of the target length): 10k / 110k / 210k / 290k
tokens at a 300k target — spanning below and above the YaRN ramp
(--rope-scaling-original-context 262144, factor 2.12 in prod).

A ~300k-token prefill takes ~2 min and disrupts concurrent serving — run in a
prod-stopped window (or against the e2e server with a large --host-kv-mib).

Usage:
  python3 tools/longctx_recall_probe.py [--target-tokens 300000]
      [--host 127.0.0.1] [--port 8080] [--max-tokens 512] [--timeout 600]
      [--json out.json]
"""

import argparse
import json
import random
import sys
import time
import urllib.request

CHARS_PER_TOKEN = 4.2  # calibration from the e2e filler
ZONES = (0.033, 0.367, 0.700, 0.967)  # ~10k / 110k / 210k / 290k at 300k


def make_codes(rng: random.Random):
    alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
    return [
        "-".join(
            "".join(rng.choice(alphabet) for _ in range(4))
            for _ in range(2)
        )
        for _ in range(len(ZONES))
    ]


def build_prompt(target_tokens: int, codes, rng: random.Random):
    """Filler document with one marker line per zone, at the zone's char offset."""
    total_chars = int(target_tokens * CHARS_PER_TOKEN)
    # Filler: distinct-ish sentences so the document is not trivially
    # compressible, ~4.2 chars/token.
    words = rng.choices(
        "the quick brown fox jumps over lazy dog near river under bridge "
        "past mill beside orchard along coast within valley beyond ridge "
        "across plain through forest around harbor near station".split(),
        k=int(total_chars / 6) + 64,
    )
    doc = " ".join(words)
    doc = doc[:total_chars]

    markers = []
    for i, (frac, code) in enumerate(zip(ZONES, codes), start=1):
        line = (
            f"MARKER {i}: The recall code for zone {i} is {code}. "
            f"Remember the exact code {code} for zone {i}."
        )
        pos = min(int(total_chars * frac), len(doc) - len(line) - 16)
        doc = doc[:pos] + "\n" + line + "\n" + doc[pos:]
        markers.append((i, code, int(pos / CHARS_PER_TOKEN)))

    question = (
        "\n\nFour MARKER lines in the text above each define a recall code "
        "for a zone (zones 1 to 4). Reply with exactly the four recall "
        "codes, one per line, in zone order. Nothing else."
    )
    return doc + question, markers


def run_once(host: str, port: int, prompt: str, max_tokens: int, timeout: int):
    payload = {
        "model": "probe",
        "input": [{"role": "user",
                   "content": [{"type": "input_text", "text": prompt}]}],
        "instructions": "You are a precise recall assistant.",
        "max_output_tokens": max_tokens,
        "store": False,
        "stream": False,
    }
    req = urllib.request.Request(
        f"http://{host}:{port}/v1/responses",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST")
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        out = json.load(resp)
    wall = time.monotonic() - t0
    text = "".join(
        part.get("text", "")
        for item in out.get("output", [])
        if item.get("type") == "message"
        for part in item.get("content", [])
        if part.get("type") == "output_text"
    )
    usage = out.get("usage", {}) or {}
    return text, usage, wall


def score(text: str, codes) -> list:
    return [code in text for code in codes]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--target-tokens", type=int, default=300000)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--max-tokens", type=int, default=512)
    p.add_argument("--timeout", type=int, default=600)
    p.add_argument("--json", dest="json_out", default=None)
    args = p.parse_args()

    rng = random.Random(20260918)
    codes = make_codes(rng)
    prompt, markers = build_prompt(args.target_tokens, codes, rng)
    est_total = int(len(prompt) / CHARS_PER_TOKEN)
    print(f"prompt: {len(prompt)} chars (~{est_total} tokens est), "
          f"zones at est tokens: {[m[2] for m in markers]}")

    results = {}
    for mode in ("cold", "warm"):
        text, usage, wall = run_once(
            args.host, args.port, prompt, args.max_tokens, args.timeout)
        got = score(text, codes)
        prompt_tokens = usage.get("input_tokens")
        # Calibrate the estimated zone positions against the real count.
        scale = (prompt_tokens / est_total) if prompt_tokens else 1.0
        zones = [
            {"zone": i, "code": code, "est_tokens": est,
             "calibrated_tokens": int(est * scale), "recalled": ok}
            for (i, code, est), ok in zip(markers, got)
        ]
        results[mode] = {
            "prompt_tokens": prompt_tokens,
            "wall_s": round(wall, 1),
            "recalled": sum(got),
            "of": len(codes),
            "zones": zones,
            "response": text[:2000],
        }
        print(f"\n=== {mode.upper()} === prompt_tokens={prompt_tokens} "
              f"wall={wall:.1f}s recalled={sum(got)}/{len(codes)}")
        for z in zones:
            mark = "OK " if z["recalled"] else "MISS"
            print(f"  {mark} zone {z['zone']}: ~{z['calibrated_tokens']} "
                  f"tokens -> {z['code']}")

    cold, warm = results["cold"], results["warm"]
    print("\n=== VERDICT ===")
    if cold["recalled"] == len(codes) and warm["recalled"] == len(codes):
        print("PASS both: attention recall intact at this length "
              "(H-A-attention ruled out; symptom is behavioral).")
    elif cold["recalled"] < len(codes) and warm["recalled"] < len(codes):
        print("FAIL both: intrinsic long-context degradation (H-A).")
    elif cold["recalled"] == len(codes) and warm["recalled"] < len(codes):
        print("FAIL warm only: restore-path position/attention bug (H-C).")
    else:
        print("WARM better than COLD: unexpected — inspect the zone detail.")

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({"args": vars(args), "results": results}, f, indent=2)
        print(f"\nwrote {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
