"""Resolve a complete two-platform ABI manifest from runtime and fallback reports."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

try:
    from .common import (
        PLATFORMS, PROVENANCE_PRIORITY, ToolError, contract_header_paths, contract_requirements,
        description_layout_invariant, media_player_metadata, read_json, reject_forbidden, validate_requirement_metadata, write_json,
    )
    from .validate_report import validate
except ImportError:
    from common import PLATFORMS, PROVENANCE_PRIORITY, ToolError, contract_header_paths, contract_requirements, description_layout_invariant, media_player_metadata, read_json, reject_forbidden, validate_requirement_metadata, write_json
    from validate_report import validate


def _text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, (list, tuple)):
        return " ".join(_text(item) for item in value)
    if isinstance(value, dict):
        return " ".join(_text(item) for item in value.values())
    return ""


def _is_old_header_fallback(entry: dict[str, Any]) -> bool:
    text = _text({"method": entry.get("method"), "evidence": entry.get("evidence")}).lower()
    return (
        "header" in text or "diagnostic_original_value" in text or "include/abi" in text
        or text.endswith(".h") or "mediaplayer/include" in text
    )


def _load_report(path: Path, contract: Any, *, strict_contract: bool, expected_platform: str | None = None) -> dict[str, Any]:
    report = read_json(path)
    environment = report.get("environment") if isinstance(report, dict) else None
    platform = environment.get("platform") if isinstance(environment, dict) else None
    expected = expected_platform if expected_platform in PLATFORMS else platform if platform in PLATFORMS else None
    errors = validate(report, expected_platform=expected, contract=contract if strict_contract else None)
    if errors:
        raise ToolError(f"{path}: " + "; ".join(errors))
    return report


def _contract_fingerprint(contract: Any) -> str | None:
    value = contract.get("source_fingerprint") if isinstance(contract, dict) else None
    return value if isinstance(value, str) else None


def _resolve_platform(contract: Any, platform: str, primary_report: dict[str, Any],
                      fallback_reports: list[dict[str, Any]] | None = None,
                      *, workflow_commit: str | None = None, workflow_run_id: str | None = None) -> tuple[dict[str, Any], dict[str, Any]]:
    requirements = contract_requirements(contract)
    primary_errors = validate(primary_report, expected_platform=platform, contract=contract, require_complete=True)
    if primary_errors:
        raise ToolError(f"primary {platform} runtime report: " + "; ".join(primary_errors))
    reports = [primary_report] + list(fallback_reports or [])
    candidates: dict[str, dict[str, list[tuple[dict[str, Any], bool]]]] = {
        platform: {item["name"]: [] for item in requirements[platform]}
    }
    for report_index, report in enumerate(reports):
        environment = report["environment"]
        report_platform = environment["platform"]
        if report_platform != platform:
            if report_index == 0:
                raise ToolError(f"primary report platform mismatch: expected {platform}, got {report_platform}")
            continue
        expected_names = set(candidates[platform])
        for entry in report["requirements"]:
            name = entry["name"]
            if name not in expected_names:
                if report_index == 0:
                    raise ToolError(f"runtime report has unexpected requirement {platform}:{name}")
                continue
            if entry["provenance"] == "UNRESOLVED":
                continue
            reject_forbidden(entry, f"{platform}:{name}")
            if report_index >= 1 and entry["provenance"] in {"RUNTIME_OBJECT", "RUNTIME_PROBE", "RUNTIME_DERIVED"}:
                raise ToolError(f"fallback for {platform}:{name} cannot claim runtime provenance")
            if report_index >= 1 and _is_old_header_fallback(entry):
                raise ToolError(f"fallback for {platform}:{name} comes from a header")
            candidates[platform][name].append((entry, report_index >= 1))

    resolved: list[dict[str, Any]] = []
    for ordinal, contract_item in enumerate(requirements[platform]):
        name = contract_item["name"]
        options = candidates[platform][name]
        if not options:
            raise ToolError(f"unresolved {platform}:{name}")
        first_value = options[0][0]["value"]
        if any(entry["value"] != first_value for entry, _ in options[1:]):
            details = ", ".join(repr(entry["value"]) + "/" + entry["provenance"] for entry, _ in options)
            raise ToolError(f"conflicting values for {platform}:{name}: {details}")
        entry, _ = max(options, key=lambda pair: PROVENANCE_PRIORITY.get(pair[0]["provenance"], 0))
        selected: dict[str, Any] = {
            "name": name,
            "platform": platform,
            "ordinal": ordinal,
            "value": entry["value"],
            "provenance": entry["provenance"],
            "method": entry["method"],
            "evidence": entry["evidence"],
        }
        selected.update(validate_requirement_metadata(contract_item, f"{platform}:{name}"))
        for field in ("kind", "value_type", "location", "locations"):
            if field in contract_item:
                selected[field] = contract_item[field]
        if entry.get("dependencies") is not None:
            selected["dependencies"] = list(entry["dependencies"])
        resolved.append(selected)

    by_name = {entry["name"]: entry for entry in resolved}
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(name: str) -> None:
        if name in visited:
            return
        if name in visiting:
            raise ToolError(f"cyclic derived dependency {platform}:{name}")
        visiting.add(name)
        entry = by_name[name]
        if entry["provenance"] == "RUNTIME_DERIVED":
            dependencies = entry.get("dependencies", [])
            if not dependencies:
                raise ToolError(f"derived requirement has no dependencies {platform}:{name}")
            for dependency in dependencies:
                if dependency not in by_name:
                    raise ToolError(f"missing derived dependency {platform}:{name}->{dependency}")
                visit(dependency)
        visiting.remove(name)
        visited.add(name)

    for name in by_name:
        visit(name)

    paths = contract_header_paths(contract)[platform]
    runtime_environment = primary_report["environment"]
    invariants = description_layout_invariant(resolved)
    mediaplayer = media_player_metadata(contract)
    manifest = {
        "schema_version": 1,
        "mediaplayer": mediaplayer,
        "source_fingerprint": mediaplayer["fingerprint"],
        "header_paths": {platform: paths},
        "platforms": {platform: {"environment": runtime_environment, "run_id": primary_report["run_id"], "requirements": resolved}},
        "runtime_environments": {platform: runtime_environment},
        "runtime_run_ids": {platform: primary_report["run_id"]},
        "requirements": list(resolved),
        "invariants": {"description_layout": invariants},
    }
    manifest["workflow"] = {"commit": workflow_commit, "run_id": workflow_run_id}
    coverage: dict[str, Any] = {
        "schema_version": 1,
        "complete": True,
        "platforms": {},
    }
    entries = resolved
    counts: dict[str, int] = {}
    for entry in entries:
        source = entry["provenance"]
        counts[source] = counts.get(source, 0) + 1
    required = len(requirements[platform])
    coverage["platforms"][platform] = {
        "required": required,
        "resolved": len(entries),
        "missing": [],
        "percent": 100.0 if required else 100.0,
        "provenance_counts": dict(sorted(counts.items())),
        "runtime_run_id": primary_report["run_id"],
        "environment": runtime_environment,
    }
    return manifest, coverage


def resolve_platform(contract: Any, platform: str, runtime_report: dict[str, Any],
                     fallback_reports: list[dict[str, Any]] | None = None,
                     *, workflow_commit: str | None = None, workflow_run_id: str | None = None) -> tuple[dict[str, Any], dict[str, Any]]:
    if platform not in PLATFORMS:
        raise ToolError(f"invalid platform: {platform}")
    return _resolve_platform(contract, platform, runtime_report, fallback_reports,
                             workflow_commit=workflow_commit, workflow_run_id=workflow_run_id)


def resolve(contract: Any, windows_report: dict[str, Any], linux_report: dict[str, Any],
            fallback_reports: list[dict[str, Any]] | None = None,
            *, workflow_commit: str | None = None, workflow_run_id: str | None = None) -> tuple[dict[str, Any], dict[str, Any]]:
    windows_manifest, windows_coverage = _resolve_platform(contract, "windows", windows_report, fallback_reports,
                                                           workflow_commit=workflow_commit, workflow_run_id=workflow_run_id)
    linux_manifest, linux_coverage = _resolve_platform(contract, "linux", linux_report, fallback_reports,
                                                        workflow_commit=workflow_commit, workflow_run_id=workflow_run_id)
    manifest = {
        "schema_version": 1,
        "mediaplayer": windows_manifest["mediaplayer"],
        "source_fingerprint": windows_manifest["source_fingerprint"],
        "header_paths": {**windows_manifest["header_paths"], **linux_manifest["header_paths"]},
        "platforms": {**windows_manifest["platforms"], **linux_manifest["platforms"]},
        "runtime_environments": {**windows_manifest["runtime_environments"], **linux_manifest["runtime_environments"]},
        "runtime_run_ids": {**windows_manifest["runtime_run_ids"], **linux_manifest["runtime_run_ids"]},
        "requirements": windows_manifest["requirements"] + linux_manifest["requirements"],
        "invariants": {"description_layout": {
            "windows": windows_manifest.get("invariants", {}).get("description_layout", {"status": "NOT_APPLICABLE"}),
            "linux": linux_manifest.get("invariants", {}).get("description_layout", {"status": "NOT_APPLICABLE"}),
        }},
    }
    manifest["workflow"] = {"commit": workflow_commit, "run_id": workflow_run_id}
    coverage = {"schema_version": 1, "complete": True, "platforms": {
        "windows": windows_coverage["platforms"]["windows"],
        "linux": linux_coverage["platforms"]["linux"],
    }}
    return manifest, coverage


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract", required=True, type=Path)
    parser.add_argument("--platform", choices=PLATFORMS, help="resolve one platform for an intermediate CI artifact")
    parser.add_argument("--runtime-report", type=Path)
    parser.add_argument("--windows-runtime", "--windows-report", "--windows-runtime-report", type=Path, dest="windows_report")
    parser.add_argument("--linux-runtime", "--linux-report", "--linux-runtime-report", type=Path, dest="linux_report")
    parser.add_argument("--fallback-report", "--fallback", action="append", default=[], type=Path)
    parser.add_argument("--output-manifest", "--manifest", required=True, type=Path, dest="manifest")
    parser.add_argument("--output-coverage", "--coverage", required=True, type=Path, dest="coverage")
    parser.add_argument("--workflow-commit")
    parser.add_argument("--workflow-run-id")
    args = parser.parse_args(argv)
    try:
        contract = read_json(args.contract)
        fallback = [_load_report(path, contract, strict_contract=False) for path in args.fallback_report]
        if args.platform:
            if args.runtime_report is None or args.windows_report is not None or args.linux_report is not None:
                raise ToolError("single-platform mode requires --platform and --runtime-report only")
            primary = _load_report(args.runtime_report, contract, strict_contract=True, expected_platform=args.platform)
            manifest, coverage = resolve_platform(contract, args.platform, primary, fallback,
                                                  workflow_commit=args.workflow_commit, workflow_run_id=args.workflow_run_id)
        else:
            if args.runtime_report is not None or args.windows_report is None or args.linux_report is None:
                raise ToolError("final mode requires both --windows-runtime and --linux-runtime")
            windows = _load_report(args.windows_report, contract, strict_contract=True, expected_platform="windows")
            linux = _load_report(args.linux_report, contract, strict_contract=True, expected_platform="linux")
            manifest, coverage = resolve(contract, windows, linux, fallback,
                                         workflow_commit=args.workflow_commit, workflow_run_id=args.workflow_run_id)
        write_json(args.manifest, manifest)
        write_json(args.coverage, coverage)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"resolve_abi: {exc}", file=sys.stderr)
        return 2
    print(f"resolved manifest -> {args.manifest}")
    print(f"coverage -> {args.coverage}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
