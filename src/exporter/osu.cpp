#include "exporter/osu.hpp"

#include "core/mapping.hpp"

#include <set>
#include <sstream>
#include <string>

namespace keyconv {

namespace {

std::optional<std::pair<std::string, std::string>> parseKeyValue(const std::string& line) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    auto trim = [](const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::string{};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    };
    return std::make_pair(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
}

void writeHitObjects(std::ostringstream& out, const Chart& chart, int keyCount) {
    for (const auto& note : chart.notes) {
        const int x = laneToX(note.lane, keyCount);
        if (note.type == NoteType::Hold) {
            const int endTime = note.endTime.value_or(note.time);
            out << x << ",192," << note.time << ",128,0," << endTime << ":0:0:0:0:\n";
        } else {
            out << x << ",192," << note.time << ",1,0,0:0:0:0:\n";
        }
    }
}

std::string keyWeaverDifficultyMarker(int keyCount) {
    return "KeyWeaver" + std::to_string(keyCount) + "K";
}

std::string keyWeaverDifficultyName(const std::optional<std::string>& existing, int keyCount) {
    const auto marker = keyWeaverDifficultyMarker(keyCount);
    if (!existing.has_value() || existing->empty()) {
        return marker;
    }
    if (existing->find(marker) != std::string::npos) {
        return *existing;
    }
    return *existing + " " + marker;
}

void writeDefaultHeader(std::ostringstream& out, int keyCount) {
    out << "osu file format v14\n\n";
    out << "[General]\n";
    out << "Mode:3\n\n";
    out << "[Metadata]\n";
    out << "Title:Converted\n";
    out << "Artist:Unknown\n";
    out << "Creator:KeyWeaver\n";
    out << "Version:" << keyWeaverDifficultyMarker(keyCount) << "\n\n";
    out << "[Difficulty]\n";
    out << "CircleSize:" << keyCount << "\n\n";
    out << "[HitObjects]\n";
}

}  // namespace

std::string exportOsu(const Chart& chart, std::optional<int> targetKeyCount) {
    const int keyCount = targetKeyCount.value_or(chart.meta.targetKeyCount.value_or(chart.meta.sourceKeyCount));
    std::ostringstream out;

    if (chart.raw.sectionOrder.empty()) {
        writeDefaultHeader(out, keyCount);
        writeHitObjects(out, chart, keyCount);
        return out.str();
    }

    for (const auto& line : chart.raw.preamble) {
        out << line << "\n";
    }

    std::set<std::string> written;
    bool wroteDifficulty = false;
    bool wroteHitObjects = false;
    bool wroteMetadata = false;

    for (const auto& section : chart.raw.sectionOrder) {
        if (written.count(section) > 0) {
            continue;
        }
        written.insert(section);

        out << "[" << section << "]\n";

        const auto found = chart.raw.sections.find(section);
        const auto& lines = found != chart.raw.sections.end() ? found->second : std::vector<std::string>{};

        if (section == "Metadata") {
            bool wroteCreator = false;
            bool wroteVersion = false;
            for (const auto& line : lines) {
                const auto pair = parseKeyValue(line);
                if (pair.has_value() && pair->first == "Creator" &&
                    (pair->second.empty() || pair->second == "keyconv")) {
                    out << "Creator:KeyWeaver\n";
                    wroteCreator = true;
                } else if (pair.has_value() && pair->first == "Creator") {
                    out << line << "\n";
                    wroteCreator = true;
                } else if (pair.has_value() && pair->first == "Version") {
                    out << "Version:" << keyWeaverDifficultyName(pair->second, keyCount) << "\n";
                    wroteVersion = true;
                } else {
                    out << line << "\n";
                }
            }
            if (!wroteCreator && chart.meta.creator.has_value()) {
                out << "Creator:" << *chart.meta.creator << "\n";
            }
            if (!wroteVersion) {
                out << "Version:" << keyWeaverDifficultyName(chart.meta.version, keyCount) << "\n";
            }
            wroteMetadata = true;
        } else if (section == "Difficulty") {
            bool wroteCircleSize = false;
            for (const auto& line : lines) {
                const auto pair = parseKeyValue(line);
                if (pair.has_value() && pair->first == "CircleSize") {
                    out << "CircleSize:" << keyCount << "\n";
                    wroteCircleSize = true;
                } else {
                    out << line << "\n";
                }
            }
            if (!wroteCircleSize) {
                out << "CircleSize:" << keyCount << "\n";
            }
            wroteDifficulty = true;
        } else if (section == "HitObjects") {
            writeHitObjects(out, chart, keyCount);
            wroteHitObjects = true;
        } else {
            for (const auto& line : lines) {
                out << line << "\n";
            }
        }
    }

    if (!wroteMetadata) {
        out << "\n[Metadata]\n";
        out << "Title:" << (chart.meta.title.has_value() ? *chart.meta.title : std::string("Converted")) << "\n";
        out << "Artist:" << (chart.meta.artist.has_value() ? *chart.meta.artist : std::string("Unknown")) << "\n";
        out << "Creator:" << (chart.meta.creator.has_value() ? *chart.meta.creator : std::string("KeyWeaver")) << "\n";
        out << "Version:" << keyWeaverDifficultyName(chart.meta.version, keyCount) << "\n";
    }
    if (!wroteDifficulty) {
        out << "\n[Difficulty]\n";
        out << "CircleSize:" << keyCount << "\n";
    }
    if (!wroteHitObjects) {
        out << "\n[HitObjects]\n";
        writeHitObjects(out, chart, keyCount);
    }

    return out.str();
}

}  // namespace keyconv
