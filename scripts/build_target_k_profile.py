#!/usr/bin/env python3
import argparse
import json
import math
import os
import re
import shutil
import subprocess
from pathlib import Path


EXCLUDE_TOKENS = (
    "7to10",
    "7to10c",
    "7 to 10",
    "7-to-10",
    "convert",
    "converted",
    "keyweaver",
)

STYLE_EXCLUDE_TOKENS = (
    "nknc",
)

STYLE_EXCLUDE_PATTERNS = (
    r"\d+k\d+c",
    r"\d+to\d+c",
)


def lower_text(value):
    return (value or "").casefold()


def compact_key(value):
    return "".join(ch for ch in lower_text(value) if ch.isalnum())


def token_list(values):
    result = []
    for value in values or []:
        for item in str(value).split(","):
            token = item.strip()
            if token:
                result.append(token)
    return result


def has_style_exclude_pattern(compact_haystack):
    return any(re.search(pattern, compact_haystack) for pattern in STYLE_EXCLUDE_PATTERNS)


def target_key_path_matches(path, target_keys):
    key = compact_key(str(path))
    return any(token in key for token in (f"{target_keys}k", f"{target_keys}key", f"{target_keys}keys"))


def iter_osu_paths(root, author_tokens, target_keys, curated_patterns, author_path_prefilter, target_key_path_prefilter):
    use_rg = author_path_prefilter and not curated_patterns and shutil.which("rg") is not None
    if use_rg:
        try:
            completed = subprocess.run(
                ["rg", "--files", str(root), "-g", "*.osu"],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding="utf-8",
                errors="replace",
            )
            for raw in completed.stdout.splitlines():
                path = Path(raw)
                path_key = compact_key(str(path))
                if any(compact_key(author) in path_key for author in author_tokens):
                    if target_key_path_prefilter and not target_key_path_matches(path, target_keys):
                        continue
                    yield path
            return
        except (OSError, subprocess.CalledProcessError):
            pass

    for directory, _, files in os.walk(root):
        for filename in files:
            if not filename.casefold().endswith(".osu"):
                continue
            path = Path(directory) / filename
            if target_key_path_prefilter and not target_key_path_matches(path, target_keys):
                continue
            yield path


def load_curated_patterns(path):
    if path is None:
        return []
    patterns = []
    with Path(path).open("r", encoding="utf-8-sig", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            title = line.rsplit("[", 1)[0].strip() if "[" in line else line
            version = line.rsplit("[", 1)[1].rstrip("]").strip() if "[" in line and "]" in line else ""
            version_key = compact_key(version)
            relaxed_version_key = version_key
            for prefix in ("ues", "ue"):
                if relaxed_version_key.startswith(prefix):
                    relaxed_version_key = relaxed_version_key[len(prefix) :]
                    break
            key = compact_key(line)
            if key:
                patterns.append(
                    {
                        "raw": line,
                        "key": key,
                        "titleKey": compact_key(title),
                        "versionKey": version_key,
                        "relaxedVersionKey": relaxed_version_key,
                        "pathKey": compact_key(title),
                    }
                )
    if not patterns:
        raise SystemExit(f"No curated patterns found in {path}")
    return patterns


def entropy(distribution):
    total = sum(distribution)
    if total <= 0 or len(distribution) <= 1:
        return 0.0
    result = 0.0
    for count in distribution:
        if count <= 0:
            continue
        p = count / total
        result -= p * math.log(p)
    return result / math.log(len(distribution))


def lane_from_x(x, keys):
    lane = int(math.floor(x * keys / 512.0))
    return max(0, min(keys - 1, lane))


def parse_osu_meta(path):
    meta = {}
    section = None
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
                continue
    return meta


def parse_osu_notes(path):
    notes = []
    section = None
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
            if section == "HitObjects" and "," in line:
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


def matched_curated_pattern(path, meta, curated_patterns):
    if not curated_patterns:
        return None
    title = meta.get("Title", "")
    version = meta.get("Version", "")
    title_key = compact_key(title)
    metadata_version_key = compact_key(version)
    path_candidate = compact_key(f"{path.stem} {path}")
    candidate = compact_key(f"{title} [{version}] {path.stem} {path}")
    for pattern in curated_patterns:
        if pattern["key"] in candidate:
            return pattern["raw"], 120
        title_matches = pattern["titleKey"] and (
            pattern["titleKey"] in title_key or pattern["titleKey"] in path_candidate
        )
        desired_version_key = pattern["relaxedVersionKey"] or pattern["versionKey"]
        version_matches = (
            not desired_version_key
            or desired_version_key in metadata_version_key
            or desired_version_key in path_candidate
        )
        if title_matches and version_matches:
            score = 60
            if pattern["titleKey"] in title_key:
                score += 20
            if desired_version_key == metadata_version_key:
                score += 25
            elif desired_version_key and desired_version_key in metadata_version_key:
                score += 10
            if "ln" in metadata_version_key and "ln" not in pattern["relaxedVersionKey"]:
                score -= 25
            return pattern["raw"], score
    return None


def is_reference_chart(path, meta, author_tokens, style_exclude_tokens, curated_patterns):
    try:
        if int(float(meta.get("Mode", "0"))) != 3:
            return False
        if int(round(float(meta.get("CircleSize", "0")))) != 10:
            return False
    except ValueError:
        return False

    creator = lower_text(meta.get("Creator"))
    version = lower_text(meta.get("Version"))
    haystack = lower_text(str(path)) + "\n" + creator + "\n" + version
    if any(token in haystack for token in EXCLUDE_TOKENS):
        return False
    compact_haystack = compact_key(haystack)
    for token in style_exclude_tokens:
        if compact_key(token) in compact_haystack:
            return False
    if has_style_exclude_pattern(compact_haystack):
        return False

    if curated_patterns:
        return matched_curated_pattern(path, meta, curated_patterns) is not None

    return any(lower_text(author) in creator or lower_text(author) in version for author in author_tokens)


def feature_summary(items):
    if not items:
        return {}
    names = sorted(items[0].keys())
    result = {}
    for name in names:
        values = [item[name] for item in items]
        q25 = percentile(values, 0.25)
        q75 = percentile(values, 0.75)
        result[name] = {
            "median": percentile(values, 0.50),
            "iqr": q75 - q25,
            "p10": percentile(values, 0.10),
            "p25": q25,
            "p75": q75,
            "p90": percentile(values, 0.90),
            "mean": sum(values) / len(values),
        }
    return result


def window_features(notes, keys, window_ms):
    if not notes:
        return []
    lane_notes = [(lane_from_x(x, keys), time, is_hold, end_time) for x, time, is_hold, end_time in notes]
    min_time = min(time for _, time, _, _ in lane_notes)
    max_time = max(time for _, time, _, _ in lane_notes)
    max_bucket = (max_time - min_time) // window_ms
    buckets = {}
    for note in lane_notes:
        bucket = (note[1] - min_time) // window_ms
        buckets.setdefault(bucket, []).append(note)

    features = []
    for bucket in range(max_bucket + 1):
        window = buckets.get(bucket, [])
        if not window:
            continue

        distribution = [0] * keys
        by_time = {}
        hold_count = 0
        for lane, time, is_hold, _ in window:
            distribution[lane] += 1
            by_time.setdefault(time, []).append(lane)
            if is_hold:
                hold_count += 1

        total = len(window)
        edge_width = min(2, keys // 2)
        edge_count = sum(distribution[:edge_width]) + sum(distribution[keys - edge_width :])
        left = sum(distribution[: keys // 2])
        right = sum(distribution[keys // 2 :])
        chord_slices = [lanes for lanes in by_time.values() if len(lanes) >= 2]
        chord_spans = [
            (max(lanes) - min(lanes)) / max(1, keys - 1)
            for lanes in chord_slices
        ]

        sorted_by_time = sorted(window, key=lambda item: (item[1], item[0]))
        same_lane_repeats = 0
        for i in range(1, len(sorted_by_time)):
            prev_lane, prev_time, _, _ = sorted_by_time[i - 1]
            lane, time, _, _ = sorted_by_time[i]
            if lane == prev_lane and 0 < time - prev_time <= 180:
                same_lane_repeats += 1

        features.append(
            {
                "densityNps": total * 1000.0 / window_ms,
                "laneEntropy": entropy(distribution),
                "edgeUsage": edge_count / total,
                "activeLaneRate": sum(1 for count in distribution if count > 0) / keys,
                "chordSpan": sum(chord_spans) / len(chord_spans) if chord_spans else 0.0,
                "chordRate": len(chord_slices) / len(by_time) if by_time else 0.0,
                "holdRate": hold_count / total,
                "handBalance": 1.0 - abs(left - right) / total,
                "adjacentExpansion": sum(distribution[lane] for lane in (2, 5, 7) if lane < keys) / total,
                "jackRisk": same_lane_repeats / total,
            }
        )
    return features


def density_bucket_summary(windows):
    if not windows:
        return {}
    densities = [item["densityNps"] for item in windows]
    low_cut = percentile(densities, 0.33)
    high_cut = percentile(densities, 0.66)
    buckets = {
        "all": windows,
        "low": [item for item in windows if item["densityNps"] <= low_cut],
        "mid": [item for item in windows if low_cut < item["densityNps"] <= high_cut],
        "high": [item for item in windows if item["densityNps"] > high_cut],
        "lnHeavy": [item for item in windows if item["holdRate"] >= percentile([w["holdRate"] for w in windows], 0.75)],
        "chordHeavy": [
            item for item in windows if item["chordRate"] >= percentile([w["chordRate"] for w in windows], 0.75)
        ],
        "jackRisk": [item for item in windows if item["jackRisk"] > 0.0],
    }
    result = {
        "densityCuts": {
            "lowMaxNps": low_cut,
            "midMaxNps": high_cut,
        }
    }
    for name, items in buckets.items():
        result[name] = {
            "windowCount": len(items),
            "features": feature_summary(items),
        }
    return result


def chart_metrics(notes, keys, window_ms):
    if not notes:
        return None
    lane_times = [(lane_from_x(x, keys), time) for x, time, _, _ in notes]
    distribution = [0] * keys
    for lane, _ in lane_times:
        distribution[lane] += 1

    total = sum(distribution)
    edge_width = min(2, keys // 2)
    edge_count = sum(distribution[:edge_width]) + sum(distribution[keys - edge_width :])
    left = sum(distribution[: keys // 2])
    right = sum(distribution[keys // 2 :])
    hand_balance = 1.0 if total <= 0 else 1.0 - abs(left - right) / total
    expanded_lane_count = sum(distribution[lane] for lane in (2, 5, 7) if lane < keys)

    by_time = {}
    for lane, time in lane_times:
        by_time.setdefault(time, []).append(lane)
    chord_spans = []
    for lanes in by_time.values():
        if len(lanes) >= 2:
            chord_spans.append((max(lanes) - min(lanes)) / max(1, keys - 1))

    min_time = min(time for _, time in lane_times)
    max_time = max(time for _, time in lane_times)
    sorted_lane_times = sorted(lane_times, key=lambda item: item[1])
    left_index = 0
    right_index = 0
    active_rates = []
    start = min_time
    while start <= max_time:
        while left_index < len(sorted_lane_times) and sorted_lane_times[left_index][1] < start:
            left_index += 1
        while right_index < len(sorted_lane_times) and sorted_lane_times[right_index][1] < start + 2000:
            right_index += 1
        active = {lane for lane, _ in sorted_lane_times[left_index:right_index]}
        if active:
            active_rates.append(len(active) / keys)
        start += 1000

    return {
        "laneEntropy": entropy(distribution),
        "edgeUsage": edge_count / total if total else 0.0,
        "activeLaneRate": sum(active_rates) / len(active_rates) if active_rates else 0.0,
        "chordSpan": sum(chord_spans) / len(chord_spans) if chord_spans else 0.0,
        "handBalance": hand_balance,
        "adjacentExpansion": expanded_lane_count / total if total else 0.0,
        "windows": window_features(notes, keys, window_ms),
    }


def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = (len(ordered) - 1) * p
    lower = int(math.floor(index))
    upper = int(math.ceil(index))
    if lower == upper:
        return ordered[lower]
    return ordered[lower] * (upper - index) + ordered[upper] * (index - lower)


def build_profile(root,
                  author_tokens,
                  style_exclude_tokens,
                  target_keys,
                  curated_patterns,
                  curated_path_prefilter,
                  author_path_prefilter,
                  target_key_path_prefilter,
                  profile_name,
                  profile_kind,
                  window_ms):
    metrics = []
    matched_paths = []
    matched_curated = []
    candidate_records = []

    def add_chart(path, curated_label=None):
        try:
            notes = parse_osu_notes(path)
        except OSError:
            return
        chart = chart_metrics(notes, target_keys, window_ms)
        if chart is None:
            return
        metrics.append(chart)
        matched_paths.append(str(path))
        if curated_label is not None:
            matched_curated.append(curated_label)

    for path in iter_osu_paths(root,
                               author_tokens,
                               target_keys,
                               curated_patterns,
                               author_path_prefilter,
                               target_key_path_prefilter):
        if curated_patterns and curated_path_prefilter:
            path_key = compact_key(str(path))
            if not any(pattern["pathKey"] and pattern["pathKey"] in path_key for pattern in curated_patterns):
                continue
        if not curated_patterns and author_path_prefilter:
            path_key = compact_key(str(path))
            if not any(compact_key(author) in path_key for author in author_tokens):
                continue
        try:
            meta = parse_osu_meta(path)
        except OSError:
            continue
        if not is_reference_chart(path, meta, author_tokens, style_exclude_tokens, curated_patterns):
            continue
        if curated_patterns:
            curated_match = matched_curated_pattern(path, meta, curated_patterns)
            if curated_match is None:
                continue
            label, score = curated_match
            candidate_records.append((score, label, str(path), path))
            continue
        add_chart(path)

    if curated_patterns:
        seen_curated = set()
        for _, label, _, path in sorted(candidate_records, key=lambda item: (-item[0], item[1], item[2])):
            if label in seen_curated:
                continue
            seen_curated.add(label)
            add_chart(path, label)
    else:
        seen_curated = set()

    if not metrics:
        raise SystemExit("No matching reference charts found")

    def mean_field(name):
        return sum(item[name] for item in metrics) / len(metrics)

    def robust_field(name):
        values = [item[name] for item in metrics]
        return max(percentile(values, 0.35), min(mean_field(name), percentile(values, 0.75)))

    all_windows = []
    for item in metrics:
        all_windows.extend(item["windows"])
    bucket_summary = density_bucket_summary(all_windows)
    all_window_features = bucket_summary.get("all", {}).get("features", {})

    chord_window_features = bucket_summary.get("chordHeavy", {}).get("features", {})

    def window_median(name, fallback, features=None):
        metric = (features or all_window_features).get(name)
        if not metric:
            return fallback
        return metric["median"]

    author_token = ",".join(author_tokens)
    profile = {
        "profileName": profile_name,
        "profileKind": profile_kind,
        "targetKeys": target_keys,
        "windowMs": window_ms,
        "sampleCount": len(metrics),
        "windowCount": len(all_windows),
        "sourceName": str(root),
        "authorToken": author_token,
        "desiredLaneEntropy": window_median("laneEntropy", robust_field("laneEntropy")),
        "desiredEdgeUsage": window_median("edgeUsage", robust_field("edgeUsage")),
        "desiredActiveLaneRate": window_median("activeLaneRate", robust_field("activeLaneRate")),
        "desiredChordSpan": window_median("chordSpan", robust_field("chordSpan"), chord_window_features),
        "desiredHandBalance": window_median("handBalance", robust_field("handBalance")),
        "desiredAdjacentExpansion": window_median("adjacentExpansion", robust_field("adjacentExpansion")),
        "densityBuckets": bucket_summary,
        "chartSummary": feature_summary(
            [
                {key: value for key, value in item.items() if key != "windows"}
                for item in metrics
            ]
        ),
        "filters": {
            "mode": 3,
            "circleSize": target_keys,
            "authorTokensInCreatorOrVersion": None if curated_patterns else author_tokens,
            "authorTokenInCreatorOrVersion": None if curated_patterns else author_token,
            "curatedList": bool(curated_patterns),
            "authorPathPrefilter": bool(author_path_prefilter),
            "targetKeyPathPrefilter": bool(target_key_path_prefilter),
            "excludedTokens": list(EXCLUDE_TOKENS),
            "styleExcludedTokens": list(style_exclude_tokens),
            "styleExcludedPatterns": list(STYLE_EXCLUDE_PATTERNS),
        },
        "curatedPatterns": [pattern["raw"] for pattern in curated_patterns],
        "matchedCuratedPatterns": matched_curated,
        "missingCuratedPatterns": [
            pattern["raw"] for pattern in curated_patterns if pattern["raw"] not in seen_curated
        ],
        "matchedChartsPreview": matched_paths[:20],
    }
    return profile


def main():
    parser = argparse.ArgumentParser(description="Build a Target-K profile from osu!mania reference charts.")
    parser.add_argument("--songs-root", default=r"D:\osu!\Songs")
    parser.add_argument("--author", action="append", help="Creator/version token to accept. May be repeated.")
    parser.add_argument("--exclude-style-token",
                        action="append",
                        default=[],
                        help="Extra token to reject from filename/Creator/Version, e.g. collab pack tags.")
    parser.add_argument("--target-keys", type=int, default=10)
    parser.add_argument("--profile-name", default="keyweaver_10k_style_v1")
    parser.add_argument("--profile-kind", default="style")
    parser.add_argument("--window-ms", type=int, default=1000)
    parser.add_argument("--curated-list")
    parser.add_argument("--no-curated-path-prefilter", action="store_true")
    parser.add_argument("--author-path-prefilter",
                        action="store_true",
                        help="For broad scans, skip files whose path does not contain any author token.")
    parser.add_argument("--target-key-path-prefilter",
                        action="store_true",
                        help="For broad scans, skip files whose path does not contain the target key token.")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    curated_patterns = load_curated_patterns(args.curated_list)
    authors = token_list(args.author) or ["u_e"]
    style_excludes = token_list(STYLE_EXCLUDE_TOKENS) + token_list(args.exclude_style_token)
    profile = build_profile(
        Path(args.songs_root),
        authors,
        style_excludes,
        args.target_keys,
        curated_patterns,
        not args.no_curated_path_prefilter,
        args.author_path_prefilter,
        args.target_key_path_prefilter,
        args.profile_name,
        args.profile_kind,
        args.window_ms,
    )
    output = Path(args.out)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"matched={profile['sampleCount']}")
    if curated_patterns:
        print(f"missing={len(profile['missingCuratedPatterns'])}")
    print(f"out={output}")


if __name__ == "__main__":
    main()
