#!/usr/bin/env python3
"""Build local, non-overwriting NK2 legacy, phrase, notes and LN comparison OSZs.

Only Python's standard library is required. Original charts and assets are read
in place; every generated chart lives in a newly created output directory.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
import zipfile
from collections import Counter
from bisect import bisect_right
from pathlib import Path, PurePosixPath, PureWindowsPath


AUDIT_FIELDS = (
    "invalidLanes", "invalidHolds", "sameTimeCollisions", "longNoteConflicts",
    "createdLaneRepeats", "preservedLaneRepeats", "retainedSourceNotes",
    "droppedSourceNotes", "addedNotes", "retimedSourceNotes",
    "changedHoldDurations", "unknownSourceNotes",
)
VARIANTS = ((4, "off", "A", "A Legacy 4K", False), (4, "phrase", "B", "B Phrase 4K", False),
            (10, "off", "A", "A Legacy 10K", False), (10, "phrase", "B", "B Phrase 10K", False))
NOTES_VARIANTS = ((10, "phrase", "B", "B Phrase 10K", False),
                  (10, "phrase", "C", "C Phrase Notes 10K", True))
DENSE_LN_VARIANTS = NOTES_VARIANTS + ((10, "phrase", "D", "D Dense LN 10K", True),)
SOURCE_LN_VARIANTS = DENSE_LN_VARIANTS + ((10, "phrase", "E", "E Source LN 10K", True),)
BOOST_LN_VARIANTS = SOURCE_LN_VARIANTS + ((10, "phrase", "F", "F Source LN Plus 10K", True),)
KEY_TEST_VARIANTS = ((5, "off", "A", "A Legacy 5K", False), (5, "phrase", "B", "B Phrase 5K", False)) + tuple(
    (8, search, variant, label.replace("10K", "8K"), support)
    for _, search, variant, label, support in BOOST_LN_VARIANTS
)
ADOPTED_REPAIR_VARIANTS = (
    (5, "off", "A", "A Fixed 5K", False), (4, "phrase", "B", "B Adopted 4K", False),
    (5, "phrase", "B", "B Adopted 5K", False),
) + tuple(
    (target, "phrase", variant, f"{variant} {label} {target}K", support)
    for target in (8, 10)
    for variant, label, support in (("B", "Phrase", False), ("C", "Phrase Notes", True), ("E", "Adopted", True))
)
FOUR_KEY_TEST_VARIANTS = tuple(
    (target, "phrase", variant, f"{variant} {label} {target}K", support)
    for target in (5, 7)
    for variant, label, support in (("B", "Phrase", False), ("C", "Phrase Notes", True), ("E", "Source LN", True))
)
# Validation-only variants retain reports but are excluded from the OSZ.
COMPARISON_MODES = {
    "legacy-phrase": (VARIANTS, "NK2 Compare", frozenset()),
    "phrase-notes": (NOTES_VARIANTS, "NK2 Notes Compare", frozenset()),
    "phrase-dense-ln": (DENSE_LN_VARIANTS, "NK2 Dense LN Compare", frozenset()),
    "phrase-source-ln": (SOURCE_LN_VARIANTS, "NK2 Source LN Compare", frozenset({(10, "B")})),
    "phrase-source-ln-boost": (BOOST_LN_VARIANTS, "NK2 Source LN Plus Compare", frozenset({(10, "B"), (10, "C")})),
    "phrase-key-tests": (KEY_TEST_VARIANTS, "NK2 Key Tests Compare", frozenset({(8, "B"), (8, "C")})),
    "adopted-repair": (ADOPTED_REPAIR_VARIANTS, "NK2 Adopted E and 5K Fix Compare", frozenset({(8, "B"), (8, "C"), (10, "B"), (10, "C")})),
    "phrase-four-key-tests": (FOUR_KEY_TEST_VARIANTS, "NK2 4K to 5K and 7K Compare", frozenset({(5, "C"), (7, "C")})),
}
STANDARD_SAMPLE = re.compile(
    r"(?:normal|soft|drum)-(?:hitnormal|hitclap|hitfinish|hitwhistle)\d*\.(?:wav|ogg|mp3)$",
    re.IGNORECASE,
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sections(text: str) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    current = ""
    for line in text.splitlines():
        if line.strip().startswith("[") and line.strip().endswith("]"):
            current = line.strip()[1:-1]
        else:
            result.setdefault(current, []).append(line)
    return result


def fields(text: str, section: str) -> dict[str, str]:
    return {key.strip(): value.strip()
            for line in sections(text).get(section, []) if ":" in line
            for key, value in [line.split(":", 1)]}


def hit_objects(text: str) -> list[str]:
    return [line.strip() for line in sections(text).get("HitObjects", [])
            if line.strip() and not line.lstrip().startswith("//")]


def check_chart(text: str, target: int) -> int:
    if fields(text, "General").get("Mode") != "3":
        raise ValueError("Comparison charts must be osu!mania (Mode:3)")
    if float(fields(text, "Difficulty").get("CircleSize", "0")) != target:
        raise ValueError(f"Expected a {target}K comparison chart")
    objects = hit_objects(text)
    if not objects:
        raise ValueError("Comparison chart contains no hit objects")
    for line in objects:
        parts = line.split(",")
        if len(parts) < 5:
            raise ValueError("Malformed hit object in comparison chart")
        x, start, kind = int(parts[0]), int(parts[2]), int(parts[3])
        if not 0 <= x <= 512:
            raise ValueError("Hit object X coordinate outside mania playfield")
        if kind & 128:
            if len(parts) < 6 or int(parts[5].split(":", 1)[0]) <= start:
                raise ValueError("Invalid hold duration in comparison chart")
    return len(objects)


def rewrite_metadata(text: str, source: str, label: str, title_suffix: str = "NK2 Compare") -> str:
    original = fields(source, "Metadata")
    updates = {
        "Title": original.get("Title", "Untitled") + f" [{title_suffix}]",
        "Version": label + " - KeyWeaver Compare",
        "BeatmapID": "-1", "BeatmapSetID": "-1",
    }
    if original.get("TitleUnicode"):
        updates["TitleUnicode"] = original["TitleUnicode"] + f" [{title_suffix}]"
    if "Creator" in original:
        updates["Creator"] = original["Creator"]
    output: list[str] = []
    current = ""
    seen: set[str] = set()
    found = False
    for line in text.splitlines():
        if line.startswith("[") and line.endswith("]"):
            if current == "Metadata":
                output.extend(f"{key}:{value}" for key, value in updates.items() if key not in seen)
            current = line[1:-1]
            found |= current == "Metadata"
        elif current == "Metadata" and ":" in line:
            key = line.split(":", 1)[0].strip()
            if key in updates:
                if key in seen:
                    continue
                line = f"{key}:{updates[key]}"
                seen.add(key)
        output.append(line)
    if current == "Metadata":
        output.extend(f"{key}:{value}" for key, value in updates.items() if key not in seen)
    if not found:
        output.extend(["", "[Metadata]", *(f"{key}:{value}" for key, value in updates.items())])
    changed = "\n".join(output) + "\n"
    if hit_objects(changed) != hit_objects(text):
        raise ValueError("Metadata rewrite changed hit objects")
    return changed


def check_added_taps(base: str, extended: str, base_audit: dict[str, object],
                     extended_audit: dict[str, object]) -> dict[str, object]:
    """Compare exported note multisets, independently of generator bookkeeping."""
    original = Counter(hit_objects(base))
    candidate = Counter(hit_objects(extended))
    missing = sum((original - candidate).values())
    added = candidate - original
    added_count = sum(added.values())
    invalid_added = 0
    for line, count in added.items():
        kind = int(line.split(",")[3])
        if not kind & 1 or kind & (2 | 8 | 128):
            invalid_added += count
    if missing:
        raise ValueError(f"Phrase Notes changed or removed {missing} base hit object(s)")
    if invalid_added:
        raise ValueError("Phrase Notes added a non-tap object")
    if base_audit["addedNotes"] != 0 or extended_audit["addedNotes"] != added_count:
        raise ValueError("Phrase Notes added-note audit count disagrees with exported hit objects")
    return {"baseNotesPreserved": True, "baseNoteCount": sum(original.values()),
            "addedTapNotes": added_count, "addedHoldNotes": 0,
            "extraTapNotesVsBase": added_count, "extraHoldNotesVsBase": 0}


def check_added_dense_ln(base: str, extended: str, base_audit: dict[str, object],
                         extended_audit: dict[str, object], placement: dict[str, object],
                         *, source_ln: bool = False) -> dict[str, object]:
    """D/E preserve C; E may have no safe, source-covered room for new holds."""
    label = "Source LN" if source_ln else "Dense LN"
    original = Counter(hit_objects(base))
    candidate = Counter(hit_objects(extended))
    missing = sum((original - candidate).values())
    if missing:
        raise ValueError(f"{label} changed or removed {missing} C hit object(s)")
    added = candidate - original
    taps = holds = 0
    for line, count in added.items():
        parts = line.split(",")
        kind = int(parts[3])
        if kind & 128 and not kind & (1 | 2 | 8):
            if len(parts) < 6 or int(parts[5].split(":", 1)[0]) <= int(parts[2]):
                raise ValueError(f"{label} added an invalid hold duration")
            holds += count
        elif kind & 1 and not kind & (2 | 8 | 128):
            taps += count
        else:
            raise ValueError(f"{label} added an unsupported hit object type")
    if not source_ln and (holds <= 0 or sum(candidate.values()) <= sum(original.values())):
        raise ValueError("Dense LN comparison needs at least one added hold and more notes than C; no safe room was found")
    total_taps = int(base_audit["addedNotes"]) + taps
    if extended_audit["addedNotes"] != total_taps + holds:
        raise ValueError(f"{label} added-note audit count disagrees with exported hit objects")
    if placement.get("addedTapNotes") != total_taps or placement.get("addedHoldNotes") != holds:
        raise ValueError(f"{label} tap/hold placement counts disagree with exported hit objects")
    return {"baseNotesPreserved": True, "baseNoteCount": sum(original.values()),
            "addedTapNotes": total_taps, "addedHoldNotes": holds,
            "extraTapNotesVsBase": taps, "extraHoldNotesVsBase": holds}


def generated_hold_metrics(source: str, base: str, extended: str, target: int) -> dict[str, object]:
    """Measure C-relative generated holds using source intervals, not report claims."""
    source_holds: list[tuple[int, int]] = []
    for line in hit_objects(source):
        parts = line.split(",")
        if int(parts[3]) & 128 and len(parts) >= 6:
            start, end = int(parts[2]), int(parts[5].split(":", 1)[0])
            if end > start:
                source_holds.append((start, end))
    source_holds.sort()
    starts = [start for start, _ in source_holds]
    furthest_end: list[int] = []
    for _, end in source_holds:
        furthest_end.append(max(end, furthest_end[-1]) if furthest_end else end)
    durations: Counter[int] = Counter()
    lanes = [0] * target
    violations = 0
    added = Counter(hit_objects(extended)) - Counter(hit_objects(base))
    for line, count in added.items():
        parts = line.split(",")
        if not int(parts[3]) & 128:
            continue
        start, end = int(parts[2]), int(parts[5].split(":", 1)[0])
        durations[end - start] += count
        lanes[min(target - 1, int(parts[0]) * target // 512)] += count
        source_index = bisect_right(starts, start) - 1
        # A prefix maximum is an endpoint of one actual source hold. It does
        # not merge adjacent/overlapping intervals into synthetic coverage.
        if source_index < 0 or end > furthest_end[source_index]:
            violations += count
    total = sum(durations.values())
    return {
        "sourceValidHoldNotes": len(source_holds),
        "sourceLnCoverageViolations": violations,
        "generatedHoldNotes": total,
        "generatedHoldDurationKinds": len(durations),
        "generatedHoldDurationMs": sorted(durations),
        "generatedHoldLaneCounts": lanes,
        "generatedHoldMaxLaneShare": round(max(lanes) / total, 6) if total else 0.0,
        "generatedHoldStatus": "generated" if total else "noSourceLongNotes" if not source_holds else "noSafeRoom",
    }


def check_added_source_ln_boost(base: str, extended: str, base_audit: dict[str, object],
                                extended_audit: dict[str, object], report: dict[str, object],
                                source_hold_count: int) -> dict[str, object]:
    """F is an LN-only append to E; C-relative totals are checked separately."""
    original = Counter(hit_objects(base))
    candidate = Counter(hit_objects(extended))
    missing = sum((original - candidate).values())
    if missing:
        raise ValueError(f"Source LN Plus changed or removed {missing} E hit object(s)")
    added = candidate - original
    for line in added:
        parts = line.split(",")
        kind = int(parts[3])
        if not kind & 128 or kind & (1 | 2 | 8):
            raise ValueError("Source LN Plus added a non-hold object relative to E")
        if len(parts) < 6 or int(parts[5].split(":", 1)[0]) <= int(parts[2]):
            raise ValueError("Source LN Plus added an invalid hold duration")
    holds = sum(added.values())
    if extended_audit["addedNotes"] != int(base_audit["addedNotes"]) + holds:
        raise ValueError("Source LN Plus added-note audit count disagrees with the E-to-F difference")
    if report.get("support", {}).get("sourceLnBoostHoldAccepted") != holds:
        raise ValueError("Source LN Plus accepted-hold count disagrees with the E-to-F difference")
    if report.get("phraseSearch", {}).get("sourceLnBoost") is not True:
        raise ValueError("Source LN Plus report does not confirm boost is enabled")
    return {
        "baseNotesPreserved": True, "baseNoteCount": sum(original.values()),
        "extraTapNotesVsBase": 0, "extraHoldNotesVsBase": holds,
        "sourceLnBoostHoldAccepted": holds,
        "sourceLnBoostStatus": "generated" if holds else "noSourceLongNotes" if not source_hold_count else "noSafeRoom",
    }


def safe_asset(reference: str, parent: Path) -> tuple[str, Path]:
    value = reference.strip().strip('"').replace("\\", "/")
    relative = PurePosixPath(value)
    if (not value or relative.is_absolute() or PureWindowsPath(value).drive
            or ".." in relative.parts or ":" in value):
        raise ValueError("Chart asset must be a local relative path without traversal")
    resolved = (parent / relative).resolve(strict=True)
    if not resolved.is_relative_to(parent.resolve()) or not resolved.is_file():
        raise ValueError("Chart asset escaped the source set directory")
    if resolved.suffix.lower() in {".osu", ".osz", ".exe", ".dll", ".bat", ".ps1", ".py"}:
        raise ValueError("Executable/chart files cannot be packaged as media assets")
    return relative.as_posix(), resolved


def assets_for(text: str, parent: Path) -> dict[str, Path]:
    references = [fields(text, "General").get("AudioFilename", "")]
    for line in sections(text).get("Events", []):
        if not line.strip() or line.lstrip().startswith("//"):
            continue
        row = next(csv.reader([line], skipinitialspace=True))
        if row[0] in {"0", "Background"} and len(row) >= 3:
            references.append(row[2])
        elif row[0] in {"Sample", "5"} and len(row) >= 4:
            references.append(row[3])
        elif row[0] in {"Video", "1", "Sprite", "Animation"} or line[:1] in {" ", "_"}:
            raise ValueError("Video/storyboard assets are outside this comparison packager's scope")
    for line in hit_objects(text):
        parts = line.split(",")
        # Mania tap/hold custom sample filenames are the last colon-delimited field.
        if len(parts) >= 6 and ":" in parts[-1]:
            filename = parts[-1].split(":")[-1]
            if filename:
                references.append(filename)
    result = dict(safe_asset(reference, parent) for reference in references)
    # osu! also resolves standard hit sample names implicitly from sample indices.
    # Include set-local standard samples, without copying unrelated pack audio.
    for asset in parent.iterdir():
        if asset.is_file() and STANDARD_SAMPLE.fullmatch(asset.name):
            name, resolved = safe_asset(asset.name, parent)
            result[name] = resolved
    folded: dict[str, str] = {}
    for name in result:
        previous = folded.setdefault(name.casefold(), name)
        if previous != name:
            raise ValueError("Case-colliding asset names cannot be packaged safely")
    return result


def sanitize(value: object, redactions: list[tuple[str, str]]) -> object:
    if isinstance(value, dict):
        return {key: sanitize(item, redactions) for key, item in value.items()}
    if isinstance(value, list):
        return [sanitize(item, redactions) for item in value]
    if isinstance(value, str):
        for path, replacement in sorted(redactions, key=lambda item: len(item[0]), reverse=True):
            value = value.replace(path, replacement).replace(path.replace("\\", "/"), replacement)
        if re.search(r"[A-Za-z]:[\\/]|\\\\[^\\\s]+\\", value):
            raise ValueError("Report contains an unredacted absolute local path")
    return value


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def slug(title: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_-]+", "-", title).strip("-")[:65]
    return value or "chart"


def verify_osz(path: Path, charts: dict[str, tuple[str, int]], assets: dict[str, Path]) -> dict[str, str]:
    expected = set(charts) | set(assets)
    hashes: dict[str, str] = {}
    with zipfile.ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise ValueError("OSZ CRC verification failed")
        if len(archive.namelist()) != len(expected) or set(archive.namelist()) != expected:
            raise ValueError("OSZ member list did not match the intended charts and media")
        if sum(name.lower().endswith(".osu") for name in archive.namelist()) != len(charts):
            raise ValueError("OSZ comparison difficulty count did not match")
        for name in archive.namelist():
            data = archive.read(name)
            hashes[name] = sha256(data)
            if name in charts:
                expected_text, keys = charts[name]
                text = data.decode("utf-8")
                check_chart(text, keys)
                if data != expected_text.encode("utf-8"):
                    raise ValueError("OSZ chart bytes changed during packaging")
                audio = fields(text, "General")["AudioFilename"].replace("\\", "/")
                if audio not in archive.namelist():
                    raise ValueError("OSZ chart references missing audio")
            elif data != assets[name].read_bytes():
                raise ValueError("OSZ asset bytes changed during packaging")
    return hashes


def build(args: argparse.Namespace) -> None:
    comparison_mode = getattr(args, "comparison", "legacy-phrase")
    if comparison_mode not in COMPARISON_MODES:
        raise ValueError("Unknown comparison mode")
    key_tests_comparison = comparison_mode == "phrase-key-tests"
    four_key_comparison = comparison_mode == "phrase-four-key-tests"
    source_keys = 4 if four_key_comparison else 7
    boost_comparison = comparison_mode in {"phrase-source-ln-boost", "phrase-key-tests"}
    adopted_repair_comparison = comparison_mode == "adopted-repair"
    source_ln_comparison = comparison_mode == "phrase-source-ln"
    dense_ln_comparison = comparison_mode in {"phrase-dense-ln", "phrase-source-ln", "adopted-repair"} or boost_comparison or four_key_comparison
    notes_comparison = comparison_mode != "legacy-phrase"
    variants, title_suffix, validation_only = COMPARISON_MODES[comparison_mode]
    difficulty_count = 1 + sum((target, variant) not in validation_only for target, _, variant, _, _ in variants)
    converter = args.converter.resolve(strict=True)
    sources = [chart.resolve(strict=True) for chart in args.chart]
    if not converter.is_file() or any(not chart.is_file() or chart.suffix.lower() != ".osu" for chart in sources):
        raise ValueError("Provide a converter executable and .osu input files")
    if len(set(sources)) != len(sources):
        raise ValueError("Duplicate chart inputs are not allowed")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    work = output / ".work"
    work.mkdir()
    reports = output / "reports"
    reports.mkdir()
    redactions = [(str(output), "<output>"), (str(converter.parent), "<converter>"),
                  (str(Path.cwd()), "<workspace>")]
    redactions += [(str(source.parent), "<source-set>") for source in sources]
    rows: list[dict[str, object]] = []
    prepared: list[tuple[str, dict[str, tuple[str, int]], dict[str, Path]]] = []
    source_hashes = {source: sha256(source.read_bytes()) for source in sources}
    try:
        for ordinal, source in enumerate(sources, 1):
            source_text = source.read_text(encoding="utf-8-sig")
            check_chart(source_text, source_keys)
            metadata = fields(source_text, "Metadata")
            # Pack tracks can share Title/Creator while using different audio;
            # include the original difficulty so their new sets stay distinct.
            song_title_suffix = f"{metadata['Version']} - {title_suffix}" if four_key_comparison and metadata.get("Version") else title_suffix
            song = f"{ordinal:02d}-{slug(metadata.get('Title', source.stem))}"
            media = assets_for(source_text, source.parent)
            charts = {f"00_Source_{source_keys}K.osu": (rewrite_metadata(source_text, source_text, f"Source {source_keys}K", song_title_suffix), source_keys)}
            # A mixed-key pack must never compare a missing 8K base against 5K.
            phrase_bases: dict[int, tuple[str, dict[str, object]]] = {}
            notes_bases: dict[int, tuple[str, dict[str, object]]] = {}
            source_ln_bases: dict[int, tuple[str, dict[str, object]]] = {}
            deadline = time.perf_counter() + 60.0
            for target, search, variant, label, support in variants:
                included = (target, variant) not in validation_only
                stem = f"{song}-{variant}-{target}K"
                raw_chart = work / (stem + ".osu")
                raw_report = work / (stem + ".json")
                command = [str(converter), str(source), "--target", str(target), "--engine", "nk2",
                           "--nk2-mode", "faithful", "--nk2-search", search, "--nk2-audit",
                           "--out", str(raw_chart), "--report", str(raw_report)]
                if support:
                    command.append("--nk2-phrase-support")
                if variant in {"D", "E", "F"}:
                    command.append("--nk2-phrase-dense-ln")
                if variant in {"E", "F"}:
                    command.append("--nk2-phrase-source-ln")
                if variant == "F":
                    command.append("--nk2-phrase-source-ln-boost")
                remaining = deadline - time.perf_counter()
                if remaining <= 0:
                    raise TimeoutError("A song exceeded the 60-second comparison budget")
                start = time.perf_counter()
                result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                        errors="replace", timeout=remaining, check=False)
                elapsed = time.perf_counter() - start
                if not raw_report.is_file():
                    detail = sanitize(result.stderr.strip() or result.stdout.strip(), redactions)
                    raise ValueError(f"{song} {label}: converter produced no report ({result.returncode}): {detail}")
                report = json.loads(raw_report.read_text(encoding="utf-8-sig"))
                audit = report.get("independentSafety", {})
                if not isinstance(audit.get("passed"), bool) or any(key not in audit for key in AUDIT_FIELDS):
                    raise ValueError("Converter report omitted the independent safety audit")
                phrase_blocked = search == "phrase" and not audit["passed"] and result.returncode != 0
                if result.returncode != 0 and not phrase_blocked:
                    detail = sanitize(result.stderr.strip() or result.stdout.strip(), redactions)
                    raise ValueError(f"{song} {label}: converter failed ({result.returncode}): {detail}")
                comparison = ""
                added_check: dict[str, object] = {
                    "baseNotesPreserved": "", "baseNoteCount": "", "addedTapNotes": "", "addedHoldNotes": "",
                    "extraTapNotesVsBase": "", "extraHoldNotesVsBase": "",
                }
                boost_check: dict[str, object] = {"sourceLnBoostHoldAccepted": "", "sourceLnBoostStatus": ""}
                hold_metrics: dict[str, object] = {
                    "sourceValidHoldNotes": "", "sourceLnCoverageViolations": "", "generatedHoldNotes": "",
                    "generatedHoldDurationKinds": "", "generatedHoldDurationMs": "", "generatedHoldLaneCounts": "",
                    "generatedHoldMaxLaneShare": "", "generatedHoldStatus": "",
                }
                comparison_error = ""
                hold_count: int | str = ""
                if phrase_blocked:
                    # The CLI writes the failing audit but deliberately withholds
                    # an unsafe phrase chart. Retain those metrics in every sidecar.
                    count = report.get("placement", {}).get("outputNotes", 0)
                else:
                    converted = raw_chart.read_text(encoding="utf-8-sig")
                    count = check_chart(converted, target)
                    hold_count = sum(bool(int(line.split(",")[3]) & 128) for line in hit_objects(converted))
                    comparison = rewrite_metadata(converted, source_text, label, song_title_suffix)
                    if notes_comparison and variant == "B":
                        phrase_bases[target] = (comparison, audit)
                    elif notes_comparison and variant == "C":
                        phrase_base = phrase_bases.get(target)
                        if phrase_base is None:
                            comparison_error = "Phrase Notes is missing its B base comparison chart"
                        else:
                            try:
                                added_check = check_added_taps(phrase_base[0], comparison, phrase_base[1], audit)
                            except ValueError as error:
                                comparison_error = str(error)
                        notes_bases[target] = (comparison, audit)
                    elif dense_ln_comparison and variant in {"D", "E", "F"}:
                        notes_base = notes_bases.get(target)
                        if notes_base is None:
                            comparison_error = f"{label} is missing its C base comparison chart"
                        else:
                            try:
                                hold_metrics = generated_hold_metrics(source_text, notes_base[0], comparison, target)
                                added_check = check_added_dense_ln(notes_base[0], comparison, notes_base[1],
                                                                  audit, report.get("placement", {}), source_ln=variant in {"E", "F"})
                                if variant == "F":
                                    # F totals use C, but its base/delta fields
                                    # belong to E even when a later check fails.
                                    for key in ("baseNotesPreserved", "baseNoteCount", "extraTapNotesVsBase", "extraHoldNotesVsBase"):
                                        added_check[key] = ""
                                if variant in {"E", "F"} and hold_metrics["sourceLnCoverageViolations"]:
                                    raise ValueError(f"Source LN generated {hold_metrics['sourceLnCoverageViolations']} hold(s) outside a single original LN interval")
                                if variant == "F":
                                    source_ln_base = source_ln_bases.get(target)
                                    if source_ln_base is None:
                                        raise ValueError("Source LN Plus is missing its E base comparison chart")
                                    boost_result = check_added_source_ln_boost(source_ln_base[0], comparison, source_ln_base[1],
                                                                               audit, report, int(hold_metrics["sourceValidHoldNotes"]))
                                    added_check.update({key: value for key, value in boost_result.items() if key in added_check})
                                    boost_check.update({key: value for key, value in boost_result.items() if key in boost_check})
                            except ValueError as error:
                                comparison_error = str(error)
                        if variant == "E":
                            source_ln_bases[target] = (comparison, audit)
                    if included:
                        name = f"{len(charts):02d}_{variant}_{search}_{target}K.osu"
                        if name in media or name.casefold() in {key.casefold() for key in media}:
                            raise ValueError("Media asset conflicts with a generated chart name")
                        charts[name] = (comparison, target)
                row: dict[str, object] = {
                    "song": metadata.get("Title", ""), "sourceVersion": metadata.get("Version", ""),
                    "sourceKeys": source_keys,
                    "variant": variant, "targetKeys": target, "search": search,
                    "comparisonMode": comparison_mode, "phraseSupport": support,
                    "phraseDenseLn": variant in {"D", "E", "F"}, "phraseSourceLn": variant in {"E", "F"},
                    "phraseSourceLnBoost": variant == "F",
                    "includedInPackage": included,
                    "elapsedSeconds": round(elapsed, 6), "outputNotes": count,
                    "outputTapNotes": count - hold_count if isinstance(hold_count, int) else "",
                    "outputHoldNotes": hold_count,
                    "outputAvailable": not phrase_blocked, "exitCode": result.returncode,
                    "auditRequired": search == "phrase" or adopted_repair_comparison,
                    "auditPassed": audit["passed"], **{key: audit[key] for key in AUDIT_FIELDS},
                    "sourceSha256": source_hashes[source],
                    "outputSha256": sha256(comparison.encode("utf-8")) if comparison else "",
                    "osz": song + ".osz" if included else "", "difficulty": label,
                    "comparisonCheckPassed": not comparison_error, "comparisonCheckError": comparison_error,
                    "baseVariant": "E" if variant == "F" else "C" if variant in {"D", "E"} else "B" if variant == "C" else "",
                    "holdMetricsBaseVariant": "C" if variant in {"D", "E", "F"} else "",
                    **added_check, **hold_metrics, **boost_check,
                }
                rows.append(row)
                report["comparison"] = row
                write_json(reports / (stem + ".json"), sanitize(report, redactions))
                print(f"{song} {label}: notes={count} audit={audit['passed']} elapsed={elapsed:.3f}s", flush=True)
            prepared.append((song, charts, media))
        # Keep every audit visible. Legacy baselines may fail diagnostically,
        # but the adopted repair pack also requires its repaired A to pass.
        write_json(output / "comparison.json", {"status": "checked", "mode": comparison_mode, "results": rows})
        with (output / "summary.csv").open("w", encoding="utf-8-sig", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        failed = [row for row in rows if row["auditRequired"] and not row["auditPassed"]]
        if failed:
            raise ValueError(f"Required safety audit failed for {len(failed)} conversion(s); no OSZ packages emitted")
        comparison_failures = [row for row in rows if not row["comparisonCheckPassed"]]
        if comparison_failures:
            raise ValueError(f"Comparison verification failed: {comparison_failures[0]['comparisonCheckError']}; no OSZ packages emitted")
        for source, digest in source_hashes.items():
            if sha256(source.read_bytes()) != digest:
                raise ValueError("An original input changed during comparison")
        package_details = []
        for song, charts, media in prepared:
            if len(charts) != difficulty_count:
                raise ValueError("Comparison is missing a required difficulty")
            temporary = work / (song + ".osz")
            with zipfile.ZipFile(temporary, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
                for name, (text, _) in charts.items():
                    archive.writestr(name, text.encode("utf-8"))
                for name, asset in media.items():
                    archive.write(asset, name)
            member_hashes = verify_osz(temporary, charts, media)
            package_details.append({"file": temporary.name, "sha256": sha256(temporary.read_bytes()),
                                    "members": member_hashes})
        # Promote only after every archive passes CRC, asset and note readback.
        for package in package_details:
            (work / str(package["file"])).rename(output / str(package["file"]))
        write_json(output / "comparison.json", {"status": "verified", "mode": comparison_mode,
                                                "results": rows, "packages": package_details})
        legacy_failures = sum(row["variant"] == "A" and not row["auditPassed"] for row in rows)
        if four_key_comparison:
            mode_description = (
                "각 OSZ에는 원본 4K, B Phrase 5K, E Source LN 5K, B Phrase 7K, E Source LN 7K의 총 5개 난이도가 있습니다.\n"
                "이 모드는 4K 원본만 입력받으며 기존 비교 모드는 계속 7K 원본을 사용합니다.\n"
                "같은 팩 안의 서로 다른 곡도 구분하도록 비교 제목에 원본 난이도 이름을 함께 넣었습니다.\n"
                "각 키 수의 C Phrase Notes는 E 검증용으로만 실행했으며 OSZ에는 포함하지 않았습니다. includedInPackage=false로 구분합니다.\n"
                "B는 추가 노트 없는 구간 탐색판이며, C는 B의 모든 노트를 보존하면서 탭을 추가하는지 확인했습니다.\n"
                "E는 같은 키 수의 C 노트를 모두 보존하고, 추가 노트 개수·종류가 독립 검사 및 배치 보고서와 일치하는지 확인했습니다.\n"
                "E의 모든 생성 LN은 시작부터 끝까지 원본 4K의 유효 LN 한 개 안에 포함되며 sourceLnCoverageViolations=0입니다.\n"
                "원본에 LN이 없거나 안전한 공간이 없어 생성 LN이 0인 경우도 허용하고 generatedHoldStatus에 이유를 기록합니다.\n"
                "모든 B/C/E 출력은 독립 안전성 검사를 통과했습니다. E의 추가량과 생성 LN 통계는 각 키 수의 C를 기준으로 측정합니다.\n"
                "이전 원본과 비교 패키지는 수정하지 않고 새 폴더에 별도로 만들었습니다.\n"
            )
        elif adopted_repair_comparison:
            mode_description = (
                "각 OSZ에는 원본 7K, A Fixed 5K, B Adopted 4K, B Adopted 5K, E Adopted 8K, E Adopted 10K의 총 6개 난이도가 있습니다.\n"
                "A는 5K 배치 수정판이며, 채택 출력은 4K·5K의 B 구간 탐색과 8K·10K의 E Source LN 방식입니다.\n"
                "이 모드는 A를 포함한 모든 변환의 독립 안전성 검사 통과를 필수로 확인했습니다.\n"
                "8K·10K의 B/C는 같은 키 수 E의 검증용으로만 실행했으며 OSZ에는 포함하지 않았습니다. includedInPackage=false로 구분합니다.\n"
                "B→C의 탭 추가 및 C→E의 모든 노트 보존을 HitObjects 멀티셋으로 확인하고 추가량이 보고서와 일치하는지 검사했습니다.\n"
                "E의 생성 LN은 시작부터 끝까지 원본 7K의 유효 LN 한 개 안에 포함되며 sourceLnCoverageViolations=0입니다.\n"
                "E의 생성 LN 통계와 추가량은 C를 기준으로 비교합니다. baseVariant와 holdMetricsBaseVariant에 기준을 표시했습니다.\n"
                "원본 LN이나 안전한 공간이 없어 E의 생성 LN이 0인 경우도 generatedHoldStatus에 이유를 남깁니다.\n"
                "이전 A 5K 및 D/E/F 비교 패키지는 수정하지 않고 새 폴더에 별도로 만들었습니다.\n"
            )
        elif boost_comparison:
            mode_description = (
                "각 OSZ에는 원본 7K, A Legacy 5K, B Phrase 5K, D Dense LN 8K, E Source LN 8K, F Source LN Plus 8K의 총 6개 난이도가 있습니다.\n"
                "8K의 B/C는 검증용으로만 실행했으며 OSZ에는 포함하지 않았습니다.\n"
                f"기존 A 5K 출력 중 독립 안전성 검사 미통과: {legacy_failures}개. 결과를 숨기거나 교체하지 않았습니다.\n"
                if key_tests_comparison else
                "각 OSZ에는 원본 7K, D Dense LN 10K, E Source LN 10K, F Source LN Plus 10K의 총 4개 난이도가 있습니다.\n"
                "B/C는 검증용으로만 실행했으며 OSZ에는 포함하지 않았습니다.\n"
            ) + (
                "검증 전용 실행은 sidecar의 includedInPackage=false로 구분하며, 기준 채보를 키 수별로 분리해서 비교했습니다.\n"
                "D/E는 각각 C의 모든 노트를 보존하고, F는 E의 모든 노트를 보존하면서 LN만 추가하는지 HitObjects 멀티셋으로 확인했습니다.\n"
                "E/F의 모든 생성 LN이 원본 7K의 유효 LN 한 개 안에 완전히 포함되는지 검사했습니다. sourceLnCoverageViolations는 모두 0이어야 합니다.\n"
                "F의 증가량이 0이어도 허용하며 sourceLnBoostStatus에 noSourceLongNotes 또는 noSafeRoom을 기록합니다.\n"
                "F의 E 대비 추가 LN 수는 sourceLnBoostHoldAccepted와 일치하고, phraseSearch.sourceLnBoost=true인지 검사했습니다.\n"
                "모든 B/C/D/E/F 출력은 독립 안전성 검사를 통과했습니다. D는 기존대로 C보다 노트가 많고 생성 LN이 1개 이상이어야 합니다.\n"
                "baseVariant는 추가량을 비교한 기준입니다. F의 extraTapNotesVsBase/extraHoldNotesVsBase는 E 대비 증가량입니다.\n"
                "D/E/F의 generatedHold*는 모두 C 대비 총 생성 LN을 측정하며 holdMetricsBaseVariant=C로 표시합니다. addedTapNotes/addedHoldNotes는 원본 대비 생성량입니다.\n"
                "generatedHoldDurationKinds/DurationMs는 생성 LN 길이 종류, generatedHoldLaneCounts는 왼쪽부터 레인별 개수, generatedHoldMaxLaneShare는 가장 많은 레인의 비율입니다.\n"
            )
        elif source_ln_comparison:
            mode_description = (
                "각 OSZ에는 원본 7K, C Phrase Notes 10K, D Dense LN 10K, E Source LN 10K의 총 4개 난이도가 있습니다.\n"
                "D와 E는 각각 C의 모든 노트를 유지하면서 독립적으로 추가 탭·LN을 생성했습니다. E가 D의 노트를 포함할 필요는 없습니다.\n"
                "B는 C의 추가 탭을 검증하기 위해서만 실행했으며 OSZ에는 포함하지 않았습니다. sidecar의 includedInPackage로 구분합니다.\n"
                "B→C, C→D, C→E의 HitObjects 멀티셋 포함 관계와 추가 노트 종류·유효 LN 길이·보고서 개수를 검사했습니다.\n"
                "E의 모든 생성 LN은 시작부터 끝까지 원본 7K의 유효 LN 한 개 안에 들어가는지 별도로 검사했습니다.\n"
                "원본 LN이 없거나 안전한 공간이 없어서 E가 LN을 만들지 못한 경우도 허용하며 generatedHoldStatus에 표시합니다.\n"
                "D는 기존 기준대로 C보다 노트가 많고 추가 LN이 1개 이상이어야 합니다. B/C/D/E 모두 독립 안전성 검사를 통과했습니다.\n"
                "sourceLnCoverageViolations는 원본 LN 포함 조건 위반 수이며 E는 반드시 0입니다. D 값은 비교를 위한 측정값입니다.\n"
                "generatedHoldDurationKinds/DurationMs는 생성 LN 길이 종류, generatedHoldLaneCounts는 왼쪽부터 레인별 개수, generatedHoldMaxLaneShare는 가장 많은 레인의 비율입니다.\n"
                "addedTapNotes/addedHoldNotes는 원본 대비 생성량입니다. D/E의 extraTapNotesVsBase/extraHoldNotesVsBase는 모두 C 대비 증가량입니다.\n"
            )
        elif dense_ln_comparison:
            mode_description = (
                "각 OSZ에는 원본 7K, B Phrase 10K, C Phrase Notes 10K, D Dense LN 10K의 총 4개 난이도가 있습니다.\n"
                "B의 배치를 유지한 C에 탭을 추가하고, C의 모든 노트를 유지한 D에 추가 탭·LN을 얹었습니다.\n"
                "B→C 및 C→D의 HitObjects 멀티셋 포함 관계와 추가 노트 종류·유효 LN 길이를 검사했습니다.\n"
                "D는 C보다 노트가 많고 추가 LN이 1개 이상인 경우에만 패키지를 만들었습니다.\n"
                "B/C/D는 모두 독립 안전성 검사를 통과했고, 추가 탭·LN 수가 보고서와 일치합니다.\n"
                "addedTapNotes/addedHoldNotes는 원본 대비 생성량, extraTapNotesVsBase/extraHoldNotesVsBase는 바로 앞 비교판 대비 증가량입니다.\n"
            )
        elif notes_comparison:
            mode_description = (
                "각 OSZ에는 원본 7K, B Phrase 10K, C Phrase Notes 10K의 총 3개 난이도가 있습니다.\n"
                "B와 C는 같은 faithful 구간 탐색을 사용하며 C만 선택형 추가 탭을 활성화했습니다.\n"
                "B의 모든 HitObjects가 C에 그대로 남는지 멀티셋으로 확인하고, 늘어난 노트가 탭인지 검사했습니다.\n"
                "추가 개수가 독립 검사 addedNotes와 일치하는지도 확인했습니다.\n"
                "B와 C 모두 독립 안전성 검사 통과를 확인한 뒤 패키지를 만들었습니다.\n"
            )
        else:
            mode_description = (
                "각 OSZ에는 원본 7K와 A Legacy / B Phrase의 4K·10K, 총 5개 난이도가 있습니다.\n"
                "A와 B는 모두 faithful 모드이며, A는 기존 배치, B는 선택형 구간 탐색입니다.\n"
                f"기존 A 출력 중 독립 안전성 검사 미통과: {legacy_failures}개. 결과를 숨기거나 교체하지 않았습니다.\n"
                "B 출력은 모두 독립 검사 통과를 확인한 뒤 패키지를 만들었습니다.\n"
            )
        (output / "README_KO.md").write_text(
            "# NK2 실제 채보 비교\n\n" + mode_description +
            "음원·배경·원작자·원본 리듬을 비교할 수 있도록 같은 소스를 사용했습니다.\n"
            "원본 파일은 수정하지 않았고 비교 난이도의 맵 ID는 -1로 초기화했습니다.\n\n"
            "검사 통과는 패턴 품질이나 실제 게임 플레이의 완성도를 보증하지 않습니다.\n"
            "노트 삭제·시간 이동·원본 보존량과 처리시간은 summary.csv 및 reports에서 비교하세요.\n"
            "createdLaneRepeats는 다른 레인의 노트가 사이에 있어도 레인별 인접 노트를 검사합니다.\n"
            "원본에 있던 같은 레인 반복과 변환이 만든 반복을 구분합니다.\n\n"
            f"검증: ZIP CRC, {difficulty_count}개 난이도, 키 수, 음원 참조, 출력 노트/전체 바이트, 미디어 SHA-256 재확인.\n"
            "실제 클라이언트 import/playtest는 별도입니다. 개인 로컬 비교용 패키지입니다.\n",
            encoding="utf-8")
        checksum_files = sorted(path for path in output.rglob("*") if path.is_file() and work not in path.parents)
        (output / "SHA256SUMS.txt").write_text(
            "".join(f"{sha256(path.read_bytes())}  {path.relative_to(output).as_posix()}\n" for path in checksum_files),
            encoding="utf-8")
        print(f"Verified {len(package_details)} OSZ packages.", flush=True)
    except Exception as error:
        safe_error = sanitize(str(error), redactions)
        write_json(output / "FAILED.json", {"status": "failed", "error": safe_error, "results": rows})
        raise
    finally:
        # This directory was created exclusively by this invocation beneath the
        # newly created output directory. Never clean caller/source directories.
        if work.resolve().parent != output.resolve() or work.name != ".work":
            raise RuntimeError("Refusing unsafe temporary cleanup")
        shutil.rmtree(work)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converter", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New directory; an existing path is never overwritten")
    parser.add_argument("--chart", type=Path, action="append", required=True,
                        help="Original 4K .osu for phrase-four-key-tests; original 7K for all other modes; repeat for multiple songs")
    parser.add_argument("--comparison", choices=tuple(COMPARISON_MODES), default="legacy-phrase",
                        help="Compare legacy/phrase, added notes, LN variants, adopted E with repaired 5K, or original 4K to 5K/7K")
    args = parser.parse_args()
    try:
        build(args)
    except Exception as error:
        # Full source paths are intentionally not echoed or included in artifacts.
        print(f"Comparison packaging failed: {type(error).__name__}. See FAILED.json in the output directory if created.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
