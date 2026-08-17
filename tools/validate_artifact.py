"""Validate an extracted ABI artifact and its consumer-relative overlay paths."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

try:
    from .common import PLATFORMS, ToolError, contract_requirements, description_layout_invariant, header_paths, media_player_metadata, read_json, relative_path, reject_forbidden, sha256_file, validate_requirement_metadata
    from .validate_report import validate as validate_report
except ImportError:
    from common import PLATFORMS, ToolError, contract_requirements, description_layout_invariant, header_paths, media_player_metadata, read_json, relative_path, reject_forbidden, sha256_file, validate_requirement_metadata
    from validate_report import validate as validate_report

EVIDENCE_FILES = frozenset({
    "requirements.json", "windows-runtime.json", "linux-runtime.json", "manifest.json", "coverage.json", "README.md",
    "windows-consumer-runtime.json", "linux-consumer-runtime.json",
    "windows-consumer-console.log", "linux-consumer-console.log",
})


def _validate_consumer_runtime(root: Path, evidence: Path, summary: Any, report: Any,
                               requirements: Any, platform: str) -> list[str]:
    errors: list[str] = []
    if not isinstance(summary, dict):
        return [f"consumer runtime summary is malformed for {platform}"]
    if summary.get("schema_version") != 1:
        errors.append(f"consumer runtime schema_version is not 1 for {platform}")
    if summary.get("status") != "PASS":
        errors.append(f"consumer runtime status is not PASS for {platform}")
    if summary.get("platform") != platform:
        errors.append(f"consumer runtime platform differs for {platform}")
    for field, expected in (("fresh_server", True), ("player_required", False),
                            ("logical_screen_lifecycle", "PASS"), ("command_sender_message", "PASS"),
                            ("graceful_shutdown", True), ("forced", False), ("exit_code", 0)):
        if summary.get(field) != expected:
            errors.append(f"consumer runtime {field} is invalid for {platform}")
    commands = summary.get("commands")
    if not isinstance(commands, list) or not {"mpm help", "mpv help"}.issubset(commands):
        errors.append(f"consumer runtime commands are incomplete for {platform}")

    expected_commit = None
    try:
        expected_commit = media_player_metadata(requirements).get("commit")
    except ToolError:
        pass
    if summary.get("media_player_commit") != expected_commit:
        errors.append(f"consumer runtime MediaPlayer commit differs for {platform}")

    environment = report.get("environment") if isinstance(report, dict) else None
    if not isinstance(environment, dict):
        errors.append(f"consumer runtime Endstone environment is missing for {platform}")
        environment = {}
    for summary_key, environment_key, label in (("endstone_commit", "source_commit", "Endstone commit"),
                                                 ("endstone_version", "endstone_runtime_version", "Endstone version"),
                                                 ("bds_version", "bds_version", "BDS version")):
        expected = environment.get(environment_key)
        if summary.get(summary_key) != expected:
            errors.append(f"consumer runtime {label} differs for {platform}")

    log_name = f"{platform}-consumer-console.log"
    expected_log = f"abi-evidence/{log_name}"
    if summary.get("log") != expected_log:
        errors.append(f"consumer runtime log path is not normalized for {platform}")
    log_path = evidence / log_name
    log_hash = summary.get("log_sha256")
    if not log_path.is_file():
        errors.append(f"consumer runtime log is missing for {platform}")
    elif not isinstance(log_hash, str) or sha256_file(log_path) != log_hash:
        errors.append(f"consumer runtime log hash mismatch for {platform}")
    return errors


def _manifest_entries(manifest: dict[str, Any], platform: str) -> list[dict[str, Any]]:
    section = manifest.get("platforms", {}).get(platform, {}) if isinstance(manifest.get("platforms"), dict) else {}
    entries = section.get("requirements") if isinstance(section, dict) else None
    if entries is None:
        flat = manifest.get("requirements", manifest.get("entries", []))
        entries = [item for item in flat if isinstance(item, dict) and item.get("platform") == platform]
    if not isinstance(entries, list):
        raise ToolError(f"manifest {platform} requirements must be an array")
    paths = header_paths(manifest.get("header_paths"), (platform,))[platform]
    seen: set[str] = set()
    ordered: list[dict[str, Any]] = []
    for index, entry in enumerate(sorted(entries, key=lambda item: item.get("ordinal", 0) if isinstance(item, dict) else 0)):
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ToolError(f"invalid manifest entry {platform}[{index}]")
        if entry["name"] in seen:
            raise ToolError(f"duplicate manifest entry {platform}:{entry['name']}")
        seen.add(entry["name"])
        if entry.get("ordinal") != index:
            raise ToolError(f"invalid ordinal {platform}:{entry['name']}")
        if entry.get("provenance") in (None, "UNRESOLVED") or entry.get("value") is None:
            raise ToolError(f"unresolved manifest entry {platform}:{entry['name']}")
        reject_forbidden(entry, f"manifest:{platform}:{entry['name']}")
        validate_requirement_metadata(entry, f"manifest:{platform}:{entry['name']}")
        location = entry.get("location")
        if isinstance(location, dict):
            location = location.get("path")
        if not isinstance(location, str):
            locations = entry.get("locations")
            if isinstance(locations, list) and locations and isinstance(locations[0], dict):
                location = locations[0].get("path")
        if isinstance(location, str) and relative_path(location) not in paths:
            raise ToolError(f"manifest location outside expected headers: {platform}:{entry['name']}")
        ordered.append(entry)
    return ordered


def _hash_map(manifest: dict[str, Any], coverage: dict[str, Any]) -> dict[str, str]:
    candidates: list[Any] = [manifest.get("header_hashes"), manifest.get("headers"), manifest.get("header_metadata")]
    candidates.append(coverage.get("header_hashes") if isinstance(coverage, dict) else None)
    result: dict[str, str] = {}
    for candidate in candidates:
        if isinstance(candidate, dict):
            for key, value in candidate.items():
                if isinstance(value, dict):
                    value = value.get("sha256")
                if isinstance(value, str):
                    result[relative_path(key)] = value
        elif isinstance(candidate, list):
            for item in candidate:
                if isinstance(item, dict) and isinstance(item.get("path"), str) and isinstance(item.get("sha256"), str):
                    result[relative_path(item["path"])] = item["sha256"]
    return result


def validate(root: Path) -> list[str]:
    errors: list[str] = []
    evidence = root / "abi-evidence"
    if not evidence.is_dir():
        return ["missing abi-evidence directory"]
    actual_evidence = {path.name for path in evidence.iterdir() if path.is_file()}
    missing_evidence = EVIDENCE_FILES - actual_evidence
    extra_evidence = actual_evidence - EVIDENCE_FILES
    if missing_evidence:
        errors.append("missing evidence: " + ", ".join(sorted(missing_evidence)))
    if extra_evidence:
        errors.append("unexpected evidence: " + ", ".join(sorted(extra_evidence)))
    try:
        requirements = read_json(evidence / "requirements.json")
        manifest = read_json(evidence / "manifest.json")
        coverage = read_json(evidence / "coverage.json")
        windows = read_json(evidence / "windows-runtime.json")
        linux = read_json(evidence / "linux-runtime.json")
        windows_consumer_runtime = read_json(evidence / "windows-consumer-runtime.json")
        linux_consumer_runtime = read_json(evidence / "linux-consumer-runtime.json")
    except (OSError, ValueError) as exc:
        return errors + [f"malformed evidence JSON: {exc}"]
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        errors.append("manifest schema_version must be 1")
        manifest = {}
    if not isinstance(coverage, dict) or coverage.get("schema_version") != 1:
        errors.append("coverage schema_version must be 1")
        coverage = {}
    if coverage.get("complete") is not True:
        errors.append("coverage complete must be true")
    workflow = manifest.get("workflow") if isinstance(manifest, dict) else None
    if not isinstance(workflow, dict) or "commit" not in workflow or "run_id" not in workflow:
        errors.append("manifest workflow commit/run_id metadata is missing")
    if not isinstance(manifest.get("runtime_environments"), dict) or not isinstance(manifest.get("runtime_run_ids"), dict):
        errors.append("manifest runtime environment metadata is missing")
    consumer_validation = manifest.get("consumer_validation") if isinstance(manifest, dict) else None
    if not isinstance(consumer_validation, dict):
        errors.append("manifest consumer validation metadata is missing")
    else:
        expected_fingerprint = requirements.get("source_fingerprint") if isinstance(requirements, dict) else None
        expected_commit = requirements.get("source_commit") if isinstance(requirements, dict) else None
        for platform in PLATFORMS:
            summary = consumer_validation.get(platform)
            if not isinstance(summary, dict):
                errors.append(f"consumer validation missing for {platform}")
                continue
            if summary.get("status") != "PASS":
                errors.append(f"consumer validation status is not PASS for {platform}")
            if summary.get("overlay_removed_existing_headers") is not True:
                errors.append(f"consumer overlay cleanup was not verified for {platform}")
            for phase in ("configure", "build", "ctest"):
                phase_value = summary.get(phase)
                exit_code = phase_value.get("exit_code") if isinstance(phase_value, dict) else summary.get(f"{phase}_exit_code")
                if exit_code != 0:
                    errors.append(f"consumer {phase} exit code is not zero for {platform}")
            if summary.get("source_commit") != expected_commit:
                errors.append(f"consumer source commit differs for {platform}")
            if summary.get("source_fingerprint") != expected_fingerprint:
                errors.append(f"consumer source fingerprint differs for {platform}")
    consumer_runtime = manifest.get("consumer_runtime") if isinstance(manifest, dict) else None
    if not isinstance(consumer_runtime, dict):
        errors.append("manifest consumer runtime metadata is missing")
    else:
        for platform, summary, report in (("windows", windows_consumer_runtime, windows), ("linux", linux_consumer_runtime, linux)):
            embedded = consumer_runtime.get(platform)
            if embedded != summary:
                errors.append(f"embedded consumer runtime differs from evidence for {platform}")
            errors.extend(_validate_consumer_runtime(root, evidence, summary, report, requirements, platform))
    contract: dict[str, list[dict[str, Any]]] | None = None
    try:
        contract = contract_requirements(requirements)
    except ToolError as exc:
        errors.append(f"invalid requirements contract: {exc}")
    expected_manifest_paths: dict[str, list[str]] | None = None
    try:
        expected_manifest_paths = header_paths(manifest.get("header_paths"))
    except ToolError as exc:
        errors.append(f"invalid manifest header paths: {exc}")
    if contract is not None:
        try:
            requirements_media = media_player_metadata(requirements)
            manifest_media = media_player_metadata(manifest)
            if requirements_media != manifest_media:
                errors.append("manifest MediaPlayer identity differs from requirements")
            if requirements.get("source_fingerprint") != requirements_media["fingerprint"]:
                errors.append("requirements source fingerprint differs from MediaPlayer identity")
            if manifest.get("source_fingerprint") != manifest_media["fingerprint"]:
                errors.append("manifest source fingerprint differs from MediaPlayer identity")
            if expected_manifest_paths != header_paths(requirements.get("header_paths")):
                errors.append("manifest header paths differ from requirements")
        except ToolError as exc:
            errors.append(f"invalid MediaPlayer identity: {exc}")
    manifest_invariants = manifest.get("invariants") if isinstance(manifest, dict) else None
    if not isinstance(manifest_invariants, dict):
        errors.append("manifest invariant metadata is missing")
    for platform, report in (("windows", windows), ("linux", linux)):
        try:
            errors.extend(validate_report(report, expected_platform=platform, contract=requirements, require_complete=True))
        except (TypeError, ValueError) as exc:
            errors.append(f"invalid {platform} runtime report: {exc}")
        if isinstance(report, dict) and isinstance(manifest.get("platforms"), dict):
            manifest_section = manifest["platforms"].get(platform)
            if not isinstance(manifest_section, dict) or manifest_section.get("environment") != report.get("environment") or manifest_section.get("run_id") != report.get("run_id"):
                errors.append(f"manifest runtime environment differs for {platform}")
        if isinstance(manifest.get("runtime_environments"), dict) and manifest["runtime_environments"].get(platform) != report.get("environment"):
            errors.append(f"runtime_environments differs for {platform}")
        if isinstance(manifest.get("runtime_run_ids"), dict) and manifest["runtime_run_ids"].get(platform) != report.get("run_id"):
            errors.append(f"runtime_run_ids differs for {platform}")
    for platform in PLATFORMS:
        try:
            entries = _manifest_entries(manifest, platform)
        except ToolError as exc:
            errors.append(str(exc))
            entries = []
        section = coverage.get("platforms", {}).get(platform, {}) if isinstance(coverage.get("platforms"), dict) else {}
        required = len(contract.get(platform, [])) if contract is not None else 0
        if not isinstance(section, dict) or section.get("required") != required or section.get("resolved") != required or section.get("missing") != [] or section.get("percent") != 100.0:
            errors.append(f"coverage is not complete for {platform}")
        if len(entries) != required:
            errors.append(f"manifest coverage is not complete for {platform}")
        if contract is not None:
            expected_items = {item["name"]: item for item in contract.get(platform, [])}
            for entry in entries:
                expected = expected_items.get(entry["name"])
                if expected is None:
                    continue
                for field in ("category", "contract_identity", "runtime_required"):
                    if entry.get(field) != expected.get(field):
                        errors.append(f"manifest {platform}:{entry['name']} {field} differs from contract")
                if entry.get("runtime_required") is True and entry.get("provenance") not in {
                    "RUNTIME_OBJECT", "RUNTIME_PROBE", "RUNTIME_DERIVED"
                }:
                    errors.append(f"manifest {platform}:{entry['name']} is missing required runtime proof")
        if isinstance(section, dict):
            manifest_counts: dict[str, int] = {}
            for entry in entries:
                manifest_counts[entry["provenance"]] = manifest_counts.get(entry["provenance"], 0) + 1
            if section.get("provenance_counts") != dict(sorted(manifest_counts.items())):
                errors.append(f"coverage provenance counts differ for {platform}")
            if section.get("environment") != (manifest.get("platforms", {}).get(platform, {}).get("environment") if isinstance(manifest.get("platforms"), dict) else None):
                errors.append(f"coverage environment differs for {platform}")
            if section.get("runtime_run_id") != (manifest.get("runtime_run_ids", {}).get(platform) if isinstance(manifest.get("runtime_run_ids"), dict) else None):
                errors.append(f"coverage run id differs for {platform}")
        try:
            expected_invariant = description_layout_invariant(entries)
            if isinstance(manifest_invariants, dict):
                platform_invariants = manifest_invariants.get("description_layout")
                if isinstance(platform_invariants, dict) and platform in platform_invariants and "status" not in platform_invariants:
                    platform_invariants = platform_invariants[platform]
                if platform_invariants not in (expected_invariant, {"status": "NOT_APPLICABLE"}):
                    errors.append(f"manifest description layout invariant differs for {platform}")
        except ToolError as exc:
            errors.append(f"invalid description layout invariant for {platform}: {exc}")
    expected_paths = {path for paths in (expected_manifest_paths or {}).values() for path in paths}
    actual_paths = {path.relative_to(root).as_posix() for path in (root / "include" / "abi").rglob("*") if path.is_file()} if (root / "include" / "abi").is_dir() else set()
    if actual_paths != expected_paths:
        errors.append("generated header set differs: expected " + ", ".join(sorted(expected_paths)) + "; got " + ", ".join(sorted(actual_paths)))
    for path in expected_paths:
        target = root / Path(path)
        if not target.is_file():
            errors.append(f"missing generated header {path}")
    hashes = _hash_map(manifest, coverage)
    if set(hashes) != expected_paths:
        errors.append("generated header hashes are missing or unexpected")
    if not hashes:
        errors.append("manifest has no generated header hashes")
    for path, expected in hashes.items():
        if path not in expected_paths:
            errors.append(f"hash refers to unexpected header {path}")
        elif (root / Path(path)).is_file() and sha256_file(root / Path(path)) != expected:
            errors.append(f"header hash mismatch {path}")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", "--artifact-root", "--extracted-root", required=True, type=Path, dest="root")
    args = parser.parse_args(argv)
    try:
        errors = validate(args.root.resolve())
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"validate_artifact: {exc}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(f"invalid: {error}", file=sys.stderr)
        return 1
    print("valid ABI artifact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
