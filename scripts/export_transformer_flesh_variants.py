#!/usr/bin/env python3
"""Export Ray variants from a trained Transformer lane policy.

The variants intentionally sweep toward more model-driven, inserted-lane-heavy
placement so a human can judge whether the chart starts to feel like native 10K.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import torch

from grid_transformer_lane_policy import load_model, load_validation_chunks
from train_transformer_lane_policy import (
    DEFAULT_RAY,
    build_features,
    choose_lanes,
    count_final_conflicts,
    evaluate_model,
    export_osu,
    load_chart_text,
    parse_int,
    parse_notes,
    read_key,
    repair_created_jacks,
    unique_path,
)


DEFAULT_VARIANTS = (
    ("balanced_grid", 0.75, 0.85, 0.75, 0.00),
    ("flesh_plus", 0.75, 0.65, 0.85, 0.18),
    ("flesh_dense", 0.95, 0.50, 0.95, 0.30),
    ("flesh_max", 1.15, 0.35, 1.10, 0.45),
)


def parse_variant(text: str) -> tuple[str, float, float, float, float]:
    parts = [part.strip() for part in text.split(",")]
    if len(parts) != 5:
        raise argparse.ArgumentTypeError("variant format: name,transformer,direct,coverage,inserted_bonus")
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", parts[0]).strip("_")
    if not name:
        raise argparse.ArgumentTypeError("variant name is empty")
    return name, float(parts[1]), float(parts[2]), float(parts[3]), float(parts[4])


def export_variant(
    model: torch.nn.Module,
    ray_input: Path,
    output_dir: Path,
    source_keys: int,
    target_keys: int,
    name: str,
    transformer_weight: float,
    direct_score_weight: float,
    coverage_weight: float,
    inserted_lane_bonus: float,
) -> dict[str, object]:
    preamble, sections, section_map = load_chart_text(ray_input)
    source_keys = source_keys or parse_int(read_key(section_map.get("Difficulty", []), "CircleSize"), 7)
    notes = parse_notes(section_map.get("HitObjects", []), source_keys)
    with torch.no_grad():
        logits = model(build_features(notes, source_keys, target_keys))
    policy_stats = choose_lanes(
        notes,
        logits,
        source_keys,
        target_keys,
        transformer_weight,
        direct_score_weight=direct_score_weight,
        coverage_weight=coverage_weight,
        inserted_lane_bonus=inserted_lane_bonus,
    )
    repair_stats = repair_created_jacks(notes, target_keys)
    final_stats = count_final_conflicts(notes)
    output_path = unique_path(output_dir / f"ray_{name}.osu")
    output_text = export_osu(
        preamble,
        sections,
        notes,
        target_keys,
        f"KeyWeaverTransformerFlesh-{name}",
    )
    output_path.write_text(output_text, encoding="utf-8", newline="\n")
    note_count = max(1, len(notes))
    return {
        "name": name,
        "input": str(ray_input),
        "output": str(output_path),
        "sourceKeys": source_keys,
        "targetKeys": target_keys,
        "notes": len(notes),
        "knobs": {
            "transformerWeight": transformer_weight,
            "directScoreWeight": direct_score_weight,
            "coverageWeight": coverage_weight,
            "insertedLaneBonus": inserted_lane_bonus,
        },
        "policy": policy_stats,
        "rates": {
            "nonDirectPlacementRate": policy_stats["nonDirectPlacements"] / note_count,
            "insertedLanePlacementRate": policy_stats["insertedLanePlacements"] / note_count,
        },
        "repair": repair_stats,
        "finalSafety": final_stats,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--train-report", default="dist/transformer-flesh-research-200/train_report.json")
    parser.add_argument("--ray-input", default=DEFAULT_RAY)
    parser.add_argument("--output-dir", default="dist/transformer-flesh-variants-200")
    parser.add_argument("--variant", action="append", type=parse_variant, default=[])
    args = parser.parse_args()

    report = json.loads(Path(args.train_report).read_text(encoding="utf-8"))
    source_keys = int(report["sourceKeys"])
    target_keys = int(report["targetKeys"])
    max_notes_per_chart = int(report["maxNotesPerChart"])
    max_seq = int(report["maxSeq"])
    checkpoint_path = Path(report["modelPath"])
    manifest_path = Path(report["manifestPath"])
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    torch.set_num_threads(1)
    model = load_model(checkpoint_path, target_keys)
    validation_entries, validation_chunks = load_validation_chunks(
        manifest_path,
        source_keys,
        target_keys,
        max_notes_per_chart,
        max_seq,
    )
    variants = args.variant or list(DEFAULT_VARIANTS)
    outputs: list[dict[str, object]] = []
    for name, transformer_weight, direct_score_weight, coverage_weight, inserted_lane_bonus in variants:
        validation = evaluate_model(
            model,
            validation_chunks,
            source_keys,
            target_keys,
            transformer_weight,
            direct_score_weight,
            inserted_lane_bonus,
        )
        ray = export_variant(
            model,
            Path(args.ray_input),
            output_dir,
            source_keys,
            target_keys,
            name,
            transformer_weight,
            direct_score_weight,
            coverage_weight,
            inserted_lane_bonus,
        )
        outputs.append(
            {
                "name": name,
                "validation": validation,
                "ray": ray,
            }
        )

    report_out = {
        "sourceReport": str(Path(args.train_report)),
        "checkpoint": str(checkpoint_path),
        "manifest": str(manifest_path),
        "validationCharts": len(validation_entries),
        "validationChunks": len(validation_chunks),
        "variants": outputs,
    }
    report_path = output_dir / "flesh_variants_report.json"
    report_path.write_text(json.dumps(report_out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report_out, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
