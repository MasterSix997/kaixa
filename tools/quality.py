#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
TRANSLATION_UNIT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
SOURCE_ROOTS = ("include", "src", "apps", "plugins", "tests", "benchmarks", "examples")
EXCLUDED_PREFIXES = ("third_party/", "tests/workspaces/", "examples/generated_cmake/")


def repository_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    files = []
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        relative = Path(os.fsdecode(raw))
        normalized = relative.as_posix()
        if normalized.startswith(EXCLUDED_PREFIXES):
            continue
        files.append(relative)
    return files


def source_files(files: list[Path] | None = None) -> list[Path]:
    candidates = repository_files() if files is None else files
    return [
        ROOT / path
        for path in candidates
        if path.parts and path.parts[0] in SOURCE_ROOTS and path.suffix.lower() in SOURCE_SUFFIXES
    ]


def changed_files(base: str | None) -> list[Path]:
    command = ["git", "diff", "--name-only", "--diff-filter=ACMR", "-z"]
    if base:
        revision = subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", f"{base}^{{commit}}"],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if revision.returncode != 0:
            raise RuntimeError(f"format comparison revision does not exist: {base}")
        command.append(f"{base}...HEAD")
    else:
        command.append("HEAD")
    command.append("--")
    result = subprocess.run(command, cwd=ROOT, check=True, capture_output=True)
    paths = {Path(os.fsdecode(raw)) for raw in result.stdout.split(b"\0") if raw}
    if not base:
        tracked = set(repository_files())
        committed = {
            Path(os.fsdecode(raw))
            for raw in subprocess.run(
                ["git", "ls-files", "--cached", "-z"],
                cwd=ROOT,
                check=True,
                capture_output=True,
            ).stdout.split(b"\0")
            if raw
        }
        paths.update(tracked - committed)
    return sorted(paths)


def executable(name: str) -> str:
    path = shutil.which(name)
    if path:
        return path
    raise RuntimeError(f"required executable `{name}` was not found on PATH")


def run(command: list[str], *, env: dict[str, str] | None = None) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    completed = subprocess.run(command, cwd=ROOT, env=env)
    if completed.returncode != 0:
        raise RuntimeError(f"command failed with exit code {completed.returncode}")


def check_format(fix: bool, all_files: bool, changed_from: str | None) -> None:
    files = source_files() if all_files else source_files(changed_files(changed_from))
    if not files:
        print("clang-format: no changed source files")
        return
    command = [executable("clang-format")]
    if fix:
        command.append("-i")
    else:
        command.extend(("--dry-run", "--Werror"))
    command.extend(str(path) for path in files)
    run(command)
    print(f"clang-format: passed for {len(files)} source file(s)")


def check_rules() -> None:
    failures: list[str] = []
    conflict = re.compile(r"^(<<<<<<<|=======|>>>>>>>)")
    namespace_comment = re.compile(r"^\s*}\s*//\s*namespace\b")
    trailing_member = re.compile(r"\b[a-zA-Z][a-zA-Z0-9]*_;\s*(?://.*)?$")
    for relative in repository_files():
        if relative.suffix.lower() not in SOURCE_SUFFIXES | {".md", ".toml", ".yml", ".yaml", ".py"}:
            continue
        path = ROOT / relative
        text = path.read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), 1):
            location = f"{relative.as_posix()}:{number}"
            if "\t" in line:
                failures.append(f"{location}: tab character")
            if line.rstrip() != line:
                failures.append(f"{location}: trailing whitespace")
            if conflict.match(line):
                failures.append(f"{location}: merge conflict marker")
            if namespace_comment.match(line):
                failures.append(f"{location}: namespace closing comments are forbidden")
            if relative.suffix.lower() in {".h", ".hh", ".hpp", ".hxx"} and re.search(r"\busing\s+namespace\b", line):
                failures.append(f"{location}: using-directive in a header")
            if relative.suffix.lower() in {".h", ".hh", ".hpp", ".hxx"} and trailing_member.search(line):
                failures.append(f"{location}: data members use the m_ prefix, not a trailing underscore")
    if failures:
        print("\n".join(failures))
        raise RuntimeError(f"repository rules failed with {len(failures)} violation(s)")
    print("repository rules: passed")


def lizard_environment() -> dict[str, str]:
    environment = os.environ.copy()
    if importlib.util.find_spec("lizard") is not None:
        return environment
    local_tools = ROOT / ".kaixa" / "quality-tools"
    if (local_tools / "lizard.py").is_file():
        existing = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = str(local_tools) + (os.pathsep + existing if existing else "")
        return environment
    raise RuntimeError(
        "Lizard is not installed; run `python -m pip install -r tools/quality-requirements.txt`"
    )


def check_complexity() -> None:
    command = [
        sys.executable,
        "-m",
        "lizard",
        "-l",
        "cpp",
        "--csv",
        *SOURCE_ROOTS,
        "-x",
        "third_party/*",
        "-x",
        "tests/workspaces/*",
        "-x",
        "examples/generated_cmake/*",
    ]
    print("+", subprocess.list2cmdline(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=lizard_environment(),
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )

    baseline = {
        tuple(part.strip() for part in line.split("|", 1))
        for line in (ROOT / "tools" / "complexity-baseline.txt").read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    violations: list[str] = []
    for row in csv.reader(completed.stdout.splitlines()):
        if len(row) != 11:
            raise RuntimeError(f"unexpected Lizard CSV row with {len(row)} columns")
        nloc, ccn, _, parameters, length, _, filename, _, signature, start, _ = row
        identity = (filename.replace("\\", "/"), signature)
        exceeded = []
        if int(ccn) > 40:
            exceeded.append(f"CCN {ccn} > 40")
        if int(length) > 250:
            exceeded.append(f"length {length} > 250")
        if int(parameters) > 7:
            exceeded.append(f"parameters {parameters} > 7")
        if exceeded and identity not in baseline:
            violations.append(
                f"{identity[0]}:{start}: complexity violation: {signature} "
                f"({', '.join(exceeded)}; NLOC {nloc})"
            )
    if violations:
        print("\n".join(violations))
        raise RuntimeError(f"complexity check failed with {len(violations)} violation(s)")
    print("complexity: passed")


def find_compilation_database(explicit: Path | None) -> Path:
    if explicit:
        candidate = explicit / "compile_commands.json" if explicit.is_dir() else explicit
        if candidate.is_file():
            return candidate.resolve()
        raise RuntimeError(f"compilation database does not exist: {candidate}")

    candidates = list((ROOT / ".kaixa" / "build" / "cmake").glob("**/compile_commands.json"))
    candidates.extend((ROOT / "build").glob("**/compile_commands.json") if (ROOT / "build").exists() else [])
    if not candidates:
        raise RuntimeError(
            "compile_commands.json was not found; run `kaixa generate --config quality` first"
        )
    return max(candidates, key=lambda path: path.stat().st_mtime).resolve()


def check_tidy(database_argument: Path | None, jobs: int) -> None:
    database = find_compilation_database(database_argument)
    entries = json.loads(database.read_text(encoding="utf-8"))
    compiled = {Path(entry["file"]).resolve() for entry in entries}
    sources = [path.resolve() for path in source_files() if path.suffix.lower() in TRANSLATION_UNIT_SUFFIXES]
    selected = [path for path in sources if path in compiled]
    if not selected:
        raise RuntimeError(f"no first-party translation units were found in {database}")

    tidy = executable("clang-tidy")

    def analyze(path: Path) -> tuple[Path, int, str]:
        completed = subprocess.run(
            [tidy, "--quiet", "-p", str(database.parent), str(path)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        return path, completed.returncode, completed.stdout

    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as executor:
        futures = [executor.submit(analyze, path) for path in selected]
        for future in concurrent.futures.as_completed(futures):
            path, return_code, output = future.result()
            if output:
                print(output, end="" if output.endswith("\n") else "\n")
            if return_code != 0:
                failures += 1
                print(f"clang-tidy failed: {path.relative_to(ROOT)}")
    if failures:
        raise RuntimeError(f"clang-tidy failed for {failures} translation unit(s)")
    print(f"clang-tidy: passed for {len(selected)} translation unit(s)")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Kaixa's reproducible code-quality checks.")
    parser.add_argument(
        "checks",
        nargs="*",
        choices=("rules", "format", "complexity", "tidy"),
        default=None,
    )
    parser.add_argument("--fix", action="store_true", help="Apply clang-format instead of checking it.")
    parser.add_argument(
        "--all-files",
        action="store_true",
        help="Check every source file instead of changed files only.",
    )
    parser.add_argument(
        "--changed-from",
        help="Check formatting for source files changed since this Git revision.",
    )
    parser.add_argument("--compile-commands", type=Path, help="Compilation database file or directory.")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    checks = arguments.checks or ("rules", "format", "complexity", "tidy")
    try:
        for check in checks:
            print(f"\n== {check} ==", flush=True)
            if check == "rules":
                check_rules()
            elif check == "format":
                check_format(arguments.fix, arguments.all_files, arguments.changed_from)
            elif check == "complexity":
                check_complexity()
            elif check == "tidy":
                check_tidy(arguments.compile_commands, arguments.jobs)
    except (OSError, RuntimeError, subprocess.SubprocessError) as failure:
        print(f"quality check failed: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
