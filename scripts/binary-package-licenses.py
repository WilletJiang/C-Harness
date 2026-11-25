"""Collect licensing files for the Rust static library using locked Cargo metadata."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

manifest, output = map(Path, sys.argv[1:])
host = next(line.removeprefix("host: ") for line in subprocess.check_output(
    ["rustc", "-vV"], text=True
).splitlines() if line.startswith("host: "))
data = json.loads(subprocess.check_output(
    ["cargo", "metadata", "--locked", "--format-version=1", "--manifest-path",
     str(manifest), "--filter-platform", host], text=True
))
root = output / "rust"
root.mkdir()
summary = []
used = {node["id"] for node in data["resolve"]["nodes"]}
for crate in sorted(data["packages"], key=lambda item: (item["name"], item["version"])):
    if crate["id"] not in used or crate["source"] is None:
        continue
    label = f"{crate['name']}-{crate['version']}"
    summary.append(f"{label}\t{crate.get('license') or crate.get('license_file') or ''}")
    origin = Path(crate["manifest_path"]).parent
    files = [entry for entry in origin.iterdir() if entry.is_file() and
             re.match(r"(?i)^(licen[cs]e|copying|notice|ofl)([._-]|$)", entry.name)]
    if crate["license_file"]:
        explicit = origin / crate["license_file"]
        if explicit.is_file() and explicit not in files:
            files.append(explicit)
    if files:
        dest = root / label
        dest.mkdir()
        for file in files:
            shutil.copyfile(file, dest / file.name)
(root / "DEPENDENCIES.tsv").write_text("\n".join(summary) + "\n")
