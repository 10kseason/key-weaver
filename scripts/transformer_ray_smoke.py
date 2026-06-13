#!/usr/bin/env python3
"""Generate one osu!mania smoke conversion with a tiny untrained Transformer policy.

This is not a trained quality model. It is a local pipeline smoke that lets us
inspect whether Transformer-shaped lane logits can produce a safe chart artifact.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import torch
from torch import nn


DEFAULT_RAY = os.environ.get("KEYWEAVER_RAY_INPUT", "")


@dataclass
class Note:
    index: int
    time: int
    source_lane: int
    lane: int
    type_value: int
    hold: bool
    end_time: int | None


def clamp(value: int, low: int, high: int) -> int:
    return max(low, min(value, high))


def x_to_lane(x: int, key_count: int) -> int:
    if key_count <= 1:
        return 0
    return clamp(int(math.floor(float(x) * key_count / 512.0)), 0, key_count - 1)


def lane_to_x(lane: int, key_count: int) -> int:
    if key_count <= 1:
        return 256
    return clamp(int(math.floor((float(clamp(lane, 0, key_count - 1)) + 0.5) * 512.0 / key_count)), 0, 511)


def direct_lane(source_lane: int, source_keys: int, target_keys: int) -> int:
    if source_keys <= 1 or target_keys <= 1:
        return 0
    mapped = float(source_lane) * float(target_keys - 1) / float(source_keys - 1)
    return clamp(int(round(mapped)), 0, target_keys - 1)


def split_sections(lines: list[str]) -> tuple[list[str], list[tuple[str, list[str]]]]:
    preamble: list[str] = []
    sections: list[tuple[str, list[str]]] = []
    current: tuple[str, list[str]] | None = None
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]") and len(stripped) >= 3:
            current = (stripped[1:-1], [])
            sections.append(current)
        elif current is None:
            preamble.append(line)
        else:
            current[1].append(line)
    return preamble, sections


def read_key(lines: Iterable[str], key: str) -> str | None:
    prefix = key + ":"
    for line in lines:
        if line.startswith(prefix):
            return line[len(prefix) :].strip()
    return None


def parse_int(value: str | None, fallback: int) -> int:
    if value is None:
        return fallback
    try:
        return int(round(float(value.strip())))
    except ValueError:
        return fallback


def parse_notes(hit_lines: Iterable[str], source_keys: int) -> list[Note]:
    notes: list[Note] = []
    for line in hit_lines:
        text = line.strip()
        if not text or text.startswith("//"):
            continue
        parts = text.split(",")
        if len(parts) < 5:
            continue
        try:
            x = int(round(float(parts[0])))
            time = int(round(float(parts[2])))
            type_value = int(round(float(parts[3])))
        except ValueError:
            continue
        hold = (type_value & 128) != 0
        end_time = None
        if hold and len(parts) >= 6:
            try:
                end_time = int(round(float(parts[5].split(":", 1)[0])))
            except ValueError:
                end_time = time
        source_lane = x_to_lane(x, source_keys)
        notes.append(
            Note(
                index=len(notes),
                time=time,
                source_lane=source_lane,
                lane=direct_lane(source_lane, source_keys, source_keys),
                type_value=type_value,
                hold=hold,
                end_time=end_time,
            )
        )
    notes.sort(key=lambda note: (note.time, note.source_lane, note.index))
    return notes


def chord_sizes(notes: list[Note]) -> dict[int, int]:
    sizes: dict[int, int] = {}
    for note in notes:
        sizes[note.time] = sizes.get(note.time, 0) + 1
    return sizes


def active_holds_at(notes: list[Note], time: int) -> int:
    return sum(1 for note in notes if note.hold and note.end_time is not None and note.time < time <= note.end_time)


def build_features(notes: list[Note], source_keys: int, target_keys: int) -> torch.Tensor:
    if not notes:
        return torch.zeros((0, 12), dtype=torch.float32)
    sizes = chord_sizes(notes)
    min_time = min(note.time for note in notes)
    max_time = max((note.end_time or note.time) for note in notes)
    duration = max(1, max_time - min_time)
    rows: list[list[float]] = []
    for i, note in enumerate(notes):
        previous_gap = 2000 if i == 0 else max(0, note.time - notes[i - 1].time)
        next_gap = 2000 if i + 1 >= len(notes) else max(0, notes[i + 1].time - note.time)
        hold_duration = 0 if note.end_time is None else max(0, note.end_time - note.time)
        rows.append(
            [
                note.source_lane / max(1, source_keys - 1),
                direct_lane(note.source_lane, source_keys, target_keys) / max(1, target_keys - 1),
                source_keys / 32.0,
                target_keys / 32.0,
                (note.time - min_time) / duration,
                min(1.0, previous_gap / 2000.0),
                min(1.0, next_gap / 2000.0),
                min(1.0, sizes.get(note.time, 1) / max(1, source_keys)),
                1.0 if note.hold else 0.0,
                min(1.0, hold_duration / 4000.0),
                0.0 if note.source_lane < source_keys / 2 else 1.0,
                min(1.0, active_holds_at(notes, note.time) / max(1, source_keys)),
            ]
        )
    return torch.tensor(rows, dtype=torch.float32)


class TinyTransformerPolicy(nn.Module):
    def __init__(
        self,
        target_keys: int,
        feature_count: int = 12,
        d_model: int = 32,
        nhead: int = 4,
        dim_feedforward: int = 64,
        num_layers: int = 2,
        dropout: float = 0.0,
    ) -> None:
        super().__init__()
        if d_model <= 0:
            raise ValueError("d_model must be positive")
        if nhead <= 0:
            raise ValueError("nhead must be positive")
        if d_model % nhead != 0:
            raise ValueError("d_model must be divisible by nhead")
        if dim_feedforward <= 0:
            raise ValueError("dim_feedforward must be positive")
        if num_layers <= 0:
            raise ValueError("num_layers must be positive")
        self.target_keys = target_keys
        self.feature_count = feature_count
        self.d_model = d_model
        self.nhead = nhead
        self.dim_feedforward = dim_feedforward
        self.num_layers = num_layers
        self.dropout = dropout
        self.input = nn.Linear(feature_count, d_model)
        layer = nn.TransformerEncoderLayer(
            d_model=d_model,
            nhead=nhead,
            dim_feedforward=dim_feedforward,
            dropout=dropout,
            batch_first=True,
            activation="gelu",
        )
        self.encoder = nn.TransformerEncoder(layer, num_layers=num_layers)
        self.head = nn.Linear(d_model, target_keys)

    def config(self) -> dict[str, int | float]:
        return {
            "targetKeys": self.target_keys,
            "featureCount": self.feature_count,
            "dModel": self.d_model,
            "nhead": self.nhead,
            "dimFeedforward": self.dim_feedforward,
            "numLayers": self.num_layers,
            "dropout": self.dropout,
        }

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        if features.numel() == 0:
            return torch.zeros((0, self.head.out_features), dtype=features.dtype)
        x = self.input(features)
        positions = torch.arange(features.shape[0], dtype=features.dtype, device=features.device).unsqueeze(1)
        div = torch.exp(
            torch.arange(0, x.shape[-1], 2, dtype=features.dtype, device=features.device)
            * (-math.log(10000.0) / x.shape[-1])
        )
        pos = torch.zeros_like(x)
        pos[:, 0::2] = torch.sin(positions * div)
        pos[:, 1::2] = torch.cos(positions * div[: pos[:, 1::2].shape[1]])
        return self.head(self.encoder((x + pos).unsqueeze(0)).squeeze(0))


def unsafe_same_time(placed: list[Note], time: int, lane: int) -> bool:
    return any(note.time == time and note.lane == lane for note in placed)


def unsafe_ln(placed: list[Note], moving: Note, lane: int) -> bool:
    for note in placed:
        if note.lane != lane:
            continue
        if note.hold and note.end_time is not None and note.time < moving.time <= note.end_time:
            return True
        if moving.hold and moving.end_time is not None:
            existing_starts_inside = moving.time < note.time <= moving.end_time
            hold_overlap = note.hold and note.end_time is not None and moving.time <= note.end_time and moving.end_time >= note.time
            if existing_starts_inside or hold_overlap:
                return True
    return False


def unsafe_created_jack(placed: list[Note], moving: Note, lane: int, window_ms: int = 500) -> bool:
    for note in reversed(placed):
        delta = moving.time - note.time
        if delta <= 0:
            continue
        if delta > window_ms:
            break
        if note.lane == lane and note.source_lane != moving.source_lane:
            return True
    return False


def choose_lanes(
    notes: list[Note],
    logits: torch.Tensor,
    source_keys: int,
    target_keys: int,
    transformer_weight: float,
    direct_score_weight: float = 1.25,
    coverage_weight: float = 0.55,
    inserted_lane_bonus: float = 0.0,
) -> dict[str, int | list[int]]:
    placed: list[Note] = []
    lane_use = [0 for _ in range(target_keys)]
    direct_lanes = {direct_lane(source_lane, source_keys, target_keys) for source_lane in range(source_keys)}
    stats = {
        "accepted": 0,
        "rejectedSameTime": 0,
        "rejectedLn": 0,
        "rejectedCreatedJack": 0,
        "directFallbacks": 0,
        "nonDirectPlacements": 0,
        "insertedLanePlacements": 0,
    }
    previous_single_source: int | None = None
    previous_single_target: int | None = None

    for i, note in enumerate(notes):
        direct = direct_lane(note.source_lane, source_keys, target_keys)
        model = logits[i].detach().float()
        model = (model - model.mean()) / (model.std(unbiased=False) + 1e-6)
        scores: list[tuple[float, int]] = []
        for lane in range(target_keys):
            coverage_need = 1.0 - (lane_use[lane] + 1.0) / ((sum(lane_use) + target_keys) / target_keys + 1.0)
            direct_score = max(0.0, 1.0 - abs(lane - direct) / 4.0)
            panel_score = 0.12 if (note.source_lane < source_keys / 2) == (lane < target_keys / 2) else -0.08
            inserted_score = inserted_lane_bonus if lane not in direct_lanes else 0.0
            continuity = 0.0
            if previous_single_source is not None and previous_single_target is not None:
                source_delta = note.source_lane - previous_single_source
                target_delta = lane - previous_single_target
                if source_delta and target_delta and (source_delta > 0) == (target_delta > 0):
                    continuity = 0.20
                elif source_delta:
                    continuity = -0.25
            score = (
                transformer_weight * float(model[lane])
                + direct_score_weight * direct_score
                + coverage_weight * coverage_need
                + panel_score
                + continuity
                + inserted_score
            )
            scores.append((score, lane))
        scores.sort(key=lambda item: (-item[0], item[1]))

        chosen: int | None = None
        for _, lane in scores:
            if unsafe_same_time(placed, note.time, lane):
                stats["rejectedSameTime"] += 1
                continue
            if unsafe_ln(placed, note, lane):
                stats["rejectedLn"] += 1
                continue
            if unsafe_created_jack(placed, note, lane):
                stats["rejectedCreatedJack"] += 1
                continue
            chosen = lane
            break

        if chosen is None:
            stats["directFallbacks"] += 1
            chosen = direct
        note.lane = chosen
        if chosen != direct:
            stats["nonDirectPlacements"] += 1
        if chosen not in direct_lanes:
            stats["insertedLanePlacements"] += 1
        lane_use[chosen] += 1
        placed.append(note)
        stats["accepted"] += 1
        previous_single_source = note.source_lane
        previous_single_target = note.lane

    stats["laneDistribution"] = lane_use
    return stats


def count_final_conflicts(notes: list[Note]) -> dict[str, int]:
    same_time = 0
    ln = 0
    created_jacks = 0
    for i, note in enumerate(notes):
        for other in notes[:i]:
            if other.time == note.time and other.lane == note.lane:
                same_time += 1
            if other.lane == note.lane:
                if other.hold and other.end_time is not None and other.time < note.time <= other.end_time:
                    ln += 1
                if note.hold and note.end_time is not None and note.time < other.time <= note.end_time:
                    ln += 1
        for other in reversed(notes[:i]):
            delta = note.time - other.time
            if delta <= 0:
                continue
            if delta > 500:
                break
            if other.lane == note.lane and other.source_lane != note.source_lane:
                created_jacks += 1
    return {"sameTimeCollisions": same_time, "lnConflicts": ln, "createdJacks": created_jacks}


def lane_time_is_safe_against_all(notes: list[Note], moving_index: int, lane: int, time_offset: int) -> bool:
    moving = notes[moving_index]
    original_lane = moving.lane
    original_time = moving.time
    original_end_time = moving.end_time
    moving.lane = lane
    moving.time = max(0, moving.time + time_offset)
    if moving.end_time is not None:
        moving.end_time = max(moving.time, moving.end_time + time_offset)
    try:
        for index, note in enumerate(notes):
            if index == moving_index or note.lane != lane:
                continue
            if note.time == moving.time:
                return False
            if note.hold and note.end_time is not None and note.time < moving.time <= note.end_time:
                return False
            if moving.hold and moving.end_time is not None and moving.time < note.time <= moving.end_time:
                return False
            if abs(note.time - moving.time) <= 500 and note.source_lane != moving.source_lane:
                return False
        return True
    finally:
        moving.lane = original_lane
        moving.time = original_time
        moving.end_time = original_end_time


def lane_is_safe_against_all(notes: list[Note], moving_index: int, lane: int) -> bool:
    return lane_time_is_safe_against_all(notes, moving_index, lane, 0)


def repair_created_jacks(notes: list[Note], target_keys: int) -> dict[str, int]:
    repaired = 0
    rolled = 0
    unresolved = 0
    ordered = sorted(range(len(notes)), key=lambda index: (notes[index].time, notes[index].lane, notes[index].index))
    lane_use = [0 for _ in range(target_keys)]
    for note in notes:
        if 0 <= note.lane < target_keys:
            lane_use[note.lane] += 1

    for moving_index in ordered:
        moving = notes[moving_index]
        conflict = False
        for other_index in ordered:
            if other_index == moving_index:
                break
            other = notes[other_index]
            delta = moving.time - other.time
            if delta <= 0:
                continue
            if delta > 500:
                continue
            if other.lane == moving.lane and other.source_lane != moving.source_lane:
                conflict = True
                break
        if not conflict:
            continue

        candidates = sorted(
            range(target_keys),
            key=lambda lane: (
                abs(lane - moving.lane),
                lane_use[lane],
                lane,
            ),
        )
        for lane in candidates:
            if lane == moving.lane:
                continue
            if lane_is_safe_against_all(notes, moving_index, lane):
                if 0 <= moving.lane < target_keys:
                    lane_use[moving.lane] -= 1
                moving.lane = lane
                lane_use[lane] += 1
                repaired += 1
                break
        else:
            roll_offsets = [-16, 16, -32, 32, -48, 48, -64, 64]
            moved = False
            for offset in roll_offsets:
                for lane in candidates:
                    if lane_time_is_safe_against_all(notes, moving_index, lane, offset):
                        if 0 <= moving.lane < target_keys:
                            lane_use[moving.lane] -= 1
                        moving.lane = lane
                        moving.time = max(0, moving.time + offset)
                        if moving.end_time is not None:
                            moving.end_time = max(moving.time, moving.end_time + offset)
                        lane_use[lane] += 1
                        rolled += 1
                        moved = True
                        break
                if moved:
                    break
            if not moved:
                unresolved += 1
    return {
        "createdJackRelaneRepairs": repaired,
        "createdJackRollRepairs": rolled,
        "createdJackUnresolved": unresolved,
    }


def render_hitobjects(notes: list[Note], target_keys: int) -> list[str]:
    lines: list[str] = []
    for note in sorted(notes, key=lambda item: (item.time, item.lane, item.index)):
        x = lane_to_x(note.lane, target_keys)
        if note.hold:
            end = note.end_time if note.end_time is not None else note.time
            lines.append(f"{x},192,{note.time},128,0,{end}:0:0:0:0:")
        else:
            lines.append(f"{x},192,{note.time},1,0,0:0:0:0:")
    return lines


def export_osu(
    preamble: list[str],
    sections: list[tuple[str, list[str]]],
    notes: list[Note],
    target_keys: int,
    version_suffix: str,
) -> str:
    out: list[str] = []
    out.extend(preamble)
    if out and out[-1] != "":
        out.append("")
    written: set[str] = set()
    for section, lines in sections:
        if section in written:
            continue
        written.add(section)
        out.append(f"[{section}]")
        if section == "Metadata":
            wrote_version = False
            for line in lines:
                if line.startswith("Version:"):
                    out.append(f"Version:{line[len('Version:'):].strip()} {version_suffix}")
                    wrote_version = True
                else:
                    out.append(line)
            if not wrote_version:
                out.append(f"Version:{version_suffix}")
        elif section == "Difficulty":
            wrote_cs = False
            for line in lines:
                if line.startswith("CircleSize:"):
                    out.append(f"CircleSize:{target_keys}")
                    wrote_cs = True
                else:
                    out.append(line)
            if not wrote_cs:
                out.append(f"CircleSize:{target_keys}")
        elif section == "HitObjects":
            out.extend(render_hitobjects(notes, target_keys))
        else:
            out.extend(lines)
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def unique_path(path: Path) -> Path:
    if not path.exists():
        return path
    for index in range(2, 1000):
        candidate = path.with_name(f"{path.stem}_{index}{path.suffix}")
        if not candidate.exists():
            return candidate
    raise RuntimeError(f"Could not find unused output path near {path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", default=DEFAULT_RAY)
    parser.add_argument("--output-dir", default="dist/transformer-smoke")
    parser.add_argument("--source-keys", type=int, default=0)
    parser.add_argument("--target-keys", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20260609)
    parser.add_argument("--transformer-weight", type=float, default=0.28)
    args = parser.parse_args()

    if not args.input:
        raise SystemExit("Set --input or KEYWEAVER_RAY_INPUT to a local osu!mania reference chart.")

    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    text = input_path.read_text(encoding="utf-8-sig", errors="replace")
    lines = text.splitlines()
    preamble, sections = split_sections(lines)
    section_map = {name: body for name, body in sections}
    source_keys = args.source_keys or parse_int(read_key(section_map.get("Difficulty", []), "CircleSize"), 7)
    notes = parse_notes(section_map.get("HitObjects", []), source_keys)

    torch.manual_seed(args.seed)
    torch.set_num_threads(1)
    model = TinyTransformerPolicy(args.target_keys)
    model.eval()
    with torch.no_grad():
        logits = model(build_features(notes, source_keys, args.target_keys))

    policy_stats = choose_lanes(notes, logits, source_keys, args.target_keys, args.transformer_weight)
    repair_stats = repair_created_jacks(notes, args.target_keys)
    final_stats = count_final_conflicts(notes)
    output_path = unique_path(output_dir / "ray_transformer_smoke_10k.osu")
    report_path = output_path.with_suffix(".report.json")
    output_text = export_osu(
        preamble,
        sections,
        notes,
        args.target_keys,
        "KeyWeaverTransformerSmoke-10K",
    )
    output_path.write_text(output_text, encoding="utf-8", newline="\n")

    report = {
        "input": str(input_path),
        "output": str(output_path),
        "sourceKeys": source_keys,
        "targetKeys": args.target_keys,
        "notes": len(notes),
        "seed": args.seed,
        "transformerWeight": args.transformer_weight,
        "model": {
            "type": "TinyTransformerPolicy",
            "trained": False,
            "note": "Untrained local smoke model; logits are blended with deterministic safety heuristics.",
        },
        "policy": policy_stats,
        "repair": repair_stats,
        "finalSafety": final_stats,
    }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
