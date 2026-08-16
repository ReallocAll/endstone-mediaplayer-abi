# Endstone MediaPlayer ABI automation

This repository generates the platform ABI headers consumed by
[`ReallocAll/endstone-mediaplayer`](https://github.com/ReallocAll/endstone-mediaplayer).
The values are measured from a probe compiled against an exact Endstone release and successfully loaded by the matching real Endstone/BDS runtime. Existing MediaPlayer header values are diagnostics only and are never fallback truth.

## Supported flow

The canonical workflow is `.github/workflows/abi.yml`:

1. resolve the requested MediaPlayer ref and the latest stable (or explicitly pinned) Endstone release;
2. scan the checked-out MediaPlayer source to discover its current, actually referenced ABI requirements;
3. independently build and load the probe in fresh Windows x64 and Linux x64 Endstone/BDS processes;
4. validate the versioned JSON reports and require exactly 100% current-platform coverage;
5. generate the platform header, delete the checkout's existing ABI headers, overlay only the generated header, then configure, build, and run CTest;
6. merge both successful platform reports and publish `endstone-mediaplayer-abi`.

Windows and Linux evidence is independent. A value from one platform can never satisfy the other platform.

## Evidence hierarchy

Every resolved requirement records one of these provenances, in priority order:

1. `RUNTIME_OBJECT` — observed from a real live Endstone object;
2. `RUNTIME_PROBE` — an exact compiler/language/layout fact emitted by the loaded probe;
3. `RUNTIME_DERIVED` — an explicit dependency-checked derivation from exact runtime facts;
4. `COMPILE_MEASURED` — an exact matching-SDK/toolchain fact unavailable from a live object;
5. `STATIC_VERIFIED` — a documented exact source/compiler-layout fallback.

`ASSUMED`, `GUESSED`, `LIKELY`, `UNKNOWN`, `PLACEHOLDER`, stale header values, and cross-platform reuse are forbidden. Missing measurements, source/runtime identity mismatches, evidence conflicts, incomplete coverage, stale reports, probe load failures, unclean shutdowns, or consumer failures prevent the final artifact from being uploaded.

## Local discovery and tooling tests

The source checkout is always explicit; no tool contains a personal path:

```powershell
python tools/scan_mediaplayer_requirements.py `
  --mediaplayer-root C:\path\to\endstone-mediaplayer `
  --source-repository ReallocAll/endstone-mediaplayer `
  --source-ref dev `
  --source-commit <commit> `
  --output build/requirements.json

python -m unittest discover -s tests -v
```

The scanner fingerprints the relevant MediaPlayer source tree, distinguishes platform requirements, records unused generated definitions only as diagnostics, and fails on an undefined new `ES_*` reference. The tooling tests cover extraction drift, report/schema failures, evidence priority and conflicts, platform isolation, deterministic headers, missing-measurement gates, and malformed artifacts.

## Probe build and execution

The probe requires CMake 3.21+, Ninja, x86-64, and the Endstone-compatible compiler/standard library: clang-cl/MSVC STL on Windows and Clang/libc++ on Linux. `ENDSTONE_SOURCE_DIR` must be a read-only checkout whose HEAD matches `ABI_PROBE_SOURCE_COMMIT`.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl `
  "-DENDSTONE_SOURCE_DIR=C:\path\to\endstone" `
  "-DENDSTONE_REF=v0.11.8" `
  "-DABI_PROBE_SOURCE_REF=v0.11.8" `
  "-DABI_PROBE_SOURCE_COMMIT=<exact-commit>" `
  "-DBDS_VERSION=26.40"
cmake --build build
ctest --test-dir build --output-on-failure
```

`tools/run_endstone_probe.py` stages the built plugin into a fresh server directory, launches the official `endstone --yes --no-interactive` entry point, binds the report to a fresh run ID, waits for the structured completion marker, sends `stop`, verifies clean exit, and preserves the console log. A player is not required.

## GitHub Actions inputs

Pushes to `main` use MediaPlayer `dev` (the current release-development target) and resolve Endstone `latest` from the official GitHub release. `workflow_dispatch` accepts:

- `mediaplayer_ref` — any target MediaPlayer ref or commit;
- `endstone_ref` — `latest` or a supported release tag such as `v0.11.8`;
- `diagnostic_verbosity` — retained diagnostic detail without a runtime-skip mode.

There is intentionally no mode that can skip real Endstone execution and still publish the final artifact.

## Artifact overlay

The final artifact root mirrors MediaPlayer:

```text
include/abi/windows_x86_64.h
include/abi/linux_x86_64.h
abi-evidence/requirements.json
abi-evidence/windows-runtime.json
abi-evidence/linux-runtime.json
abi-evidence/manifest.json
abi-evidence/coverage.json
abi-evidence/README.md
```

After downloading and extracting `endstone-mediaplayer-abi`, overlay it onto an isolated MediaPlayer checkout:

```powershell
Copy-Item -Recurse -Force .\include\abi\* C:\path\to\endstone-mediaplayer\include\abi\
```

`abi-evidence/manifest.json` records the workflow, MediaPlayer, Endstone, BDS, compiler/stdlib, coverage/provenance, consumer-gate, and SHA-256 identities. Run `python tools/validate_artifact.py --root <extracted-root>` before use.

## Adding a new ABI requirement

Add the new `ES_*` use to MediaPlayer and declare it in the appropriate platform ABI header interface. The next scan will promote it into the current contract. Add a value-free measurement strategy to `src/registry.cpp` and an exact probe expression in the appropriate probe component. Add tooling fixtures if the syntax/category is new. CI must fail until both applicable platforms independently resolve the requirement and the generated-header consumer gates pass.

Static analysis remains an exception path: document why runtime object, loaded probe, runtime derivation, and matching compile measurement cannot provide the fact before accepting `STATIC_VERIFIED`.
