#include <keyconv/converter.hpp>
#include <keyconv/format/bms_exporter.hpp>
#include <keyconv/format/bms_parser.hpp>
#include <keyconv/format/osu_exporter.hpp>
#include <keyconv/format/osu_parser.hpp>
#include <keyconv/quality_report.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr const char* kToolName = "KeyWeaver";
constexpr const char* kToolVersion = "v0.5.5";

#if defined(_WIN32)
std::string utf8FromWide(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wideFromUtf8(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::filesystem::path currentExecutablePath() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

bool launchGuiForNoArgs(int argc) {
    if (argc != 1) {
        return false;
    }

    const auto exePath = currentExecutablePath();
    const auto exeDir = exePath.empty() ? std::filesystem::current_path() : exePath.parent_path();
    const auto guiPath = exeDir / L"keyconv_gui.exe";

    std::error_code existsError;
    if (std::filesystem::exists(guiPath, existsError)) {
        const auto result = ShellExecuteW(nullptr, L"open", guiPath.wstring().c_str(), nullptr,
                                          exeDir.wstring().c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) {
            return true;
        }
    }

    MessageBoxW(nullptr,
                L"KeyWeaver.exe is the command-line converter.\n\n"
                L"Put keyconv_gui.exe beside it for double-click launch, or run KeyWeaver.exe from a terminal.",
                L"KeyWeaver", MB_ICONINFORMATION | MB_OK);
    return true;
}
#endif

std::filesystem::path pathFromArgument(const std::string& value) {
#if defined(_WIN32)
    return std::filesystem::path(wideFromUtf8(value));
#else
    return std::filesystem::path(value);
#endif
}

std::string displayPath(const std::filesystem::path& path) {
#if defined(_WIN32)
    return utf8FromWide(path.wstring());
#else
    return path.string();
#endif
}

std::vector<std::string> commandLineArgs(int argc, char** argv) {
#if defined(_WIN32)
    int wideArgc = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv != nullptr) {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(wideArgc));
        for (int i = 0; i < wideArgc; ++i) {
            args.push_back(utf8FromWide(wideArgv[i]));
        }
        LocalFree(wideArgv);
        return args;
    }
#endif

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

struct CliOptions {
    std::filesystem::path input;
    std::optional<int> source;
    std::optional<int> target;
    std::optional<std::filesystem::path> out;
    keyconv::ConversionStyle style = keyconv::ConversionStyle::Playable;
    keyconv::CollisionPolicy collision = keyconv::CollisionPolicy::ShiftNearest;
    keyconv::OptimizerKind optimizer = keyconv::OptimizerKind::Greedy;
    keyconv::CompressPolicy compressPolicy = keyconv::CompressPolicy::Auto;
    keyconv::DistancePolicy distancePolicy = keyconv::DistancePolicy::AimodSafe;
    keyconv::ExpansionPolicy expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    bool expansionPolicyProvided = false;
    keyconv::EchoPolicy echoPolicy = keyconv::EchoPolicy::Off;
    keyconv::StreamEchoProfile streamEchoProfile = keyconv::StreamEchoProfile::Conservative;
    keyconv::JackPreservePolicy jackPreservePolicy = keyconv::JackPreservePolicy::PreservePlayable;
    bool gestureRailEnabled = true;
    bool echoDiagnostics = false;
    bool dpMode = false;
    int beamWidth = 16;
    int sameTimeEpsilonMs = 2;
    int minObjectGapMs = 16;
    int sameLaneMinGapMs = 20;
    int jackWindowMs = 180;
    int strictJackWindowMs = 120;
    bool allowPlayableJackSplit = true;
    int maxJackSplitLanes = 2;
    bool snapRolledNotes = true;
    int snapToleranceMs = 2;
    int maxRollMs = 64;
    double maxAddedNoteRatio = 0.45;
    int maxAddedPerSlice = 2;
    int maxAddedPerMeasure = 16;
    bool deterministicExpansion = true;
    int expansionMinGapMs = 16;
    int expansionSameLaneMinGapMs = 20;
    bool snapAddedNotes = true;
    int expansionSnapToleranceMs = 2;
    double maxEchoAddedRatio = 0.08;
    int maxEchoPerPattern = 4;
    int maxEchoPerMeasure = 8;
    int maxEchoPerSlice = 1;
    int minEchoPatternLength = 3;
    double minPatternConfidence = 0.70;
    bool echoRequiresSnap = true;
    int echoMinGapMs = 16;
    int echoSameLaneMinGapMs = 20;
    bool echoAvoidHighDensity = true;
    int echoHighDensityWindowMs = 1000;
    double echoMaxLocalNps = 12.0;
    bool dryRun = false;
    std::optional<std::filesystem::path> report;
    std::optional<std::filesystem::path> targetProfile;
    std::optional<std::string> comparePolicies;
    bool emitFeelReport = false;
    bool emitDiffReport = false;
    std::optional<std::filesystem::path> reportCsv;
    bool verbose = false;
    bool help = false;
};

void printHelp(std::ostream& out) {
    out << kToolName << " " << kToolVersion << "\n";
    out << "Usage: KeyWeaver <input.osu|input.bms> --target <number> [options]\n\n";
    out << "Options:\n";
    out << "  --source <number>       Source key count. Overrides CircleSize/BMS key-count inference.\n";
    out << "  --target <number>       Target key count. Required.\n";
    out << "  --out <path>            Output path. Defaults beside input as '<stem> KeyWeaverNK.<ext>'.\n";
    out << "  --style <style>         direct | expand | compress | playable | faithful | training | dp.\n";
    out << "  --collision <policy>    keep | shift-nearest | merge | drop. Default: shift-nearest.\n";
    out << "  --compress-policy <p>   auto | preserve-strict | no-overlap-drop | no-overlap-roll | no-overlap-hybrid | training-simplify.\n";
    out << "  --distance-policy <p>   off | warn | aimod-safe | strict. Default: aimod-safe.\n";
    out << "  --min-gap <ms>          Minimum positive object distance. Default: 16.\n";
    out << "  --same-lane-min-gap <ms> Minimum positive same-lane distance. Default: 20.\n";
    out << "  --jack-preserve-policy <p> preserve-strict | preserve-playable | avoid-new-jacks | smooth-all. Default: preserve-playable.\n";
    out << "  --jack-window-ms <ms>   Window for repeat/jack detection. Default: 180.\n";
    out << "  --strict-jack-window-ms <ms> Strict jack reference window. Default: 120.\n";
    out << "  --max-jack-split-lanes <n> Max target lanes counted as split jack. Default: 2.\n";
    out << "  --no-playable-jack-split Disallow playable jack split accounting.\n";
    out << "  --gesture-rail <on|off> Preserve detected stair/trill/jack gesture rails. Default: on.\n";
    out << "  --snap-roll             Snap rolled notes to timing grid. Default.\n";
    out << "  --no-snap-roll          Allow raw-ms roll candidates and report unsnapped rolled notes.\n";
    out << "  --snap-tolerance <ms>   Snap validation tolerance. Default: 2.\n";
    out << "  --max-roll-ms <ms>      Maximum roll distance from original time. Default: 64.\n";
    out << "  --expansion-policy <p>  auto | preserve | preserve-tap-plus | chord-fill | echo | training-scaffold | harder-remix | seeded-random.\n";
    out << "                          Default auto: preserve when target <= source, preserve-tap-plus when target > source.\n";
    out << "  --max-added-ratio <n>   Max added notes as source-note ratio. Default: 0.45.\n";
    out << "  --max-added-per-slice <n> Max added notes per source slice. Default: 2.\n";
    out << "  --max-added-per-measure <n> Max added notes per approximate measure. Default: 16.\n";
    out << "  --expansion-min-gap <ms> Minimum positive object gap for added notes. Default: 16.\n";
    out << "  --expansion-same-lane-min-gap <ms> Minimum same-lane gap for added notes. Default: 20.\n";
    out << "  --snap-added-notes      Require added notes to be timing-grid snapped. Default.\n";
    out << "  --no-snap-added-notes   Allow unsnapped added notes and report them.\n";
    out << "  --expansion-snap-tolerance <ms> Snap validation tolerance for added notes. Default: 2.\n";
    out << "  --echo-policy <p>       off | stair | trill | stream | stair-trill | stair-trill-stream | auto.\n";
    out << "  --stream-echo-profile <p> conservative | balanced | training | experimental. Default: conservative.\n";
    out << "  --echo-diagnostics      Print StreamEcho reject breakdown; does not alter conversion output.\n";
    out << "  --max-echo-ratio <n>    Max echo notes as source-note ratio. Default: 0.08.\n";
    out << "  --max-echo-per-pattern <n> Max echo notes per pattern. Default: 4.\n";
    out << "  --max-echo-per-measure <n> Max echo notes per approximate measure. Default: 8.\n";
    out << "  --max-echo-per-slice <n> Max echo notes per source slice. Default: 1.\n";
    out << "  --min-echo-pattern-length <n> Minimum pattern length for echo. Default: 3.\n";
    out << "  --min-pattern-confidence <n> Minimum PatternToken confidence for echo. Default: 0.70.\n";
    out << "  --echo-min-gap <ms>     Minimum positive object gap for echo notes. Default: 16.\n";
    out << "  --echo-same-lane-min-gap <ms> Minimum same-lane gap for echo notes. Default: 20.\n";
    out << "  --echo-max-local-nps <n> Skip non-stream echo above this local NPS. StreamEcho uses profile gates. Default: 12.\n";
    out << "  --optimizer <kind>      greedy | beam. Beam falls back to greedy in this version.\n";
    out << "  --beam-width <number>   Reserved beam width option. Default: 16.\n";
    out << "  --epsilon <ms>          Same-time slice epsilon in ms. Default: 2.\n";
    out << "  --dp                    Reserve DP mode; this version reports fallback to SP PPG.\n";
    out << "  --dry-run               Convert in memory and report only; do not write output.\n";
    out << "  --report <path>         Write conversion report JSON.\n";
    out << "  --target-profile <json> Use a Target-K reference profile JSON for K-likeness scoring.\n";
    out << "                          Target 10 auto-loads profiles/keyweaver_10k_broad_style_v1.json when bundled.\n";
    out << "  --compare-policies <list> Compare comma-separated policies without writing chart output.\n";
    out << "  --emit-feel-report      Include feel metrics in policy comparison console output.\n";
    out << "  --emit-diff-report      Include before/after diff metrics in policy comparison console output.\n";
    out << "  --report-csv <path>     Write policy comparison CSV.\n";
    out << "  --verbose               Print parser and conversion warnings.\n";
    out << "  --help                  Show this help.\n";
}

int parseInt(const std::string& value, const std::string& optionName) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing text");
        }
        return parsed;
    } catch (...) {
        throw std::runtime_error("Invalid integer for " + optionName + ": " + value);
    }
}

double parseDouble(const std::string& value, const std::string& optionName) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing text");
        }
        return parsed;
    } catch (...) {
        throw std::runtime_error("Invalid number for " + optionName + ": " + value);
    }
}

std::string requireValue(int& index, const std::vector<std::string>& args, const std::string& optionName) {
    if (index + 1 >= static_cast<int>(args.size())) {
        throw std::runtime_error("Missing value for " + optionName);
    }
    ++index;
    return args[static_cast<std::size_t>(index)];
}

std::string keyWeaverDifficultyMarker(int targetKeys) {
    return std::string(kToolName) + std::to_string(targetKeys) + "K";
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isBmsPath(const std::filesystem::path& path) {
    const auto extension = lowerAscii(path.extension().string());
    return extension == ".bms" || extension == ".bme" || extension == ".bml" || extension == ".pms";
}

std::filesystem::path defaultChartExtension(const std::filesystem::path& input, int targetKeys) {
    if (isBmsPath(input) && targetKeys == 9) {
        return std::filesystem::path(".pms");
    }
    return input.has_extension() ? input.extension() : std::filesystem::path(".osu");
}

std::filesystem::path defaultOutputPath(const std::filesystem::path& input, int targetKeys) {
    const auto parent = input.has_parent_path() ? input.parent_path() : std::filesystem::path(".");
    const auto extension = defaultChartExtension(input, targetKeys);
    const auto marker = keyWeaverDifficultyMarker(targetKeys);

    for (int suffix = 1;; ++suffix) {
        std::filesystem::path filename = input.stem();
        filename += " ";
        filename += marker;
        if (suffix > 1) {
            filename += " ";
            filename += std::to_string(suffix);
        }
        filename += extension;
        const auto candidate = parent / filename;
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
}

CliOptions parseArgs(const std::vector<std::string>& args) {
    CliOptions options;

    for (int i = 1; i < static_cast<int>(args.size()); ++i) {
        const std::string arg = args[static_cast<std::size_t>(i)];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
            continue;
        }
        if (arg == "--source") {
            options.source = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--target") {
            options.target = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--out") {
            options.out = pathFromArgument(requireValue(i, args, arg));
        } else if (arg == "--style") {
            const auto value = requireValue(i, args, arg);
            const auto style = keyconv::parseConversionStyle(value);
            if (!style.has_value()) {
                throw std::runtime_error("Invalid style: " + value);
            }
            options.style = *style;
        } else if (arg == "--collision") {
            const auto value = requireValue(i, args, arg);
            const auto policy = keyconv::parseCollisionPolicy(value);
            if (!policy.has_value()) {
                throw std::runtime_error("Invalid collision policy: " + value);
            }
            options.collision = *policy;
        } else if (arg == "--compress-policy") {
            const auto value = requireValue(i, args, arg);
            const auto policy = keyconv::parseCompressPolicy(value);
            if (!policy.has_value()) {
                throw std::runtime_error("Invalid compress policy: " + value);
            }
            options.compressPolicy = *policy;
        } else if (arg == "--distance-policy") {
            const auto value = requireValue(i, args, arg);
            const auto policy = keyconv::parseDistancePolicy(value);
            if (!policy.has_value()) {
                throw std::runtime_error("Invalid distance policy: " + value);
            }
            options.distancePolicy = *policy;
        } else if (arg == "--min-gap") {
            options.minObjectGapMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--same-lane-min-gap") {
            options.sameLaneMinGapMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--jack-preserve-policy") {
            const auto value = requireValue(i, args, arg);
            const auto policy = keyconv::parseJackPreservePolicy(value);
            if (!policy.has_value()) {
                throw std::runtime_error("Invalid jack preserve policy: " + value);
            }
            options.jackPreservePolicy = *policy;
        } else if (arg == "--jack-window-ms") {
            options.jackWindowMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--strict-jack-window-ms") {
            options.strictJackWindowMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--max-jack-split-lanes") {
            options.maxJackSplitLanes = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--playable-jack-split") {
            options.allowPlayableJackSplit = true;
        } else if (arg == "--no-playable-jack-split") {
            options.allowPlayableJackSplit = false;
        } else if (arg == "--gesture-rail" || arg == "--motif-preserve") {
            const auto value = requireValue(i, args, arg);
            if (value == "on" || value == "true" || value == "1") {
                options.gestureRailEnabled = true;
            } else if (value == "off" || value == "false" || value == "0") {
                options.gestureRailEnabled = false;
            } else {
                throw std::runtime_error("Invalid gesture rail value: " + value);
            }
        } else if (arg == "--snap-roll") {
            options.snapRolledNotes = true;
        } else if (arg == "--no-snap-roll") {
            options.snapRolledNotes = false;
        } else if (arg == "--snap-tolerance") {
            options.snapToleranceMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--max-roll-ms") {
            options.maxRollMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--expansion-policy") {
            const auto value = requireValue(i, args, arg);
            if (value == "auto") {
                options.expansionPolicyProvided = false;
                continue;
            }
            const auto policy = keyconv::parseExpansionPolicy(value);
            if (!policy.has_value()) {
                throw std::runtime_error("Invalid expansion policy: " + value);
            }
            options.expansionPolicy = *policy;
            options.expansionPolicyProvided = true;
        } else if (arg == "--echo-policy") {
            const auto value = requireValue(i, args, arg);
            const auto policy = keyconv::parseEchoPolicy(value);
            if (!policy.has_value()) {
                throw std::runtime_error("Invalid echo policy: " + value);
            }
            options.echoPolicy = *policy;
        } else if (arg == "--stream-echo-profile") {
            const auto value = requireValue(i, args, arg);
            const auto profile = keyconv::parseStreamEchoProfile(value);
            if (!profile.has_value()) {
                throw std::runtime_error("Invalid stream echo profile: " + value);
            }
            options.streamEchoProfile = *profile;
        } else if (arg == "--echo-diagnostics") {
            options.echoDiagnostics = true;
        } else if (arg == "--max-added-ratio") {
            options.maxAddedNoteRatio = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--max-added-per-slice") {
            options.maxAddedPerSlice = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--max-added-per-measure") {
            options.maxAddedPerMeasure = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--deterministic-expansion") {
            options.deterministicExpansion = true;
        } else if (arg == "--expansion-min-gap") {
            options.expansionMinGapMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--expansion-same-lane-min-gap") {
            options.expansionSameLaneMinGapMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--snap-added-notes") {
            options.snapAddedNotes = true;
        } else if (arg == "--no-snap-added-notes") {
            options.snapAddedNotes = false;
        } else if (arg == "--expansion-snap-tolerance") {
            options.expansionSnapToleranceMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--max-echo-ratio") {
            options.maxEchoAddedRatio = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--max-echo-per-pattern") {
            options.maxEchoPerPattern = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--max-echo-per-measure") {
            options.maxEchoPerMeasure = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--max-echo-per-slice") {
            options.maxEchoPerSlice = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--min-echo-pattern-length") {
            options.minEchoPatternLength = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--min-pattern-confidence") {
            options.minPatternConfidence = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--echo-requires-snap") {
            options.echoRequiresSnap = true;
        } else if (arg == "--no-echo-requires-snap") {
            options.echoRequiresSnap = false;
        } else if (arg == "--echo-min-gap") {
            options.echoMinGapMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--echo-same-lane-min-gap") {
            options.echoSameLaneMinGapMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--echo-avoid-high-density") {
            options.echoAvoidHighDensity = true;
        } else if (arg == "--no-echo-avoid-high-density") {
            options.echoAvoidHighDensity = false;
        } else if (arg == "--echo-high-density-window") {
            options.echoHighDensityWindowMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--echo-max-local-nps") {
            options.echoMaxLocalNps = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--optimizer") {
            const auto value = requireValue(i, args, arg);
            const auto optimizer = keyconv::parseOptimizerKind(value);
            if (!optimizer.has_value()) {
                throw std::runtime_error("Invalid optimizer: " + value);
            }
            options.optimizer = *optimizer;
        } else if (arg == "--beam-width") {
            options.beamWidth = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--epsilon") {
            options.sameTimeEpsilonMs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--dp") {
            options.dpMode = true;
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else if (arg == "--report") {
            options.report = pathFromArgument(requireValue(i, args, arg));
        } else if (arg == "--target-profile") {
            options.targetProfile = pathFromArgument(requireValue(i, args, arg));
        } else if (arg == "--compare-policies") {
            options.comparePolicies = requireValue(i, args, arg);
        } else if (arg == "--emit-feel-report") {
            options.emitFeelReport = true;
        } else if (arg == "--emit-diff-report") {
            options.emitDiffReport = true;
        } else if (arg == "--report-csv") {
            options.reportCsv = pathFromArgument(requireValue(i, args, arg));
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("Unknown option: " + arg);
        } else if (options.input.empty()) {
            options.input = pathFromArgument(arg);
        } else {
            throw std::runtime_error("Unexpected positional argument: " + arg);
        }
    }

    return options;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Input file not found: " + displayPath(path));
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Could not write file: " + displayPath(path));
    }
    out << text;
}

std::optional<std::string> jsonStringField(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const auto keyPos = text.find(needle);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }
    const auto colon = text.find(':', keyPos + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    auto pos = text.find('"', colon + 1);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    ++pos;
    std::string value;
    bool escaped = false;
    for (; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (escaped) {
            switch (ch) {
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    value.push_back(ch);
                    break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

std::optional<double> jsonNumberField(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const auto keyPos = text.find(needle);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }
    const auto colon = text.find(':', keyPos + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    std::size_t pos = colon + 1;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    try {
        std::size_t consumed = 0;
        const double value = std::stod(text.substr(pos), &consumed);
        if (consumed == 0) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> jsonStringTokenAt(const std::string& text,
                                             std::size_t quotePos,
                                             std::size_t* endQuote) {
    if (quotePos >= text.size() || text[quotePos] != '"') {
        return std::nullopt;
    }
    std::string value;
    bool escaped = false;
    for (std::size_t pos = quotePos + 1; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (escaped) {
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            if (endQuote != nullptr) {
                *endQuote = pos;
            }
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

std::optional<std::string> jsonObjectAt(const std::string& text, std::size_t openBrace) {
    if (openBrace >= text.size() || text[openBrace] != '{') {
        return std::nullopt;
    }

    int depth = 0;
    for (std::size_t pos = openBrace; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (ch == '"') {
            std::size_t endQuote = pos;
            if (!jsonStringTokenAt(text, pos, &endQuote).has_value()) {
                return std::nullopt;
            }
            pos = endQuote;
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(openBrace, pos - openBrace + 1);
            }
            if (depth < 0) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> jsonDirectObjectField(const std::string& text, const std::string& key) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    const int keyDepth = first < text.size() && text[first] == '{' ? 1 : 0;

    int depth = 0;
    for (std::size_t pos = 0; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (ch == '"') {
            std::size_t endQuote = pos;
            const auto token = jsonStringTokenAt(text, pos, &endQuote);
            if (!token.has_value()) {
                return std::nullopt;
            }
            if (depth == keyDepth && *token == key) {
                std::size_t valuePos = endQuote + 1;
                while (valuePos < text.size() &&
                       std::isspace(static_cast<unsigned char>(text[valuePos]))) {
                    ++valuePos;
                }
                if (valuePos >= text.size() || text[valuePos] != ':') {
                    return std::nullopt;
                }
                ++valuePos;
                while (valuePos < text.size() &&
                       std::isspace(static_cast<unsigned char>(text[valuePos]))) {
                    ++valuePos;
                }
                return jsonObjectAt(text, valuePos);
            }
            pos = endQuote;
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
        }
    }
    return std::nullopt;
}

keyconv::TargetKFeatureStat loadTargetKFeatureStat(const std::string& featuresBody,
                                                   const std::string& key) {
    keyconv::TargetKFeatureStat stat;
    const auto statBody = jsonDirectObjectField(featuresBody, key);
    if (!statBody.has_value()) {
        return stat;
    }

    stat.present = true;
    if (const auto value = jsonNumberField(*statBody, "mean"); value.has_value()) {
        stat.mean = *value;
    }
    if (const auto value = jsonNumberField(*statBody, "median"); value.has_value()) {
        stat.median = *value;
    }
    if (const auto value = jsonNumberField(*statBody, "iqr"); value.has_value()) {
        stat.iqr = *value;
    }
    if (const auto value = jsonNumberField(*statBody, "p10"); value.has_value()) {
        stat.p10 = *value;
    }
    if (const auto value = jsonNumberField(*statBody, "p25"); value.has_value()) {
        stat.p25 = *value;
    }
    if (const auto value = jsonNumberField(*statBody, "p75"); value.has_value()) {
        stat.p75 = *value;
    }
    if (const auto value = jsonNumberField(*statBody, "p90"); value.has_value()) {
        stat.p90 = *value;
    }
    return stat;
}

keyconv::TargetKBucketProfile loadTargetKBucketProfile(const std::string& bucketsBody,
                                                       const std::string& key) {
    keyconv::TargetKBucketProfile bucket;
    const auto bucketBody = jsonDirectObjectField(bucketsBody, key);
    if (!bucketBody.has_value()) {
        return bucket;
    }

    bucket.present = true;
    if (const auto value = jsonNumberField(*bucketBody, "windowCount"); value.has_value()) {
        bucket.windowCount = static_cast<int>(std::lround(*value));
    }
    const auto featuresBody = jsonDirectObjectField(*bucketBody, "features");
    if (!featuresBody.has_value()) {
        return bucket;
    }
    bucket.activeLaneRate = loadTargetKFeatureStat(*featuresBody, "activeLaneRate");
    bucket.adjacentExpansion = loadTargetKFeatureStat(*featuresBody, "adjacentExpansion");
    bucket.chordRate = loadTargetKFeatureStat(*featuresBody, "chordRate");
    bucket.chordSpan = loadTargetKFeatureStat(*featuresBody, "chordSpan");
    bucket.densityNps = loadTargetKFeatureStat(*featuresBody, "densityNps");
    bucket.edgeUsage = loadTargetKFeatureStat(*featuresBody, "edgeUsage");
    bucket.handBalance = loadTargetKFeatureStat(*featuresBody, "handBalance");
    bucket.holdRate = loadTargetKFeatureStat(*featuresBody, "holdRate");
    bucket.jackRisk = loadTargetKFeatureStat(*featuresBody, "jackRisk");
    bucket.laneEntropy = loadTargetKFeatureStat(*featuresBody, "laneEntropy");
    return bucket;
}

keyconv::TargetKDensityBuckets loadTargetKDensityBuckets(const std::string& text) {
    keyconv::TargetKDensityBuckets buckets;
    const auto bucketsBody = jsonDirectObjectField(text, "densityBuckets");
    if (!bucketsBody.has_value()) {
        return buckets;
    }

    buckets.present = true;
    if (const auto cutsBody = jsonDirectObjectField(*bucketsBody, "densityCuts"); cutsBody.has_value()) {
        if (const auto value = jsonNumberField(*cutsBody, "lowMaxNps"); value.has_value()) {
            buckets.lowMaxNps = *value;
        }
        if (const auto value = jsonNumberField(*cutsBody, "midMaxNps"); value.has_value()) {
            buckets.midMaxNps = *value;
        }
    }
    buckets.all = loadTargetKBucketProfile(*bucketsBody, "all");
    buckets.low = loadTargetKBucketProfile(*bucketsBody, "low");
    buckets.mid = loadTargetKBucketProfile(*bucketsBody, "mid");
    buckets.high = loadTargetKBucketProfile(*bucketsBody, "high");
    buckets.lnHeavy = loadTargetKBucketProfile(*bucketsBody, "lnHeavy");
    buckets.chordHeavy = loadTargetKBucketProfile(*bucketsBody, "chordHeavy");
    buckets.jackRisk = loadTargetKBucketProfile(*bucketsBody, "jackRisk");
    return buckets;
}

keyconv::TargetKProfile loadTargetKProfile(const std::filesystem::path& path) {
    const auto text = readFile(path);
    keyconv::TargetKProfile profile;
    profile.sourceName = displayPath(path);

    if (const auto value = jsonNumberField(text, "targetKeys"); value.has_value()) {
        profile.targetKeys = static_cast<int>(std::lround(*value));
    }
    if (const auto value = jsonNumberField(text, "sampleCount"); value.has_value()) {
        profile.sampleCount = static_cast<int>(std::lround(*value));
    }
    if (const auto value = jsonNumberField(text, "windowMs"); value.has_value()) {
        profile.windowMs = static_cast<int>(std::lround(*value));
    }
    if (const auto value = jsonStringField(text, "profileName"); value.has_value()) {
        profile.profileName = *value;
    }
    if (const auto value = jsonStringField(text, "profileKind"); value.has_value()) {
        profile.profileKind = *value;
    }
    if (const auto value = jsonStringField(text, "sourceName"); value.has_value()) {
        profile.sourceName = *value;
    }
    if (const auto value = jsonStringField(text, "authorToken"); value.has_value()) {
        profile.authorToken = *value;
    }
    if (const auto value = jsonNumberField(text, "desiredLaneEntropy"); value.has_value()) {
        profile.desiredLaneEntropy = *value;
    }
    if (const auto value = jsonNumberField(text, "desiredEdgeUsage"); value.has_value()) {
        profile.desiredEdgeUsage = *value;
    }
    if (const auto value = jsonNumberField(text, "desiredActiveLaneRate"); value.has_value()) {
        profile.desiredActiveLaneRate = *value;
    }
    if (const auto value = jsonNumberField(text, "desiredChordSpan"); value.has_value()) {
        profile.desiredChordSpan = *value;
    }
    if (const auto value = jsonNumberField(text, "desiredHandBalance"); value.has_value()) {
        profile.desiredHandBalance = *value;
    }
    if (const auto value = jsonNumberField(text, "desiredAdjacentExpansion"); value.has_value()) {
        profile.desiredAdjacentExpansion = *value;
    }
    profile.densityBuckets = loadTargetKDensityBuckets(text);

    if (profile.targetKeys <= 0 || profile.sampleCount <= 0) {
        throw std::runtime_error("Target profile must include positive targetKeys and sampleCount: " +
                                 displayPath(path));
    }
    return profile;
}

std::optional<std::filesystem::path> bundledTargetProfilePath(int targetKeyCount) {
    if (targetKeyCount != 10) {
        return std::nullopt;
    }

    const auto relativePath = std::filesystem::path("profiles") / "keyweaver_10k_broad_style_v1.json";
    std::vector<std::filesystem::path> roots;
#if defined(_WIN32)
    const auto exePath = currentExecutablePath();
    if (!exePath.empty()) {
        roots.push_back(exePath.parent_path());
    }
#endif
    std::error_code currentPathError;
    const auto cwd = std::filesystem::current_path(currentPathError);
    if (!currentPathError) {
        roots.push_back(cwd);
    }

    for (const auto& root : roots) {
        const auto candidate = root / relativePath;
        std::error_code existsError;
        if (std::filesystem::is_regular_file(candidate, existsError)) {
            return candidate;
        }
    }
    return std::nullopt;
}

void validateOptions(const CliOptions& options) {
    if (options.help) {
        return;
    }
    if (options.input.empty()) {
        throw std::runtime_error("Input file is required");
    }
    if (!options.target.has_value()) {
        throw std::runtime_error("--target is required");
    }
    if (*options.target < 1 || *options.target > 32) {
        throw std::runtime_error("--target must be between 1 and 32");
    }
    if (options.source.has_value() && (*options.source < 1 || *options.source > 32)) {
        throw std::runtime_error("--source must be between 1 and 32");
    }
    if (options.comparePolicies.has_value() && options.comparePolicies->empty()) {
        throw std::runtime_error("--compare-policies must not be empty");
    }
    if (isBmsPath(options.input) && options.out.has_value() && !isBmsPath(*options.out)) {
        throw std::runtime_error("BMS input can only write BMS-family output (.bms, .bme, .bml, .pms)");
    }
    if (options.beamWidth < 1) {
        throw std::runtime_error("--beam-width must be at least 1");
    }
    if (options.sameTimeEpsilonMs < 0) {
        throw std::runtime_error("--epsilon must be non-negative");
    }
    if (options.minObjectGapMs < 0) {
        throw std::runtime_error("--min-gap must be non-negative");
    }
    if (options.sameLaneMinGapMs < 0) {
        throw std::runtime_error("--same-lane-min-gap must be non-negative");
    }
    if (options.jackWindowMs < 0) {
        throw std::runtime_error("--jack-window-ms must be non-negative");
    }
    if (options.strictJackWindowMs < 0) {
        throw std::runtime_error("--strict-jack-window-ms must be non-negative");
    }
    if (options.maxJackSplitLanes < 1) {
        throw std::runtime_error("--max-jack-split-lanes must be at least 1");
    }
    if (options.snapToleranceMs < 0) {
        throw std::runtime_error("--snap-tolerance must be non-negative");
    }
    if (options.maxRollMs < 0) {
        throw std::runtime_error("--max-roll-ms must be non-negative");
    }
    if (options.maxAddedNoteRatio < 0.0) {
        throw std::runtime_error("--max-added-ratio must be non-negative");
    }
    if (options.maxAddedPerSlice < 0) {
        throw std::runtime_error("--max-added-per-slice must be non-negative");
    }
    if (options.maxAddedPerMeasure < 0) {
        throw std::runtime_error("--max-added-per-measure must be non-negative");
    }
    if (options.expansionMinGapMs < 0) {
        throw std::runtime_error("--expansion-min-gap must be non-negative");
    }
    if (options.expansionSameLaneMinGapMs < 0) {
        throw std::runtime_error("--expansion-same-lane-min-gap must be non-negative");
    }
    if (options.expansionSnapToleranceMs < 0) {
        throw std::runtime_error("--expansion-snap-tolerance must be non-negative");
    }
    if (options.maxEchoAddedRatio < 0.0) {
        throw std::runtime_error("--max-echo-ratio must be non-negative");
    }
    if (options.maxEchoPerPattern < 0) {
        throw std::runtime_error("--max-echo-per-pattern must be non-negative");
    }
    if (options.maxEchoPerMeasure < 0) {
        throw std::runtime_error("--max-echo-per-measure must be non-negative");
    }
    if (options.maxEchoPerSlice < 0) {
        throw std::runtime_error("--max-echo-per-slice must be non-negative");
    }
    if (options.minEchoPatternLength < 0) {
        throw std::runtime_error("--min-echo-pattern-length must be non-negative");
    }
    if (options.minPatternConfidence < 0.0) {
        throw std::runtime_error("--min-pattern-confidence must be non-negative");
    }
    if (options.echoMinGapMs < 0) {
        throw std::runtime_error("--echo-min-gap must be non-negative");
    }
    if (options.echoSameLaneMinGapMs < 0) {
        throw std::runtime_error("--echo-same-lane-min-gap must be non-negative");
    }
    if (options.echoHighDensityWindowMs < 1) {
        throw std::runtime_error("--echo-high-density-window must be at least 1");
    }
    if (options.echoMaxLocalNps < 0.0) {
        throw std::runtime_error("--echo-max-local-nps must be non-negative");
    }
}

void printEchoDiagnostics(const keyconv::ConversionReport& report, std::ostream& out) {
    const auto& quality = report.quality;
    out << "StreamEcho diagnostics:\n";
    out << "Stage counts:\n";
    out << "- raw pattern candidates: " << quality.streamRawPatternCandidates << "\n";
    out << "- eligible pattern candidates: " << quality.streamEligiblePatternCandidates << "\n";
    out << "- raw lane candidates: " << quality.streamRawLaneCandidates << "\n";
    out << "- safe lane candidates: " << quality.streamSafeLaneCandidates << "\n";
    out << "- accepted: " << quality.streamAcceptedCandidates << "\n";
    out << "Primary rejection:\n";
    out << "- pattern confidence: " << quality.rejectedStreamPrimaryByPatternConfidence << "\n";
    out << "- pattern length: " << quality.rejectedStreamPrimaryByPatternLength << "\n";
    out << "- burst-like: " << quality.rejectedStreamPrimaryByBurst << "\n";
    out << "- jack-heavy: " << quality.rejectedStreamPrimaryByJack << "\n";
    out << "- LN-heavy: " << quality.rejectedStreamPrimaryByLNHeavy << "\n";
    out << "- local NPS: " << quality.rejectedStreamPrimaryByLocalNps << "\n";
    out << "- no underused lane: " << quality.rejectedStreamPrimaryByNoUnderusedLane << "\n";
    out << "- slice chord full: " << quality.rejectedStreamPrimaryBySliceChordFull << "\n";
    out << "- lane role: " << quality.rejectedStreamPrimaryByLaneRole << "\n";
    out << "- collision: " << quality.rejectedStreamPrimaryByCollision << "\n";
    out << "- distance: " << quality.rejectedStreamPrimaryByDistance << "\n";
    out << "- snap: " << quality.rejectedStreamPrimaryBySnap << "\n";
    out << "- budget: " << quality.rejectedStreamPrimaryByBudget << "\n";
    out << "Any rejection:\n";
    out << "- local NPS: " << quality.rejectedStreamEchoByLocalNps << "\n";
    out << "- burst-like: " << quality.rejectedStreamEchoByBurst << "\n";
    out << "- jack-heavy: " << quality.rejectedStreamEchoByJack << "\n";
    out << "- LN-heavy: " << quality.rejectedStreamEchoByLNHeavy << "\n";
    out << "- no underused lane: " << quality.rejectedStreamEchoByNoUnderusedLane << "\n";
    out << "- pattern confidence: " << quality.rejectedStreamEchoByPatternConfidence << "\n";
    out << "- pattern length: " << quality.rejectedStreamEchoByPatternLength << "\n";
    out << "- slice chord full: " << quality.rejectedStreamEchoBySliceChordFull << "\n";
    out << "- lane role: " << quality.rejectedStreamEchoByLaneRole << "\n";
}

std::string trim(const std::string& value) {
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

std::string normalizePolicyToken(const std::string& value) {
    std::string normalized = trim(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        if (std::isspace(ch) || ch == '_') {
            return '-';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return normalized;
}

std::vector<std::string> splitCommaList(const std::string& value) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream in(value);
    while (std::getline(in, current, ',')) {
        current = trim(current);
        if (!current.empty()) {
            parts.push_back(current);
        }
    }
    if (parts.empty()) {
        throw std::runtime_error("--compare-policies did not contain any policy names");
    }
    return parts;
}

keyconv::ExpansionPolicy defaultExpansionPolicy(int sourceKeyCount, int targetKeyCount) {
    return targetKeyCount > sourceKeyCount ? keyconv::ExpansionPolicy::PreserveTapPlus
                                           : keyconv::ExpansionPolicy::PreserveNoteCount;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << ch;
                break;
        }
    }
    return out.str();
}

std::string csvEscape(const std::string& value) {
    const bool needsQuotes = value.find_first_of(",\"\n\r") != std::string::npos;
    if (!needsQuotes) {
        return value;
    }
    std::ostringstream out;
    out << '"';
    for (const char ch : value) {
        if (ch == '"') {
            out << "\"\"";
        } else {
            out << ch;
        }
    }
    out << '"';
    return out.str();
}

std::string joinTags(const std::vector<std::string>& tags) {
    std::ostringstream out;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) {
            out << "|";
        }
        out << tags[i];
    }
    return out.str();
}

struct PolicyComparisonRow {
    std::string policy;
    keyconv::ConvertOptions options;
    keyconv::ConversionReport report;
};

keyconv::ConvertOptions optionsForPolicyToken(const std::string& token,
                                              const keyconv::ConvertOptions& baseOptions) {
    const auto normalized = normalizePolicyToken(token);
    auto options = baseOptions;

    if (normalized == "preserve" || normalized == "preserve-note-count") {
        options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
        options.echoPolicy = keyconv::EchoPolicy::Off;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Conservative;
        return options;
    }
    if (normalized == "tap-plus" || normalized == "preserve-tap-plus") {
        options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
        options.echoPolicy = keyconv::EchoPolicy::Off;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Conservative;
        return options;
    }

    if (normalized == "echo" || normalized == "echo-conservative") {
        options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
        options.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Conservative;
        return options;
    }
    if (normalized == "echo-balanced") {
        options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
        options.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Balanced;
        return options;
    }
    if (normalized == "echo-training") {
        options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
        options.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Training;
        return options;
    }

    if (normalized == "training" || normalized == "training-scaffold") {
        options.expansionPolicy = keyconv::ExpansionPolicy::TrainingScaffold;
        options.echoPolicy = keyconv::EchoPolicy::Off;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Training;
        return options;
    }

    if (normalized == "harder" || normalized == "harder-remix") {
        options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
        options.echoPolicy = keyconv::EchoPolicy::Off;
        return options;
    }
    if (normalized == "harder-balanced" || normalized == "harder-remix-balanced") {
        options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
        options.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Balanced;
        return options;
    }
    if (normalized == "harder-training" || normalized == "harder-remix-training") {
        options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
        options.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Training;
        return options;
    }

    const auto expansionPolicy = keyconv::parseExpansionPolicy(normalized);
    if (expansionPolicy.has_value()) {
        options.expansionPolicy = *expansionPolicy;
        if (*expansionPolicy == keyconv::ExpansionPolicy::DeterministicEcho) {
            options.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
        }
        return options;
    }

    throw std::runtime_error("Invalid comparison policy: " + token);
}

std::vector<PolicyComparisonRow> runPolicyComparison(const keyconv::Chart& chart,
                                                     const keyconv::ConvertOptions& baseOptions,
                                                     const std::string& policyList) {
    const auto tokens = splitCommaList(policyList);
    const keyconv::Converter converter;
    std::vector<PolicyComparisonRow> rows;
    for (const auto& token : tokens) {
        PolicyComparisonRow row;
        row.policy = normalizePolicyToken(token);
        row.options = optionsForPolicyToken(token, baseOptions);
        row.report = converter.convert(chart, row.options).report;
        rows.push_back(std::move(row));
    }
    return rows;
}

bool comparisonSafetyOk(const keyconv::QualityReport& quality) {
    return quality.collisionCount == 0 && quality.lnConflictCount == 0 &&
           quality.nearTimeConflicts == 0 && quality.unsnappedAddedNotes == 0 && quality.createdJacks == 0;
}

std::string comparisonToCsv(const std::vector<PolicyComparisonRow>& rows) {
    std::ostringstream out;
    out << std::setprecision(10);
    out << "policy,expansionPolicy,composerProfile,totalNotes,addedNotes,addedByTapPlus,addedNoteRatio,kLikenessScore,"
           "targetProfileChartCount,"
           "adaptiveGrowthBudgetEnabled,adaptiveBudgetAverageRatio,rejectedByAdaptiveBudget,"
           "laneEntropy,"
           "patternPreserveScore,playabilityScore,collisionCount,lnConflictCount,nearTimeConflicts,"
           "unsnappedAddedNotes,sourceJackGroups,preservedJackGroups,splitJackGroups,createdJacks,preventedJacks,"
           "jackPreserveScore,densityDelta,chordRateBefore,chordRateAfter,laneCoverageBefore,"
           "laneCoverageAfter,laneEntropyBefore,laneEntropyAfter,jackRateBefore,jackRateAfter,"
           "handSpreadAfter,lnAnchorPressureBefore,lnAnchorPressureAfter,safety,feelTags\n";
    for (const auto& row : rows) {
        const auto& q = row.report.quality;
        out << csvEscape(row.policy) << ","
            << csvEscape(q.expansionPolicy) << ","
            << csvEscape(q.expansionComposerProfile) << ","
            << row.report.totalNotes << ","
            << q.addedNotes << ","
            << q.addedByTapPlus << ","
            << q.addedNoteRatio << ","
            << q.kLikenessScore << ","
            << q.targetProfileChartCount << ","
            << (q.adaptiveGrowthBudgetEnabled ? 1 : 0) << ","
            << q.adaptiveBudgetAverageRatio << ","
            << q.rejectedByAdaptiveBudget << ","
            << q.laneEntropy << ","
            << q.patternPreserveScore << ","
            << q.playabilityScore << ","
            << q.collisionCount << ","
            << q.lnConflictCount << ","
            << q.nearTimeConflicts << ","
            << q.unsnappedAddedNotes << ","
            << q.sourceJackGroups << ","
            << q.preservedJackGroups << ","
            << q.splitJackGroups << ","
            << q.createdJacks << ","
            << q.preventedJacks << ","
            << q.jackPreserveScore << ","
            << q.densityDelta << ","
            << q.chordRateBefore << ","
            << q.chordRateAfter << ","
            << q.laneCoverageBefore << ","
            << q.laneCoverageAfter << ","
            << q.laneEntropyBefore << ","
            << q.laneEntropyAfter << ","
            << q.jackRateBefore << ","
            << q.jackRateAfter << ","
            << q.handSpreadAfter << ","
            << q.lnAnchorPressureBefore << ","
            << q.lnAnchorPressureAfter << ","
            << (comparisonSafetyOk(q) ? "ok" : "check") << ","
            << csvEscape(joinTags(q.feelTags)) << "\n";
    }
    return out.str();
}

std::string comparisonToJson(const std::vector<PolicyComparisonRow>& rows,
                             int sourceKeyCount,
                             int targetKeyCount) {
    std::ostringstream out;
    out << std::setprecision(10);
    out << "{\n";
    out << "  \"toolName\": \"" << kToolName << "\",\n";
    out << "  \"toolVersion\": \"" << kToolVersion << "\",\n";
    out << "  \"sourceKeyCount\": " << sourceKeyCount << ",\n";
    out << "  \"targetKeyCount\": " << targetKeyCount << ",\n";
    out << "  \"policies\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const auto& q = row.report.quality;
        out << "    {\n";
        out << "      \"policy\": \"" << jsonEscape(row.policy) << "\",\n";
        out << "      \"expansionPolicy\": \"" << jsonEscape(q.expansionPolicy) << "\",\n";
        out << "      \"streamEchoProfile\": \"" << jsonEscape(q.streamEchoProfile) << "\",\n";
        out << "      \"composerProfile\": \"" << jsonEscape(q.expansionComposerProfile) << "\",\n";
        out << "      \"totalNotes\": " << row.report.totalNotes << ",\n";
        out << "      \"addedNotes\": " << q.addedNotes << ",\n";
        out << "      \"addedByTapPlus\": " << q.addedByTapPlus << ",\n";
        out << "      \"addedNoteRatio\": " << q.addedNoteRatio << ",\n";
        out << "      \"kLikenessScore\": " << q.kLikenessScore << ",\n";
        out << "      \"targetProfileChartCount\": " << q.targetProfileChartCount << ",\n";
        out << "      \"targetProfileWindowMs\": " << q.targetProfileWindowMs << ",\n";
        out << "      \"targetProfileName\": \"" << jsonEscape(q.targetProfileName) << "\",\n";
        out << "      \"targetProfileKind\": \"" << jsonEscape(q.targetProfileKind) << "\",\n";
        out << "      \"targetProfileSource\": \"" << jsonEscape(q.targetProfileSource) << "\",\n";
        out << "      \"targetProfileAuthor\": \"" << jsonEscape(q.targetProfileAuthor) << "\",\n";
        out << "      \"adaptiveGrowthBudgetEnabled\": "
            << (q.adaptiveGrowthBudgetEnabled ? "true" : "false") << ",\n";
        out << "      \"adaptiveBudgetWindowMs\": " << q.adaptiveBudgetWindowMs << ",\n";
        out << "      \"adaptiveBudgetWindows\": " << q.adaptiveBudgetWindows << ",\n";
        out << "      \"adaptiveBudgetAverageRatio\": " << q.adaptiveBudgetAverageRatio << ",\n";
        out << "      \"adaptiveBudgetMinRatio\": " << q.adaptiveBudgetMinRatio << ",\n";
        out << "      \"adaptiveBudgetMaxRatio\": " << q.adaptiveBudgetMaxRatio << ",\n";
        out << "      \"rejectedByAdaptiveBudget\": " << q.rejectedByAdaptiveBudget << ",\n";
        out << "      \"laneCoverageScore\": " << q.laneCoverageScore << ",\n";
        out << "      \"laneEntropyScore\": " << q.laneEntropyScore << ",\n";
        out << "      \"edgeUsageScore\": " << q.edgeUsageScore << ",\n";
        out << "      \"activeLaneWindowScore\": " << q.activeLaneWindowScore << ",\n";
        out << "      \"spatialSpanScore\": " << q.spatialSpanScore << ",\n";
        out << "      \"adjacentExpansionScore\": " << q.adjacentExpansionScore << ",\n";
        out << "      \"anchorPreserveScore\": " << q.anchorPreserveScore << ",\n";
        out << "      \"patternVocabularyScore\": " << q.patternVocabularyScore << ",\n";
        out << "      \"addedRatioFitScore\": " << q.addedRatioFitScore << ",\n";
        out << "      \"targetKSafetyScore\": " << q.targetKSafetyScore << ",\n";
        out << "      \"laneEntropy\": " << q.laneEntropy << ",\n";
        out << "      \"patternPreserveScore\": " << q.patternPreserveScore << ",\n";
        out << "      \"playabilityScore\": " << q.playabilityScore << ",\n";
        out << "      \"collisionCount\": " << q.collisionCount << ",\n";
        out << "      \"lnConflictCount\": " << q.lnConflictCount << ",\n";
        out << "      \"nearTimeConflicts\": " << q.nearTimeConflicts << ",\n";
        out << "      \"unsnappedAddedNotes\": " << q.unsnappedAddedNotes << ",\n";
        out << "      \"sourceJackGroups\": " << q.sourceJackGroups << ",\n";
        out << "      \"preservedJackGroups\": " << q.preservedJackGroups << ",\n";
        out << "      \"splitJackGroups\": " << q.splitJackGroups << ",\n";
        out << "      \"createdJacks\": " << q.createdJacks << ",\n";
        out << "      \"preventedJacks\": " << q.preventedJacks << ",\n";
        out << "      \"jackPreserveScore\": " << q.jackPreserveScore << ",\n";
        out << "      \"densityDelta\": " << q.densityDelta << ",\n";
        out << "      \"chordRateBefore\": " << q.chordRateBefore << ",\n";
        out << "      \"chordRateAfter\": " << q.chordRateAfter << ",\n";
        out << "      \"laneCoverageBefore\": " << q.laneCoverageBefore << ",\n";
        out << "      \"laneCoverageAfter\": " << q.laneCoverageAfter << ",\n";
        out << "      \"laneEntropyBefore\": " << q.laneEntropyBefore << ",\n";
        out << "      \"laneEntropyAfter\": " << q.laneEntropyAfter << ",\n";
        out << "      \"jackRateBefore\": " << q.jackRateBefore << ",\n";
        out << "      \"jackRateAfter\": " << q.jackRateAfter << ",\n";
        out << "      \"handSpreadAfter\": " << q.handSpreadAfter << ",\n";
        out << "      \"lnAnchorPressureBefore\": " << q.lnAnchorPressureBefore << ",\n";
        out << "      \"lnAnchorPressureAfter\": " << q.lnAnchorPressureAfter << ",\n";
        out << "      \"safetyOk\": " << (comparisonSafetyOk(q) ? "true" : "false") << ",\n";
        out << "      \"feelTags\": [";
        for (std::size_t tagIndex = 0; tagIndex < q.feelTags.size(); ++tagIndex) {
            if (tagIndex > 0) {
                out << ", ";
            }
            out << "\"" << jsonEscape(q.feelTags[tagIndex]) << "\"";
        }
        out << "]\n";
        out << "    }" << (i + 1 < rows.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

void printPolicyComparison(const std::vector<PolicyComparisonRow>& rows,
                           bool emitFeelReport,
                           bool emitDiffReport,
                           std::ostream& out) {
    out << std::setprecision(6);
    out << "Policy comparison:\n";
    out << "policy,notes,added,ratio,weave,entropy,pattern,playability,safety";
    if (emitDiffReport) {
        out << ",densityDelta,chordBefore,chordAfter,laneCoverageBefore,laneCoverageAfter";
    }
    if (emitFeelReport) {
        out << ",feelTags";
    }
    out << "\n";

    for (const auto& row : rows) {
        const auto& q = row.report.quality;
        out << row.policy << ","
            << row.report.totalNotes << ","
            << q.addedNotes << ","
            << q.addedNoteRatio << ","
            << q.kLikenessScore << ","
            << q.laneEntropy << ","
            << q.patternPreserveScore << ","
            << q.playabilityScore << ","
            << (comparisonSafetyOk(q) ? "ok" : "check");
        if (emitDiffReport) {
            out << ","
                << q.densityDelta << ","
                << q.chordRateBefore << ","
                << q.chordRateAfter << ","
                << q.laneCoverageBefore << ","
                << q.laneCoverageAfter;
        }
        if (emitFeelReport) {
            out << "," << joinTags(q.feelTags);
        }
        out << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = commandLineArgs(argc, argv);
#if defined(_WIN32)
        if (launchGuiForNoArgs(static_cast<int>(args.size()))) {
            return 0;
        }
#endif

        const auto cli = parseArgs(args);
        validateOptions(cli);

        if (cli.help) {
            printHelp(std::cout);
            return 0;
        }

        const auto inputText = readFile(cli.input);
        const bool bmsInput = isBmsPath(cli.input);
        const keyconv::ParseOptions parseOptions{cli.source};
        auto chart = bmsInput ? keyconv::parseBms(inputText, parseOptions)
                              : keyconv::parseOsu(inputText, parseOptions);

        keyconv::ConvertOptions convertOptions;
        convertOptions.sourceKeyCount = cli.source.value_or(chart.meta.sourceKeyCount);
        convertOptions.targetKeyCount = *cli.target;
        convertOptions.style = cli.style;
        convertOptions.collisionPolicy = cli.collision;
        convertOptions.optimizer = cli.optimizer;
        convertOptions.compressPolicy = cli.compressPolicy;
        convertOptions.distancePolicy = cli.distancePolicy;
        convertOptions.expansionPolicy = cli.expansionPolicyProvided
                                             ? cli.expansionPolicy
                                             : defaultExpansionPolicy(convertOptions.sourceKeyCount,
                                                                      convertOptions.targetKeyCount);
        convertOptions.echoPolicy = cli.echoPolicy;
        convertOptions.streamEchoProfile = cli.streamEchoProfile;
        convertOptions.jackPreservePolicy = cli.jackPreservePolicy;
        convertOptions.gestureRailEnabled = cli.gestureRailEnabled;
        convertOptions.echoDiagnostics = cli.echoDiagnostics;
        convertOptions.dpMode = cli.dpMode;
        convertOptions.beamWidth = cli.beamWidth;
        convertOptions.sameTimeEpsilonMs = cli.sameTimeEpsilonMs;
        convertOptions.minObjectGapMs = cli.minObjectGapMs;
        convertOptions.sameLaneMinGapMs = cli.sameLaneMinGapMs;
        convertOptions.jackWindowMs = cli.jackWindowMs;
        convertOptions.strictJackWindowMs = cli.strictJackWindowMs;
        convertOptions.allowPlayableJackSplit = cli.allowPlayableJackSplit;
        convertOptions.maxJackSplitLanes = cli.maxJackSplitLanes;
        convertOptions.snapRolledNotes = cli.snapRolledNotes;
        convertOptions.snapToleranceMs = cli.snapToleranceMs;
        convertOptions.maxRollMs = cli.maxRollMs;
        convertOptions.maxAddedNoteRatio = cli.maxAddedNoteRatio;
        convertOptions.maxAddedPerSlice = cli.maxAddedPerSlice;
        convertOptions.maxAddedPerMeasure = cli.maxAddedPerMeasure;
        convertOptions.deterministicExpansion = cli.deterministicExpansion;
        convertOptions.expansionMinGapMs = cli.expansionMinGapMs;
        convertOptions.expansionSameLaneMinGapMs = cli.expansionSameLaneMinGapMs;
        convertOptions.snapAddedNotes = cli.snapAddedNotes;
        convertOptions.expansionSnapToleranceMs = cli.expansionSnapToleranceMs;
        convertOptions.maxEchoAddedRatio = cli.maxEchoAddedRatio;
        convertOptions.maxEchoPerPattern = cli.maxEchoPerPattern;
        convertOptions.maxEchoPerMeasure = cli.maxEchoPerMeasure;
        convertOptions.maxEchoPerSlice = cli.maxEchoPerSlice;
        convertOptions.minEchoPatternLength = cli.minEchoPatternLength;
        convertOptions.minPatternConfidence = cli.minPatternConfidence;
        convertOptions.echoRequiresSnap = cli.echoRequiresSnap;
        convertOptions.echoMinGapMs = cli.echoMinGapMs;
        convertOptions.echoSameLaneMinGapMs = cli.echoSameLaneMinGapMs;
        convertOptions.echoAvoidHighDensity = cli.echoAvoidHighDensity;
        convertOptions.echoHighDensityWindowMs = cli.echoHighDensityWindowMs;
        convertOptions.echoMaxLocalNps = cli.echoMaxLocalNps;
        auto profilePath = cli.targetProfile;
        if (!profilePath.has_value()) {
            profilePath = bundledTargetProfilePath(convertOptions.targetKeyCount);
        }
        if (profilePath.has_value()) {
            auto profile = loadTargetKProfile(*profilePath);
            if (profile.targetKeys != convertOptions.targetKeyCount) {
                throw std::runtime_error("Target profile key count does not match --target");
            }
            convertOptions.targetKProfile = std::move(profile);
        }

        if (cli.comparePolicies.has_value()) {
            const auto rows = runPolicyComparison(chart, convertOptions, *cli.comparePolicies);
            std::cout << kToolName << " " << kToolVersion << "\n";
            std::cout << "Input: " << displayPath(cli.input) << "\n";
            std::cout << "Mode: " << (bmsInput ? "BMS policy comparison" : "osu!mania policy comparison") << "\n";
            std::cout << "Source keys: " << convertOptions.sourceKeyCount << "\n";
            std::cout << "Target keys: " << convertOptions.targetKeyCount << "\n\n";
        if (convertOptions.targetKProfile.has_value()) {
            std::cout << "Target profile: " << convertOptions.targetKProfile->profileName << " / "
                      << convertOptions.targetKProfile->sourceName << " ("
                      << convertOptions.targetKProfile->sampleCount << " charts)\n";
        }
            printPolicyComparison(rows, cli.emitFeelReport, cli.emitDiffReport, std::cout);

            if (cli.report.has_value()) {
                writeFile(*cli.report,
                          comparisonToJson(rows, convertOptions.sourceKeyCount, convertOptions.targetKeyCount));
                std::cout << "Comparison report written: " << displayPath(*cli.report) << "\n";
            }
            if (cli.reportCsv.has_value()) {
                writeFile(*cli.reportCsv, comparisonToCsv(rows));
                std::cout << "Comparison CSV written: " << displayPath(*cli.reportCsv) << "\n";
            }

            if (cli.verbose) {
                for (const auto& row : rows) {
                    if (row.report.warnings.empty()) {
                        continue;
                    }
                    std::cout << "\nVerbose warnings for " << row.policy << ":\n";
                    for (const auto& warning : row.report.warnings) {
                        std::cout << "- " << warning << "\n";
                    }
                }
            }

            return 0;
        }

        const keyconv::Converter converter;
        const auto result = converter.convert(chart, convertOptions);
        auto outputPath = cli.out;
        if (!cli.dryRun && !outputPath.has_value()) {
            outputPath = defaultOutputPath(cli.input, convertOptions.targetKeyCount);
        }

        std::cout << kToolName << " " << kToolVersion << "\n";
        std::cout << "Input: " << displayPath(cli.input) << "\n";
        std::cout << "Mode: " << (bmsInput ? "BMS" : "osu!mania") << "\n";
        std::cout << "Source keys: " << convertOptions.sourceKeyCount << "\n";
        std::cout << "Target keys: " << convertOptions.targetKeyCount << "\n";
        std::cout << "Style: " << keyconv::toString(convertOptions.style) << "\n";
        std::cout << "Collision policy: " << keyconv::toString(convertOptions.collisionPolicy) << "\n";
        std::cout << "Compress policy: " << keyconv::toString(convertOptions.compressPolicy) << "\n";
        std::cout << "Distance policy: " << keyconv::toString(convertOptions.distancePolicy) << "\n";
        std::cout << "Expansion policy: " << keyconv::toString(convertOptions.expansionPolicy) << "\n";
        std::cout << "Echo policy: " << keyconv::toString(convertOptions.echoPolicy) << "\n";
        std::cout << "Stream echo profile: " << keyconv::toString(convertOptions.streamEchoProfile) << "\n";
        std::cout << "Echo diagnostics: " << (convertOptions.echoDiagnostics ? "yes" : "no") << "\n";
        std::cout << "Optimizer: " << keyconv::toString(convertOptions.optimizer) << "\n";
        std::cout << "Same-time epsilon: " << convertOptions.sameTimeEpsilonMs << " ms\n";
        std::cout << "Min object gap: " << convertOptions.minObjectGapMs << " ms\n";
        std::cout << "Same-lane min gap: " << convertOptions.sameLaneMinGapMs << " ms\n";
        std::cout << "Jack preserve policy: " << keyconv::toString(convertOptions.jackPreservePolicy) << "\n";
        std::cout << "Gesture rail: " << (convertOptions.gestureRailEnabled ? "on" : "off") << "\n";
        std::cout << "Jack window: " << convertOptions.jackWindowMs << " ms\n";
        std::cout << "Strict jack window: " << convertOptions.strictJackWindowMs << " ms\n";
        std::cout << "Playable jack split: " << (convertOptions.allowPlayableJackSplit ? "yes" : "no") << "\n";
        std::cout << "Max jack split lanes: " << convertOptions.maxJackSplitLanes << "\n";
        std::cout << "Snap rolled notes: " << (convertOptions.snapRolledNotes ? "yes" : "no") << "\n";
        std::cout << "Snap tolerance: " << convertOptions.snapToleranceMs << " ms\n";
        std::cout << "Max roll: " << convertOptions.maxRollMs << " ms\n";
        std::cout << "Max added ratio: " << convertOptions.maxAddedNoteRatio << "\n";
        std::cout << "Max added per slice: " << convertOptions.maxAddedPerSlice << "\n";
        std::cout << "Max added per measure: " << convertOptions.maxAddedPerMeasure << "\n";
        std::cout << "Expansion min gap: " << convertOptions.expansionMinGapMs << " ms\n";
        std::cout << "Expansion same-lane min gap: " << convertOptions.expansionSameLaneMinGapMs << " ms\n";
        std::cout << "Snap added notes: " << (convertOptions.snapAddedNotes ? "yes" : "no") << "\n";
        std::cout << "Expansion snap tolerance: " << convertOptions.expansionSnapToleranceMs << " ms\n";
        std::cout << "Max echo ratio: " << convertOptions.maxEchoAddedRatio << "\n";
        std::cout << "Max echo per pattern: " << convertOptions.maxEchoPerPattern << "\n";
        std::cout << "Max echo per measure: " << convertOptions.maxEchoPerMeasure << "\n";
        std::cout << "Max echo per slice: " << convertOptions.maxEchoPerSlice << "\n";
        std::cout << "Echo min gap: " << convertOptions.echoMinGapMs << " ms\n";
        std::cout << "Echo same-lane min gap: " << convertOptions.echoSameLaneMinGapMs << " ms\n";
        std::cout << "Echo max local NPS: " << convertOptions.echoMaxLocalNps << "\n";
        if (convertOptions.targetKProfile.has_value()) {
            std::cout << "Target profile: " << convertOptions.targetKProfile->profileName << " / "
                      << convertOptions.targetKProfile->sourceName << " ("
                      << convertOptions.targetKProfile->sampleCount << " charts";
            if (!convertOptions.targetKProfile->authorToken.empty()) {
                std::cout << ", author " << convertOptions.targetKProfile->authorToken;
            }
            std::cout << ")\n";
        }
        if (convertOptions.dpMode) {
            std::cout << "DP mode: requested\n";
        }
        std::cout << "\n";
        std::cout << keyconv::reportToText(result.report) << "\n";
        if (cli.echoDiagnostics) {
            printEchoDiagnostics(result.report, std::cout);
            std::cout << "\n";
        }

        if (!cli.dryRun) {
            const auto outputText = bmsInput ? keyconv::exportBms(result.chart, convertOptions.targetKeyCount)
                                             : keyconv::exportOsu(result.chart, convertOptions.targetKeyCount);
            writeFile(*outputPath, outputText);
            std::cout << "Output written: " << displayPath(*outputPath) << "\n";
        } else {
            std::cout << "Dry run: output file not written\n";
        }

        if (cli.report.has_value()) {
            writeFile(*cli.report, keyconv::reportToJson(result.report));
            std::cout << "Report written: " << displayPath(*cli.report) << "\n";
        }

        if (cli.verbose && !result.report.warnings.empty()) {
            std::cout << "\nVerbose warnings:\n";
            for (const auto& warning : result.report.warnings) {
                std::cout << "- " << warning << "\n";
            }
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        std::cerr << "Run with --help for usage.\n";
        return 1;
    }
}
