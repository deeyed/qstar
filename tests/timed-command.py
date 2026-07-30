#!/usr/bin/env python3
"""Run one command and report elapsed time plus child peak RSS."""

from __future__ import annotations

import json
import resource
import subprocess
import sys
import time
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 5 or sys.argv[3] != "--":
        print("usage: timed-command.py stdout stderr -- command ...", file=sys.stderr)
        return 2
    stdout_path = Path(sys.argv[1])
    stderr_path = Path(sys.argv[2])
    command = sys.argv[4:]
    start = time.monotonic_ns()
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr, check=False)
    elapsed_ms = max(0, (time.monotonic_ns() - start) // 1_000_000)
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    if sys.platform == "darwin":
        peak //= 1024
    print(
        json.dumps(
            {
                "elapsed_ms": elapsed_ms,
                "peak_kb": int(peak),
                "returncode": result.returncode,
            },
            sort_keys=True,
        )
    )
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
