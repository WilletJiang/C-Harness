"""Frozen host graders and fixed workspace templates for paired evaluations."""

import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time
from .smoke.cases import CASES
from .endurance import task
from .runners.common import terminate


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tree_hashes(root):
    return {str(p.relative_to(root)): sha256(p) for p in sorted(root.rglob("*")) if p.is_file()}


def freeze(root, names):
    criteria = root / "criteria"
    criteria.mkdir()
    sources = Path(__file__).parent
    (criteria / "__init__.py").write_text("")
    shutil.copy2(sources / "smoke/cases.py", criteria / "smoke.py")
    with (criteria / "smoke.py").open("a") as output:
        output.write('\nif __name__ == "__main__":\n'
                     '    case = next(c for c in CASES if c.case_name == sys.argv[2])\n'
                     '    print(json.dumps(case.validate(Path(sys.argv[1]))))\n')
    for name in ("checks.py", "store_checks.py", "quality.py", "task.py"):
        shutil.copy2(sources / "endurance" / name, criteria / name)
    hashes = tree_hashes(criteria)
    for path in criteria.iterdir():
        path.chmod(0o444)
    cases = {case.case_name: case for case in CASES}
    templates = {}
    for name in names:
        workspace = root / "templates" / name
        prompt = task.prepare(workspace) if name == "endurance" else cases[name].prepare(workspace)
        (root / (name + ".prompt.txt")).write_text(prompt)
        templates[name] = {
            "prompt": prompt, "prompt_file": name + ".prompt.txt",
            "prompt_sha256": hashlib.sha256(prompt.encode()).hexdigest(),
            "input_sha256": tree_hashes(workspace),
        }
    return hashes, templates


async def invoke(criteria, module, workspace, extra=()):
    bootstrap = (
        "import runpy,sys; root,module,*args=sys.argv[1:]; "
        "sys.path.insert(0,root); sys.argv=[module,*args]; "
        "runpy.run_module(module,run_name='__main__')"
    )
    process = await asyncio.create_subprocess_exec(
        sys.executable, "-I", "-B", "-c", bootstrap, str(criteria.parent),
        "criteria." + module, str(workspace), *extra,
        cwd=criteria.parent,
        env={"PATH": os.defpath, "PYTHONDONTWRITEBYTECODE": "1"},
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, start_new_session=True,
    )
    try:
        stdout, stderr = await asyncio.wait_for(process.communicate(), 60)
        (workspace.parent / ("grader-" + module + ".stdout")).write_bytes(stdout)
        (workspace.parent / ("grader-" + module + ".stderr")).write_bytes(stderr)
        result = json.loads(stdout.decode().splitlines()[-1])
        if process.returncode:
            result["grader_error"] = f"grader exit {process.returncode}"
        return result
    except (asyncio.TimeoutError, UnicodeError, ValueError, IndexError) as exc:
        return {"grader_error": type(exc).__name__}
    finally:
        await terminate(process)


async def grade(root, name, workspace, frozen, initial):
    started = time.monotonic()
    criteria = root / "criteria"
    try:
        if tree_hashes(criteria) != frozen:
            raise ValueError("Frozen grader changed before grading")
        if name != "endurance":
            result = await invoke(criteria, "smoke", workspace, (name,))
        else:
            original = await invoke(criteria, "checks", workspace)
            audit = await invoke(criteria, "quality", workspace)
            generated = await generated_tests(workspace)
            unchanged = all(
                not (workspace / name).is_symlink() and (workspace / name).is_file()
                and sha256(workspace / name) == initial[name]
                for name in ("SPEC.md", "tests/test_smoke.py")
            )
            readme = workspace / "README.md"
            deliverables = (
                readme.is_file() and not readme.is_symlink() and readme.stat().st_size > 300
                and any(p.name != "test_smoke.py" for p in (workspace / "tests").rglob("test_*.py"))
            )
            result = {
                "original": original, "audit": audit, "generated_tests": generated,
                "protected_inputs_unchanged": unchanged, "deliverables_present": bool(deliverables),
                "pass": all(
                    part.get("total", 0) > 0 and part.get("passed") == part.get("total")
                    and not part.get("grader_error") for part in (original, audit)
                ) and unchanged and bool(deliverables) and generated["pass"],
            }
        if tree_hashes(criteria) != frozen:
            raise ValueError("Frozen grader changed during grading")
        result["pass"] = bool(result.get("pass")) and not result.get("grader_error")
    except Exception as exc:
        result = {"pass": False, "grader_error": f"{type(exc).__name__}: {exc}"}
    result["grader_seconds"] = round(time.monotonic() - started, 6)
    return result


async def generated_tests(workspace):
    process = await asyncio.create_subprocess_exec(
        sys.executable, "-B", "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py",
        cwd=workspace, env={"PATH": os.defpath, "PYTHONDONTWRITEBYTECODE": "1"},
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, start_new_session=True,
    )
    try:
        stdout, stderr = await asyncio.wait_for(process.communicate(), 60)
        return {"pass": process.returncode == 0, "exit_code": process.returncode,
                "stdout": stdout.decode(errors="replace")[-4000:],
                "stderr": stderr.decode(errors="replace")[-4000:]}
    except asyncio.TimeoutError:
        return {"pass": False, "timed_out": True}
    finally:
        await terminate(process)
