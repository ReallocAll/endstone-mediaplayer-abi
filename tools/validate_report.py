"""Validate a runtime ABI report before it can participate in resolution."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

try:
    from .common import PROVENANCES, ToolError, contract_requirements, ensure_platform, ensure_value, read_json, reject_forbidden
except ImportError:
    from common import PROVENANCES, ToolError, contract_requirements, ensure_platform, ensure_value, read_json, reject_forbidden

NAME = re.compile(r"^ES_[A-Za-z0-9_]+$")
ENVIRONMENT_FIELDS = (
    "platform", "arch", "pointer_size", "endstone_runtime_version", "api_version",
    "source_ref", "source_commit", "compiler", "stdlib", "bds_version",
    "probe_loaded", "clean_start",
)


def validate(report: Any, *, expected_platform: str | None = None, expected_run_id: str | None = None,
             expected_source_ref: str | None = None, expected_source_commit: str | None = None,
             expected_api_version: str | None = None, contract: Any | None = None,
             require_complete: bool = False) -> list[str]:
    errors: list[str] = []
    if not isinstance(report, dict):
        return ["report must be an object"]
    if report.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    run_id = report.get("run_id")
    if not isinstance(run_id, str) or not run_id.strip():
        errors.append("run_id must be nonempty")
    elif expected_run_id is not None and run_id != expected_run_id:
        errors.append("run_id does not match expected value")
    if not isinstance(report.get("complete"), bool):
        errors.append("complete must be boolean")
    elif require_complete and report.get("complete") is not True:
        errors.append("complete must be true")
    environment = report.get("environment")
    if not isinstance(environment, dict):
        errors.append("environment must be an object")
        environment = {}
    for field in ENVIRONMENT_FIELDS:
        if field not in environment:
            errors.append(f"environment missing {field}")
    platform = environment.get("platform")
    if platform not in {"windows", "linux"}:
        errors.append("environment.platform must be windows or linux")
    if expected_platform is not None and platform != expected_platform:
        errors.append("platform does not match expected value")
    if environment.get("arch") != "x86_64":
        errors.append("environment.arch must be x86_64")
    if environment.get("pointer_size") != 8:
        errors.append("environment.pointer_size must be 8")
    if environment.get("probe_loaded") is not True:
        errors.append("environment.probe_loaded must be true")
    if environment.get("clean_start") is not True:
        errors.append("environment.clean_start must be true")
    if expected_source_ref is not None and environment.get("source_ref") != expected_source_ref:
        errors.append("source_ref does not match expected value")
    if expected_source_commit is not None and environment.get("source_commit") != expected_source_commit:
        errors.append("source_commit does not match expected value")
    if expected_api_version is not None and environment.get("api_version") != expected_api_version:
        errors.append("api_version does not match expected value")

    entries = report.get("requirements")
    seen: set[str] = set()
    if not isinstance(entries, list):
        errors.append("requirements must be an array")
        entries = []
    for index, entry in enumerate(entries):
        prefix = f"requirements[{index}]"
        if not isinstance(entry, dict):
            errors.append(f"{prefix} must be an object")
            continue
        name = entry.get("name")
        if not isinstance(name, str) or not NAME.fullmatch(name):
            errors.append(f"{prefix}.name must be an ES_* name")
        elif name in seen:
            errors.append(f"duplicate requirement {name}")
        else:
            seen.add(name)
        provenance = entry.get("provenance")
        if provenance not in PROVENANCES:
            errors.append(f"{prefix}.provenance is invalid")
        value = entry.get("value")
        try:
            ensure_value(value, allow_null=provenance == "UNRESOLVED")
        except ToolError as exc:
            errors.append(f"{prefix}.value: {exc}")
        if provenance == "UNRESOLVED" and value is not None:
            errors.append(f"{prefix}.UNRESOLVED value must be null")
        if provenance != "UNRESOLVED" and value is None:
            errors.append(f"{prefix} resolved provenance requires a value")
        method = entry.get("method")
        if not isinstance(method, str) or not method.strip():
            errors.append(f"{prefix}.method must be nonempty")
        if "evidence" not in entry or entry.get("evidence") in (None, "", []):
            errors.append(f"{prefix}.evidence must be present")
        try:
            reject_forbidden({"provenance": provenance, "method": method, "evidence": entry.get("evidence")}, prefix)
        except ToolError as exc:
            errors.append(str(exc))
        dependencies = entry.get("dependencies")
        if provenance == "RUNTIME_DERIVED":
            if not isinstance(dependencies, list) or not dependencies:
                errors.append(f"{prefix}.dependencies required for RUNTIME_DERIVED")
            elif any(not isinstance(item, str) or not NAME.fullmatch(item) for item in dependencies):
                errors.append(f"{prefix}.dependencies must contain ES_* names")
        elif dependencies is not None and not isinstance(dependencies, list):
            errors.append(f"{prefix}.dependencies must be an array")
    if report.get("complete") is True and any(item.get("provenance") == "UNRESOLVED" for item in entries if isinstance(item, dict)):
        errors.append("complete report cannot contain unresolved requirements")
    if contract is not None and platform in {"windows", "linux"}:
        try:
            expected_names = {item["name"] for item in contract_requirements(contract)[platform]}
            missing = expected_names - seen
            extra = seen - expected_names
            if missing:
                errors.append("missing contract requirements: " + ", ".join(sorted(missing)))
            if extra:
                errors.append("unexpected requirements: " + ", ".join(sorted(extra)))
        except ToolError as exc:
            errors.append(f"contract: {exc}")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--contract", type=Path)
    parser.add_argument("--expected-platform", "--platform", dest="expected_platform")
    parser.add_argument("--expected-run-id", "--run-id", dest="expected_run_id")
    parser.add_argument("--expected-source-ref", "--source-ref", dest="expected_source_ref")
    parser.add_argument("--expected-source-commit", "--source-commit", dest="expected_source_commit")
    parser.add_argument("--expected-api-version", "--api-version", dest="expected_api_version")
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args(argv)
    try:
        report = read_json(args.report)
        contract = read_json(args.contract) if args.contract else None
        errors = validate(
            report,
            expected_platform=args.expected_platform,
            expected_run_id=args.expected_run_id,
            expected_source_ref=args.expected_source_ref,
            expected_source_commit=args.expected_source_commit,
            expected_api_version=args.expected_api_version,
            contract=contract,
            require_complete=args.require_complete,
        )
    except (OSError, ValueError, TypeError) as exc:
        print(f"validate_report: {exc}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(f"invalid: {error}", file=sys.stderr)
        return 1
    print("valid runtime report")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
