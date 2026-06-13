#!/usr/bin/env python3
"""Grid-search a trained KeyWeaver Transformer lane policy.

This script reuses a trained smoke checkpoint plus its validation manifest. It
does not retrain the model; it only sweeps placement knobs and exports one Ray
chart for the highest-ranked setting.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable

import torch

from train_transformer_lane_policy import (
    DEFAULT_RAY,
    TinyTransformerPolicy,
    TrainingChunk,
    apply_model_to_ray,
    build_features,
    choose_lanes,
    count_final_conflicts,
    evaluate_model,
    load_chart_text,
    parse_int,
    parse_notes,
    read_key,
    repair_created_jacks,
    training_pair_from_reference,
)


def parse_float_list(text: str) -> list[float]:
    values: list[float] = []
    for item in text.split(","):
        item = item.strip()
        if item:
            values.append(float(item))
    if not values:
        raise argparse.ArgumentTypeError("expected at least one comma-separated float")
    return values


def load_validation_chunks(
    manifest_path: Path,
    source_keys: int,
    target_keys: int,
    max_notes_per_chart: int,
    max_seq: int,
) -> tuple[list[dict[str, object]], list[TrainingChunk]]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    validation_entries = [entry for entry in manifest if entry.get("split") == "validation"]
    chunks: list[TrainingChunk] = []
    for entry in validation_entries:
        _, chart_chunks = training_pair_from_reference(
            Path(str(entry["path"])),
            source_keys,
            target_keys,
            max_notes_per_chart,
            max_seq,
        )
        chunks.extend(chart_chunks)
    return validation_entries, chunks


def load_model(checkpoint_path: Path, target_keys: int) -> TinyTransformerPolicy:
    checkpoint = torch.load(checkpoint_path, map_location="cpu")
    raw_config = checkpoint.get("model_config", {})
    model_config = {
        "feature_count": int(raw_config.get("feature_count", raw_config.get("featureCount", 12))),
        "d_model": int(raw_config.get("d_model", raw_config.get("dModel", 32))),
        "nhead": int(raw_config.get("nhead", 4)),
        "dim_feedforward": int(raw_config.get("dim_feedforward", raw_config.get("dimFeedforward", 64))),
        "num_layers": int(raw_config.get("num_layers", raw_config.get("numLayers", 2))),
        "dropout": float(raw_config.get("dropout", 0.0)),
    }
    model = TinyTransformerPolicy(target_keys, **model_config)
    model.load_state_dict(checkpoint["state_dict"])
    model.eval()
    return model


def ray_stats_for_combo(
    model: TinyTransformerPolicy,
    ray_input: Path,
    source_keys: int,
    target_keys: int,
    transformer_weight: float,
    direct_score_weight: float,
    coverage_weight: float,
    inserted_lane_bonus: float,
) -> dict[str, object]:
    _, _, section_map = load_chart_text(ray_input)
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
    return {
        "sourceKeys": source_keys,
        "targetKeys": target_keys,
        "notes": len(notes),
        "policy": policy_stats,
        "repair": repair_stats,
        "finalSafety": final_stats,
    }


def score_combo(validation: dict[str, float], ray: dict[str, object]) -> float:
    direct_accuracy = validation["directAccuracy"]
    hybrid_accuracy = validation["hybridAccuracy"]
    label_rate = validation["insertedLaneLabelRate"]
    use_rate = validation["hybridInsertedLaneUseRate"]
    recall = validation["hybridInsertedLaneRecall"]
    ray_policy = ray["policy"]  # type: ignore[index]
    ray_notes = max(1, int(ray["notes"]))  # type: ignore[arg-type]
    ray_inserted_rate = float(ray_policy["insertedLanePlacements"]) / ray_notes  # type: ignore[index]
    ray_non_direct_rate = float(ray_policy["nonDirectPlacements"]) / ray_notes  # type: ignore[index]
    safety = ray["finalSafety"]  # type: ignore[index]
    safety_penalty = (
        int(safety["sameTimeCollisions"])  # type: ignore[index]
        + int(safety["lnConflicts"])  # type: ignore[index]
        + int(safety["createdJacks"])  # type: ignore[index]
    )
    return (
        hybrid_accuracy
        - 0.60 * max(0.0, direct_accuracy - hybrid_accuracy)
        + 0.08 * recall
        - 0.18 * abs(use_rate - label_rate)
        - 0.10 * abs(ray_inserted_rate - label_rate)
        + 0.03 * min(ray_non_direct_rate, 0.45)
        - 100.0 * safety_penalty
    )


def iter_combos(
    transformer_weights: Iterable[float],
    direct_score_weights: Iterable[float],
    coverage_weights: Iterable[float],
    inserted_lane_bonuses: Iterable[float],
) -> Iterable[dict[str, float]]:
    for transformer_weight in transformer_weights:
        for direct_score_weight in direct_score_weights:
            for coverage_weight in coverage_weights:
                for inserted_lane_bonus in inserted_lane_bonuses:
                    yield {
                        "transformerWeight": transformer_weight,
                        "directScoreWeight": direct_score_weight,
                        "coverageWeight": coverage_weight,
                        "insertedLaneBonus": inserted_lane_bonus,
                    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--train-report", default="dist/transformer-flesh-research-200/train_report.json")
    parser.add_argument("--checkpoint", default="")
    parser.add_argument("--manifest", default="")
    parser.add_argument("--ray-input", default=DEFAULT_RAY)
    parser.add_argument("--output-dir", default="dist/transformer-grid-search-200")
    parser.add_argument("--transformer-weights", type=parse_float_list, default=parse_float_list("0.45,0.55,0.65,0.75"))
    parser.add_argument("--direct-score-weights", type=parse_float_list, default=parse_float_list("0.85,0.95,1.05,1.15"))
    parser.add_argument("--coverage-weights", type=parse_float_list, default=parse_float_list("0.35,0.55,0.75"))
    parser.add_argument("--inserted-lane-bonuses", type=parse_float_list, default=parse_float_list("0.00,0.06,0.12,0.18"))
    parser.add_argument("--top", type=int, default=12)
    args = parser.parse_args()

    report = json.loads(Path(args.train_report).read_text(encoding="utf-8"))
    source_keys = int(report["sourceKeys"])
    target_keys = int(report["targetKeys"])
    max_notes_per_chart = int(report["maxNotesPerChart"])
    max_seq = int(report["maxSeq"])
    checkpoint_path = Path(args.checkpoint or report["modelPath"])
    manifest_path = Path(args.manifest or report["manifestPath"])
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
    if not validation_chunks:
        raise SystemExit("No validation chunks found")

    rows: list[dict[str, object]] = []
    for combo in iter_combos(
        args.transformer_weights,
        args.direct_score_weights,
        args.coverage_weights,
        args.inserted_lane_bonuses,
    ):
        validation = evaluate_model(
            model,
            validation_chunks,
            source_keys,
            target_keys,
            combo["transformerWeight"],
            combo["directScoreWeight"],
            combo["insertedLaneBonus"],
        )
        ray = ray_stats_for_combo(
            model,
            Path(args.ray_input),
            source_keys,
            target_keys,
            combo["transformerWeight"],
            combo["directScoreWeight"],
            combo["coverageWeight"],
            combo["insertedLaneBonus"],
        )
        rows.append(
            {
                **combo,
                "score": score_combo(validation, ray),
                "validation": validation,
                "ray": {
                    "policy": ray["policy"],
                    "finalSafety": ray["finalSafety"],
                    "notes": ray["notes"],
                },
            }
        )

    rows.sort(key=lambda row: float(row["score"]), reverse=True)
    best = rows[0]
    best_ray = apply_model_to_ray(
        model,
        Path(args.ray_input),
        output_dir,
        source_keys,
        target_keys,
        float(best["transformerWeight"]),
        float(best["directScoreWeight"]),
        float(best["coverageWeight"]),
        float(best["insertedLaneBonus"]),
    )
    report_out = {
        "sourceReport": str(Path(args.train_report)),
        "checkpoint": str(checkpoint_path),
        "manifest": str(manifest_path),
        "validationCharts": len(validation_entries),
        "validationChunks": len(validation_chunks),
        "gridSize": len(rows),
        "bestRay": best_ray,
        "best": best,
        "top": rows[: args.top],
    }
    output_path = output_dir / "grid_report.json"
    output_path.write_text(json.dumps(report_out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report_out, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
