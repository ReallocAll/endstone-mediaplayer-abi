"""Assemble the fixed ABI overlay and evidence layout."""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

try:
    from .common import ToolError, header_paths, read_json, sha256_file, write_json
except ImportError:
    from common import ToolError, header_paths, read_json, sha256_file, write_json


def package(root: Path, requirements: Path, windows: Path, linux: Path, manifest: Path, coverage: Path, readme: Path | None = None,
            workflow_commit: str | None = None, workflow_run_id: str | None = None,
            windows_consumer_summary: Path | None = None, linux_consumer_summary: Path | None = None) -> None:
    evidence = root / "abi-evidence"
    evidence.mkdir(parents=True, exist_ok=True)
    for source, name in ((requirements, "requirements.json"), (windows, "windows-runtime.json"), (linux, "linux-runtime.json"), (coverage, "coverage.json")):
        shutil.copyfile(source, evidence / name)
    manifest_value = read_json(manifest)
    if not isinstance(manifest_value, dict):
        raise ToolError("manifest must be an object")
    paths_by_platform = header_paths(manifest_value.get("header_paths"))
    hashes = {}
    for paths in paths_by_platform.values():
        for relative in paths:
            target = root / Path(relative)
            if not target.is_file():
                raise ToolError(f"missing generated header {relative}")
            hashes[relative] = sha256_file(target)
    manifest_value["header_hashes"] = hashes
    summaries = {}
    if windows_consumer_summary:
        summaries["windows"] = read_json(windows_consumer_summary)
    if linux_consumer_summary:
        summaries["linux"] = read_json(linux_consumer_summary)
    if summaries:
        manifest_value["consumer_validation"] = summaries
    workflow_commit = workflow_commit or os.environ.get("ABI_WORKFLOW_COMMIT") or os.environ.get("GITHUB_SHA")
    workflow_run_id = workflow_run_id or os.environ.get("ABI_WORKFLOW_RUN_ID") or os.environ.get("GITHUB_RUN_ID")
    if workflow_commit or workflow_run_id:
        manifest_value["workflow"] = {"commit": workflow_commit, "run_id": workflow_run_id}
    write_json(evidence / "manifest.json", manifest_value)
    if readme is None:
        (evidence / "README.md").write_text("# ABI artifact\n\nGenerated runtime ABI overlay.\n", encoding="utf-8", newline="\n")
    else:
        shutil.copyfile(readme, evidence / "README.md")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--requirements", required=True, type=Path)
    parser.add_argument("--windows-runtime", required=True, type=Path)
    parser.add_argument("--linux-runtime", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--coverage", required=True, type=Path)
    parser.add_argument("--readme", type=Path)
    parser.add_argument("--workflow-commit")
    parser.add_argument("--workflow-run-id")
    parser.add_argument("--windows-consumer-summary", type=Path)
    parser.add_argument("--linux-consumer-summary", type=Path)
    args = parser.parse_args(argv)
    try:
        package(args.root, args.requirements, args.windows_runtime, args.linux_runtime, args.manifest, args.coverage, args.readme,
                args.workflow_commit, args.workflow_run_id, args.windows_consumer_summary, args.linux_consumer_summary)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"package_artifact: {exc}", file=sys.stderr)
        return 2
    print(f"artifact -> {args.root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
