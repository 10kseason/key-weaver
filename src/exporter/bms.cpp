#include "exporter/bms.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace keyconv {

namespace {

constexpr int kResolution = 192;

struct Position {
    int measure = 0;
    double fraction = 0.0;
};

struct PositionLess {
    bool operator()(const Position& lhs, const Position& rhs) const {
        if (lhs.measure != rhs.measure) {
            return lhs.measure < rhs.measure;
        }
        return lhs.fraction < rhs.fraction - 1e-9;
    }
};

struct Segment {
    Position begin;
    Position end;
    double beginMs = 0.0;
    double endMs = 0.0;
    double bpm = 120.0;
};

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool isDigits(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

std::optional<double> parseDouble(const std::string& value) {
    try {
        return std::stod(trim(value));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> parseHex(std::string_view value) {
    int result = 0;
    for (const char ch : value) {
        int digit = -1;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        } else if (ch >= 'A' && ch <= 'F') {
            digit = 10 + ch - 'A';
        } else if (ch >= 'a' && ch <= 'f') {
            digit = 10 + ch - 'a';
        }
        if (digit < 0) {
            return std::nullopt;
        }
        result = result * 16 + digit;
    }
    return result;
}

std::optional<std::pair<int, int>> parseDataHeader(const std::string& line) {
    const auto text = trim(line);
    if (text.size() < 7 || text.front() != '#') {
        return std::nullopt;
    }
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon < 6) {
        return std::nullopt;
    }
    const auto measureText = text.substr(1, 3);
    const auto channelText = text.substr(4, 2);
    if (!isDigits(measureText) || !isDigits(channelText)) {
        return std::nullopt;
    }
    return std::make_pair(std::stoi(measureText), std::stoi(channelText));
}

std::optional<std::pair<std::string, std::string>> parseHeaderLine(const std::string& line) {
    const auto text = trim(line);
    if (text.size() < 2 || text.front() != '#') {
        return std::nullopt;
    }
    if (parseDataHeader(text).has_value()) {
        return std::nullopt;
    }

    const auto body = text.substr(1);
    const auto split = body.find_first_of(" \t");
    if (split != std::string::npos) {
        return std::make_pair(upper(body.substr(0, split)), trim(body.substr(split + 1)));
    }

    std::size_t index = 0;
    while (index < body.size() && std::isalpha(static_cast<unsigned char>(body[index])) != 0) {
        ++index;
    }
    if (index == 0 || index == body.size()) {
        return std::make_pair(upper(body), std::string{});
    }
    return std::make_pair(upper(body.substr(0, index)), trim(body.substr(index)));
}

bool isNormalKeyChannel(int channel) {
    return (channel >= 11 && channel <= 19) || (channel >= 21 && channel <= 29);
}

bool isLongKeyChannel(int channel) {
    return (channel >= 51 && channel <= 59) || (channel >= 61 && channel <= 69);
}

bool isPlayableLine(const std::string& line) {
    const auto data = parseDataHeader(line);
    return data.has_value() && (isNormalKeyChannel(data->second) || isLongKeyChannel(data->second));
}

bool isKeyModeHeader(const std::string& name) {
    return name.size() == 2 && name[0] >= '4' && name[0] <= '8' && name[1] == 'K';
}

std::vector<std::string> objectTokens(const std::string& payload) {
    std::vector<std::string> tokens;
    if (payload.size() < 2) {
        return tokens;
    }
    const std::size_t count = payload.size() / 2;
    for (std::size_t index = 0; index < count; ++index) {
        tokens.push_back(payload.substr(index * 2, 2));
    }
    return tokens;
}

double measureFactorAt(const std::map<int, double>& measureFactors, int measure) {
    const auto found = measureFactors.find(measure);
    return found == measureFactors.end() ? 1.0 : found->second;
}

double measureDurationMs(double bpm, double factor) {
    const double safeBpm = bpm > 0.0 ? bpm : 120.0;
    return 240000.0 * factor / safeBpm;
}

double durationBetween(const Position& begin,
                       const Position& end,
                       double bpm,
                       const std::map<int, double>& measureFactors) {
    if (begin.measure == end.measure) {
        return measureDurationMs(bpm, measureFactorAt(measureFactors, begin.measure)) *
               std::max(0.0, end.fraction - begin.fraction);
    }

    double total = measureDurationMs(bpm, measureFactorAt(measureFactors, begin.measure)) *
                   std::max(0.0, 1.0 - begin.fraction);
    for (int measure = begin.measure + 1; measure < end.measure; ++measure) {
        total += measureDurationMs(bpm, measureFactorAt(measureFactors, measure));
    }
    total += measureDurationMs(bpm, measureFactorAt(measureFactors, end.measure)) * std::max(0.0, end.fraction);
    return total;
}

struct BmsTimingMap {
    double baseBpm = 120.0;
    std::map<int, double> measureFactors;
    std::map<Position, std::vector<double>, PositionLess> bpmEvents;
};

BmsTimingMap parseTimingMap(const RawChartData& raw) {
    BmsTimingMap timing;
    std::map<std::string, double> extendedBpms;

    for (const auto& line : raw.originalLines) {
        if (const auto header = parseHeaderLine(line); header.has_value()) {
            if (header->first == "BPM") {
                if (const auto bpm = parseDouble(header->second); bpm.has_value() && *bpm > 0.0) {
                    timing.baseBpm = *bpm;
                }
            } else if (header->first.size() == 5 && header->first.rfind("BPM", 0) == 0) {
                if (const auto bpm = parseDouble(header->second); bpm.has_value() && *bpm > 0.0) {
                    extendedBpms[upper(header->first.substr(3, 2))] = *bpm;
                }
            }
            continue;
        }

        const auto data = parseDataHeader(line);
        if (!data.has_value()) {
            continue;
        }
        const auto colon = line.find(':');
        const auto payload = colon == std::string::npos ? std::string{} : trim(line.substr(colon + 1));
        if (data->second == 2) {
            if (const auto factor = parseDouble(payload); factor.has_value() && *factor > 0.0) {
                timing.measureFactors[data->first] = *factor;
            }
            continue;
        }

        if (data->second != 3 && data->second != 8) {
            continue;
        }
        const auto tokens = objectTokens(payload);
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            const auto token = upper(tokens[index]);
            if (token == "00") {
                continue;
            }
            Position position{data->first, static_cast<double>(index) / static_cast<double>(tokens.size())};
            if (data->second == 3) {
                if (const auto bpm = parseHex(token); bpm.has_value() && *bpm > 0) {
                    timing.bpmEvents[position].push_back(static_cast<double>(*bpm));
                }
            } else {
                const auto found = extendedBpms.find(token);
                if (found != extendedBpms.end()) {
                    timing.bpmEvents[position].push_back(found->second);
                }
            }
        }
    }
    return timing;
}

std::vector<Segment> buildSegments(const BmsTimingMap& timing, int maxMeasure, int maxTimeMs) {
    std::set<Position, PositionLess> positions;
    positions.insert(Position{0, 0.0});
    for (int measure = 1; measure <= maxMeasure + 8; ++measure) {
        positions.insert(Position{measure, 0.0});
    }
    for (const auto& [position, bpms] : timing.bpmEvents) {
        (void)bpms;
        positions.insert(position);
    }

    std::vector<Segment> segments;
    Position previous{0, 0.0};
    double currentMs = 0.0;
    double currentBpm = timing.baseBpm > 0.0 ? timing.baseBpm : 120.0;
    bool first = true;

    for (const auto& position : positions) {
        if (first) {
            const auto found = timing.bpmEvents.find(position);
            if (found != timing.bpmEvents.end() && !found->second.empty()) {
                currentBpm = found->second.back();
            }
            previous = position;
            first = false;
            continue;
        }

        const double nextMs = currentMs + durationBetween(previous, position, currentBpm, timing.measureFactors);
        if (nextMs > currentMs + 1e-6) {
            segments.push_back(Segment{previous, position, currentMs, nextMs, currentBpm});
        }
        currentMs = nextMs;
        const auto found = timing.bpmEvents.find(position);
        if (found != timing.bpmEvents.end() && !found->second.empty()) {
            currentBpm = found->second.back();
        }
        previous = position;
        if (currentMs > static_cast<double>(maxTimeMs) + 240000.0) {
            break;
        }
    }
    if (segments.empty()) {
        segments.push_back(Segment{Position{0, 0.0}, Position{1, 0.0}, 0.0,
                                   measureDurationMs(timing.baseBpm, 1.0), timing.baseBpm});
    }
    return segments;
}

Position positionForTime(int timeMs, const std::vector<Segment>& segments, const std::map<int, double>& measureFactors) {
    const double target = static_cast<double>(std::max(0, timeMs));
    for (const auto& segment : segments) {
        if (target <= segment.endMs + 0.5) {
            const double elapsed = std::max(0.0, target - segment.beginMs);
            const double measureMs = measureDurationMs(segment.bpm, measureFactorAt(measureFactors, segment.begin.measure));
            double fraction = segment.begin.fraction + (measureMs > 0.0 ? elapsed / measureMs : 0.0);
            int measure = segment.begin.measure;
            while (fraction >= 1.0) {
                fraction -= 1.0;
                ++measure;
            }
            return Position{measure, std::clamp(fraction, 0.0, 0.999999)};
        }
    }

    const auto& last = segments.back();
    const double elapsed = std::max(0.0, target - last.beginMs);
    const double measureMs = measureDurationMs(last.bpm, measureFactorAt(measureFactors, last.begin.measure));
    double fraction = last.begin.fraction + (measureMs > 0.0 ? elapsed / measureMs : 0.0);
    int measure = last.begin.measure;
    while (fraction >= 1.0) {
        fraction -= 1.0;
        ++measure;
    }
    return Position{measure, std::clamp(fraction, 0.0, 0.999999)};
}

std::vector<int> channelsForKeyCount(int keyCount) {
    if (keyCount <= 0) {
        return {};
    }
    if (keyCount <= 5) {
        std::vector<int> channels;
        for (int index = 0; index < keyCount; ++index) {
            channels.push_back(11 + index);
        }
        return channels;
    }
    if (keyCount == 6) {
        return {11, 12, 13, 14, 15, 16};
    }
    if (keyCount == 7) {
        return {11, 12, 13, 14, 15, 18, 19};
    }
    if (keyCount == 8) {
        return {16, 11, 12, 13, 14, 15, 18, 19};
    }
    if (keyCount == 9) {
        return {11, 12, 13, 14, 15, 16, 17, 18, 19};
    }
    if (keyCount == 10) {
        return {11, 12, 13, 14, 15, 21, 22, 23, 24, 25};
    }
    if (keyCount == 12) {
        return {16, 11, 12, 13, 14, 15, 26, 21, 22, 23, 24, 25};
    }
    if (keyCount == 14) {
        return {11, 12, 13, 14, 15, 18, 19, 21, 22, 23, 24, 25, 28, 29};
    }
    if (keyCount == 16) {
        return {16, 11, 12, 13, 14, 15, 18, 19, 26, 21, 22, 23, 24, 25, 28, 29};
    }
    if (keyCount == 18) {
        return {11, 12, 13, 14, 15, 18, 19, 16, 17,
                21, 22, 23, 24, 25, 28, 29, 26, 27};
    }
    return {};
}

int longChannelForNormalChannel(int channel) {
    return channel + 40;
}

std::string channelText(int channel) {
    std::ostringstream out;
    out << std::setw(2) << std::setfill('0') << channel;
    return out.str();
}

std::string measureText(int measure) {
    std::ostringstream out;
    out << std::setw(3) << std::setfill('0') << measure;
    return out.str();
}

std::string noteToken(const Note& note) {
    const auto raw = upper(trim(note.raw));
    if (raw.size() == 2 && raw != "00") {
        return raw;
    }
    return "01";
}

void placeObject(std::map<std::pair<int, int>, std::vector<std::string>>& lines,
                 int measure,
                 int channel,
                 int slot,
                 const std::string& token) {
    auto& cells = lines[{measure, channel}];
    if (cells.empty()) {
        cells.assign(kResolution, "00");
    }
    const int clamped = std::clamp(slot, 0, kResolution - 1);
    if (cells[static_cast<std::size_t>(clamped)] == "00") {
        cells[static_cast<std::size_t>(clamped)] = token;
        return;
    }
    for (int delta = 1; delta < kResolution; ++delta) {
        const int right = clamped + delta;
        if (right < kResolution && cells[static_cast<std::size_t>(right)] == "00") {
            cells[static_cast<std::size_t>(right)] = token;
            return;
        }
        const int left = clamped - delta;
        if (left >= 0 && cells[static_cast<std::size_t>(left)] == "00") {
            cells[static_cast<std::size_t>(left)] = token;
            return;
        }
    }
}

std::pair<int, int> slotForPosition(const Position& position) {
    int measure = position.measure;
    int slot = static_cast<int>(std::lround(position.fraction * static_cast<double>(kResolution)));
    if (slot >= kResolution) {
        slot = 0;
        ++measure;
    }
    return {measure, slot};
}

std::string joinedCells(const std::vector<std::string>& cells) {
    std::string joined;
    joined.reserve(cells.size() * 2);
    for (const auto& cell : cells) {
        joined += cell;
    }
    return joined;
}

std::string keyWeaverMarker(int keyCount) {
    return "KeyWeaver" + std::to_string(keyCount) + "K";
}

int playerModeForKeyCount(int keyCount) {
    return keyCount >= 10 ? 3 : 1;
}

void writeKeyModeHeaders(std::ostream& out, int keyCount) {
    out << "#PLAYER " << playerModeForKeyCount(keyCount) << "\n";
    if (keyCount >= 4 && keyCount <= 8) {
        out << "#" << keyCount << "K\n";
    }
}

}  // namespace

std::string exportBms(const Chart& chart, std::optional<int> targetKeyCount) {
    const int keyCount = targetKeyCount.value_or(chart.meta.targetKeyCount.value_or(chart.meta.sourceKeyCount));
    const auto channels = channelsForKeyCount(keyCount);
    if (static_cast<int>(channels.size()) != keyCount) {
        throw std::invalid_argument(
            "BMS export cannot represent " + std::to_string(keyCount) +
            "K safely; supported BMS/PMS key counts are 1K..10K, 12K, 14K, 16K, and 18K");
    }
    const auto timing = parseTimingMap(chart.raw);

    int maxTime = 0;
    for (const auto& note : chart.notes) {
        maxTime = std::max(maxTime, note.endTime.value_or(note.time));
    }
    const auto segments = buildSegments(timing, std::max(1, maxTime / 1000), maxTime);

    std::map<std::pair<int, int>, std::vector<std::string>> noteLines;
    for (const auto& note : chart.notes) {
        if (note.lane < 0 || note.lane >= static_cast<int>(channels.size())) {
            continue;
        }
        const int normalChannel = channels[static_cast<std::size_t>(note.lane)];
        const auto start = slotForPosition(positionForTime(note.time, segments, timing.measureFactors));
        const auto token = noteToken(note);
        if (note.type == NoteType::Hold && note.endTime.has_value() && *note.endTime > note.time) {
            const int longChannel = longChannelForNormalChannel(normalChannel);
            const auto end = slotForPosition(positionForTime(*note.endTime, segments, timing.measureFactors));
            placeObject(noteLines, start.first, longChannel, start.second, token);
            placeObject(noteLines, end.first, longChannel, end.second, token);
        } else {
            placeObject(noteLines, start.first, normalChannel, start.second, token);
        }
    }

    std::ostringstream out;
    bool wroteLntype = false;
    bool wroteSubtitle = false;
    bool wroteAnyHeader = false;

    if (!chart.raw.originalLines.empty()) {
        for (const auto& line : chart.raw.originalLines) {
            if (isPlayableLine(line)) {
                continue;
            }
            const auto header = parseHeaderLine(line);
            if (header.has_value()) {
                wroteAnyHeader = true;
                if (header->first == "PLAYER" || isKeyModeHeader(header->first)) {
                    continue;
                }
                if (header->first == "LNTYPE") {
                    wroteLntype = true;
                }
                if (header->first == "SUBTITLE") {
                    wroteSubtitle = true;
                    const auto marker = keyWeaverMarker(keyCount);
                    if (header->second.find(marker) == std::string::npos) {
                        out << "#SUBTITLE " << header->second << " " << marker << "\n";
                    } else {
                        out << line << "\n";
                    }
                    continue;
                }
            }
            out << line << "\n";
        }
    } else {
        wroteAnyHeader = true;
        out << "#GENRE Converted\n";
        out << "#TITLE " << (chart.meta.title.has_value() ? *chart.meta.title : std::string("Converted")) << "\n";
        out << "#ARTIST " << (chart.meta.artist.has_value() ? *chart.meta.artist : std::string("Unknown")) << "\n";
        out << "#BPM 120\n";
        out << "#TOTAL 100\n";
        out << "#WAV01 keyweaver.wav\n";
    }

    if (!wroteAnyHeader) {
        out << "#TITLE Converted\n#BPM 120\n#WAV01 keyweaver.wav\n";
    }
    writeKeyModeHeaders(out, keyCount);
    if (!wroteSubtitle) {
        out << "#SUBTITLE " << keyWeaverMarker(keyCount) << "\n";
    }
    if (!wroteLntype) {
        out << "#LNTYPE 1\n";
    }

    out << "\n*---------------------- KEYWEAVER CONVERTED NOTES\n\n";
    for (const auto& [key, cells] : noteLines) {
        out << "#" << measureText(key.first) << channelText(key.second) << ":" << joinedCells(cells) << "\n";
    }
    return out.str();
}

}  // namespace keyconv
