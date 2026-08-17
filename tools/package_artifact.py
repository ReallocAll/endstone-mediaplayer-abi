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


RUNTIME_EVIDENCE = {
    "windows": ("windows-consumer-runtime.json", "windows-consumer-console.log"),
    "linux": ("linux-consumer-runtime.json", "linux-consumer-console.log"),
}


def _runtime_input(runtime: Path | None, platform: str) -> Path:
    if runtime is None:
        raise ToolError(f"missing consumer runtime summary for {platform}")
    return runtime


def _prepare_runtime(summary_path: Path, log_path: Path, platform: str, evidence: Path) -> dict[str, object]:
    summary = read_json(summary_path)
    if not isinstance(summary, dict):
        raise ToolError(f"consumer runtime summary must be an object for {platform}")
    if not log_path.is_file():
        raise ToolError(f"missing consumer runtime log for {platform}: {log_path}")
    log_name = RUNTIME_EVIDENCE[platform][1]
    log_hash = sha256_file(log_path)
    supplied_hash = summary.get("log_sha256")
    if supplied_hash is not None and supplied_hash != log_hash:
        raise ToolError(f"consumer runtime log hash mismatch for {platform}")
    normalized = dict(summary)
    normalized["log"] = f"abi-evidence/{log_name}"
    normalized["log_sha256"] = log_hash
    shutil.copyfile(log_path, evidence / log_name)
    write_json(evidence / RUNTIME_EVIDENCE[platform][0], normalized)
    return normalized


def package(root: Path, requirements: Path, windows: Path, linux: Path, manifest: Path, coverage: Path, readme: Path | None = None,
            workflow_commit: str | None = None, workflow_run_id: str | None = None,
            windows_consumer_summary: Path | None = None, linux_consumer_summary: Path | None = None,
            windows_consumer_runtime: Path | None = None, linux_consumer_runtime: Path | None = None,
            windows_consumer_log: Path | None = None, linux_consumer_log: Path | None = None) -> None:
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
    windows_runtime = _runtime_input(windows_consumer_runtime, "windows")
    linux_runtime = _runtime_input(linux_consumer_runtime, "linux")
    if windows_consumer_log is None:
        raise ToolError("missing consumer runtime log for windows")
    if linux_consumer_log is None:
        raise ToolError("missing consumer runtime log for linux")
    summaries = {
        "windows": _prepare_runtime(windows_runtime, windows_consumer_log, "windows", evidence),
        "linux": _prepare_runtime(linux_runtime, linux_consumer_log, "linux", evidence),
    }
    manifest_value["consumer_runtime"] = summaries
    if windows_consumer_summary or linux_consumer_summary:
        if not windows_consumer_summary or not linux_consumer_summary:
            raise ToolError("consumer validation summaries are required for both platforms")
        manifest_value["consumer_validation"] = {
            "windows": read_json(windows_consumer_summary),
            "linux": read_json(linux_consumer_summary),
        }
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
    parser.add_argument("--windows-consumer-runtime", "--windows-consumer-runtime-summary",
                        dest="windows_consumer_runtime", required=True, type=Path)
    parser.add_argument("--linux-consumer-runtime", "--linux-consumer-runtime-summary",
                        dest="linux_consumer_runtime", required=True, type=Path)
    parser.add_argument("--windows-consumer-log", required=True, type=Path)
    parser.add_argument("--linux-consumer-log", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        package(args.root, args.requirements, args.windows_runtime, args.linux_runtime, args.manifest, args.coverage, args.readme,
                args.workflow_commit, args.workflow_run_id, args.windows_consumer_summary, args.linux_consumer_summary,
                windows_consumer_runtime=args.windows_consumer_runtime,
                linux_consumer_runtime=args.linux_consumer_runtime, windows_consumer_log=args.windows_consumer_log,
                linux_consumer_log=args.linux_consumer_log)
    except (OSError, ValueError, TypeError, KeyError, ToolError) as exc:
        print(f"package_artifact: {exc}", file=sys.stderr)
        return 2
    print(f"artifact -> {args.root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
