#include "parser/bms.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace keyconv {

namespace {

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

struct DataLine {
    int measure = 0;
    int channel = 0;
    std::string payload;
};

struct RawObject {
    Position position;
    int channel = 0;
    std::string token;
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

std::optional<int> parseHex(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
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

std::optional<double> parseDouble(const std::string& value) {
    try {
        return std::stod(trim(value));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<DataLine> parseDataLine(const std::string& line) {
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

    DataLine data;
    data.measure = std::stoi(measureText);
    data.channel = std::stoi(channelText);
    data.payload = trim(text.substr(colon + 1));
    return data;
}

std::optional<std::pair<std::string, std::string>> parseHeaderLine(const std::string& line) {
    const auto text = trim(line);
    if (text.size() < 2 || text.front() != '#') {
        return std::nullopt;
    }
    if (parseDataLine(text).has_value()) {
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

int normalChannelForLongChannel(int channel) {
    return channel - 40;
}

bool isPlayableChannel(int channel) {
    return isNormalKeyChannel(channel) || isLongKeyChannel(channel);
}

std::vector<int> laneOrderForChannels(const std::set<int>& usedChannels,
                                      bool doublePlay,
                                      std::optional<int> forcedKeyCount) {
    if (forcedKeyCount == 18) {
        return {11, 12, 13, 14, 15, 18, 19, 16, 17,
                21, 22, 23, 24, 25, 28, 29, 26, 27};
    }
    const std::vector<int> singleWithScratch = {16, 11, 12, 13, 14, 15, 18, 19};
    const std::vector<int> singleNoScratch = {11, 12, 13, 14, 15, 18, 19, 16};
    const std::vector<int> doubleNoScratch = {11, 12, 13, 14, 15, 18, 19, 21, 22, 23, 24, 25, 28, 29};
    const std::vector<int> doubleWithScratch = {16, 11, 12, 13, 14, 15, 18, 19,
                                                26, 21, 22, 23, 24, 25, 28, 29};

    const bool hasScratch = usedChannels.count(16) > 0 || usedChannels.count(26) > 0;
    const auto& preferred = doublePlay ? (hasScratch ? doubleWithScratch : doubleNoScratch)
                                       : (hasScratch ? singleWithScratch : singleNoScratch);

    std::vector<int> order;
    for (const int channel : preferred) {
        if (usedChannels.count(channel) > 0) {
            order.push_back(channel);
        }
    }
    for (const int channel : usedChannels) {
        if (std::find(order.begin(), order.end(), channel) == order.end()) {
            order.push_back(channel);
        }
    }
    return order;
}

std::vector<std::string> objectTokens(const std::string& payload) {
    std::vector<std::string> tokens;
    if (payload.size() < 2) {
        return tokens;
    }
    const std::size_t count = payload.size() / 2;
    tokens.reserve(count);
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
    if (PositionLess{}(end, begin)) {
        return 0.0;
    }
    if (begin.measure == end.measure) {
        const double factor = measureFactorAt(measureFactors, begin.measure);
        return measureDurationMs(bpm, factor) * std::max(0.0, end.fraction - begin.fraction);
    }

    double total = 0.0;
    total += measureDurationMs(bpm, measureFactorAt(measureFactors, begin.measure)) *
             std::max(0.0, 1.0 - begin.fraction);
    for (int measure = begin.measure + 1; measure < end.measure; ++measure) {
        total += measureDurationMs(bpm, measureFactorAt(measureFactors, measure));
    }
    total += measureDurationMs(bpm, measureFactorAt(measureFactors, end.measure)) *
             std::max(0.0, end.fraction);
    return total;
}

std::map<Position, int, PositionLess> buildTimeMap(
    const std::set<Position, PositionLess>& positions,
    const std::map<Position, std::vector<double>, PositionLess>& bpmEvents,
    double baseBpm,
    const std::map<int, double>& measureFactors) {
    std::map<Position, int, PositionLess> times;
    Position previous{0, 0.0};
    double currentTime = 0.0;
    double currentBpm = baseBpm > 0.0 ? baseBpm : 120.0;

    for (const auto& position : positions) {
        currentTime += durationBetween(previous, position, currentBpm, measureFactors);
        times[position] = static_cast<int>(std::lround(currentTime));

        const auto found = bpmEvents.find(position);
        if (found != bpmEvents.end() && !found->second.empty()) {
            currentBpm = found->second.back();
        }
        previous = position;
    }
    return times;
}

std::optional<std::string> headerValue(const std::map<std::string, std::string>& headers,
                                       const std::string& name) {
    const auto found = headers.find(name);
    if (found == headers.end()) {
        return std::nullopt;
    }
    return found->second;
}

int readPlayerMode(const std::map<std::string, std::string>& headers) {
    const auto value = headerValue(headers, "PLAYER");
    if (!value.has_value()) {
        return 1;
    }
    try {
        return std::stoi(*value);
    } catch (...) {
        return 1;
    }
}

}  // namespace

Chart parseBms(const std::string& text, const ParseOptions& options) {
    Chart chart;
    chart.meta.format = "bms";

    std::map<std::string, std::string> headers;
    std::map<std::string, double> extendedBpms;
    std::map<int, double> measureFactors;
    std::map<Position, std::vector<double>, PositionLess> bpmEvents;
    std::set<Position, PositionLess> timedPositions;
    std::set<int> usedChannels;
    std::vector<RawObject> normalObjects;
    std::vector<RawObject> longObjects;

    double baseBpm = 120.0;
    int maxMeasure = 0;

    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        chart.raw.originalLines.push_back(line);

        if (const auto header = parseHeaderLine(line); header.has_value()) {
            headers[header->first] = header->second;
            if (header->first == "BPM") {
                if (const auto bpm = parseDouble(header->second); bpm.has_value() && *bpm > 0.0) {
                    baseBpm = *bpm;
                }
            } else if (header->first.size() == 5 && header->first.rfind("BPM", 0) == 0) {
                if (const auto bpm = parseDouble(header->second); bpm.has_value() && *bpm > 0.0) {
                    extendedBpms[upper(header->first.substr(3, 2))] = *bpm;
                }
            } else if (header->first.rfind("STOP", 0) == 0) {
                chart.warnings.push_back("BMS STOP timing is not supported yet; parsed note times may drift.");
            } else if (header->first == "RANDOM" || header->first == "IF" || header->first == "ENDIF") {
                chart.warnings.push_back("BMS random/control-flow directives are preserved but not evaluated.");
            }
            continue;
        }

        const auto data = parseDataLine(line);
        if (!data.has_value()) {
            continue;
        }
        maxMeasure = std::max(maxMeasure, data->measure);

        const auto tokens = objectTokens(data->payload);
        if (tokens.empty()) {
            continue;
        }

        if (data->channel == 2) {
            if (const auto factor = parseDouble(data->payload); factor.has_value() && *factor > 0.0) {
                measureFactors[data->measure] = *factor;
            }
            continue;
        }

        for (std::size_t index = 0; index < tokens.size(); ++index) {
            const auto token = upper(tokens[index]);
            if (token == "00") {
                continue;
            }

            Position position{data->measure, static_cast<double>(index) / static_cast<double>(tokens.size())};
            timedPositions.insert(position);

            if (data->channel == 3) {
                if (const auto bpm = parseHex(token); bpm.has_value() && *bpm > 0) {
                    bpmEvents[position].push_back(static_cast<double>(*bpm));
                }
                continue;
            }
            if (data->channel == 8) {
                const auto found = extendedBpms.find(token);
                if (found != extendedBpms.end()) {
                    bpmEvents[position].push_back(found->second);
                } else {
                    chart.warnings.push_back("Unknown extended BPM token: " + token);
                }
                continue;
            }
            if (!isPlayableChannel(data->channel)) {
                continue;
            }

            const int normalChannel = isLongKeyChannel(data->channel)
                                          ? normalChannelForLongChannel(data->channel)
                                          : data->channel;
            usedChannels.insert(normalChannel);

            RawObject object;
            object.position = position;
            object.channel = normalChannel;
            object.token = token;
            if (isLongKeyChannel(data->channel)) {
                longObjects.push_back(object);
            } else {
                normalObjects.push_back(object);
            }
        }
    }

    chart.meta.title = headerValue(headers, "TITLE");
    chart.meta.artist = headerValue(headers, "ARTIST");
    chart.meta.creator = headerValue(headers, "SUBARTIST");
    chart.meta.version = headerValue(headers, "SUBTITLE");

    const bool doublePlay = readPlayerMode(headers) == 3 ||
                            std::any_of(usedChannels.begin(), usedChannels.end(), [](int channel) {
                                return channel >= 21 && channel <= 29;
                            });
    const auto laneOrder = laneOrderForChannels(usedChannels, doublePlay, options.sourceKeyCount);
    std::map<int, int> laneByChannel;
    for (std::size_t index = 0; index < laneOrder.size(); ++index) {
        laneByChannel[laneOrder[index]] = static_cast<int>(index);
    }

    if (options.sourceKeyCount.has_value()) {
        chart.meta.sourceKeyCount = *options.sourceKeyCount;
    } else {
        chart.meta.sourceKeyCount = std::max(1, static_cast<int>(laneOrder.size()));
    }

    Position songEnd{maxMeasure + 1, 0.0};
    timedPositions.insert(Position{0, 0.0});
    timedPositions.insert(songEnd);
    const auto timeByPosition = buildTimeMap(timedPositions, bpmEvents, baseBpm, measureFactors);

    TimingPoint baseTiming;
    baseTiming.time = 0;
    baseTiming.beatLength = 60000.0 / (baseBpm > 0.0 ? baseBpm : 120.0);
    baseTiming.uninherited = true;
    baseTiming.rawLine = "#BPM " + std::to_string(baseBpm);
    chart.timingPoints.push_back(baseTiming);
    for (const auto& [position, bpms] : bpmEvents) {
        const auto time = timeByPosition.find(position);
        if (time == timeByPosition.end() || bpms.empty()) {
            continue;
        }
        TimingPoint point;
        point.time = time->second;
        point.beatLength = 60000.0 / bpms.back();
        point.uninherited = true;
        chart.timingPoints.push_back(point);
    }

    int noteIndex = 0;
    auto makeNote = [&](const RawObject& object, NoteType type, std::optional<int> endTime) {
        const auto lane = laneByChannel.find(object.channel);
        const auto time = timeByPosition.find(object.position);
        if (lane == laneByChannel.end() || time == timeByPosition.end()) {
            return;
        }
        Note note;
        note.id = "b" + std::to_string(noteIndex++);
        note.time = time->second;
        note.lane = lane->second;
        note.sourceLane = note.lane;
        note.type = type;
        note.endTime = endTime;
        note.raw = object.token;
        chart.notes.push_back(std::move(note));
    };

    for (const auto& object : normalObjects) {
        makeNote(object, NoteType::Tap, std::nullopt);
    }

    std::sort(longObjects.begin(), longObjects.end(), [](const RawObject& lhs, const RawObject& rhs) {
        const PositionLess less;
        if (less(lhs.position, rhs.position)) {
            return true;
        }
        if (less(rhs.position, lhs.position)) {
            return false;
        }
        return lhs.channel < rhs.channel;
    });

    std::map<int, RawObject> pendingLongStarts;
    for (const auto& object : longObjects) {
        const auto pending = pendingLongStarts.find(object.channel);
        if (pending == pendingLongStarts.end()) {
            pendingLongStarts[object.channel] = object;
            continue;
        }

        const auto startTime = timeByPosition.find(pending->second.position);
        const auto endTime = timeByPosition.find(object.position);
        if (startTime != timeByPosition.end() && endTime != timeByPosition.end() && endTime->second > startTime->second) {
            makeNote(pending->second, NoteType::Hold, endTime->second);
        } else {
            chart.warnings.push_back("Skipping invalid BMS long-note pair.");
        }
        pendingLongStarts.erase(pending);
    }

    for (const auto& [channel, object] : pendingLongStarts) {
        (void)channel;
        chart.warnings.push_back("Unmatched BMS long-note start was treated as a tap.");
        makeNote(object, NoteType::Tap, std::nullopt);
    }

    std::stable_sort(chart.notes.begin(), chart.notes.end(), [](const Note& lhs, const Note& rhs) {
        if (lhs.time != rhs.time) {
            return lhs.time < rhs.time;
        }
        if (lhs.lane != rhs.lane) {
            return lhs.lane < rhs.lane;
        }
        return lhs.id < rhs.id;
    });

    if (chart.notes.empty()) {
        throw std::runtime_error("BMS playable note channels missing");
    }
    return chart;
}

}  // namespace keyconv
