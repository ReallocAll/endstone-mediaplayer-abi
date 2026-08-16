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
from tools.common import ToolError, read_json, sha256_file


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

    def test_determinism(self):
        self.assertEqual(self.contract(), self.contract())

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
        package(self.output, contract_path, windows_path, linux_path, manifest_path, coverage_path,
                windows_consumer_summary=windows_summary_path, linux_consumer_summary=linux_summary_path)
        self.assertEqual(validate_artifact(self.output), [])
        self.assertEqual(sha256_file(self.output / "include" / "abi" / "windows_x86_64.h"), self.metadata["headers"][1]["sha256"])

    def test_artifact_missing_or_malformed_fails(self):
        evidence = self.output / "abi-evidence"
        evidence.mkdir()
        (evidence / "requirements.json").write_text("{", encoding="utf-8")
        self.assertTrue(validate_artifact(self.output))

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
        package(output, contract_path, windows_path, linux_path, manifest_path, coverage_path,
                windows_consumer_summary=windows_summary_path, linux_consumer_summary=linux_summary_path)
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
        package(self.output, paths["requirements.json"], paths["windows.json"], paths["linux.json"], paths["manifest.json"], paths["coverage.json"],
                windows_consumer_summary=paths["windows-summary.json"], linux_consumer_summary=paths["linux-summary.json"])
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
        package(self.output, paths["requirements.json"], paths["windows.json"], paths["linux.json"], paths["manifest.json"], paths["coverage.json"],
                windows_consumer_summary=paths["windows-summary.json"], linux_consumer_summary=paths["linux-summary.json"])
        self.assertEqual(validate_artifact(self.output), [])
        manifest = read_json(self.output / "abi-evidence" / "manifest.json")
        manifest["consumer_validation"]["linux"]["build"]["exit_code"] = 1
        (self.output / "abi-evidence" / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.assertTrue(any("build exit code" in error for error in validate_artifact(self.output)))


if __name__ == "__main__":
    unittest.main()
