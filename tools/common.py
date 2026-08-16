"""Shared, dependency-free helpers for the ABI tool chain."""

from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path
from typing import Any, Iterable

PLATFORMS = ("windows", "linux")
ARCH = "x86_64"
FORBIDDEN_MARKERS = ("ASSUMED", "GUESSED", "LIKELY", "UNKNOWN", "PLACEHOLDER")
PROVENANCE_PRIORITY = {
    "RUNTIME_OBJECT": 5,
    "RUNTIME_PROBE": 4,
    "RUNTIME_DERIVED": 3,
    "COMPILE_MEASURED": 2,
    "STATIC_VERIFIED": 1,
}
PROVENANCES = frozenset(PROVENANCE_PRIORITY) | {"UNRESOLVED"}


class ToolError(ValueError):
    """An input or contract violation that should result in a clean CLI error."""


def read_json(path: str | os.PathLike[str]) -> Any:
    with Path(path).open("r", encoding="utf-8") as stream:
        return json.load(stream)


def write_json(path: str | os.PathLike[str], value: Any) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    target.write_text(text, encoding="utf-8", newline="\n")


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: str | os.PathLike[str]) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_fingerprint(root: Path, paths: Iterable[Path]) -> str:
    """Hash source names/text independent of host paths and checkout EOLs."""
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        # Git may materialize the same text blob with LF, CRLF, or mixed
        # newlines depending on core.autocrlf and attributes.  The scanner
        # parses these files as text, so its semantic fingerprint must use a
        # canonical newline representation as well.
        data = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        digest.update(len(relative).to_bytes(8, "big"))
        digest.update(relative)
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()


def ensure_platform(platform: Any) -> str:
    if platform not in PLATFORMS:
        raise ToolError(f"invalid platform: {platform!r}")
    return str(platform)


def ensure_value(value: Any, *, allow_null: bool = False) -> int | str | None:
    if value is None and allow_null:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ToolError("value must be an integer or string")
    return value


def reject_forbidden(value: Any, where: str = "value") -> None:
    """Reject forbidden provenance vocabulary recursively, case-insensitively."""
    if isinstance(value, dict):
        for key, item in value.items():
            reject_forbidden(key, f"{where}.key")
            reject_forbidden(item, f"{where}.{key}")
    elif isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            reject_forbidden(item, f"{where}[{index}]")
    elif isinstance(value, str):
        upper = value.upper()
        for marker in FORBIDDEN_MARKERS:
            if marker in upper:
                raise ToolError(f"forbidden marker {marker} in {where}")


def relative_path(value: str | os.PathLike[str]) -> str:
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise ToolError(f"path must be consumer-relative: {value!r}")
    return path.as_posix()


def header_paths(value: Any, required_platforms: Iterable[str] = PLATFORMS) -> dict[str, list[str]]:
    """Normalize scanner/manifest header paths without inventing defaults."""
    if not isinstance(value, dict):
        raise ToolError("header_paths must be an object")
    output: dict[str, list[str]] = {}
    platforms = tuple(required_platforms)
    if not platforms:
        raise ToolError("header_paths must contain at least one platform")
    for platform in platforms:
        ensure_platform(platform)
        raw = value.get(platform)
        if isinstance(raw, str):
            raw = [raw]
        if not isinstance(raw, list) or not raw:
            raise ToolError(f"header_paths.{platform} must be a nonempty array")
        paths: list[str] = []
        for item in raw:
            if not isinstance(item, str):
                raise ToolError(f"header path for {platform} must be a string")
            path = relative_path(item)
            if path in paths:
                raise ToolError(f"duplicate header path {path}")
            paths.append(path)
        output[platform] = paths
    return output


def contract_header_paths(contract: Any) -> dict[str, list[str]]:
    if not isinstance(contract, dict):
        raise ToolError("contract must be an object")
    return header_paths(contract.get("header_paths"), PLATFORMS)


def media_player_metadata(contract: Any) -> dict[str, Any]:
    """Return explicitly MediaPlayer-scoped identity, with flat-key compatibility."""
    if not isinstance(contract, dict):
        raise ToolError("contract must be an object")
    nested = contract.get("mediaplayer")
    if isinstance(nested, dict):
        metadata = dict(nested)
    else:
        metadata = {}
    for source_key, target_key in (("source_repository", "repository"), ("source_ref", "ref"), ("source_commit", "commit"), ("source_fingerprint", "fingerprint")):
        if target_key not in metadata and source_key in contract:
            metadata[target_key] = contract[source_key]
        if target_key not in metadata and source_key in metadata:
            metadata[target_key] = metadata[source_key]
    if not isinstance(metadata.get("fingerprint"), str) or not metadata["fingerprint"]:
        raise ToolError("MediaPlayer source fingerprint is required")
    metadata.setdefault("repository", None)
    metadata.setdefault("ref", None)
    metadata.setdefault("commit", None)
    return metadata


def contract_requirements(contract: Any) -> dict[str, list[dict[str, Any]]]:
    """Normalize scanner contracts and compact hand-authored contracts.

    Accepted forms are ``platforms.<platform>.requirements`` and a flat
    ``requirements``/``entries`` list carrying a platform field.
    """
    if not isinstance(contract, dict):
        raise ToolError("contract must be an object")
    output: dict[str, list[dict[str, Any]]] = {platform: [] for platform in PLATFORMS}
    platforms = contract.get("platforms")
    if isinstance(platforms, dict):
        for platform in PLATFORMS:
            section = platforms.get(platform, {})
            items = section.get("requirements", []) if isinstance(section, dict) else section
            if not isinstance(items, list):
                raise ToolError(f"{platform} requirements must be an array")
            for item in items:
                if not isinstance(item, dict) or not item.get("name"):
                    raise ToolError(f"invalid {platform} requirement")
                copy = dict(item)
                copy["platform"] = platform
                output[platform].append(copy)
    for key in ("requirements", "entries"):
        items = contract.get(key)
        if not isinstance(items, list):
            continue
        for item in items:
            if not isinstance(item, dict) or not item.get("name"):
                raise ToolError(f"invalid {key} item")
            platform = item.get("platform")
            if platform == "both":
                targets = PLATFORMS
            elif platform in PLATFORMS:
                targets = (platform,)
            else:
                raise ToolError(f"requirement {item.get('name')} has no platform")
            for target in targets:
                copy = dict(item)
                copy["platform"] = target
                output[target].append(copy)
    for platform in PLATFORMS:
        seen: set[str] = set()
        normalized: list[dict[str, Any]] = []
        for ordinal, item in enumerate(output[platform]):
            name = str(item["name"])
            if name in seen:
                raise ToolError(f"duplicate contract requirement {platform}:{name}")
            seen.add(name)
            copy = dict(item)
            copy.setdefault("ordinal", ordinal)
            normalized.append(copy)
        output[platform] = normalized
    if not any(output.values()):
        raise ToolError("contract has no requirements")
    return output


def names_by_platform(contract: Any) -> dict[str, list[str]]:
    requirements = contract_requirements(contract)
    return {platform: [item["name"] for item in requirements[platform]] for platform in PLATFORMS}


def parse_int_literal(text: str) -> int | None:
    value = text.strip()
    value = re.sub(r"(?i)(ull|llu|ul|lu|ll|u|l)$", "", value)
    if not re.fullmatch(r"[-+]?(?:0[xX][0-9a-fA-F]+|0[bB][01]+|0[oO][0-7]+|[0-9]+)", value):
        return None
    digits = value.lstrip("+-")
    if digits.isdigit():
        base = 8 if len(digits) > 1 and digits.startswith("0") else 10
        return int(value, base)
    return int(value, 0)
