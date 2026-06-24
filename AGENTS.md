# KeyWeaver Agent Maintenance Guide

This repository is a C++20/CMake osu!mania and BMS-family key-count converter. Treat this file as the first local instruction source for future Codex/agent maintenance work in this workspace.

## Operating Rules

- Inspect the current state before editing: `git status --short`, relevant source files, and recent docs.
- Keep changes narrowly scoped. Do not refactor unrelated converter, GUI, release, or documentation code while fixing a focused issue.
- Preserve deterministic converter behavior unless the user explicitly asks for an experimental path.
- Ask before deleting artifacts, overwriting release zips, pushing to GitHub, publishing packages, starting long runs, or installing/downloading dependencies.
- If the user says `ㄱㄱ`, `자율`, `킵고잉`, `계속 진행`, or `알아서 해봐`, proceed autonomously on low-risk local implementation and verification. Ask only for risky actions.
- Do not treat generated release folders, smoke outputs, or old build folders as authoritative source. Source of truth is the tracked source/docs plus the current user request.

## Build And Test

Default local build:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful targeted checks:

```powershell
cmake --build build --target KeyWeaver keyconv_tests keyconv_public_header_smoke
build\KeyWeaver.exe --help
build\KeyWeaver.exe samples\simple_4k.osu --target 10 --dry-run
build\KeyWeaver.exe samples\simple_4k.osu samples\simple_7k_ln.osu --target 10 --batch --jobs 2 --batch-quiet --dry-run
```

Release packaging is a stronger gate and may overwrite/create large artifacts, so run it only when the user asks:

```powershell
.\scripts\package_release.ps1 -Version <version>
```

## Algorithm Boundaries

- Classic conversion flows through parser -> `keyconv::Converter` -> core assignment/expansion/compression/safety/report -> exporter.
- NK2 is experimental. Keep classic and NK2 behavior separate unless the task explicitly bridges them.
- BMS-family input must remain BMS-family output. Do not add BMS-to-osu output silently.
- Reconverted charts with KeyWeaver/key-conversion markers should be skipped or guarded, not converted again.
- Safety invariants matter more than style changes: no same-time same-lane collisions, no unsafe LN overlap, no avoidable source-different created jacks, and no hidden nondeterminism in default paths.

## ONNX Runtime Lane Policy

- ONNX Runtime support is experimental and optional. Default builds must continue to work without ONNX Runtime installed.
- Keep ONNX integration behind `KEYWEAVER_WITH_ONNXRUNTIME` and `--onnx-policy`; do not make it a required core dependency.
- ONNX policy is batch-only. Single-chart conversions must stay rule-based unless the user explicitly reopens this boundary.
- `--onnx-provider auto|cpu|cuda|dml` selects the ONNX Runtime execution provider. `auto` should prefer GPU providers exposed by the loaded ORT package, then fall back to CPU in non-strict mode.
- The NuGet `Microsoft.ML.OnnxRuntime` package root is a valid `ONNXRUNTIME_ROOT`; it keeps headers under `build/native/include` and Windows native files under `runtimes/win-x64/native`.
- GPU execution requires a CUDA or DirectML ONNX Runtime package/provider DLLs, not just the CPU package.
- On Windows, make sure the matching `onnxruntime.dll` and provider DLLs such as `onnxruntime_providers*.dll` / `DirectML.dll` are beside the smoke executable or copied by CMake. `C:\Windows\System32\onnxruntime.dll` may be older and can cause API-version mismatch failures.
- The ONNX model is advisory only. It may suggest target lanes, but existing safety checks must reject predictions that would create collisions, LN conflicts, or unsafe repeats.
- `--onnx-policy-strict` is for validation. Non-strict mode should fall back to deterministic placement when ONNX Runtime is unavailable or a model fails.
- Do not train, download, or install model/runtime assets without user approval. Preparing code, docs, and local smoke tests is fine.

## Documentation

- Update README/CHANGELOG when adding user-facing CLI options, build flags, reports, GUI behavior, or release/package behavior.
- Update algorithm docs when changing conversion defaults, generated-note placement, lane scoring, jack/LN handling, NK2 behavior, or safety contracts.
- Keep reports explicit about verified, fallback, skipped, and failed states.

## Final Reports

Keep final user reports short and factual:

- changed files or feature area
- verification commands and results
- remaining risks or unverified paths

When a command fails because of the environment, distinguish that from a product failure.
