from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

from tools.generate_headers import generate
from tools.package_artifact import package
from tools.resolve_abi import resolve, resolve_platform
from tools.scan_mediaplayer_requirements import scan
from tools.validate_artifact import validate as validate_artifact
from tools.validate_report import validate as validate_report
from tools.common import ToolError, read_json, requirement_metadata, sha256_file


class ToolchainFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "include" / "abi").mkdir(parents=True)
        (self.root / "include" / "helper.h").write_text("#define ES_HELPER 7\n", encoding="utf-8")
        (self.root / "src").mkdir()
        (self.root / "src" / "main.cpp").write_text("#include <helper.h>\nint x = ES_WIN; int y = ES_HELPER;\n", encoding="utf-8")
        (self.root / "CMakeLists.txt").write_text("add_compile_definitions(ES_TEXT)\n", encoding="utf-8")
        (self.root / "include" / "abi" / "windows_x86_64.h").write_text(
            "#ifndef OLD\n#define ES_WIN 4\n#define ES_TEXT \"hello\"\n#define ES_UNUSED 88\n", encoding="utf-8"
        )
        (self.root / "include" / "abi" / "linux_x86_64.h").write_text(
            "#define ES_WIN 4u\n#define ES_TEXT \"hello\"\n#define ES_UNUSED 88\n", encoding="utf-8"
        )

    def tearDown(self):
        self.temp.cleanup()

    def contract(self):
        return scan(self.root, "ref", "commit")

    def report(self, contract, platform, values=None, provenance="RUNTIME_OBJECT", complete=True):
        values = values or {item["name"]: item["diagnostic_original_value"] for item in contract["platforms"][platform]["requirements"]}
        entries = []
        for item in contract["platforms"][platform]["requirements"]:
            name = item["name"]
            value = values.get(name)
            item_provenance = provenance
            dependencies = None
            if isinstance(value, tuple):
                value, dependencies = value
                item_provenance = "RUNTIME_DERIVED"
            entry = {
                "name": name,
                "value": value,
                "provenance": item_provenance,
                "method": "runtime probe",
                "evidence": {"symbol": name},
                "category": item["category"],
                "contract_identity": item["contract_identity"],
                "runtime_required": item["runtime_required"],
            }
            if dependencies is not None:
                entry["dependencies"] = dependencies
            entries.append(entry)
        return {
            "schema_version": 1,
            "run_id": platform + "-run",
            "complete": complete,
            "environment": {
                "platform": platform,
                "arch": "x86_64",
                "pointer_size": 8,
                "endstone_runtime_version": "0.11",
                "api_version": "0.11",
                "source_ref": "ref",
                "source_commit": "commit",
                "compiler": "test",
                "stdlib": "test",
                "bds_version": "test",
                "probe_loaded": True,
                "clean_start": True,
            },
            "requirements": entries,
        }

    def consumer_summary(self, contract, platform):
        return {
            "status": "PASS",
            "overlay_removed_existing_headers": True,
            "configure": {"exit_code": 0},
            "build": {"exit_code": 0},
            "ctest": {"exit_code": 0},
            "source_commit": contract["source_commit"],
            "source_fingerprint": contract["source_fingerprint"],
            "platform": platform,
        }

    def consumer_runtime(self, contract, platform, prefix="runtime"):
        log_path = self.root / f"{prefix}-{platform}.log"
        log_path.write_text(f"{platform} consumer runtime\n", encoding="utf-8")
        summary = {
            "schema_version": 1,
            "status": "PASS",
            "platform": platform,
            "run_id": f"{platform}-consumer-run",
            "fresh_server": True,
            "player_required": False,
            "commands": ["mpm help", "mpv help"],
            "logical_screen_lifecycle": "PASS",
            "command_sender_message": "PASS",
            "graceful_shutdown": True,
            "forced": False,
            "exit_code": 0,
            "media_player_commit": contract["source_commit"],
            "endstone_commit": "commit",
            "endstone_version": "0.11",
            "bds_version": "test",
            "log": str(log_path),
            "log_sha256": sha256_file(log_path),
        }
        summary_path = self.root / f"{prefix}-{platform}-runtime.json"
        summary_path.write_text(json.dumps(summary), encoding="utf-8")
        return summary_path, log_path


class ExtractorTests(ToolchainFixture):
    def test_current_style_platform_and_helpers(self):
        result = self.contract()
        self.assertEqual(result["schema_version"], 1)
        self.assertEqual([x["name"] for x in result["platforms"]["windows"]["requirements"]], ["ES_WIN", "ES_TEXT"])
        self.assertEqual([x["name"] for x in result["unused_generated_definitions"]], ["ES_UNUSED", "ES_UNUSED"])
        self.assertEqual(result["platforms"]["linux"]["requirements"][0]["value_type"], "int")
        self.assertEqual(result["helpers"][0]["name"], "ES_HELPER")
        self.assertIn("include/abi/windows_x86_64.h", result["header_paths"]["windows"])

    def test_source_repository_is_media_player_metadata(self):
        result = scan(self.root, "ref", "commit", "https://example.invalid/mp")
        self.assertEqual(result["mediaplayer"]["repository"], "https://example.invalid/mp")
        self.assertEqual(result["mediaplayer"]["ref"], "ref")

    def test_requirement_semantic_metadata_is_present(self):
        result = self.contract()
        for platform in ("windows", "linux"):
            for item in result["platforms"][platform]["requirements"]:
                self.assertIn(item["category"], {
                    "external_abi", "consumer_synthetic_layout", "compatibility_storage",
                    "derived_invariant", "runtime_behavior",
                })
                self.assertTrue(item["contract_identity"])
                self.assertIsInstance(item["runtime_required"], bool)

    def test_determinism(self):
        self.assertEqual(self.contract(), self.contract())

    def test_fingerprint_is_checkout_eol_independent(self):
        before = self.contract()["source_fingerprint"]
        path = self.root / "CMakeLists.txt"
        lf = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        path.write_bytes(lf.replace(b"\n", b"\r\n"))
        self.assertEqual(before, self.contract()["source_fingerprint"])

    def test_nested_tool_worktree_is_out_of_scope(self):
        before = self.contract()
        nested = self.root / ".claude" / "worktrees" / "agent"
        nested.mkdir(parents=True)
        (nested / "CMakeLists.txt").write_text("add_compile_definitions(ES_FOREIGN)\n", encoding="utf-8")
        self.assertEqual(before, self.contract())

    def test_snapshot_drift(self):
        before = self.contract()["source_fingerprint"]
        path = self.root / "include" / "abi" / "linux_x86_64.h"
        path.write_text(path.read_text(encoding="utf-8") + "#define ES_DRIFT 9\n", encoding="utf-8")
        after = self.contract()
        self.assertNotEqual(before, after["source_fingerprint"])

    def test_new_consumer_reference_promotes_unused_definition(self):
        before = self.contract()
        self.assertNotIn("ES_UNUSED", [item["name"] for item in before["platforms"]["windows"]["requirements"]])
        (self.root / "src" / "main.cpp").write_text(
            (self.root / "src" / "main.cpp").read_text(encoding="utf-8") + "int promoted = ES_UNUSED;\n",
            encoding="utf-8",
        )
        after = self.contract()
        self.assertIn("ES_UNUSED", [item["name"] for item in after["platforms"]["windows"]["requirements"]])
        self.assertNotEqual(before["source_fingerprint"], after["source_fingerprint"])

    def test_new_undefined_reference_fails(self):
        (self.root / "src" / "new.cpp").write_text("int z = ES_NEW_REF;\n", encoding="utf-8")
        with self.assertRaises(ToolError):
            scan(self.root)

    def test_permission_empty_vector_stride_cannot_become_an_abi_requirement(self):
        header = self.root / "include" / "abi" / "windows_x86_64.h"
        header.write_text(header.read_text(encoding="utf-8") + "#define ES_PERMISSION_SIZE 1\n", encoding="utf-8")
        source = self.root / "src" / "main.cpp"
        source.write_text(source.read_text(encoding="utf-8") + "int permission_stride = ES_PERMISSION_SIZE;\n",
                          encoding="utf-8")
        with self.assertRaisesRegex(ToolError, "permissions vector is empty"):
            self.contract()


class ReportTests(ToolchainFixture):
    def setUp(self):
        super().setUp()
        self.contract_value = self.contract()
        self.good = self.report(self.contract_value, "windows")

    def test_valid(self):
        self.assertEqual(validate_report(self.good, expected_platform="windows", expected_run_id="windows-run", contract=self.contract_value), [])

    def test_missing_environment(self):
        report = copy.deepcopy(self.good)
        del report["environment"]["compiler"]
        self.assertTrue(validate_report(report))

    def test_wrong_platform(self):
        self.assertTrue(validate_report(self.good, expected_platform="linux"))

    def test_duplicate(self):
        report = copy.deepcopy(self.good)
        report["requirements"].append(copy.deepcopy(report["requirements"][0]))
        self.assertTrue(any("duplicate" in error for error in validate_report(report)))

    def test_type(self):
        report = copy.deepcopy(self.good)
        report["requirements"][0]["value"] = True
        self.assertTrue(validate_report(report))

    def test_unresolved_is_allowed_only_as_incomplete(self):
        report = copy.deepcopy(self.good)
        report["requirements"][0]["value"] = None
        report["requirements"][0]["provenance"] = "UNRESOLVED"
        report["complete"] = False
        self.assertEqual(validate_report(report), [])
        report["complete"] = True
        self.assertTrue(validate_report(report))


class ResolverTests(ToolchainFixture):
    def setUp(self):
        super().setUp()
        self.contract_value = self.contract()
        self.windows = self.report(self.contract_value, "windows")
        self.linux = self.report(self.contract_value, "linux")

    def test_priority_and_derived(self):
        fallback = self.report(self.contract_value, "windows", provenance="COMPILE_MEASURED")
        self.windows["requirements"][0]["value"] = 4
        self.windows["requirements"][0]["provenance"] = "RUNTIME_DERIVED"
        self.windows["requirements"][0]["dependencies"] = ["ES_TEXT"]
        manifest, coverage = resolve(self.contract_value, self.windows, self.linux, [fallback])
        self.assertEqual(manifest["platforms"]["windows"]["requirements"][0]["provenance"], "RUNTIME_DERIVED")
        self.assertEqual(coverage["platforms"]["windows"]["percent"], 100.0)

    def test_conflict_fails(self):
        fallback = self.report(self.contract_value, "windows", provenance="COMPILE_MEASURED")
        fallback["requirements"][0]["value"] = 999
        with self.assertRaises(ToolError):
            resolve(self.contract_value, self.windows, self.linux, [fallback])

    def test_forbidden_fails(self):
        fallback = self.report(self.contract_value, "windows", provenance="COMPILE_MEASURED")
        fallback["requirements"][0]["method"] = "guessed"
        with self.assertRaises(ToolError):
            resolve(self.contract_value, self.windows, self.linux, [fallback])

    def test_old_header_fallback_fails(self):
        fallback = self.report(self.contract_value, "windows", provenance="STATIC_VERIFIED")
        fallback["requirements"][0]["evidence"] = "old header"
        with self.assertRaises(ToolError):
            resolve(self.contract_value, self.windows, self.linux, [fallback])

    def test_missing_linux_fails(self):
        self.linux["requirements"][0]["provenance"] = "UNRESOLVED"
        self.linux["requirements"][0]["value"] = None
        with self.assertRaises(ToolError):
            resolve(self.contract_value, self.windows, self.linux)

    def test_media_player_and_endstone_sources_are_distinct(self):
        contract = copy.deepcopy(self.contract_value)
        contract["source_ref"] = "media-player-ref"
        contract["mediaplayer"]["ref"] = "media-player-ref"
        self.windows["environment"]["source_ref"] = "endstone-ref"
        self.linux["environment"]["source_ref"] = "endstone-ref"
        manifest, _ = resolve(contract, self.windows, self.linux)
        self.assertEqual(manifest["mediaplayer"]["ref"], "media-player-ref")
        self.assertEqual(manifest["platforms"]["windows"]["environment"]["source_ref"], "endstone-ref")

    def test_primary_incomplete_fails_final_resolution(self):
        self.windows["complete"] = False
        with self.assertRaises(ToolError):
            resolve(self.contract_value, self.windows, self.linux)

    def test_single_platform_manifest_is_intermediate_only(self):
        manifest, coverage = resolve_platform(self.contract_value, "windows", self.windows)
        output = self.root / "single"
        generate(manifest, output)
        self.assertTrue((output / "include" / "abi" / "windows_x86_64.h").is_file())
        self.assertFalse((output / "include" / "abi" / "linux_x86_64.h").exists())
        self.assertEqual(sorted(manifest["header_paths"]), ["windows"])
        self.assertEqual(sorted(coverage["platforms"]), ["windows"])
        self.assertTrue(validate_artifact(output))


class GeneratorArtifactTests(ToolchainFixture):
    def setUp(self):
        super().setUp()
        contract = self.contract()
        windows = self.report(contract, "windows")
        linux = self.report(contract, "linux")
        self.manifest, self.coverage = resolve(contract, windows, linux)
        self.output = self.root / "artifact"
        self.metadata = generate(self.manifest, self.output)

    def test_deterministic_guard_names_paths_and_golden(self):
        first = (self.output / "include" / "abi" / "windows_x86_64.h").read_bytes()
        other = self.root / "other"
        generate(self.manifest, other)
        self.assertEqual(first, (other / "include" / "abi" / "windows_x86_64.h").read_bytes())
        self.assertIn(b"#ifndef ES_ABI_WINDOWS_X86_64_H", first)
        self.assertIn(b"// RUNTIME_OBJECT", first)
        self.assertEqual(self.metadata["headers"][0]["path"], "include/abi/linux_x86_64.h")

    def test_placeholder_and_bad_names_fail(self):
        bad = copy.deepcopy(self.manifest)
        bad["platforms"]["windows"]["requirements"][0]["evidence"] = "placeholder"
        with self.assertRaises(ToolError):
            generate(bad, self.root / "bad")

    def synthetic_manifest(self, offset=64, size=8, impl_size=72, alignment=8):
        names = (
            ("ES_PLUGIN_OFF_DESCRIPTION", offset),
            ("ES_DESCRIPTION_SIZE", size),
            ("ES_PLUGIN_IMPL_SIZE", impl_size),
            ("ES_DESCRIPTION_ALIGN", alignment),
        )
        requirements = []
        for ordinal, (name, value) in enumerate(names):
            metadata = requirement_metadata(name)
            requirements.append({
                "name": name,
                "platform": "windows",
                "ordinal": ordinal,
                "value": value,
                "provenance": "RUNTIME_PROBE",
                "method": "synthetic test",
                "evidence": "synthetic test",
                **metadata,
                "location": {"path": "include/abi/windows_x86_64.h", "line": ordinal + 1},
            })
        return {
            "schema_version": 1,
            "header_paths": {"windows": ["include/abi/windows_x86_64.h"]},
            "platforms": {"windows": {"requirements": requirements}},
        }

    def test_synthetic_layout_invariants_are_emitted_and_bad_samples_rejected(self):
        output = self.root / "synthetic"
        generate(self.synthetic_manifest(), output)
        header = (output / "include" / "abi" / "windows_x86_64.h").read_text(encoding="utf-8")
        self.assertIn("ES_PLUGIN_OFF_DESCRIPTION + ES_DESCRIPTION_SIZE != ES_PLUGIN_IMPL_SIZE", header)
        with self.assertRaises(ToolError):
            generate(self.synthetic_manifest(offset=200, size=8, impl_size=176), self.root / "bad-bounds")
        with self.assertRaises(ToolError):
            generate(self.synthetic_manifest(offset=7, size=8, impl_size=15), self.root / "bad-alignment")

    def test_runtime_required_manifest_needs_runtime_proof(self):
        contract = self.contract()
        contract["platforms"]["windows"]["requirements"][0]["runtime_required"] = True
        windows = self.report(contract, "windows", provenance="COMPILE_MEASURED")
        linux = self.report(contract, "linux")
        manifest, coverage = resolve(contract, windows, linux)
        output = self.root / "runtime-required"
        generate(manifest, output)
        paths = {}
        values = {
            "requirements.json": contract,
            "windows.json": windows,
            "linux.json": linux,
            "manifest.json": manifest,
            "coverage.json": coverage,
            "windows-summary.json": self.consumer_summary(contract, "windows"),
            "linux-summary.json": self.consumer_summary(contract, "linux"),
        }
        for name, value in values.items():
            path = self.root / ("runtime-" + name)
            path.write_text(json.dumps(value), encoding="utf-8")
            paths[name] = path
        runtime_paths = {platform: self.consumer_runtime(contract, platform) for platform in ("windows", "linux")}
        package(output, paths["requirements.json"], paths["windows.json"], paths["linux.json"], paths["manifest.json"], paths["coverage.json"],
                windows_consumer_summary=paths["windows-summary.json"], linux_consumer_summary=paths["linux-summary.json"],
                windows_consumer_runtime=runtime_paths["windows"][0], linux_consumer_runtime=runtime_paths["linux"][0],
                windows_consumer_log=runtime_paths["windows"][1], linux_consumer_log=runtime_paths["linux"][1])
        self.assertTrue(any("missing required runtime proof" in error for error in validate_artifact(output)))


    def test_artifact_exact_hash_and_overlay(self):
        contract_path = self.root / "requirements.json"
        windows_path = self.root / "windows.json"
        linux_path = self.root / "linux.json"
        manifest_path = self.root / "manifest.json"
        coverage_path = self.root / "coverage.json"
        windows_summary_path = self.root / "windows-summary.json"
        linux_summary_path = self.root / "linux-summary.json"
        contract = self.contract()
        windows = self.report(contract, "windows")
        linux = self.report(contract, "linux")
        for path, value in ((contract_path, contract), (windows_path, windows), (linux_path, linux), (manifest_path, self.manifest), (coverage_path, self.coverage),
                            (windows_summary_path, self.consumer_summary(contract, "windows")), (linux_summary_path, self.consumer_summary(contract, "linux"))):
            path.write_text(json.dumps(value), encoding="utf-8")
        runtime_paths = {platform: self.consumer_runtime(contract, platform) for platform in ("windows", "linux")}
        package(self.output, contract_path, windows_path, linux_path, manifest_path, coverage_path,
                windows_consumer_summary=windows_summary_path, linux_consumer_summary=linux_summary_path,
                windows_consumer_runtime=runtime_paths["windows"][0], linux_consumer_runtime=runtime_paths["linux"][0],
                windows_consumer_log=runtime_paths["windows"][1], linux_consumer_log=runtime_paths["linux"][1])
        self.assertEqual(validate_artifact(self.output), [])
        self.assertEqual(sha256_file(self.output / "include" / "abi" / "windows_x86_64.h"), self.metadata["headers"][1]["sha256"])

    def test_artifact_missing_or_malformed_fails(self):
        evidence = self.output / "abi-evidence"
        evidence.mkdir()
        (evidence / "requirements.json").write_text("{", encoding="utf-8")
        self.assertTrue(validate_artifact(self.output))

    def test_consumer_runtime_missing_malformed_and_hash_mismatch_fail(self):
        contract = self.contract()
        files = {
            "requirements.json": contract,
            "windows.json": self.report(contract, "windows"),
            "linux.json": self.report(contract, "linux"),
            "manifest.json": self.manifest,
            "coverage.json": self.coverage,
            "windows-summary.json": self.consumer_summary(contract, "windows"),
            "linux-summary.json": self.consumer_summary(contract, "linux"),
        }
        paths = {}
        for name, value in files.items():
            path = self.root / ("runtime-proof-" + name)
            path.write_text(json.dumps(value), encoding="utf-8")
            paths[name] = path
        runtime_paths = {platform: self.consumer_runtime(contract, platform, "proof") for platform in ("windows", "linux")}
        package(self.output, paths["requirements.json"], paths["windows.json"], paths["linux.json"], paths["manifest.json"], paths["coverage.json"],
                windows_consumer_summary=paths["windows-summary.json"], linux_consumer_summary=paths["linux-summary.json"],
                windows_consumer_runtime=runtime_paths["windows"][0], linux_consumer_runtime=runtime_paths["linux"][0],
                windows_consumer_log=runtime_paths["windows"][1], linux_consumer_log=runtime_paths["linux"][1])
        evidence = self.output / "abi-evidence"
        (evidence / "windows-consumer-runtime.json").unlink()
        self.assertTrue(any("missing evidence" in error for error in validate_artifact(self.output)))

        package(self.output, paths["requirements.json"], paths["windows.json"], paths["linux.json"], paths["manifest.json"], paths["coverage.json"],
                windows_consumer_summary=paths["windows-summary.json"], linux_consumer_summary=paths["linux-summary.json"],
                windows_consumer_runtime=runtime_paths["windows"][0], linux_consumer_runtime=runtime_paths["linux"][0],
                windows_consumer_log=runtime_paths["windows"][1], linux_consumer_log=runtime_paths["linux"][1])
        (evidence / "windows-consumer-runtime.json").write_text("{", encoding="utf-8")
        self.assertTrue(any("malformed evidence JSON" in error for error in validate_artifact(self.output)))

        package(self.output, paths["requirements.json"], paths["windows.json"], paths["linux.json"], paths["manifest.json"], paths["coverage.json"],
                windows_consumer_summary=paths["windows-summary.json"], linux_consumer_summary=paths["linux-summary.json"],
                windows_consumer_runtime=runtime_paths["windows"][0], linux_consumer_runtime=runtime_paths["linux"][0],
                windows_consumer_log=runtime_paths["windows"][1], linux_consumer_log=runtime_paths["linux"][1])
        (evidence / "windows-consumer-console.log").write_text("tampered\n", encoding="utf-8")
        self.assertTrue(any("log hash mismatch" in error for error in validate_artifact(self.output)))

    def test_nondefault_discovered_header_paths_round_trip(self):
        contract = self.contract()
        custom_paths = {"windows": ["include/abi/win_custom.h"], "linux": ["include/abi/lin_custom.h"]}
        contract["header_paths"] = custom_paths
        for platform in ("windows", "linux"):
            for item in contract["platforms"][platform]["requirements"]:
                item["location"]["path"] = custom_paths[platform][0]
                item["locations"] = [item["location"]]
        windows = self.report(contract, "windows")
        linux = self.report(contract, "linux")
        manifest, coverage = resolve(contract, windows, linux)
        output = self.root / "custom-artifact"
        generate(manifest, output)
        self.assertTrue((output / "include" / "abi" / "win_custom.h").is_file())
        self.assertFalse((output / "include" / "abi" / "windows_x86_64.h").exists())
        contract_path = self.root / "custom-requirements.json"
        windows_path = self.root / "custom-windows.json"
        linux_path = self.root / "custom-linux.json"
        manifest_path = self.root / "custom-manifest.json"
        coverage_path = self.root / "custom-coverage.json"
        windows_summary_path = self.root / "custom-windows-summary.json"
        linux_summary_path = self.root / "custom-linux-summary.json"
        for path, value in ((contract_path, contract), (windows_path, windows), (linux_path, linux), (manifest_path, manifest), (coverage_path, coverage),
                            (windows_summary_path, self.consumer_summary(contract, "windows")), (linux_summary_path, self.consumer_summary(contract, "linux"))):
            path.write_text(json.dumps(value), encoding="utf-8")
        runtime_paths = {platform: self.consumer_runtime(contract, platform) for platform in ("windows", "linux")}
        package(output, contract_path, windows_path, linux_path, manifest_path, coverage_path,
                windows_consumer_summary=windows_summary_path, linux_consumer_summary=linux_summary_path,
                windows_consumer_runtime=runtime_paths["windows"][0], linux_consumer_runtime=runtime_paths["linux"][0],
                windows_consumer_log=runtime_paths["windows"][1], linux_consumer_log=runtime_paths["linux"][1])
        self.assertEqual(validate_artifact(output), [])

    def test_artifact_manifest_metadata_cross_check(self):
        contract = self.contract()
        files = {
            "requirements.json": contract,
            "windows.json": self.report(contract, "windows"),
            "linux.json": self.report(contract, "linux"),
            "manifest.json": self.manifest,
            "coverage.json": self.coverage,
            "windows-summary.json": self.consumer_summary(contract, "windows"),
            "linux-summary.json": self.consumer_summary(contract, "linux"),
        }
        paths = {}
        for name, value in files.items():
            path = self.root / name
            path.write_text(json.dumps(value), encoding="utf-8")
            paths[name] = path
        runtime_paths = {platform: self.consumer_runtime(contract, platform) for platform in ("windows", "linux")}
        package(self.output, paths["requirements.json"], paths["windows.json"], paths["linux.json"], paths["manifest.json"], paths["coverage.json"],
                windows_consumer_summary=paths["windows-summary.json"], linux_consumer_summary=paths["linux-summary.json"],
                windows_consumer_runtime=runtime_paths["windows"][0], linux_consumer_runtime=runtime_paths["linux"][0],
                windows_consumer_log=runtime_paths["windows"][1], linux_consumer_log=runtime_paths["linux"][1])
        evidence = self.output / "abi-evidence"
        manifest = read_json(evidence / "manifest.json")
        manifest["mediaplayer"]["fingerprint"] = "different"
        (evidence / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.assertTrue(any("MediaPlayer identity" in error for error in validate_artifact(self.output)))

    def test_consumer_summary_is_required_and_validated(self):
        contract = self.contract()
        files = {
            "requirements.json": contract,
            "windows.json": self.report(contract, "windows"),
            "linux.json": self.report(contract, "linux"),
            "manifest.json": self.manifest,
            "coverage.json": self.coverage,
            "windows-summary.json": self.consumer_summary(contract, "windows"),
            "linux-summary.json": self.consumer_summary(contract, "linux"),
        }
        paths = {}
        for name, value in files.items():
            path = self.root / ("summary-" + name)
            path.write_text(json.dumps(value), encoding="utf-8")
            paths[name] = path
        runtime_paths = {platform: self.consumer_runtime(contract, platform) for platform in ("windows", "linux")}
        package(self.output, paths["requirements.json"], paths["windows.json"], paths["linux.json"], paths["manifest.json"], paths["coverage.json"],
                windows_consumer_summary=paths["windows-summary.json"], linux_consumer_summary=paths["linux-summary.json"],
                windows_consumer_runtime=runtime_paths["windows"][0], linux_consumer_runtime=runtime_paths["linux"][0],
                windows_consumer_log=runtime_paths["windows"][1], linux_consumer_log=runtime_paths["linux"][1])
        self.assertEqual(validate_artifact(self.output), [])
        manifest = read_json(self.output / "abi-evidence" / "manifest.json")
        manifest["consumer_validation"]["linux"]["build"]["exit_code"] = 1
        (self.output / "abi-evidence" / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.assertTrue(any("build exit code" in error for error in validate_artifact(self.output)))


class SourceSemanticTests(unittest.TestCase):
    def test_overload_contracts_are_exact(self):
        source = (Path(__file__).parents[1] / "src" / "vtable_probe.cpp").read_text(encoding="utf-8")
        self.assertIn("void (endstone::Block::*)(const endstone::BlockData &, bool)", source)
        self.assertIn("endstone::ItemStack (endstone::ItemType::*)(int) const", source)

    def test_synthetic_offset_does_not_use_live_plugin(self):
        source = (Path(__file__).parents[1] / "src" / "layout_probe.cpp").read_text(encoding="utf-8")
        start = source.index('if (name == "ES_PLUGIN_OFF_DESCRIPTION")')
        end = source.index('if (name == "ES_DESCRIPTION_SIZE")', start)
        block = source[start:end]
        self.assertIn("MinimalPlugin plugin", block)
        self.assertNotIn("live_plugin->getDescription", block)


if __name__ == "__main__":
    unittest.main()
