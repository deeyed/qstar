#!/usr/bin/env python3
"""Materialize generated object outputs from a QStar response file."""

from __future__ import annotations

import shlex
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] != "posix":
        return 2
    argv: list[str] = []
    for atom in sys.argv[2:]:
        if not atom.startswith("@"):
            argv.append(atom)
            continue
        for line in Path(atom[1:]).read_text(encoding="utf-8").splitlines():
            values = shlex.split(line, posix=True)
            if len(values) != 1:
                raise SystemExit(f"invalid POSIX response atom: {line!r}")
            argv.append(values[0])
    outputs = [Path(atom) for atom in argv if atom.endswith((".o", ".obj"))]
    if len(outputs) != 49:
        raise SystemExit(f"expected 49 generated objects, got {len(outputs)}")
    for output in outputs:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(f"generated object {output.name}\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
