#!/usr/bin/env python3
"""Expand QStar response files and validate wide final-action object order."""

from __future__ import annotations

import json
import os
import shlex
import stat
import sys
from pathlib import Path


def parse_msvc_atom(line: str) -> str:
    result: list[str] = []
    index = 0
    quoted = False

    while index < len(line):
        if line[index] == '"':
            quoted = not quoted
            index += 1
            continue
        if line[index] != "\\":
            result.append(line[index])
            index += 1
            continue
        start = index
        while index < len(line) and line[index] == "\\":
            index += 1
        count = index - start
        if index < len(line) and line[index] == '"':
            result.extend("\\" for _ in range(count // 2))
            if count % 2:
                result.append('"')
                index += 1
            else:
                quoted = not quoted
                index += 1
        else:
            result.extend("\\" for _ in range(count))
    if quoted:
        raise ValueError(f"unterminated MSVC response atom: {line!r}")
    return "".join(result)


def parse_atom(line: str, style: str) -> str:
    if style == "msvc":
        return parse_msvc_atom(line)
    values = shlex.split(line, posix=True)
    if len(values) != 1:
        raise ValueError(f"response line is not one POSIX atom: {line!r}")
    return values[0]


def expand(argv: list[str], style: str) -> tuple[list[str], str]:
    expanded: list[str] = []
    response_file = ""
    for atom in argv:
        if not atom.startswith("@"):
            expanded.append(atom)
            continue
        response_file = atom[1:]
        for line in Path(response_file).read_text(encoding="utf-8").splitlines():
            expanded.append(parse_atom(line, style))
    return expanded, response_file


def output_path(argv: list[str]) -> str:
    for index, atom in enumerate(argv):
        if atom == "-o" and index + 1 < len(argv):
            return argv[index + 1]
        if atom in {"rcs", "rc"} and index + 1 < len(argv):
            return argv[index + 1]
        if atom.startswith("-femit-bin="):
            return atom.split("=", 1)[1]
    raise ValueError("fake final action has no output option")


def is_object(atom: str) -> bool:
    value = atom.split("=", 1)[1] if atom.startswith("object=") else atom
    return value.endswith((".o", ".obj"))


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in {"posix", "msvc"}:
        return 2
    style = sys.argv[1]
    argv, response_file = expand(sys.argv[2:], style)
    output = output_path(argv)
    objects = [atom.split("=", 1)[-1] for atom in argv if is_object(atom)]
    expected = None
    for atom in argv:
        if atom.startswith("--expect-objects="):
            expected = int(atom.split("=", 1)[1])
    if expected is not None and len(objects) != expected:
        raise SystemExit(
            f"object count mismatch: expected {expected}, observed {len(objects)}"
        )
    if len(objects) != len(dict.fromkeys(objects)):
        raise SystemExit("duplicate object input")
    missing = [path for path in objects if not Path(path).is_file()]
    if missing:
        preview = ", ".join(missing[:3])
        raise SystemExit(f"missing object input: {preview}")

    artifact = Path(output)
    artifact.parent.mkdir(parents=True, exist_ok=True)
    artifact.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    artifact.chmod(artifact.stat().st_mode | stat.S_IXUSR)
    record = {
        "schema": "qstar-wide-final-observation-v1",
        "style": style,
        "response_file": response_file,
        "argv": argv,
        "objects": objects,
        "output": output,
        "tool_role": next(
            (atom.split("=", 1)[1] for atom in argv if atom.startswith("--tool-role=")),
            "",
        ),
    }
    Path(f"{output}.wide.json").write_text(
        json.dumps(record, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
