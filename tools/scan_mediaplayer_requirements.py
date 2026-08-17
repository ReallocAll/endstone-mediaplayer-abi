"""Extract the source-level ABI macro contract from a MediaPlayer checkout."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable

try:
    from .common import ToolError, parse_int_literal, requirement_metadata, source_fingerprint, write_json
except ImportError:  # direct ``python tools/script.py`` execution
    from common import ToolError, parse_int_literal, requirement_metadata, source_fingerprint, write_json

MACRO = re.compile(r"^\s*#\s*define\s+(ES_[A-Za-z0-9_]+)(?!\s*\()\s+(.+?)\s*$")
TOKEN = re.compile(r"\bES_[A-Za-z0-9_]+\b")
DEFINE_ANY = re.compile(r"^\s*#\s*define\s+(ES_[A-Za-z0-9_]+)(?:\s|\(|$)")
STRING = re.compile(r'^"(?:\\.|[^"\\])*"$')
PLATFORM_NAMES = {"windows", "linux"}


def _strip_comments(line: str, in_block: bool) -> tuple[str, bool]:
    result: list[str] = []
    cursor = 0
    in_string = False
    escaped = False
    while cursor < len(line):
        if in_block:
            end = line.find("*/", cursor)
            if end < 0:
                return "".join(result), True
            cursor = end + 2
            in_block = False
            continue
        char = line[cursor]
        if in_string:
            result.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            cursor += 1
            continue
        if char == '"':
            in_string = True
            result.append(char)
            cursor += 1
            continue
        start_line = line.find("//", cursor)
        start_block = line.find("/*", cursor)
        starts = [item for item in (start_line, start_block) if item >= 0]
        if not starts:
            result.append(line[cursor:])
            break
        start = min(starts)
        result.append(line[cursor:start])
        if start == start_line:
            break
        cursor = start + 2
        in_block = True
    return "".join(result), in_block


def _literal(raw: str) -> tuple[str, int | str] | None:
    value = raw.strip()
    # A trailing comment is not part of a macro value.
    value = value.split("//", 1)[0].strip()
    if STRING.fullmatch(value):
        return "string", value[1:-1]
    number = parse_int_literal(value)
    if number is not None:
        return "int", number
    return None


def _platform_for(path: Path, mapping: dict[str, str]) -> str | None:
    name = path.name.lower()
    for platform, marker in mapping.items():
        if marker.lower() in name:
            return platform
    for platform in PLATFORM_NAMES:
        if platform in name:
            return platform
    return None


def _parse_header(root: Path, path: Path) -> list[dict]:
    definitions: list[dict] = []
    in_block = False
    for line_number, raw in enumerate(path.read_text(encoding="utf-8", errors="strict").splitlines(), 1):
        line, in_block = _strip_comments(raw, in_block)
        match = MACRO.match(line)
        if not match:
            continue
        literal = _literal(match.group(2))
        if literal is None:
            continue
        value_type, value = literal
        metadata = requirement_metadata(match.group(1))
        definitions.append(
            {
                "name": match.group(1),
                "kind": "object_macro",
                "value_type": value_type,
                "diagnostic_original_value": value,
                "location": {"path": path.relative_to(root).as_posix(), "line": line_number},
                **metadata,
            }
        )
    return definitions


def _source_files(root: Path) -> list[Path]:
    accepted = {".h", ".hh", ".hpp", ".c", ".cc", ".cpp", ".cxx", ".cmake", ".in"}
    files: list[Path] = []
    for path in root.rglob("*"):
        if not path.is_file() or any(part in {".git", "build", ".cache", "__pycache__"} for part in path.parts):
            continue
        relative = path.relative_to(root)
        top_level = relative.parts[0].lower() if relative.parts else ""
        in_source_tree = top_level in {"include", "src", "tests"}
        # CMake inputs belong to the repository root or its dedicated cmake/
        # tree.  Do not traverse ignored editor/agent worktrees which may
        # contain complete nested checkouts and would make a clean checkout
        # fingerprint depend on local tooling state.
        is_cmake_file = path.name.lower().startswith("cmakelists") or path.suffix.lower() == ".cmake"
        is_cmake = is_cmake_file and (len(relative.parts) == 1 or top_level == "cmake")
        if (in_source_tree and path.suffix.lower() in accepted) or is_cmake:
            files.append(path)
    return sorted(files, key=lambda item: item.relative_to(root).as_posix())


def _clean_source_line(path: Path, raw: str, in_block: bool) -> tuple[str, bool]:
    line, in_block = _strip_comments(raw, in_block)
    if (path.name.lower().startswith("cmakelists") or path.suffix.lower() == ".cmake") and line.lstrip().startswith("#"):
        return "", in_block
    return line, in_block


def scan(root: Path, source_ref: str | None = None, source_commit: str | None = None,
         source_repository: str | None = None,
         platform_mapping: dict[str, str] | None = None) -> dict:
    # Preserve the pre-repository positional call shape for library users.
    if isinstance(source_repository, dict) and platform_mapping is None:
        platform_mapping = source_repository
        source_repository = None
    root = root.resolve()
    include_abi = root / "include" / "abi"
    if not include_abi.is_dir():
        raise ToolError(f"missing MediaPlayer include/abi directory: {include_abi}")
    mapping = dict(platform_mapping or {})
    for platform in PLATFORM_NAMES:
        mapping.setdefault(platform, platform)
    headers: dict[str, list[Path]] = {platform: [] for platform in sorted(PLATFORM_NAMES)}
    for path in sorted(include_abi.rglob("*"), key=lambda item: item.as_posix()):
        if path.is_file() and path.suffix.lower() in {".h", ".hh", ".hpp"}:
            platform = _platform_for(path, mapping)
            if platform in headers:
                headers[platform].append(path)
    definitions: dict[str, list[dict]] = {platform: [] for platform in sorted(PLATFORM_NAMES)}
    generated_names: set[str] = set()
    header_paths: dict[str, list[str]] = {platform: [] for platform in sorted(PLATFORM_NAMES)}
    fingerprint_files: list[Path] = []
    for platform in sorted(PLATFORM_NAMES):
        for path in headers[platform]:
            header_paths[platform].append(path.relative_to(root).as_posix())
            fingerprint_files.append(path)
            for definition in _parse_header(root, path):
                definitions[platform].append(definition)
                generated_names.add(definition["name"])
            # Include guards and marker macros are declarations too, although
            # they are intentionally not ABI requirements.
            in_block = False
            for raw in path.read_text(encoding="utf-8", errors="strict").splitlines():
                line, in_block = _clean_source_line(path, raw, in_block)
                declaration = DEFINE_ANY.match(line)
                if declaration:
                    generated_names.add(declaration.group(1))
    if not any(definitions.values()):
        raise ToolError("no platform ES_* object macros found in include/abi")

    source_files = _source_files(root)
    fingerprint_files.extend(path for path in source_files if path not in fingerprint_files)
    all_definitions: dict[str, list[dict]] = {}
    for path in source_files:
        in_block = False
        for line_number, raw in enumerate(path.read_text(encoding="utf-8", errors="strict").splitlines(), 1):
            line, in_block = _clean_source_line(path, raw, in_block)
            match = MACRO.match(line)
            declaration = DEFINE_ANY.match(line)
            if declaration and declaration.group(1) not in all_definitions:
                all_definitions[declaration.group(1)] = []
            if declaration:
                all_definitions[declaration.group(1)].append({"path": path.relative_to(root).as_posix(), "line": line_number})
            if not match:
                continue
    references: dict[str, list[dict]] = {}
    for path in source_files:
        in_block = False
        for line_number, raw in enumerate(path.read_text(encoding="utf-8", errors="strict").splitlines(), 1):
            line, in_block = _clean_source_line(path, raw, in_block)
            for match in TOKEN.finditer(line):
                name = match.group(0)
                # A definition is not a reference at its own declaration.
                if re.match(r"^\s*#\s*define\s+" + re.escape(name) + r"(?:\s|$)", line):
                    continue
                references.setdefault(name, []).append(
                    {"path": path.relative_to(root).as_posix(), "line": line_number, "column": match.start() + 1}
                )

    generated_header_paths = {
        path.relative_to(root).as_posix()
        for platform_paths in headers.values()
        for path in platform_paths
    }
    # A generated platform header is the definition source, not a consumer.
    # References in it must not make historical/unused definitions required.
    external_references = {
        name: [location for location in locations if location["path"] not in generated_header_paths]
        for name, locations in references.items()
    }
    external_references = {name: locations for name, locations in external_references.items() if locations}

    if "ES_PERMISSION_SIZE" in external_references:
        raise ToolError(
            "ES_PERMISSION_SIZE is not an external ABI requirement: the MediaPlayer "
            "permissions vector is empty and must be constructed without an element stride"
        )

    requirements: dict[str, list[dict]] = {}
    unused_generated_definitions: dict[str, list[dict]] = {}
    for platform in sorted(PLATFORM_NAMES):
        ordered: list[dict] = []
        unused: list[dict] = []
        for item in definitions[platform]:
            if item["name"] not in external_references:
                unused.append(
                    {
                        "name": item["name"],
                        "platform": platform,
                        "location": item["location"],
                        "diagnostic_original_value": item["diagnostic_original_value"],
                        **requirement_metadata(item["name"]),
                    }
                )
                continue
            copy = dict(item)
            copy["platform"] = platform
            copy["ordinal"] = len(ordered)
            copy["locations"] = [copy["location"]]
            ordered.append(copy)
        requirements[platform] = ordered
        unused_generated_definitions[platform] = unused

    helpers = []
    undefined = []
    for name in sorted(external_references):
        if name in generated_names:
            continue
        definitions_here = all_definitions.get(name, [])
        if definitions_here:
            helpers.append({"name": name, "locations": definitions_here})
        else:
            undefined.append({"name": name, "locations": external_references[name]})
    if undefined:
        names = ", ".join(item["name"] for item in undefined)
        raise ToolError(f"undefined ES_* references: {names}")
    return {
        "schema_version": 1,
        "source_fingerprint": source_fingerprint(root, fingerprint_files),
        "source_repository": source_repository,
        "source_ref": source_ref,
        "source_commit": source_commit,
        "mediaplayer": {
            "repository": source_repository,
            "ref": source_ref,
            "commit": source_commit,
            "fingerprint": source_fingerprint(root, fingerprint_files),
        },
        "header_paths": header_paths,
        "platforms": {platform: {"requirements": requirements[platform]} for platform in sorted(PLATFORM_NAMES)},
        "helpers": helpers,
        "references": [{"name": name, "locations": external_references[name]} for name in sorted(external_references)],
        "unused_generated_definitions": [item for platform in sorted(PLATFORM_NAMES) for item in unused_generated_definitions[platform]],
        "undefined_references": [],
    }


def _mapping(values: Iterable[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        try:
            platform, marker = value.split("=", 1)
        except ValueError as exc:
            raise ToolError(f"--platform-map requires platform=filename-marker: {value!r}") from exc
        if platform not in PLATFORM_NAMES or not marker:
            raise ToolError(f"invalid platform mapping: {value!r}")
        result[platform] = marker
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mediaplayer-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-ref")
    parser.add_argument("--source-commit")
    parser.add_argument("--source-repository")
    parser.add_argument("--platform-map", action="append", default=[])
    args = parser.parse_args(argv)
    try:
        result = scan(args.mediaplayer_root, args.source_ref, args.source_commit, args.source_repository, _mapping(args.platform_map))
        write_json(args.output, result)
    except (OSError, UnicodeError, ToolError) as exc:
        print(f"scan_mediaplayer_requirements: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
