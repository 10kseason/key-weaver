#include "parser/osu.hpp"

#include "core/mapping.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace keyconv {

namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream in(value);
    while (std::getline(in, current, delimiter)) {
        parts.push_back(current);
    }
    if (!value.empty() && value.back() == delimiter) {
        parts.emplace_back();
    }
    return parts;
}

bool isSectionHeader(const std::string& line) {
    const auto text = trim(line);
    return text.size() >= 3 && text.front() == '[' && text.back() == ']';
}

std::string sectionName(const std::string& line) {
    const auto text = trim(line);
    return text.substr(1, text.size() - 2);
}

std::optional<std::pair<std::string, std::string>> parseKeyValue(const std::string& line) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    return std::make_pair(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
}

std::optional<int> parseIntLoose(const std::string& value) {
    try {
        return static_cast<int>(std::lround(std::stod(trim(value))));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> parseDoubleLoose(const std::string& value) {
    try {
        return std::stod(trim(value));
    } catch (...) {
        return std::nullopt;
    }
}

void parseSections(const std::string& text, RawChartData& raw) {
    std::istringstream in(text);
    std::string line;
    std::optional<std::string> currentSection;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        raw.originalLines.push_back(line);

        if (isSectionHeader(line)) {
            currentSection = sectionName(line);
            raw.sectionOrder.push_back(*currentSection);
            raw.sections[*currentSection];
            continue;
        }

        if (currentSection.has_value()) {
            raw.sections[*currentSection].push_back(line);
        } else {
            raw.preamble.push_back(line);
        }
    }
}

std::optional<std::string> readStringField(const RawChartData& raw,
                                           const std::string& section,
                                           const std::string& key) {
    const auto found = raw.sections.find(section);
    if (found == raw.sections.end()) {
        return std::nullopt;
    }
    for (const auto& line : found->second) {
        const auto pair = parseKeyValue(line);
        if (pair.has_value() && pair->first == key) {
            return pair->second;
        }
    }
    return std::nullopt;
}

std::optional<int> readIntField(const RawChartData& raw,
                                const std::string& section,
                                const std::string& key) {
    const auto value = readStringField(raw, section, key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return parseIntLoose(*value);
}

std::vector<TimingPoint> parseTimingPoints(const RawChartData& raw, std::vector<std::string>& warnings) {
    std::vector<TimingPoint> timingPoints;
    const auto found = raw.sections.find("TimingPoints");
    if (found == raw.sections.end()) {
        return timingPoints;
    }

    for (const auto& line : found->second) {
        const auto text = trim(line);
        if (text.empty() || text.rfind("//", 0) == 0) {
            continue;
        }
        const auto parts = split(text, ',');
        if (parts.size() < 2) {
            warnings.push_back("Skipping invalid TimingPoint line: " + text);
            continue;
        }
        const auto time = parseIntLoose(parts[0]);
        const auto beatLength = parseDoubleLoose(parts[1]);
        if (!time.has_value() || !beatLength.has_value()) {
            warnings.push_back("Skipping invalid TimingPoint line: " + text);
            continue;
        }

        TimingPoint point;
        point.time = *time;
        point.beatLength = *beatLength;
        point.rawLine = line;
        if (parts.size() > 2) {
            point.meter = parseIntLoose(parts[2]);
        }
        if (parts.size() > 3) {
            point.sampleSet = parseIntLoose(parts[3]);
        }
        if (parts.size() > 4) {
            point.sampleIndex = parseIntLoose(parts[4]);
        }
        if (parts.size() > 5) {
            point.volume = parseIntLoose(parts[5]);
        }
        if (parts.size() > 6) {
            const auto uninherited = parseIntLoose(parts[6]);
            if (uninherited.has_value()) {
                point.uninherited = *uninherited != 0;
            }
        }
        if (parts.size() > 7) {
            point.effects = parseIntLoose(parts[7]);
        }
        timingPoints.push_back(point);
    }

    return timingPoints;
}

std::vector<Note> parseHitObjects(const RawChartData& raw, int keyCount, std::vector<std::string>& warnings) {
    std::vector<Note> notes;
    const auto found = raw.sections.find("HitObjects");
    if (found == raw.sections.end()) {
        throw std::runtime_error("HitObjects section missing");
    }

    int index = 0;
    for (const auto& line : found->second) {
        const auto text = trim(line);
        if (text.empty() || text.rfind("//", 0) == 0) {
            continue;
        }

        const auto parts = split(text, ',');
        if (parts.size() < 5) {
            warnings.push_back("Skipping invalid HitObject line: " + text);
            continue;
        }

        const auto x = parseIntLoose(parts[0]);
        const auto time = parseIntLoose(parts[2]);
        const auto typeValue = parseIntLoose(parts[3]);
        if (!x.has_value() || !time.has_value() || !typeValue.has_value()) {
            warnings.push_back("Skipping invalid HitObject line: " + text);
            continue;
        }

        Note note;
        note.id = "n" + std::to_string(index++);
        note.time = *time;
        note.lane = xToLane(*x, keyCount);
        note.sourceLane = note.lane;
        note.raw = line;

        if ((*typeValue & 128) != 0) {
            note.type = NoteType::Hold;
            if (parts.size() >= 6) {
                const auto params = split(parts[5], ':');
                if (!params.empty()) {
                    note.endTime = parseIntLoose(params[0]);
                }
            }
            if (!note.endTime.has_value()) {
                warnings.push_back("Hold note has invalid endTime at time " + std::to_string(note.time));
            }
        } else {
            note.type = NoteType::Tap;
        }

        notes.push_back(note);
    }

    return notes;
}

}  // namespace

Chart parseOsu(const std::string& text, const ParseOptions& options) {
    Chart chart;
    parseSections(text, chart.raw);

    chart.meta.title = readStringField(chart.raw, "Metadata", "Title");
    chart.meta.artist = readStringField(chart.raw, "Metadata", "Artist");
    chart.meta.creator = readStringField(chart.raw, "Metadata", "Creator");
    chart.meta.version = readStringField(chart.raw, "Metadata", "Version");
    chart.meta.mode = readIntField(chart.raw, "General", "Mode");

    if (chart.meta.mode.has_value() && *chart.meta.mode != 3) {
        chart.warnings.push_back("Mode is not osu!mania (Mode: 3)");
    }

    const auto circleSize = readIntField(chart.raw, "Difficulty", "CircleSize");
    if (options.sourceKeyCount.has_value()) {
        chart.meta.sourceKeyCount = *options.sourceKeyCount;
        if (circleSize.has_value() && *circleSize != *options.sourceKeyCount) {
            chart.warnings.push_back("CLI source key count differs from CircleSize; using CLI source");
        }
    } else if (circleSize.has_value()) {
        chart.meta.sourceKeyCount = *circleSize;
    } else {
        chart.meta.sourceKeyCount = 4;
        chart.warnings.push_back("CircleSize missing; defaulting source key count to 4");
    }

    chart.timingPoints = parseTimingPoints(chart.raw, chart.warnings);
    chart.notes = parseHitObjects(chart.raw, chart.meta.sourceKeyCount, chart.warnings);
    return chart;
}

}  // namespace keyconv

