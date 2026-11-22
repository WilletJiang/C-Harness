"""Three paired, concurrent long-task trials with an undisclosed external cutoff."""

import argparse, asyncio, hashlib, json, os, pathlib, shutil, subprocess
from .task import prepare, protected_hashes, PROMPT, SPEC
from ..runners.common import StartBarrier
from ..runners.harness import run_harness
from ..compare import accounting
from ..compare_grade import invoke, generated_tests, tree_hashes
from . import visual
from ..runners.codex import run_codex
from ..runners.wire import verify as verify_wire


async def grade(directory, protected, criteria, image_task=False):
    workspace = directory / "workspace"
    env = {
        "PATH": "/usr/bin:/bin",
        "HOME": str(directory),
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    p = await asyncio.create_subprocess_exec(
        "/usr/bin/python3",
        "-B",
        "-m",
        "criteria.checks",
        str(workspace),
        cwd=criteria.parent,
        env=env,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        start_new_session=True,
    )
    try:
        stdout, stderr = await asyncio.wait_for(p.communicate(), 45)
        result = json.loads(stdout.decode().splitlines()[-1])
    except (asyncio.TimeoutError, json.JSONDecodeError, IndexError) as e:
        if p.returncode is None:
            p.kill()
        await p.wait()
        result = {"passed": 0, "total": 33, "grader_error": type(e).__name__}
    try:
        result["protected_inputs_unchanged"] = protected_hashes(workspace) == protected
    except OSError:
        result["protected_inputs_unchanged"] = False
    result["readme_written"] = (workspace / "README.md").exists() and len(
        (workspace / "README.md").read_text()
    ) > 300
    result["additional_test_files"] = len(
        [
            p
            for p in (workspace / "tests").rglob("test_*.py")
            if p.name != "test_smoke.py"
        ]
    )
    result["audit"] = await invoke(criteria, "quality", workspace)
    if image_task:
        result["visual"] = await invoke(criteria, "visual_checks", workspace)
    result["generated_tests"] = await generated_tests(workspace)
    return result


async def run(args):
    root = args.output.resolve()
    if root.exists() and any(root.iterdir()):
        raise RuntimeError("A fresh empty output directory is required")
    root.mkdir(parents=True, exist_ok=True)
    if not os.environ.get("AH_ACCESS_TOKEN"):
        raise RuntimeError("Authorized subscription token required")
    template = root / "template"
    prompt = visual.prepare(template) if args.visual else prepare(template)
    initial = tree_hashes(template)
    image_names = ["references/" + name for name in visual.IMAGES] if args.visual else []
    wire = await verify_wire(args.codex, args.model, args.effort,
                             images=[template / name for name in image_names])
    criteria = root / "criteria"
    criteria.mkdir()
    for name in ("__init__.py", "checks.py", "store_checks.py", "quality.py", "task.py",
                 "visual.py", "visual_checks.py"):
        shutil.copy2(pathlib.Path(__file__).with_name(name), criteria / name)
    manifest = {
        "model": args.model,
        "reasoning": args.effort,
        "duration_seconds": args.duration,
        "repetitions": args.repetitions,
        "same_service_route": True,
        "codex_wire_request": wire,
        "endpoint": "https://api.openai.com/v1/responses",
        "prompt_sha256": hashlib.sha256(prompt.encode()).hexdigest(),
        "input_sha256": initial, "image_files": image_names,
        "images_supplied_as_native_input": bool(image_names),
        "runtime_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "driver_sha256": hashlib.sha256(args.driver.read_bytes()).hexdigest(),
        "features": {"skills":False,"mcp":False,"plugins":False,"agents":False,"auto_context":False},
        "spec_sha256": hashlib.sha256(SPEC.encode()).hexdigest(),
        "grader_sha256": hashlib.sha256(
            b"".join(
                (criteria / name).read_bytes()
                for name in ("checks.py", "store_checks.py")
            )
        ).hexdigest(),
        "cutoff_disclosed_to_model": False,
        "followup_prompts": 0,
        "codex_version": subprocess.check_output(
            [args.codex, "--version"], text=True
        ).strip(),
    }
    manifest["grader_files_sha256"] = tree_hashes(criteria)
    for path in criteria.iterdir():
        path.chmod(0o444)
    (root / "prompt.txt").write_text(prompt)
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2))
    # Smoke runs are explicitly separate from task/token totals.
    preflights = []
    for runner in ("harness", "codex"):
        d = root / ("preflight-" + runner)
        d.mkdir()
        (d / "workspace").mkdir()
        (d / "workspace" / "README.md").write_text(
            "Empty model access probe workspace.\n"
        )
        probe_prompt = "Read README.md using a tool, then reply with exactly READY. Do not change files."
        if runner == "harness":
            r = await run_harness(
                d, args.driver, args.codex, args.model, args.effort, probe_prompt, 120
            )
        else:
            r = await run_codex(d, args.codex, args.model, args.effort, probe_prompt, 120)
        r = accounting(r)
        preflights.append(r)
        print(json.dumps({"preflight": r}), flush=True)
        (root / "preflights.json").write_text(json.dumps(preflights, indent=2))
        if runner == "harness" and (
            r["served_models"] != [args.model]
            or r["served_reasoning_efforts"] != [args.effort]
        ):
            raise RuntimeError(
                "Harness response did not confirm requested model and effort"
            )
        if r["state"] != "completed" or r.get("failure") or r["errors"]:
            raise RuntimeError(
                runner + " preflight did not verify requested model/route"
            )
        if r["observed_tool_actions"] < 1:
            raise RuntimeError(runner + " preflight did not execute a tool")
    barrier = StartBarrier(args.repetitions * 2)
    tasks = []
    results = []

    async def one(runner, index):
        d = root / (runner + "-" + str(index))
        shutil.copytree(template, d / "workspace")
        assert tree_hashes(d / "workspace") == initial
        images = [d / "workspace" / name for name in image_names]
        hashes = protected_hashes(d / "workspace")
        print(json.dumps({"launching": runner, "trial": index}), flush=True)
        if runner == "harness":
            r = await run_harness(
                d,
                args.driver,
                args.codex,
                args.model,
                args.effort,
                prompt,
                args.duration,
                barrier, images=images,
            )
        else:
            r = await run_codex(
                d, args.codex, args.model, args.effort, prompt, args.duration, barrier, images=images
            )
        r = accounting(r)
        r["trial"] = index
        r["model"] = args.model
        r["reasoning"] = args.effort
        assert tree_hashes(criteria) == manifest["grader_files_sha256"]
        r["grade"] = await grade(d, hashes, criteria, args.visual)
        r["grade"]["visual_inputs_unchanged"] = all(
            (d / "workspace" / name).is_file() and
            hashlib.sha256((d / "workspace" / name).read_bytes()).hexdigest() == initial[name]
            for name in [*image_names, *(["SCENARIO.md"] if args.visual else [])])
        assert tree_hashes(criteria) == manifest["grader_files_sha256"]
        sections = [r["grade"], r["grade"]["audit"]]
        if args.visual:
            sections.append(r["grade"]["visual"])
        r["accepted"] = (r["state"] == "completed" and not r["timed_out"] and not r.get("failure")
                         and all(part.get("total",0) > 0 and part.get("passed") == part.get("total")
                                 and not part.get("grader_error") for part in sections)
                         and r["grade"]["generated_tests"]["pass"]
                         and r["grade"]["readme_written"] and r["grade"]["additional_test_files"] > 0
                         and r["grade"]["protected_inputs_unchanged"]
                         and r["grade"]["visual_inputs_unchanged"])
        (d / "result.json").write_text(json.dumps(r, ensure_ascii=False, indent=2))
        results.append(r)
        (root / "results.json").write_text(
            json.dumps(results, ensure_ascii=False, indent=2)
        )
        print(json.dumps({"finished": r}, ensure_ascii=False), flush=True)
        return r

    for i in range(1, args.repetitions + 1):
        for name in ("harness", "codex"):
            tasks.append(asyncio.create_task(one(name, i)))
    while any(not task.done() for task in tasks):
        await asyncio.sleep(10)
        summary = []
        for d in sorted(root.glob("*-[123]")):
            path = d / "progress.json"
            if path.exists():
                try:
                    last = json.loads(path.read_text())
                except json.JSONDecodeError:
                    continue
                summary.append(
                    {
                        "run": d.name,
                        **last,
                        "result_ready": (d / "result.json").exists(),
                    }
                )
        print(json.dumps({"progress": summary}), flush=True)
    await asyncio.gather(*tasks)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=pathlib.Path, required=True)
    p.add_argument("--driver", type=pathlib.Path, required=True)
    p.add_argument("--codex", default=shutil.which("codex"))
    p.add_argument("--model", default="gpt-6.1-terra")
    p.add_argument(
        "--effort", default="xhigh", choices=["low", "medium", "high", "xhigh"]
    )
    p.add_argument("--visual", action="store_true", help="Attach image-only scenario facts and grade integration")
    p.add_argument("--duration", type=int, default=600)
    p.add_argument("--repetitions", type=int, default=3)
    args = p.parse_args()
    if not 1 <= args.duration <= 1800 or not 1 <= args.repetitions <= 3:
        p.error("duration 1..1800; repetitions 1..3")
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
