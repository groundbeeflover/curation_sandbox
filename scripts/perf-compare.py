#!/usr/bin/env python3
"""Compare a benchmark result with the performance budgets and with the machine's baseline.

    python3 scripts/perf-compare.py build/bench/empty-room.json --budgets bench/budgets.json \\
        --baseline bench/baselines/radeon-780m/empty-room-standard.json --milestone M1

Prints a Markdown table, ready for a pull request: one row per measured metric (and per budget
that applies to it), with metric, budget, baseline, now, change and verdict. Exits 1 when
- a budget enforced at or before --milestone (its enforced_from) is exceeded, or
- a metric is more than 10% worse than the baseline.
A budget not enforced yet is still reported, with the milestone it counts from, so the gap stays
visible, and so is a metric that a budget or the baseline has but the run did not measure,
without failing (lux, bake and still timings come from runs of their own). Without a baseline
file (a machine's first run) only the budgets are compared; the missing file must still be where
the run's baseline belongs, <machine>/<scene>-<tier>.json, so that a mistyped path cannot skip
the comparison. Exits 2 on input it cannot use.

A budget in bench/budgets.json applies to a run of its scene and tier, on its reference hardware
(the run's machine or CPU slug, or "any"), and at its size where it names one.

Result schema, version 1: the JSON file cs-bench writes and bench/baselines/ keeps.

  {
    "schema": 1,
    "scene": "empty-room",       the benchmark scene: bench/scenes/<scene>, or generated
    "tier": "standard",          the --tier it ran at: standard or raytraced
    "size": [1920, 1080],        render size in pixels
    "frames": 600,               frames rendered along the scripted camera path
    "machine": "radeon-780m",    slug of the GPU, from cs-bench --device-report; names the
                                 baseline folder bench/baselines/<machine>/
    "cpu": "ryzen-7-8840u",      slug of the CPU model
    "device": {"name": "AMD Radeon 780M (RADV PHOENIX)", "type": "integrated",
               "driver": "Mesa 25.2.4", "api_version": "1.3.296"},
                                 type is Vulkan's device type: integrated, discrete,
                                 virtual or cpu
    "git_revision": "6656a51",
    "metrics": {"frame_time_p95_s": 0.0052, ...}
  }

Metrics are in SI units (seconds, bytes), each a number from 0 to 1e15, and every one is a cost:
lower is better. A run writes the ones it measures and leaves out the others; a name not listed
here is an error, so a misspelt metric cannot pass unchecked.

  frame_time_p50_s, frame_time_p95_s, frame_time_p99_s
                         percentiles of the wall-clock time from the start of one frame to the
                         start of the next: the frame time a user sees
  gpu_time_p50_s, gpu_time_p95_s, gpu_time_p99_s
                         percentiles of a frame's GPU time, from timestamp queries
  gpu_memory_bytes       peak device memory in use
  process_memory_bytes   peak resident memory of the process other than GPU memory: on a cpu
                         device (lavapipe), whose GPU memory lives in the process, the writer
                         subtracts gpu_memory_bytes
  load_time_s            open the scene to its first frame, textures cached
  startup_time_s         start the app to its first frame, shader cache warm
  lux_refresh_time_s     after an edit, lux on every work at a standard error of at most 2%
  bake_preview_time_s, bake_full_time_s
                         after an edit, the preview and the full bake of indirect light
  bake_frame_time_p95_s  frame_time_p95_s while the bake runs
  still_1080p_time_s     a 1080p still at 64 samples per pixel, denoised
  still_4k_time_s        a 4K still at 256 samples per pixel, denoised

Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = 1
MAX_FILE_BYTES = 1 << 20
MAX_VALUE = 1e15
REGRESSION = 0.10  # a metric more than this much worse than the baseline fails
TIERS = ("standard", "raytraced")
METRICS = {  # name: unit, in the order of the table
    "frame_time_p50_s": "s", "frame_time_p95_s": "s", "frame_time_p99_s": "s",
    "gpu_time_p50_s": "s", "gpu_time_p95_s": "s", "gpu_time_p99_s": "s",
    "gpu_memory_bytes": "bytes", "process_memory_bytes": "bytes",
    "load_time_s": "s", "startup_time_s": "s", "lux_refresh_time_s": "s",
    "bake_preview_time_s": "s", "bake_full_time_s": "s", "bake_frame_time_p95_s": "s",
    "still_1080p_time_s": "s", "still_4k_time_s": "s",
}
SLUG = re.compile(r"[a-z0-9][a-z0-9.-]{0,63}")
MILESTONE = re.compile(r"M([1-9][0-9]?)")


class InputError(Exception):
    """Input the script cannot use; main() exits 2."""


def _require(ok: object, message: str) -> None:
    if not ok:
        raise InputError(message)


def _reject_constant(name: str):
    raise ValueError(f"{name} is not a number")


def load_json(path: Path) -> object:
    try:
        _require(path.stat().st_size <= MAX_FILE_BYTES, f"{path}: over {MAX_FILE_BYTES} bytes")
        return json.loads(path.read_text(encoding="utf-8"), parse_constant=_reject_constant)
    except OSError as error:
        raise InputError(f"{path}: {error.strerror}") from error
    except (ValueError, RecursionError) as error:  # not UTF-8, not JSON, NaN, nested too deep
        raise InputError(f"{path}: not valid JSON: {error}") from error


def _is_number(value: object) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and 0 <= value <= MAX_VALUE)


def _is_slug(value: object) -> bool:
    return isinstance(value, str) and SLUG.fullmatch(value) is not None


def _is_size(value: object) -> bool:
    return (isinstance(value, list) and len(value) == 2
            and all(type(v) is int and 0 < v <= 65536 for v in value))


def milestone_number(text: str) -> int:
    match = MILESTONE.fullmatch(text)
    if not match:
        raise argparse.ArgumentTypeError(f"{text!r} is not a milestone such as M1")
    return int(match.group(1))


def check_result(data: object, source: Path) -> dict:
    """The result if it follows the schema; otherwise InputError says what does not."""
    _require(isinstance(data, dict), f"{source}: not a JSON object")
    _require(type(data.get("schema")) is int and data["schema"] == SCHEMA,
             f"{source}: schema is {data.get('schema')!r}; this script reads {SCHEMA}")
    for key in ("scene", "machine", "cpu"):
        _require(_is_slug(data.get(key)), f"{source}: {key} must be a lower-case slug")
    _require(data.get("tier") in TIERS, f"{source}: tier must be one of {', '.join(TIERS)}")
    _require(_is_slug(data.get("git_revision", "unknown")),
             f"{source}: git_revision must be a lower-case slug")
    _require(_is_size(data.get("size")), f"{source}: size must be [width, height] in pixels")
    metrics = data.get("metrics")
    _require(isinstance(metrics, dict) and metrics, f"{source}: metrics must be an object")
    for name, value in metrics.items():
        _require(name in METRICS, f"{source}: unknown metric {name[:64]!r}")
        _require(_is_number(value), f"{source}: {name} must be a number from 0 to {MAX_VALUE:g}")
    return data


def check_budgets(data: object, source: Path) -> list[dict]:
    """The budgets if each is well formed; otherwise InputError says which is not."""
    budgets = data.get("budgets") if isinstance(data, dict) else None
    _require(isinstance(budgets, list), f"{source}: budgets must be a list")
    for i, budget in enumerate(budgets):
        where = f"{source}: budgets[{i}]"
        _require(isinstance(budget, dict), f"{where} is not an object")
        metric = budget.get("metric")
        _require(isinstance(metric, str) and metric in METRICS, f"{where}: metric {metric!r}?")
        _require(budget.get("unit") == METRICS[metric],
                 f"{where}: the unit of {metric} is {METRICS[metric]}, not {budget.get('unit')!r}")
        _require(_is_number(budget.get("limit")) and budget["limit"] > 0,
                 f"{where}: limit must be a number above 0")
        _require(_is_slug(budget.get("scene")), f"{where}: scene must be a lower-case slug")
        _require(budget.get("tier") in TIERS, f"{where}: tier must be one of {', '.join(TIERS)}")
        hardware = budget.get("hardware")
        _require(isinstance(hardware, list) and hardware and all(map(_is_slug, hardware)),
                 f"{where}: hardware must list machine or CPU slugs, or \"any\"")
        _require(isinstance(budget.get("enforced_from"), str)
                 and MILESTONE.fullmatch(budget["enforced_from"]),
                 f"{where}: enforced_from must be a milestone such as M2")
        _require("size" not in budget or _is_size(budget["size"]),
                 f"{where}: size must be [width, height] in pixels")
    return budgets


def applies(budget: dict, result: dict) -> bool:
    """Whether the budget holds for this run: its scene, tier, hardware and size."""
    return (budget["scene"] == result["scene"] and budget["tier"] == result["tier"]
            and not {"any", result["machine"], result["cpu"]}.isdisjoint(budget["hardware"])
            and budget.get("size", result["size"]) == result["size"])


def fmt(value: float, unit: str) -> str:
    if unit == "bytes":
        return f"{value / 1e9:.2f} GB" if value >= 1e9 else f"{value / 1e6:.1f} MB"
    return f"{value * 1e3:.2f} ms" if value < 1 else f"{value:.2f} s"


def compare(result: dict, budgets: list[dict], baseline: dict | None,
            milestone: int) -> tuple[list[list[str]], list[str]]:
    """The table's rows, and a line for each failure. A metric that a budget or the baseline
    has but the run did not measure gets a row saying so; it does not fail, because lux, bake
    and still timings come from runs of their own."""
    rows, failures = [], []
    measured, before = result["metrics"], baseline["metrics"] if baseline else {}
    applicable = [b for b in budgets if applies(b, result)]
    names = set(measured) | set(before) | {b["metric"] for b in applicable}
    for name in sorted(names, key=list(METRICS).index):
        unit, now, base = METRICS[name], measured.get(name), before.get(name)

        def show(value: float | None) -> str:
            return "-" if value is None else fmt(value, unit)

        change = f"{now / base - 1:+.1%}" if now is not None and base else "-"
        worse = now is not None and base is not None and now > base * (1 + REGRESSION)
        for budget in [b for b in applicable if b["metric"] == name] or [None]:
            reasons, notes = [], []
            if now is None:
                notes.append("not measured")
            elif budget is not None:
                over = now > budget["limit"]
                if milestone >= milestone_number(budget["enforced_from"]):
                    if over:
                        reasons.append("over budget")
                        failures.append(f"{name} over its budget of {show(budget['limit'])}")
                else:
                    notes.append(f"{'over' if over else 'within'} budget, "
                                 f"enforced from {budget['enforced_from']}")
            if worse:
                reasons.append(f"more than {REGRESSION:.0%} worse than the baseline")
            verdict = ([f"FAIL: {', '.join(reasons)}"] if reasons else []) + notes
            checked = budget is not None or base is not None
            rows.append([name, show(budget["limit"] if budget else None), show(base), show(now),
                         change,
                         "; ".join(verdict) or ("pass" if checked else "-")])
        if worse:
            failures.append(f"{name} {show(now)} against {show(base)} in the baseline")
    return rows, failures


def table(rows: list[list[str]]) -> str:
    header = ["metric", "budget", "baseline", "now", "change", "verdict"]
    widths = [max(len(row[i]) for row in [header, *rows]) for i in range(len(header))]

    def line(cells: list[str]) -> str:
        return "| " + " | ".join(c.ljust(w) for c, w in zip(cells, widths)) + " |"

    return "\n".join([line(header), line(["-" * w for w in widths]), *map(line, rows)])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="perf-compare.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("result", type=Path, help="a cs-bench result, in the schema above")
    parser.add_argument("--budgets", type=Path, default=ROOT / "bench" / "budgets.json",
                        help="the budgets (default: bench/budgets.json)")
    parser.add_argument("--baseline", type=Path,
                        help="the machine's last accepted result, "
                             "bench/baselines/<machine>/<scene>-<tier>.json; "
                             "a missing file is a first run")
    parser.add_argument("--milestone", type=milestone_number, required=True,
                        help="the current milestone, such as M1: a budget enforced from it or "
                             "earlier fails when exceeded")
    args = parser.parse_args(argv)

    try:
        result = check_result(load_json(args.result), args.result)
        budgets = check_budgets(load_json(args.budgets), args.budgets)
        baseline = None
        if args.baseline and not args.baseline.exists():
            # A first run on a machine, but only where its baseline belongs: a mistyped path
            # would otherwise skip the comparison with the baseline unnoticed.
            expected = (result["machine"], f"{result['scene']}-{result['tier']}.json")
            _require(args.baseline.parts[-2:] == expected,
                     f"{args.baseline}: no such file, and not where this run's baseline "
                     f"belongs (<baselines>/{'/'.join(expected)})")
        elif args.baseline:
            baseline = check_result(load_json(args.baseline), args.baseline)
            for key in ("scene", "tier", "size", "machine"):
                _require(baseline[key] == result[key],
                         f"{args.baseline}: its {key} is {baseline[key]!r} and the result's "
                         f"{result[key]!r}: a baseline compares only like with like")
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    rows, failures = compare(result, budgets, baseline, args.milestone)
    width, height = result["size"]
    print(f"{result['scene']}, {result['tier']} tier, {width}x{height}, on {result['machine']} "
          f"and {result['cpu']}, git {result.get('git_revision', 'unknown')}; "
          f"milestone M{args.milestone}")
    if baseline:
        print(f"baseline: {args.baseline}, git {baseline.get('git_revision', 'unknown')}")
    elif args.baseline:
        print(f"no baseline at {args.baseline}: a first run here, compared only with the budgets")
    else:
        print("no baseline given: compared only with the budgets")
    print()
    print(table(rows))
    print()
    if failures:
        print("FAIL:\n" + "\n".join(f"- {failure}" for failure in failures))
        return 1
    print("pass: no enforced budget exceeded"
          + (", nothing more than 10% worse than the baseline" if baseline else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
