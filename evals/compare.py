"""Repeated paired trials; acceptance is measured by frozen host graders."""

import argparse
import asyncio
import json
import os
from pathlib import Path
import random
import shutil
import statistics
import time
from .compare_grade import freeze, grade, sha256, tree_hashes
from .metrics import summarize
from .runners.harness import run_harness
from .runners.codex import run_codex

FEATURES = {
    "AH_EVAL_AUTO_CONTEXT": {"0", "1"},
    "AH_EVAL_AGENTS": {str(i) for i in range(33)},
    "AH_EVAL_PARALLEL_TOOLS": {str(i) for i in range(1, 17)},
    "AH_EVAL_CACHE_KEY": {"session", "omit"},
    "AH_EVAL_CACHE_DIAGNOSTICS": {"0", "1"},
    "AH_EVAL_TRACE_REQUESTS": {"0", "1"},
}
TIERS = {"update": "short", "intervals": "medium", "reconcile": "medium", "endurance": "long"}


def settings(values):
    result = {}
    for option in values:
        name, separator, value = option.partition("=")
        if not separator or name not in FEATURES or value not in FEATURES[name]:
            raise ValueError("Expected a documented AH_EVAL feature flag with an allowed value")
        if name in result:
            raise ValueError("Duplicate feature flag: " + name)
        result[name] = value
    return result


def schedule(names, repetitions, seed, order):
    rng = random.Random(seed)
    plan = []
    first = rng.randrange(2)
    for index in range(repetitions):
        for case_index, name in enumerate(names):
            reverse = rng.randrange(2) if order == "random" else (first + index + case_index) % 2
            arms = ["baseline", "candidate"]
            if reverse:
                arms.reverse()
            plan.append({"case": name, "trial": index + 1, "order": arms})
    return plan


def saved_events(directory):
    path = directory / "trace.jsonl"
    if not path.exists():
        return []
    events = []
    for line in path.read_text().splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            break  # A killed writer may leave an incomplete final line.
    return events


def accounting(raw):
    events = raw.get("events", [])
    starts = sum(row["event"].get("event") == "model.started" for row in events)
    tools = {
        row["event"]["operation_id"] for row in events
        if row["event"].get("event") in
        ("tool.started", "tool.completed", "tool.late_result", "tool.outcome_unknown")
        and row["event"].get("operation_id")
    }
    configs = [row["event"].get("data") for row in events
               if row["event"].get("event") == "evaluation.config"]
    result = summarize(dict(raw, events=list(events)))
    if result["runner"] == "harness":
        result["model_calls"] = starts or result.get("model_calls")
        result["tool_calls"] = len(tools) if tools else result.get("tool_calls")
        result["usage_complete"] &= starts > 0 and starts == result["confirmed_model_responses"]
    else:
        result["model_calls"] = None  # App-server usage notifications are not exact call counts.
        result["tool_calls"] = None
        result["usage_complete"] &= result["confirmed_model_responses"] > 0
    reports = [row["event"].get("data", {}).get("usage") or {} for row in events
               if row["event"].get("event") in ("model.completed", "model.late_result")]
    fields = {
        "input_tokens": ("input_tokens",), "output_tokens": ("output_tokens",),
        "cached_input_tokens": ("input_tokens_details", "cached_tokens"),
        "reasoning_output_tokens": ("output_tokens_details", "reasoning_tokens"),
    }
    if result["runner"] == "codex":
        reports = [row["event"].get("params", {}).get("tokenUsage", {}).get("total", {})
                   for row in events if row["event"].get("method") == "thread/tokenUsage/updated"][-1:]
        fields = dict(zip(fields, [("inputTokens",), ("outputTokens",),
                                  ("cachedInputTokens",), ("reasoningOutputTokens",)]))
    complete = {}
    for name, path in fields.items():
        values = []
        for usage in reports:
            value = usage
            for component in path:
                value = value.get(component) if isinstance(value, dict) else None
            values.append(isinstance(value, int) and not isinstance(value, bool) and value >= 0)
        complete[name] = bool(values) and all(values)
        if not any(values):
            result["usage"][name] = None
    result["usage_fields_complete"] = complete
    result["usage_complete"] &= all(complete.values())
    result["reported_evaluation_config"] = configs[-1] if configs else None
    return result


def report(results, plan):
    arms = {}
    for arm in ("baseline", "candidate"):
        rows = [row for row in results if row["arm"] == arm]
        accepted = [row["time_to_acceptance_seconds"] for row in rows if row["accepted"]]
        arms[arm] = {
            "attempts": len(rows), "accepted": len(accepted),
            "censored": sum(row["censored"] for row in rows),
            "execution_failures": sum(not row["natural_completion"] and not row["censored"] for row in rows),
            "usage_incomplete_attempts": sum(not row["usage_complete"] for row in rows),
            "median_accepted_seconds": statistics.median(accepted) if accepted else None,
            "total_attempt_and_grader_seconds": round(sum(row["elapsed_seconds"] for row in rows), 6),
        }
    pairs = []
    for pair in plan:
        rows = {row["arm"]: row for row in results
                if row["case"] == pair["case"] and row["trial"] == pair["trial"]}
        both = len(rows) == 2 and all(row["accepted"] for row in rows.values())
        ratio = (rows["baseline"]["time_to_acceptance_seconds"] /
                 max(rows["candidate"]["time_to_acceptance_seconds"], 1e-9)) if both else None
        same_features = len(rows) == 2 and rows["baseline"]["features"] == rows["candidate"]["features"]
        supported = len(rows) == 2 and all(row["requested_features_confirmed"] is not False for row in rows.values())
        pairs.append({**pair, "both_accepted": both, "requested_features_confirmed": supported,
                      "identical_requested_feature_flags": same_features,
                      "baseline_over_candidate_time": ratio})
    cases = {}
    for name in dict.fromkeys(pair["case"] for pair in plan):
        cases[name] = {}
        for arm in ("baseline", "candidate"):
            subset = [row for row in results if row["case"] == name and row["arm"] == arm]
            times = [row["time_to_acceptance_seconds"] for row in subset if row["accepted"]]
            cases[name][arm] = {"attempts": len(subset), "accepted": len(times),
                                "censored": sum(row["censored"] for row in subset),
                                "median_accepted_seconds": statistics.median(times) if times else None}
    return {"arms": arms, "cases": cases, "pairs": pairs,
            "interpretation": "Accepted-only times exclude failures; read acceptance and censoring alongside them. "
                              "No tail percentile or superiority claim is inferred from these small samples."}


async def run(args, runners=None):
    runners = runners or {"harness": run_harness, "codex": run_codex}
    root = args.output.resolve()
    if root.is_relative_to(Path(__file__).resolve().parents[1]):
        raise ValueError("Evaluation output must be outside the repository")
    if root.exists() and (not root.is_dir() or any(root.iterdir())):
        raise ValueError("A fresh empty output directory is required; attempts are never overwritten")
    common = settings(args.env)
    features = {arm: dict(common, **settings(getattr(args, arm + "_env")))
                for arm in ("baseline", "candidate")}
    names = args.case or ["update", "intervals", "reconcile"]
    if args.long and "endurance" not in names:
        names = [*names, "endurance"]
    plan = schedule(names, args.repetitions, args.seed, args.order)
    variants = {}
    for arm in ("baseline", "candidate"):
        runner = getattr(args, arm + "_runner")
        driver = getattr(args, arm + "_driver")
        if runner == "harness" and (not driver or not driver.is_file()):
            raise ValueError(arm + " requires an existing --" + arm + "-driver")
        variants[arm] = {"runner": runner, "driver": str(driver.resolve()) if driver else None,
                         "driver_sha256": sha256(driver) if driver else None,
                         "features": features[arm],
                         "ignored_features": sorted(features[arm]) if runner == "codex" else []}
    root.mkdir(parents=True, exist_ok=True)
    frozen, templates = freeze(root, names)
    manifest = {
        "model": args.model, "reasoning_effort": args.effort, "seed": args.seed,
        "order_policy": args.order, "schedule": plan, "variants": variants,
        "timeouts_seconds": {"smoke": args.timeout, "endurance": args.long_timeout},
        "cutoff_disclosed_to_model": False, "followup_prompts": 0,
        "grader_sha256": frozen,
        "acceptance_policy": "Natural completion plus frozen smoke validators; endurance additionally "
                             "requires original checks, audit, protected inputs, deliverables and generated tests.",
        "tasks": {name: {k: v for k, v in value.items() if k != "prompt"}
                  for name, value in templates.items()},
        "fairness": {
            "identical_requested_model_and_effort": True,
            "identical_requested_feature_flags": features["baseline"] == features["candidate"],
            "same_runner": variants["baseline"]["runner"] == variants["candidate"]["runner"],
            "inherited_eval_flags_removed": sorted(k for k in os.environ if k.startswith("AH_EVAL_")),
            "feature_flags_apply_only_to_harness": True,
            "route": "https://api.openai.com/v1/responses",
            "caveats": "Different runners retain different prompts/tools. Provider cache and load may vary. "
                       "Explicit per-arm settings are recorded; unacknowledged features are not verified.",
        },
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2))
    results = []
    for pair in plan:
        name = pair["case"]
        for position, arm in enumerate(pair["order"]):
            variant = variants[arm]
            directory = root / f"{name}-{pair['trial']:03d}-{arm}"
            directory.mkdir()
            workspace = directory / "workspace"
            shutil.copytree(root / "templates" / name, workspace)
            if tree_hashes(workspace) != templates[name]["input_sha256"]:
                raise ValueError("Prepared workspace differs from frozen input")
            duration = args.long_timeout if name == "endurance" else args.timeout
            started = time.monotonic()
            try:
                if variant["driver"] and sha256(variant["driver"]) != variant["driver_sha256"]:
                    raise ValueError("Evaluation driver changed after the manifest was recorded")
                if variant["runner"] == "harness":
                    environment = {k: v for k, v in os.environ.items() if not k.startswith("AH_EVAL_")}
                    environment.update(features[arm])
                    pending = runners["harness"](
                        directory, Path(variant["driver"]), args.codex, args.model, args.effort,
                        templates[name]["prompt"], duration, environment=environment,
                    )
                else:
                    pending = runners["codex"](
                        directory, args.codex, args.model, args.effort, templates[name]["prompt"], duration,
                    )
                raw = await asyncio.wait_for(pending, duration)
            except Exception as exc:
                detail = str(exc)
                for key in ("AH_ACCESS_TOKEN", "OPENAI_API_KEY", "ACCESS_TOKEN"):
                    if os.environ.get(key):
                        detail = detail.replace(os.environ[key], "[redacted]")
                raw = {"runner": variant["runner"], "state": "failed",
                       "timed_out": isinstance(exc, asyncio.TimeoutError),
                       "failure": type(exc).__name__, "failure_detail": detail[:4000],
                       "events": saved_events(directory), "exit_code": None}
            run_seconds = time.monotonic() - started
            row = accounting(raw)
            row["grade"] = await grade(root, name, workspace, frozen, templates[name]["input_sha256"])
            elapsed = time.monotonic() - started
            natural = row.get("state") == "completed" and not row["timed_out"] and not row.get("failure")
            if variant["runner"] == "harness":
                natural &= row.get("exit_code") == 0
            accepted = bool(natural and row["grade"]["pass"])
            reported = row.get("reported_evaluation_config") or {}
            row["requested_features_confirmed"] = (
                all(reported.get(key) == value for key, value in features[arm].items())
                if features[arm] else None
            )
            row.update(arm=arm, case=name, tier=TIERS[name], trial=pair["trial"], order_position=position,
                       model=args.model, reasoning_effort=args.effort, features=features[arm],
                       accepted=accepted, censored=bool(row["timed_out"]), natural_completion=bool(natural),
                       runner_wall_seconds=round(run_seconds, 6), elapsed_seconds=round(elapsed, 6),
                       time_to_acceptance_seconds=round(elapsed, 6) if accepted else None)
            results.append(row)
            (directory / "result.json").write_text(json.dumps(row, indent=2))
            (root / "results.json").write_text(json.dumps(results, indent=2))
            (root / "summary.json").write_text(json.dumps(report(results, plan), indent=2))
            print(json.dumps({"case": name, "trial": pair["trial"], "arm": arm,
                              "accepted": accepted, "censored": row["censored"]}), flush=True)
    return results


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    for arm in ("baseline", "candidate"):
        p.add_argument("--" + arm + "-driver", type=Path)
        p.add_argument("--" + arm + "-runner", choices=("harness", "codex"), default="harness")
        p.add_argument("--" + arm + "-env", action="append", default=[], metavar="FLAG=VALUE")
    p.add_argument("--env", action="append", default=[], metavar="FLAG=VALUE")
    p.add_argument("--codex", default=shutil.which("codex"))
    p.add_argument("--model", default="gpt-6.1-sol")
    p.add_argument("--effort", choices=("low", "medium", "high", "xhigh"), default="low")
    p.add_argument("--repetitions", type=int, default=3)
    p.add_argument("--seed", type=int, default=417)
    p.add_argument("--order", choices=("alternating", "random"), default="alternating")
    p.add_argument("--case", choices=tuple(TIERS), action="append")
    p.add_argument("--long", action="store_true", help="Include the Workboard endurance task")
    p.add_argument("--timeout", type=int, default=240)
    p.add_argument("--long-timeout", type=int, default=1200)
    return p


def main():
    p = parser()
    args = p.parse_args()
    if not args.codex or not os.environ.get("AH_ACCESS_TOKEN"):
        p.error("An authorized AH_ACCESS_TOKEN and Codex executable are required")
    if not 1 <= args.repetitions <= 100 or not all(1 <= n <= 1800 for n in (args.timeout, args.long_timeout)):
        p.error("repetitions must be 1..100 and timeouts 1..1800 seconds")
    if args.case and len(set(args.case)) != len(args.case):
        p.error("Cases must not repeat; use --repetitions")
    try:
        results = asyncio.run(run(args))
    except ValueError as exc:
        p.error(str(exc))
    return 0 if all(row["accepted"] for row in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
