#!/usr/bin/env python3
"""Verify fake wide-final observation records without shell JSON parsing."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("record", type=Path)
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--style", choices=("posix", "msvc"), required=True)
    parser.add_argument("--first")
    parser.add_argument("--last")
    parser.add_argument("--response", choices=("yes", "no"))
    args = parser.parse_args()

    value = json.loads(args.record.read_text(encoding="utf-8"))
    if value.get("schema") != "qstar-wide-final-observation-v1":
        raise SystemExit(f"{args.record}: unexpected schema")
    if value.get("style") != args.style:
        raise SystemExit(f"{args.record}: unexpected response style")
    objects = value.get("objects")
    if not isinstance(objects, list) or len(objects) != args.count:
        raise SystemExit(
            f"{args.record}: object count {len(objects or [])}, expected {args.count}"
        )
    if len(objects) != len(set(objects)):
        raise SystemExit(f"{args.record}: duplicate object input")
    if args.first is not None and (not objects or objects[0] != args.first):
        raise SystemExit(f"{args.record}: unexpected first object")
    if args.last is not None and (not objects or objects[-1] != args.last):
        raise SystemExit(f"{args.record}: unexpected last object")
    if args.response == "yes" and not value.get("response_file"):
        raise SystemExit(f"{args.record}: expected response file")
    if args.response == "no" and value.get("response_file"):
        raise SystemExit(f"{args.record}: unexpected response file")
    print(
        f"wide_final_record status=ok path={args.record} "
        f"objects={args.count} style={args.style} "
        f"response={'yes' if value.get('response_file') else 'no'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
