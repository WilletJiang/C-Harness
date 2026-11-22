"""Paired orchestration and real frozen graders, entirely without model access."""

import asyncio
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from . import compare
from .compare_grade import freeze, grade
from .smoke.test_cases import migrate, CORRECT_INTERVALS
from .trace import Trace


def response(*, timed_out=False, usage=True):
    events = [{"seconds": 0, "event": {"event": "model.started"}}]
    if usage:
        events.append({"seconds": 1, "event": {"event": "model.completed", "data": {
            "model": "fixture", "reasoning": {"effort": "low"},
            "usage": {"input_tokens": 50, "output_tokens": 20,
                      "input_tokens_details": {"cached_tokens": 5},
                      "output_tokens_details": {"reasoning_tokens": 8}},
        }}})
    for event in ("tool.started", "tool.completed"):
        events.append({"seconds": 1, "event": {"event": event, "operation_id": "same-operation"}})
    return {"runner": "harness", "state": "completed", "seconds": 1,
            "timed_out": timed_out, "exit_code": 0, "events": events}


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ah-compare-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for arm in ("baseline", "candidate"):
            (self.root / arm).write_text("fixture " + arm)
        self.args = compare.parser().parse_args([
            "--output", str(self.root / "output"), "--case", "update",
            "--baseline-driver", str(self.root / "baseline"),
            "--candidate-driver", str(self.root / "candidate"), "--model", "fixture",
        ])

    def test_seeded_order_alternates_each_case_and_random_is_reproducible(self):
        first = compare.schedule(["update", "intervals"], 3, 17, "alternating")
        for name in ("update", "intervals"):
            orders = [p["order"] for p in first if p["case"] == name]
            self.assertEqual(orders[0], orders[2])
            self.assertNotEqual(orders[0], orders[1])
        self.assertEqual(compare.schedule(["update"], 8, 9, "random"),
                         compare.schedule(["update"], 8, 9, "random"))

    def test_failures_censoring_and_accepted_time_include_actual_grading(self):
        calls = []

        async def fake(directory, driver, codex, model, effort, prompt, duration, **kwargs):
            arm = driver.name
            trial = int(directory.name.split("-")[1])
            calls.append((trial, arm))
            self.assertEqual((model, effort, duration), ("fixture", "low", 240))
            self.assertNotIn("AH_EVAL_ENDPOINT", kwargs["environment"])
            self.assertEqual(kwargs["environment"]["AH_EVAL_AUTO_CONTEXT"], "1")
            if not (trial == 3 and arm == "baseline"):
                migrate(directory / "workspace")
            if trial == 2 and arm == "baseline":
                raise RuntimeError("preserved fixture failure")
            return response(timed_out=trial == 2 and arm == "candidate")

        self.args.env = ["AH_EVAL_AUTO_CONTEXT=1"]
        with patch.dict(os.environ, {"AH_EVAL_ENDPOINT": "must-not-leak-into-comparison"}):
            rows = asyncio.run(compare.run(self.args, {"harness": fake}))
        self.assertEqual(len(rows), 6)
        self.assertEqual(sum(r["accepted"] for r in rows), 3)
        censored = next(r for r in rows if r["censored"])
        self.assertTrue(censored["grade"]["pass"])
        self.assertFalse(censored["accepted"])
        self.assertIsNone(censored["time_to_acceptance_seconds"])
        failure = next(r for r in rows if r.get("failure"))
        self.assertIn("preserved fixture failure", failure["failure_detail"])
        self.assertTrue(failure["grade"]["pass"])
        self.assertFalse(failure["usage_complete"])
        self.assertIsNone(failure["usage"]["input_tokens"])
        for row in rows:
            if row["accepted"]:
                self.assertGreaterEqual(row["time_to_acceptance_seconds"] + 0.00001,
                                        row["runner_wall_seconds"] + row["grade"]["grader_seconds"])
                self.assertEqual(row["model_calls"], 1)
                self.assertEqual(row["tool_calls"], 1)
                self.assertEqual(row["usage"]["cached_input_tokens"], 5)
                self.assertEqual(row["usage"]["reasoning_output_tokens"], 8)
                self.assertFalse(row["requested_features_confirmed"])
        manifest = json.loads((self.args.output / "manifest.json").read_text())
        planned = [(p["trial"], arm) for p in manifest["schedule"] for arm in p["order"]]
        self.assertEqual(calls, planned)
        self.assertFalse(manifest["cutoff_disclosed_to_model"])
        self.assertNotIn("must-not-leak", json.dumps(manifest))
        self.assertEqual(len(list(self.args.output.glob("update-*/result.json"))), 6)
        summary = json.loads((self.args.output / "summary.json").read_text())
        self.assertEqual(summary["arms"]["baseline"]["attempts"], 3)
        self.assertEqual(summary["arms"]["candidate"]["censored"], 1)
        self.assertIsNotNone(summary["pairs"][0]["baseline_over_candidate_time"])
        self.assertIsNone(summary["pairs"][1]["baseline_over_candidate_time"])
        with self.assertRaises(ValueError):
            asyncio.run(compare.run(self.args, {"harness": fake}))

    def test_real_frozen_grader_detects_correctness_and_mutation(self):
        root = self.root / "criteria-test"
        root.mkdir()
        frozen, templates = freeze(root, ["intervals", "endurance"])
        workspace = root / "templates/intervals"
        result = asyncio.run(grade(root, "intervals", workspace, frozen, templates["intervals"]["input_sha256"]))
        self.assertFalse(result["pass"])
        (workspace / "intervals.py").write_text(CORRECT_INTERVALS)
        result = asyncio.run(grade(root, "intervals", workspace, frozen, templates["intervals"]["input_sha256"]))
        self.assertTrue(result["pass"], result)
        long = asyncio.run(grade(root, "endurance", root / "templates/endurance", frozen,
                                templates["endurance"]["input_sha256"]))
        self.assertFalse(long["pass"])
        self.assertEqual(long["original"]["total"], 33)
        self.assertTrue(long["protected_inputs_unchanged"])
        self.assertFalse(long["generated_tests"]["pass"])
        path = root / "criteria/smoke.py"
        path.chmod(0o644)
        path.write_text("print('replacement grader')")
        result = asyncio.run(grade(root, "intervals", workspace, frozen, templates["intervals"]["input_sha256"]))
        self.assertFalse(result["pass"])
        self.assertIn("Frozen grader changed", result["grader_error"])

    def test_only_known_feature_values_can_enter_manifest(self):
        for option in ("AH_ACCESS_TOKEN=secret", "AH_EVAL_ENDPOINT=https://secret", "AH_EVAL_AGENTS=-1"):
            with self.assertRaises(ValueError):
                compare.settings([option])
        self.assertEqual(compare.settings(["AH_EVAL_AGENTS=4"]), {"AH_EVAL_AGENTS": "4"})
        result = compare.accounting(response(usage=False))
        self.assertFalse(result["usage_complete"])
        self.assertIsNone(result["usage"]["output_tokens"])
        partial = response()
        del partial["events"][1]["event"]["data"]["usage"]["input_tokens_details"]
        result = compare.accounting(partial)
        self.assertFalse(result["usage_complete"])
        self.assertIsNone(result["usage"]["cached_input_tokens"])
        self.assertEqual(result["usage"]["input_tokens"], 50)
        result = compare.accounting({
            "runner": "codex", "state": "completed", "timed_out": False,
            "events": [{"seconds": 1, "event": {"method": "thread/tokenUsage/updated",
                "params": {"tokenUsage": {"total": {"inputTokens": 50, "outputTokens": 20,
                    "cachedInputTokens": 5, "reasoningOutputTokens": 8}}}}}],
        })
        self.assertTrue(result["usage_complete"])
        self.assertIsNone(result["model_calls"])
        self.assertEqual(result["usage"]["reasoning_output_tokens"], 8)

    def test_external_deadline_cancels_runner_and_retains_partial_usage(self):
        stopped = []

        async def fake(directory, *args, **kwargs):
            migrate(directory / "workspace")
            (directory / "trace.jsonl").write_text(
                "\n".join(json.dumps(row) for row in response()["events"]) + '\n{"incomplete":'
            )
            try:
                await asyncio.sleep(5)
            finally:
                stopped.append(directory.name)

        self.args.repetitions = 1
        self.args.timeout = 0.02
        rows = asyncio.run(compare.run(self.args, {"harness": fake}))
        self.assertEqual(len(stopped), 2)
        for row in rows:
            self.assertTrue(row["censored"])
            self.assertTrue(row["grade"]["pass"])
            self.assertFalse(row["accepted"])
            self.assertFalse(row["usage_complete"])
            self.assertEqual(row["usage"]["input_tokens"], 50)

    def test_concurrent_response_trace_uses_its_own_request(self):
        trace = Trace(self.root, "fixture-secret")
        for index in (1, 2):
            trace.write({"event": "provider.request", "request_id": index,
                         "data": {"model": "fixture", "input": [], "prompt_cache_key": str(index)}})
        for index in (2, 1):
            trace.write({"event": "provider.response", "request_id": index,
                         "data": {"id": "r" + str(index), "prompt_cache_key": str(index)}})
        trace.close()
        rows = [json.loads(line) for line in (self.root / "cache-metrics.jsonl").read_text().splitlines()]
        responses = [row for row in rows if row["kind"] == "cache.response"]
        self.assertEqual([row["request_index"] for row in responses], [2, 1])
        self.assertTrue(all(row["cache_key_echo_matches_request"] for row in responses))


if __name__ == "__main__":
    unittest.main()
