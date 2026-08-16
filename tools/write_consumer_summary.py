"""Write a deterministic PASS summary after an overlay consumer gate succeeds."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("windows", "linux"))
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-fingerprint", required=True)
    parser.add_argument("--configure-command", required=True)
    parser.add_argument("--build-command", required=True)
    parser.add_argument("--ctest-command", required=True)
    args = parser.parse_args()
    value = {
        "schema_version": 1,
        "platform": args.platform,
        "status": "PASS",
        "overlay_removed_existing_headers": True,
        "source_commit": args.source_commit,
        "source_fingerprint": args.source_fingerprint,
        "configure": {"command": args.configure_command, "exit_code": 0},
        "build": {"command": args.build_command, "exit_code": 0},
        "ctest": {"command": args.ctest_command, "exit_code": 0},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    print(f"consumer summary -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
