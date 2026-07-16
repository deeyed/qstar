#!/usr/bin/env python3
"""Validate QStar compatibility against opt-in downstream worktrees."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT_DIR = REPO_ROOT / "tests" / "downstream-compat"
PROJECT_ENV = {
    "parus": "QSTAR_PARUS_ROOT",
    "delos": "QSTAR_DELOS_ROOT",
    "ribon": "QSTAR_RIBON_ROOT",
}


class GateFailure(RuntimeError):
    pass


def enabled(name: str) -> bool:
    return os.environ.get(name, "0").lower() in {"1", "true", "yes", "on"}


def fail(message: str) -> None:
    raise GateFailure(message)


def tail(text: str, lines: int = 100) -> str:
    return "\n".join(text.splitlines()[-lines:])


def run(argv: list[str], cwd: Path, label: str) -> str:
    proc = subprocess.run(
        argv,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        fail(
            f"{label} failed with exit {proc.returncode}\n"
            f"stdout tail:\n{tail(proc.stdout)}\n"
            f"stderr tail:\n{tail(proc.stderr)}"
        )
    return proc.stdout


def source_commit(root: Path) -> str:
    proc = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return proc.stdout.strip() if proc.returncode == 0 else "unknown"


def copy_project(name: str, source: Path, destination: Path) -> None:
    def ignore(_path: str, names: list[str]) -> list[str]:
        ignored = [entry for entry in names if entry in {"build", "__pycache__"}]
        if name != "delos" and ".git" in names:
            ignored.append(".git")
        return ignored

    shutil.copytree(source, destination, symlinks=True, ignore=ignore)

    generated_config = source / "build" / "generated" / "config"
    if generated_config.is_dir():
        shutil.copytree(
            generated_config,
            destination / "build" / "generated" / "config",
            symlinks=True,
            dirs_exist_ok=True,
        )


def require_minimums(snapshot: dict, graph: dict, commands: dict) -> None:
    values = {
        "target_count": graph.get("target_count", 0),
        "generated_action_count": graph.get("generated_action_count", 0),
        "stage_count": graph.get("stage_count", 0),
        "command_count": commands.get("command_count", 0),
    }
    for key, minimum in snapshot.get("minimums", {}).items():
        actual = values.get(key)
        if actual is None or actual < minimum:
            fail(f"{snapshot['project']}: {key}={actual} is below snapshot minimum {minimum}")

    kind_counts = Counter(target.get("kind", "") for target in graph.get("targets", []))
    for kind, minimum in snapshot.get("minimum_target_kinds", {}).items():
        actual = kind_counts.get(kind, 0)
        if actual < minimum:
            fail(
                f"{snapshot['project']}: target kind {kind} count {actual} "
                f"is below snapshot minimum {minimum}"
            )


def require_subset(actual: list[str], required: list[str], context: str) -> None:
    missing = [value for value in required if value not in actual]
    if missing:
        fail(f"{context}: missing snapshot values: {', '.join(missing)}")


def validate_target(snapshot: dict, actual: dict) -> None:
    label = snapshot["label"]
    if actual.get("kind") != snapshot["kind"]:
        fail(
            f"target {label}: kind={actual.get('kind')} expected={snapshot['kind']}"
        )
    expected_context = snapshot.get("compile_context")
    if expected_context and actual.get("compile_context") != expected_context:
        fail(
            f"target {label}: compile_context={actual.get('compile_context')} "
            f"expected={expected_context}"
        )
    artifact_paths = [artifact.get("path", "") for artifact in actual.get("artifacts", [])]
    require_subset(artifact_paths, snapshot.get("artifacts", []), f"target {label} artifacts")


def validate_graph(snapshot: dict, graph: dict, commands: dict) -> None:
    project = snapshot["project"]
    if graph.get("schema") != "qstar-targets-v1":
        fail(f"{project}: unexpected target schema {graph.get('schema')}")
    if commands.get("schema") != "qstar-commands-v1":
        fail(f"{project}: unexpected command schema {commands.get('schema')}")

    require_minimums(snapshot, graph, commands)

    targets = {target["label"]: target for target in graph.get("targets", [])}
    for required in snapshot.get("targets", []):
        actual = targets.get(required["label"])
        if actual is None:
            fail(f"{project}: missing target {required['label']}")
        validate_target(required, actual)

    generated = {
        action["label"]: action for action in graph.get("generated_actions", [])
    }
    for required in snapshot.get("generated_actions", []):
        label = required["label"]
        actual = generated.get(label)
        if actual is None:
            fail(f"{project}: missing generated action {label}")
        if actual.get("toolset", "") != required.get("toolset", ""):
            fail(
                f"{project}: generated action {label} toolset={actual.get('toolset')} "
                f"expected={required.get('toolset', '')}"
            )
        require_subset(
            actual.get("outputs", []), required.get("outputs", []), f"generated action {label} outputs"
        )

    stages = {stage["label"]: stage for stage in graph.get("stages", [])}
    for required in snapshot.get("stages", []):
        label = required["label"]
        actual = stages.get(label)
        if actual is None:
            fail(f"{project}: missing stage {label}")
        if actual.get("root") != required.get("root"):
            fail(
                f"{project}: stage {label} root={actual.get('root')} "
                f"expected={required.get('root')}"
            )
        destinations = [entry.get("dst", "") for entry in actual.get("files", [])]
        require_subset(destinations, required.get("destinations", []), f"stage {label}")

    command_names = [command["name"] for command in commands.get("commands", [])]
    require_subset(command_names, snapshot.get("commands", []), f"{project} commands")


def command_argv_lines(output: str) -> list[str]:
    return [line.strip() for line in output.splitlines() if "command_argv " in line]


def qstar_prefix(qstar: Path, generator: str | None = None) -> list[str]:
    argv = [str(qstar)]
    if generator:
        argv.extend(["-G", generator])
    argv.extend(["--file", "qstar.lua", "--color", "never", "--progress", "off"])
    return argv


def validate_dry_runs(
    qstar: Path, project_root: Path, snapshot: dict, evidence_dir: Path
) -> None:
    for index, spec in enumerate(snapshot.get("dry_runs", [])):
        backend_argv: dict[str, list[str]] = {}
        for generator in ("stella", "ninja"):
            output = run(
                qstar_prefix(qstar, generator) + ["dry-run", spec["label"]],
                project_root,
                f"{snapshot['project']} {generator} dry-run {spec['label']}",
            )
            (evidence_dir / f"dry-run-{index}-{generator}.txt").write_text(
                output, encoding="utf-8"
            )
            for pattern in spec.get("contains", []):
                if pattern not in output:
                    fail(
                        f"{snapshot['project']} {generator} dry-run {spec['label']}: "
                        f"missing pattern {pattern!r}"
                    )
            backend_argv[generator] = command_argv_lines(output)

        if not backend_argv["stella"]:
            fail(f"{snapshot['project']} dry-run {spec['label']}: no command_argv lines")
        if backend_argv["stella"] != backend_argv["ninja"]:
            fail(
                f"{snapshot['project']} dry-run {spec['label']}: "
                "Stella/Ninja command_argv snapshots differ"
            )


def run_optional_actions(
    qstar: Path, project_root: Path, snapshot: dict, key: str, evidence_dir: Path
) -> None:
    for index, spec in enumerate(snapshot.get(key, [])):
        command = spec.get("command", "build")
        output = run(
            qstar_prefix(qstar, spec.get("generator", "stella"))
            + [command, spec["label"]],
            project_root,
            f"{snapshot['project']} {key} {spec['label']}",
        )
        (evidence_dir / f"{key}-{index}.txt").write_text(output, encoding="utf-8")


def validate_project(
    name: str, source: Path, qstar: Path, temp_root: Path, run_builds: bool, run_smokes: bool
) -> None:
    snapshot_path = SNAPSHOT_DIR / f"{name}.json"
    snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
    if snapshot.get("schema") != "qstar-downstream-compat-v1":
        fail(f"{name}: unexpected snapshot schema {snapshot.get('schema')}")

    actual_commit = source_commit(source)
    expected_commit = snapshot.get("source_commit", "unknown")
    if actual_commit != expected_commit:
        print(
            f"qstar-downstream-safe-upgrade: {name} source_commit={actual_commit} "
            f"snapshot_commit={expected_commit} mode=compatible-subset"
        )

    project_root = temp_root / name
    evidence_dir = temp_root / "evidence" / name
    evidence_dir.mkdir(parents=True)
    copy_project(name, source, project_root)

    check_output = run(
        qstar_prefix(qstar) + ["check", "//..."], project_root, f"{name} check //..."
    )
    (evidence_dir / "check.txt").write_text(check_output, encoding="utf-8")

    targets_output = run(
        qstar_prefix(qstar) + ["list-targets", "--format", "json"],
        project_root,
        f"{name} list-targets",
    )
    commands_output = run(
        qstar_prefix(qstar) + ["commands", "--format", "json"],
        project_root,
        f"{name} commands",
    )
    (evidence_dir / "targets.json").write_text(targets_output, encoding="utf-8")
    (evidence_dir / "commands.json").write_text(commands_output, encoding="utf-8")

    graph = json.loads(targets_output)
    commands = json.loads(commands_output)
    validate_graph(snapshot, graph, commands)
    validate_dry_runs(qstar, project_root, snapshot, evidence_dir)

    if run_builds:
        run_optional_actions(qstar, project_root, snapshot, "builds", evidence_dir)
    if run_smokes:
        run_optional_actions(qstar, project_root, snapshot, "smokes", evidence_dir)

    print(
        f"qstar-downstream-safe-upgrade: {name} status=ok "
        f"targets={graph['target_count']} generated={graph['generated_action_count']} "
        f"stages={graph['stage_count']} commands={commands['command_count']} "
        f"builds={'on' if run_builds else 'off'} smokes={'on' if run_smokes else 'off'}"
    )


def main() -> int:
    qstar = Path(os.environ.get("QSTAR_TEST_QSTAR", REPO_ROOT / "build/bin/qstar")).resolve()
    if not qstar.is_file():
        fail(f"qstar binary not found: {qstar}")

    roots: dict[str, Path] = {}
    for name, env_name in PROJECT_ENV.items():
        value = os.environ.get(env_name, "").strip()
        if value:
            root = Path(value).expanduser().resolve()
            if not (root / "qstar.lua").is_file():
                fail(f"{env_name} does not contain qstar.lua: {root}")
            roots[name] = root

    if "ribon" not in roots and "parus" in roots:
        candidate = roots["parus"] / "stand" / "Ribon"
        if (candidate / "qstar.lua").is_file():
            roots["ribon"] = candidate

    missing = [name for name in PROJECT_ENV if name not in roots]
    if missing and enabled("QSTAR_DOWNSTREAM_REQUIRED"):
        fail(f"required downstream roots missing: {', '.join(missing)}")
    if not roots:
        print(
            "qstar-downstream-safe-upgrade: status=skipped "
            "reason=set-QSTAR_PARUS_ROOT-QSTAR_DELOS_ROOT-QSTAR_RIBON_ROOT"
        )
        return 0

    temp_root = Path(
        tempfile.mkdtemp(prefix="qstar-downstream-safe-upgrade.", dir=os.environ.get("TMPDIR"))
    )
    keep_temp = enabled("QSTAR_DOWNSTREAM_KEEP_TEMP")
    success = False
    try:
        for name in PROJECT_ENV:
            if name in roots:
                validate_project(
                    name,
                    roots[name],
                    qstar,
                    temp_root,
                    enabled("QSTAR_DOWNSTREAM_RUN_BUILDS"),
                    enabled("QSTAR_DOWNSTREAM_RUN_SMOKES"),
                )
        success = True
        evidence = str(temp_root / "evidence") if keep_temp else "discarded"
        print(
            f"qstar-downstream-safe-upgrade: status=ok projects={len(roots)} "
            f"evidence={evidence}"
        )
        return 0
    finally:
        if success and not keep_temp:
            shutil.rmtree(temp_root)
        else:
            print(f"qstar-downstream-safe-upgrade: preserved={temp_root}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GateFailure as exc:
        print(f"qstar-downstream-safe-upgrade: {exc}", file=sys.stderr)
        raise SystemExit(1)
