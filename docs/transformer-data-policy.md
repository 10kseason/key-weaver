# Transformer Data Policy

This document defines the data and permission boundary for future KeyWeaver
Transformer or ONNX model work. It is an engineering policy for this repository,
not legal advice.

## Scope

Transformer work may use osu!mania `.osu` chart structure as training signal:
hit-object timing, lane, hold duration, source key count, target key count,
metadata needed for filtering, and derived aggregate features.

Transformer work must not train on or package beatmap-set assets such as audio,
background images, videos, storyboards, skin elements, or keysound samples unless
that asset class has its own explicit permission path. The default approved
model input is chart-only note data.

## Allowed Training Sources

Use opt-in sources only. A chart can enter a training set when all of these are
true:

- The mapper or rights holder has granted permission for model training.
- The permission covers the intended use: local research, model distribution,
  generated or converted chart output, public release, and commercial use if any
  of those apply.
- The chart has no unresolved co-mapper, guest difficulty, storyboard, sample,
  or package-level asset dependency that would add another rights holder to the
  training data.
- The chart is not a converted chart, reference-pack conversion, or output from
  KeyWeaver or another converter unless the purpose is a separate converter
  regression test.

The current intended seed corpus is restricted to u_e-owned charts and
CircusGalop charts when CircusGalop has explicitly opted in for the relevant
scope. Expanding beyond that set requires updating this document or adding a
dataset manifest entry that records the new permission boundary.

A local osu! Songs metadata scan on 2026-06-10 found 734 u_e or CircusGalop
osu!mania 10K charts before converter exclusions. The current
exclusion rules removed 102 converted or converter-tagged charts, leaving 632
eligible unique chart files: 155 matched u_e and 477 matched CircusGalop, with no
overlap. This count is a local corpus snapshot, not a license grant.

A follow-up CircusGalop metadata scan found 528 clean CircusGalop-authored
osu!mania charts across all key counts after the same converter exclusions:
1K=1, 4K=18, 5K=15, 7K=16, 10K=477, and 18K=1. The 4K/5K/7K subsets are useful
for source-key diversity when training a model that learns 4K/5K/7K -> 10K
placement behavior. The 1K and 18K subsets are too sparse for a default training
target but can stay visible as audit data. The scan also found 50 CircusGalop 8K
charts before exclusions, but they were all filtered as converted or
converter-tagged and should stay out of the clean training set.

## Dataset Manifest

Every reusable training run should produce a sanitized manifest. It should keep
enough information to audit the corpus without publishing private machine paths
or private messages:

- chart identifier or relative source reference
- mapper name or permission group
- source key count and target key count
- whether the source is original-authored, explicitly permitted, or excluded
- permission scope summary
- content hash or other reproducibility marker when practical
- exclusion reason for rejected candidates

Do not commit private DM screenshots, email bodies, local Songs-folder paths, or
personal account tokens. Keep private permission evidence outside the repository,
and commit only a short manifest-safe summary.

## Model Packaging

Bundled models are disabled-by-default until they have a reproducible manifest,
documented permission scope, and a smoke test that proves the model path remains
advisory. The converter must continue to reject unsafe predictions that create
same-time same-lane collisions, active-LN conflicts, or unsafe source-different
same-lane repeats.

If a model is trained only for local research, do not package it in release zips
or advertise it as a supported model. If a model is packaged, include the
manifest summary and the limitations in release notes.

The GUI reserves the preset label `Transformer model (u_e X CircusGalop Chart
dataset model)` for this corpus. The preset expects an ONNX lane-policy file at
`models/u_e_circusgalop_chart_dataset_lane_policy.onnx` beside
`KeyWeaver.exe` or in the current working folder. When the file is installed,
GUI Classic fast batch selects it by default and runs it in strict mode so
missing ONNX Runtime support is visible; single-chart Convert and Matrix stay
rule-based.

Reusable local model builds should also keep the exporter sidecar JSON next to
the ONNX file. The sidecar records the checkpoint architecture, training report
summary, runtime input/output contract, and ONNX Runtime verification result.

## Out Of Scope

- Training on audio, background art, videos, storyboard data, or keysounds.
- Scraping arbitrary osu! beatmaps as training data without opt-in permission.
- Treating osu! availability as proof that model-training rights exist.
- Publishing original `.osu` training files as part of the model package.
- Using Transformer output to bypass existing KeyWeaver safety checks.
