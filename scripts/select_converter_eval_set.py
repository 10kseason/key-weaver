#!/usr/bin/env python3
"""Select a reproducible random KeyWeaver evaluation set from an osu! Songs tree."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


DEFAULT_EXCLUDE_TOKENS = (
    "7to10",
    "7 to 10",
    "7-to-10",
    "4k10c",
    "5k10c",
    "6k10c",
    "7k10c",
    "4to10c",
    "5to10c",
    "6to10c",
    "7to10c",
    "converted",
    "convert",
    "keyweaver",
    "nknc",
)

STYLE_EXCLUDE_PATTERNS = (
    r"\d+k\d+c",
    r"\d+to\d+c",
)


@dataclass(frozen=True)
class ChartStats:
    relativePath: str
    title: str
    artist: str
    version: str
    creator: str
    sourceKeys: int
    notes: int
    durationMs: int
    avgNps: float
    maxWindowNps: float
    chordRate: float
    maxChordSize: int
    holdRate: float
    laneCoverage: float
    fastJackRate: float
    minPositiveDeltaMs: int
    maxEmptyGapMs: int
    primaryTag: str


def lower_text(value: str | None) -> str:
    return (value or "").casefold()


def compact_key(value: str | None) -> str:
    return "".join(ch for ch in lower_text(value) if ch.isalnum())


def parse_int_list(raw: str) -> list[int]:
    values: list[int] = []
    for item in str(raw).split(","):
        token = item.strip().lower().removesuffix("k")
        if not token:
            continue
        values.append(int(token))
    if not values:
        raise argparse.ArgumentTypeError("expected at least one key count")
    return sorted(set(values))


def default_songs_root() -> str:
    return os.environ.get("KEYWEAVER_SONGS_ROOT") or os.environ.get("OSU_SONGS_ROOT") or ""


def iter_osu_paths(root: Path) -> Iterable[Path]:
    rg = shutil.which("rg")
    if rg is not None:
        try:
            completed = subprocess.run(
                [rg, "--files", str(root), "-g", "*.osu"],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding="utf-8",
                errors="replace",
            )
            for raw in completed.stdout.splitlines():
                yield Path(raw)
            return
        except (OSError, subprocess.CalledProcessError):
            pass

    for directory, _, filenames in os.walk(root):
        for filename in filenames:
            if filename.casefold().endswith(".osu"):
                yield Path(directory) / filename


def parse_meta(path: Path) -> dict[str, str]:
    meta: dict[str, str] = {}
    section: str | None = None
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                if section == "HitObjects":
                    break
                continue
            if section in ("General", "Metadata", "Difficulty") and ":" in line:
                key, value = line.split(":", 1)
                meta[key.strip()] = value.strip()
    return meta


def parse_notes(path: Path) -> list[tuple[int, int, bool, int]]:
    notes: list[tuple[int, int, bool, int]] = []
    section: str | None = None
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
            if section != "HitObjects" or "," not in line:
                continue
            parts = line.split(",")
            if len(parts) < 4:
                continue
            try:
                x = int(float(parts[0]))
                time = int(float(parts[2]))
                object_type = int(parts[3])
            except ValueError:
                continue
            is_hold = (object_type & 128) != 0
            end_time = time
            if is_hold and len(parts) >= 6:
                try:
                    end_time = int(float(parts[5].split(":", 1)[0]))
                except ValueError:
                    end_time = time
            notes.append((x, time, is_hold, end_time))
    return notes


def lane_from_x(x: int, keys: int) -> int:
    lane = int(math.floor(float(x) * float(keys) / 512.0))
    return max(0, min(keys - 1, lane))


def has_excluded_marker(path: Path, meta: dict[str, str], exclude_tokens: list[str]) -> bool:
    haystack = "\n".join(
        [
            str(path),
            meta.get("Title", ""),
            meta.get("Version", ""),
            meta.get("Creator", ""),
            meta.get("Tags", ""),
        ]
    )
    lower_haystack = lower_text(haystack)
    if any(lower_text(token) in lower_haystack for token in exclude_tokens):
        return True
    compact_haystack = compact_key(haystack)
    return any(re.search(pattern, compact_haystack) for pattern in STYLE_EXCLUDE_PATTERNS)


def relative_path(path: Path, root: Path) -> str:
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path)


def classify_primary_tag(stats: dict[str, float | int]) -> str:
    if float(stats["fastJackRate"]) >= 0.04:
        return "jack-risk"
    if float(stats["holdRate"]) >= 0.25:
        return "ln-heavy"
    if float(stats["chordRate"]) >= 0.35:
        return "chord-heavy"
    if float(stats["avgNps"]) >= 7.0 or float(stats["maxWindowNps"]) >= 18.0:
        return "dense"
    return "normal"


def compute_stats(path: Path, root: Path, meta: dict[str, str], source_keys: int, window_ms: int, fast_jack_ms: int) -> ChartStats | None:
    notes = parse_notes(path)
    if not notes:
        return None

    lane_notes = sorted(
        ((lane_from_x(x, source_keys), time, is_hold, end_time) for x, time, is_hold, end_time in notes),
        key=lambda item: (item[1], item[0], item[3]),
    )
    times = [time for _, time, _, _ in lane_notes]
    min_time = min(times)
    max_time = max(max(time, end_time) for _, time, _, end_time in lane_notes)
    duration_ms = max(1, max_time - min_time)

    distribution = [0] * source_keys
    by_time: dict[int, list[int]] = defaultdict(list)
    hold_count = 0
    for lane, time, is_hold, _ in lane_notes:
        distribution[lane] += 1
        by_time[time].append(lane)
        if is_hold:
            hold_count += 1

    window_counts: Counter[int] = Counter()
    for _, time, _, _ in lane_notes:
        window_counts[(time - min_time) // max(1, window_ms)] += 1

    fast_jacks = 0
    lane_last_time: dict[int, int] = {}
    positive_deltas: list[int] = []
    for lane, time, _, _ in lane_notes:
        previous = lane_last_time.get(lane)
        if previous is not None:
            delta = time - previous
            if delta > 0:
                positive_deltas.append(delta)
            if 0 < delta <= fast_jack_ms:
                fast_jacks += 1
        lane_last_time[lane] = time

    all_positive_gaps = [
        times[index] - times[index - 1]
        for index in range(1, len(times))
        if times[index] - times[index - 1] > 0
    ]
    chord_slices = [lanes for lanes in by_time.values() if len(lanes) >= 2]
    metric_values: dict[str, float | int] = {
        "avgNps": len(lane_notes) * 1000.0 / duration_ms,
        "maxWindowNps": max(window_counts.values(), default=0) * 1000.0 / max(1, window_ms),
        "chordRate": len(chord_slices) / len(by_time) if by_time else 0.0,
        "maxChordSize": max((len(lanes) for lanes in by_time.values()), default=0),
        "holdRate": hold_count / len(lane_notes),
        "laneCoverage": sum(1 for count in distribution if count > 0) / source_keys,
        "fastJackRate": fast_jacks / len(lane_notes),
    }
    return ChartStats(
        relativePath=relative_path(path, root),
        title=meta.get("Title", ""),
        artist=meta.get("Artist", ""),
        version=meta.get("Version", ""),
        creator=meta.get("Creator", ""),
        sourceKeys=source_keys,
        notes=len(lane_notes),
        durationMs=int(duration_ms),
        avgNps=round(float(metric_values["avgNps"]), 3),
        maxWindowNps=round(float(metric_values["maxWindowNps"]), 3),
        chordRate=round(float(metric_values["chordRate"]), 4),
        maxChordSize=int(metric_values["maxChordSize"]),
        holdRate=round(float(metric_values["holdRate"]), 4),
        laneCoverage=round(float(metric_values["laneCoverage"]), 4),
        fastJackRate=round(float(metric_values["fastJackRate"]), 4),
        minPositiveDeltaMs=min(positive_deltas) if positive_deltas else 0,
        maxEmptyGapMs=max(all_positive_gaps) if all_positive_gaps else 0,
        primaryTag=classify_primary_tag(metric_values),
    )


def rejection_reason(args: argparse.Namespace, stats: ChartStats) -> str | None:
    if stats.notes < args.min_notes:
        return "tooFewNotes"
    if stats.notes > args.max_notes:
        return "tooManyNotes"
    if stats.durationMs < args.min_duration_ms:
        return "tooShort"
    if stats.durationMs > args.max_duration_ms:
        return "tooLong"
    if stats.avgNps > args.max_avg_nps:
        return "avgNpsHigh"
    if stats.maxWindowNps > args.max_window_nps:
        return "windowNpsHigh"
    if stats.chordRate > args.max_chord_rate:
        return "chordRateHigh"
    if args.max_chord_size > 0 and stats.maxChordSize > args.max_chord_size:
        return "chordSizeHigh"
    if stats.holdRate > args.max_hold_rate:
        return "holdRateHigh"
    if stats.laneCoverage < args.min_lane_coverage:
        return "laneCoverageLow"
    if stats.fastJackRate > args.max_fast_jack_rate:
        return "fastJackRateHigh"
    if stats.maxEmptyGapMs > args.max_empty_gap_ms:
        return "emptyGapHigh"
    return None


def scan_candidates(args: argparse.Namespace, root: Path) -> tuple[list[ChartStats], Counter[str]]:
    candidates: list[ChartStats] = []
    rejected: Counter[str] = Counter()
    source_key_set = set(args.source_keys)
    exclude_tokens = list(DEFAULT_EXCLUDE_TOKENS) + list(args.exclude_token)

    for index, path in enumerate(iter_osu_paths(root), start=1):
        if args.scan_limit and index > args.scan_limit:
            break
        try:
            meta = parse_meta(path)
            if int(float(meta.get("Mode", "0"))) != 3:
                rejected["nonMania"] += 1
                continue
            source_keys = int(round(float(meta.get("CircleSize", "0"))))
        except (OSError, ValueError):
            rejected["metadataError"] += 1
            continue

        if source_keys not in source_key_set:
            rejected["sourceKeysOutsideSet"] += 1
            continue
        if not args.allow_converted and has_excluded_marker(path, meta, exclude_tokens):
            rejected["convertedOrExcludedMarker"] += 1
            continue

        try:
            stats = compute_stats(path, root, meta, source_keys, args.window_ms, args.fast_jack_ms)
        except OSError:
            rejected["readError"] += 1
            continue
        if stats is None:
            rejected["noNotes"] += 1
            continue

        reason = rejection_reason(args, stats)
        if reason:
            rejected[reason] += 1
            continue
        candidates.append(stats)
    return candidates, rejected


def pick_round_robin(items: list[ChartStats], count: int, rng: random.Random) -> list[ChartStats]:
    by_tag: dict[str, list[ChartStats]] = defaultdict(list)
    for item in items:
        by_tag[item.primaryTag].append(item)
    tags = list(by_tag)
    for tag_items in by_tag.values():
        rng.shuffle(tag_items)
    rng.shuffle(tags)

    selected: list[ChartStats] = []
    while len(selected) < count and any(by_tag.values()):
        for tag in list(tags):
            if by_tag[tag]:
                selected.append(by_tag[tag].pop())
                if len(selected) >= count:
                    break
        tags = [tag for tag in tags if by_tag[tag]]
    return selected


def select_eval_set(candidates: list[ChartStats], args: argparse.Namespace) -> list[ChartStats]:
    rng = random.Random(args.seed)
    by_key: dict[int, list[ChartStats]] = defaultdict(list)
    for candidate in candidates:
        by_key[candidate.sourceKeys].append(candidate)

    selected: list[ChartStats] = []
    for source_keys in args.source_keys:
        group = by_key.get(source_keys, [])
        rng.shuffle(group)
        if args.balance_tags:
            selected.extend(pick_round_robin(group, args.per_source_key, rng))
        else:
            selected.extend(group[: args.per_source_key])
    rng.shuffle(selected)
    return selected


def write_outputs(root: Path, out_dir: Path, selected: list[ChartStats], rejected: Counter[str], args: argparse.Namespace) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    records = [asdict(item) for item in selected]

    manifest = {
        "kind": "keyweaver-converter-eval-set",
        "createdAtUtc": datetime.now(timezone.utc).isoformat(),
        "songsRoot": "<redacted>",
        "sourceKeys": args.source_keys,
        "seed": args.seed,
        "perSourceKey": args.per_source_key,
        "selectedCount": len(selected),
        "selectedBySourceKey": dict(Counter(item.sourceKeys for item in selected)),
        "selectedByPrimaryTag": dict(Counter(item.primaryTag for item in selected)),
        "filters": {
            "minNotes": args.min_notes,
            "maxNotes": args.max_notes,
            "minDurationMs": args.min_duration_ms,
            "maxDurationMs": args.max_duration_ms,
            "maxAvgNps": args.max_avg_nps,
            "maxWindowNps": args.max_window_nps,
            "windowMs": args.window_ms,
            "maxChordRate": args.max_chord_rate,
            "maxChordSize": args.max_chord_size,
            "maxHoldRate": args.max_hold_rate,
            "minLaneCoverage": args.min_lane_coverage,
            "fastJackMs": args.fast_jack_ms,
            "maxFastJackRate": args.max_fast_jack_rate,
            "maxEmptyGapMs": args.max_empty_gap_ms,
            "excludeTokens": list(DEFAULT_EXCLUDE_TOKENS) + list(args.exclude_token),
            "allowConverted": args.allow_converted,
        },
        "rejectedSummary": dict(rejected),
        "charts": records,
    }
    (out_dir / "eval_set.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    csv_path = out_dir / "eval_set.csv"
    fieldnames = list(ChartStats.__dataclass_fields__.keys())
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for record in records:
            writer.writerow(record)

    relative_paths = out_dir / "eval_set_relative_paths.txt"
    relative_paths.write_text(
        "".join(f"{item.relativePath}\n" for item in selected),
        encoding="utf-8",
    )

    if args.write_absolute_paths:
        absolute_path_file = out_dir / "eval_set_paths.local.txt"
        absolute_path_file.write_text(
            "".join(f"{root / item.relativePath}\n" for item in selected),
            encoding="utf-8",
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pick a deterministic random osu!mania eval set from an osu! Songs folder."
    )
    parser.add_argument("--songs-root", default=default_songs_root(), help="osu! Songs folder.")
    parser.add_argument("--out-dir", default="dist/converter-eval-set", help="Output folder.")
    parser.add_argument("--source-keys", type=parse_int_list, default=parse_int_list("4,5,7"))
    parser.add_argument("--per-source-key", type=int, default=25)
    parser.add_argument("--seed", type=int, default=20260612)
    parser.add_argument("--scan-limit", type=int, default=0, help="Maximum .osu files to inspect; 0 means unlimited.")
    parser.add_argument("--min-notes", type=int, default=120)
    parser.add_argument("--max-notes", type=int, default=3500)
    parser.add_argument("--min-duration-ms", type=int, default=30_000)
    parser.add_argument("--max-duration-ms", type=int, default=300_000)
    parser.add_argument("--max-avg-nps", type=float, default=14.0)
    parser.add_argument("--max-window-nps", type=float, default=34.0)
    parser.add_argument("--window-ms", type=int, default=2000)
    parser.add_argument("--max-chord-rate", type=float, default=0.70)
    parser.add_argument("--max-chord-size", type=int, default=0, help="0 means allow up to the chart key count.")
    parser.add_argument("--max-hold-rate", type=float, default=0.85)
    parser.add_argument("--min-lane-coverage", type=float, default=0.50)
    parser.add_argument("--fast-jack-ms", type=int, default=80)
    parser.add_argument("--max-fast-jack-rate", type=float, default=0.18)
    parser.add_argument("--max-empty-gap-ms", type=int, default=25_000)
    parser.add_argument("--exclude-token", action="append", default=[], help="Extra case-insensitive exclusion token.")
    parser.add_argument(
        "--allow-converted",
        action="store_true",
        help="Do not reject converted-chart markers. Intended only for controlled test fixtures.",
    )
    parser.add_argument("--no-balance-tags", dest="balance_tags", action="store_false")
    parser.add_argument(
        "--write-absolute-paths",
        action="store_true",
        help="Also write eval_set_paths.local.txt with local absolute paths. This is intentionally off by default.",
    )
    parser.set_defaults(balance_tags=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.songs_root:
        raise SystemExit("Set --songs-root, KEYWEAVER_SONGS_ROOT, or OSU_SONGS_ROOT to a local osu! Songs folder.")
    root = Path(args.songs_root).expanduser().resolve()
    if not root.is_dir():
        raise SystemExit(f"Songs root not found: {root}")
    if args.per_source_key < 1:
        raise SystemExit("--per-source-key must be at least 1")

    print(f"[eval-set] scanning {root}")
    candidates, rejected = scan_candidates(args, root)
    selected = select_eval_set(candidates, args)
    write_outputs(root, Path(args.out_dir), selected, rejected, args)

    by_key = Counter(item.sourceKeys for item in selected)
    by_tag = Counter(item.primaryTag for item in selected)
    print(f"[eval-set] candidates={len(candidates)} selected={len(selected)}")
    print(f"[eval-set] selected by key: {dict(sorted(by_key.items()))}")
    print(f"[eval-set] selected by tag: {dict(sorted(by_tag.items()))}")
    print(f"[eval-set] wrote {Path(args.out_dir) / 'eval_set.json'}")
    print(f"[eval-set] wrote {Path(args.out_dir) / 'eval_set.csv'}")
    print(f"[eval-set] wrote {Path(args.out_dir) / 'eval_set_relative_paths.txt'}")
    if args.write_absolute_paths:
        print(f"[eval-set] wrote {Path(args.out_dir) / 'eval_set_paths.local.txt'}")
    return 0 if selected else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(1)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        raise SystemExit(130)
