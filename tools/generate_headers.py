"""Generate deterministic MediaPlayer-compatible ABI headers from a manifest."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

try:
    from .common import ToolError, ensure_value, header_paths, read_json, relative_path, reject_forbidden, sha256_bytes, write_json
except ImportError:
    from common import ToolError, ensure_value, header_paths, read_json, relative_path, reject_forbidden, sha256_bytes, write_json


def _entries(manifest: dict[str, Any], platform: str) -> list[dict[str, Any]]:
    section = manifest.get("platforms", {}).get(platform, {}) if isinstance(manifest.get("platforms"), dict) else {}
    entries = section.get("requirements") if isinstance(section, dict) else None
    if entries is None:
        flat = manifest.get("requirements", manifest.get("entries", []))
        entries = [item for item in flat if isinstance(item, dict) and item.get("platform") == platform]
    if not isinstance(entries, list):
        raise ToolError(f"manifest {platform} requirements must be an array")
    seen: set[str] = set()
    ordered = sorted(entries, key=lambda item: item.get("ordinal", 0) if isinstance(item, dict) else 0)
    for index, entry in enumerate(ordered):
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ToolError(f"invalid manifest entry {platform}[{index}]")
        if entry["name"] in seen:
            raise ToolError(f"duplicate manifest entry {platform}:{entry['name']}")
        seen.add(entry["name"])
        if entry.get("ordinal", index) != index:
            raise ToolError(f"non-contiguous ordinal {platform}:{entry['name']}")
        if entry.get("provenance") in (None, "UNRESOLVED"):
            raise ToolError(f"unresolved manifest entry {platform}:{entry['name']}")
        try:
            ensure_value(entry.get("value"))
            reject_forbidden(entry, f"{platform}:{entry['name']}")
        except ToolError:
            raise
    return ordered


def _format_value(value: Any) -> str:
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ToolError("header value must be integer or string")
    if isinstance(value, int):
        return str(value)
    return json.dumps(value, ensure_ascii=False)


def _guard(platform: str) -> str:
    parts = [part for part in platform.replace("\\", "/").split("/") if part]
    return "ES_ABI_" + "_".join(parts).replace(".", "_").upper()


def _entry_path(entry: dict[str, Any], fallback: str | None = None) -> str:
    location = entry.get("location")
    if isinstance(location, dict):
        location = location.get("path")
    if not isinstance(location, str):
        locations = entry.get("locations")
        if isinstance(locations, list) and locations and isinstance(locations[0], dict):
            location = locations[0].get("path")
    return relative_path(location if isinstance(location, str) else fallback or "")


def generate(manifest: dict[str, Any], output_root: Path) -> dict[str, Any]:
    if manifest.get("schema_version") != 1:
        raise ToolError("manifest schema_version must be 1")
    raw_paths = manifest.get("header_paths")
    if not isinstance(raw_paths, dict):
        raise ToolError("manifest header_paths must be an object")
    paths_by_platform = header_paths(raw_paths, tuple(raw_paths))
    metadata: dict[str, Any] = {"schema_version": 1, "headers": []}
    for platform, paths in paths_by_platform.items():
        entries = _entries(manifest, platform)
        for entry in entries:
            location = _entry_path(entry, paths[0] if len(paths) == 1 else None)
            if location not in paths:
                raise ToolError(f"requirement {entry['name']} points outside manifest headers: {location}")
        for rel in paths:
            rel = relative_path(rel)
            selected = [entry for entry in entries if _entry_path(entry, rel) == rel]
            # A one-header platform may omit location in a compact hand-written manifest.
            if len(paths) == 1 and not selected:
                selected = entries
            guard = _guard(rel.rsplit("/", 1)[-1])
            lines = [
                "// AUTO-GENERATED ABI header. Do not edit.",
                f"// Platform: {platform}, path: {rel}.",
                "",
                f"#ifndef {guard}",
                f"#define {guard}",
                "",
            ]
            for entry in selected:
                lines.append(f"#define {entry['name']} {_format_value(entry['value'])} // {entry['provenance']}")
            lines.extend(["", f"#endif // {guard}", ""])
            data = "\n".join(lines).encode("utf-8")
            target = output_root / Path(rel)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            metadata["headers"].append({"platform": platform, "path": rel, "sha256": sha256_bytes(data), "bytes": len(data)})
    metadata["headers"] = sorted(metadata["headers"], key=lambda item: (item["platform"], item["path"]))
    return metadata


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", "--resolved-manifest", required=True, type=Path)
    parser.add_argument("--output-root", "--output-dir", required=True, type=Path, dest="output_root")
    parser.add_argument("--metadata", type=Path, help="metadata output (defaults to output-root/header-metadata.json)")
    args = parser.parse_args(argv)
    try:
        metadata = generate(read_json(args.manifest), args.output_root)
        write_json(args.metadata or (args.output_root / "header-metadata.json"), metadata)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"generate_headers: {exc}", file=sys.stderr)
        return 2
    for header in metadata["headers"]:
        print(f"{header['path']} sha256={header['sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
