#!/usr/bin/env python3
"""Export additive Ray variants from a trained Transformer lane policy.

Unlike the earlier relane-only variants, these variants add safe companion tap
notes on model-preferred lanes. This is intentionally an inspection tool, not a
release converter path.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path

import torch

from grid_transformer_lane_policy import load_model
from train_transformer_lane_policy import (
    DEFAULT_RAY,
    Note,
    build_features,
    choose_lanes,
    count_final_conflicts,
    direct_lane,
    export_osu,
    inserted_lane_set,
    load_chart_text,
    parse_int,
    parse_notes,
    read_key,
    repair_created_jacks,
    unique_path,
)


DEFAULT_VARIANTS = (
    ("add_light", 0.75, 0.65, 0.85, 0.18, 0.12, 7),
    ("add_medium", 0.95, 0.50, 0.95, 0.30, 0.22, 8),
    ("add_heavy", 1.15, 0.35, 1.10, 0.45, 0.34, 9),
    ("add_max", 1.25, 0.25, 1.20, 0.55, 0.45, 10),
)


def parse_variant(text: str) -> tuple[str, float, float, float, float, float, int]:
    parts = [part.strip() for part in text.split(",")]
    if len(parts) != 7:
        raise argparse.ArgumentTypeError(
            "variant format: name,transformer,direct,coverage,inserted_bonus,add_rate,max_chord"
        )
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", parts[0]).strip("_")
    if not name:
        raise argparse.ArgumentTypeError("variant name is empty")
    return name, float(parts[1]), float(parts[2]), float(parts[3]), float(parts[4]), float(parts[5]), int(parts[6])


def lane_counts(notes: list[Note], target_keys: int) -> list[int]:
    counts = [0 for _ in range(target_keys)]
    for note in notes:
        if 0 <= note.lane < target_keys:
            counts[note.lane] += 1
    return counts


def chord_counts(notes: list[Note]) -> dict[int, int]:
    counts: dict[int, int] = {}
    for note in notes:
        counts[note.time] = counts.get(note.time, 0) + 1
    return counts


def generated_note_is_safe(notes: list[Note], candidate: Note, jack_window_ms: int) -> bool:
    for note in notes:
        if note.lane != candidate.lane:
            continue
        if note.time == candidate.time:
            return False
        if note.hold and note.end_time is not None and note.time < candidate.time <= note.end_time:
            return False
        if abs(note.time - candidate.time) <= jack_window_ms and note.source_lane != candidate.source_lane:
            return False
    return True


def add_companion_notes(
    notes: list[Note],
    logits: torch.Tensor,
    source_keys: int,
    target_keys: int,
    add_rate: float,
    max_chord: int,
    transformer_weight: float,
    direct_score_weight: float,
    coverage_weight: float,
    inserted_lane_bonus: float,
    jack_window_ms: int,
) -> dict[str, object]:
    target_additions = max(0, int(round(len(notes) * add_rate)))
    if target_additions == 0:
        return {
            "targetAdditions": 0,
            "generatedNotes": 0,
            "skippedUnsafe": 0,
            "skippedChordFull": 0,
            "insertedGeneratedNotes": 0,
        }

    inserted_lanes = inserted_lane_set(source_keys, target_keys)
    current_lane_counts = lane_counts(notes, target_keys)
    current_chords = chord_counts(notes)
    total_placed = max(1, len(notes))
    candidates: list[tuple[float, int, int]] = []

    for index, note in enumerate(notes):
        direct = direct_lane(note.source_lane, source_keys, target_keys)
        model = logits[index].detach().float()
        model = (model - model.mean()) / (model.std(unbiased=False) + 1e-6)
        for lane in range(target_keys):
            if lane == note.lane:
                continue
            if lane == direct and lane not in inserted_lanes:
                continue
            direct_score = max(0.0, 1.0 - abs(lane - direct) / 4.0)
            coverage_need = 1.0 - (current_lane_counts[lane] + 1.0) / ((total_placed + target_keys) / target_keys + 1.0)
            inserted_score = inserted_lane_bonus if lane in inserted_lanes else 0.0
            chord_score = min(0.35, 0.08 * max(0, current_chords.get(note.time, 1) - 1))
            hold_head_score = 0.08 if note.hold else 0.0
            score = (
                transformer_weight * float(model[lane])
                + direct_score_weight * direct_score
                + coverage_weight * coverage_need
                + inserted_score
                + chord_score
                + hold_head_score
            )
            candidates.append((score, index, lane))

    candidates.sort(key=lambda item: (-item[0], notes[item[1]].time, item[2]))
    generated: list[Note] = []
    used_anchors: set[int] = set()
    skipped_unsafe = 0
    skipped_chord_full = 0
    inserted_generated = 0

    for _, note_index, lane in candidates:
        if len(generated) >= target_additions:
            break
        if note_index in used_anchors:
            continue
        source = notes[note_index]
        if current_chords.get(source.time, 0) >= max_chord:
            skipped_chord_full += 1
            continue
        candidate = Note(
            index=len(notes) + len(generated),
            time=source.time,
            source_lane=source.source_lane,
            lane=lane,
            type_value=1,
            hold=False,
            end_time=None,
        )
        if not generated_note_is_safe(notes + generated, candidate, jack_window_ms):
            skipped_unsafe += 1
            continue
        generated.append(candidate)
        used_anchors.add(note_index)
        current_lane_counts[lane] += 1
        current_chords[source.time] = current_chords.get(source.time, 0) + 1
        if lane in inserted_lanes:
            inserted_generated += 1

    notes.extend(generated)
    notes.sort(key=lambda note: (note.time, note.lane, note.index))
    return {
        "targetAdditions": target_additions,
        "generatedNotes": len(generated),
        "skippedUnsafe": skipped_unsafe,
        "skippedChordFull": skipped_chord_full,
        "insertedGeneratedNotes": inserted_generated,
        "finalNoteCount": len(notes),
        "laneDistributionAfterAdd": lane_counts(notes, target_keys),
    }


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
    add_rate: float,
    max_chord: int,
    jack_window_ms: int,
) -> dict[str, object]:
    preamble, sections, section_map = load_chart_text(ray_input)
    source_keys = source_keys or parse_int(read_key(section_map.get("Difficulty", []), "CircleSize"), 7)
    notes = parse_notes(section_map.get("HitObjects", []), source_keys)
    original_note_count = len(notes)
    features = build_features(notes, source_keys, target_keys)
    with torch.no_grad():
        logits = model(features)
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
    add_stats = add_companion_notes(
        notes,
        logits,
        source_keys,
        target_keys,
        add_rate,
        max_chord,
        transformer_weight,
        direct_score_weight,
        coverage_weight,
        inserted_lane_bonus,
        jack_window_ms,
    )
    repair_stats = repair_created_jacks(notes, target_keys)
    final_stats = count_final_conflicts(notes)
    output_path = unique_path(output_dir / f"ray_{name}.osu")
    output_text = export_osu(
        preamble,
        sections,
        notes,
        target_keys,
        f"KeyWeaverTransformerAddFlesh-{name}",
    )
    output_path.write_text(output_text, encoding="utf-8", newline="\n")
    return {
        "name": name,
        "input": str(ray_input),
        "output": str(output_path),
        "sourceKeys": source_keys,
        "targetKeys": target_keys,
        "originalNotes": original_note_count,
        "notes": len(notes),
        "knobs": {
            "transformerWeight": transformer_weight,
            "directScoreWeight": direct_score_weight,
            "coverageWeight": coverage_weight,
            "insertedLaneBonus": inserted_lane_bonus,
            "addRate": add_rate,
            "maxChord": max_chord,
        },
        "policy": policy_stats,
        "add": add_stats,
        "repair": repair_stats,
        "finalSafety": final_stats,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--train-report", default="dist/transformer-flesh-research-200/train_report.json")
    parser.add_argument("--ray-input", default=DEFAULT_RAY)
    parser.add_argument("--output-dir", default="dist/transformer-note-flesh-variants-200")
    parser.add_argument("--jack-window-ms", type=int, default=500)
    parser.add_argument("--variant", action="append", type=parse_variant, default=[])
    args = parser.parse_args()

    report = json.loads(Path(args.train_report).read_text(encoding="utf-8"))
    source_keys = int(report["sourceKeys"])
    target_keys = int(report["targetKeys"])
    checkpoint_path = Path(report["modelPath"])
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    torch.set_num_threads(1)
    model = load_model(checkpoint_path, target_keys)
    variants = args.variant or list(DEFAULT_VARIANTS)
    outputs = [
        export_variant(
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
            add_rate,
            max_chord,
            args.jack_window_ms,
        )
        for name, transformer_weight, direct_score_weight, coverage_weight, inserted_lane_bonus, add_rate, max_chord in variants
    ]

    report_out = {
        "sourceReport": str(Path(args.train_report)),
        "checkpoint": str(checkpoint_path),
        "jackWindowMs": args.jack_window_ms,
        "variants": outputs,
    }
    report_path = output_dir / "note_flesh_variants_report.json"
    report_path.write_text(json.dumps(report_out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report_out, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
