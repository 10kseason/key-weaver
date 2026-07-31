#include <keyconv/converter.hpp>
#include <keyconv/format/bms_exporter.hpp>
#include <keyconv/format/bms_parser.hpp>
#include <keyconv/format/osu_exporter.hpp>
#include <keyconv/format/osu_parser.hpp>
#include <keyconv/quality_report.hpp>
#include <keyconv/reconvert_guard.hpp>

#include "nk2/nk2_convert.hpp"
#include "nk2/nk2_report.hpp"
#if defined(KEYWEAVER_WITH_ONNXRUNTIME)
#include "onnx/onnx_cuda_policy.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
constexpr const char* kToolVersion = "v1.2.0";
constexpr const char* kDefaultBatchOnnxPolicyModel = "lane_policy_student_mlp_u_e_circusgalop.onnx";
constexpr const char* kLegacyBatchOnnxPolicyModel = "u_e_circusgalop_chart_dataset_lane_policy.onnx";
using SteadyClock = std::chrono::steady_clock;

class ConvertedInputError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SourceKeyFilterSkip : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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

std::wstring quoteShellArg(std::wstring_view value) {
    std::wstring result = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'"') {
            result += L"\\\"";
        } else {
            result += ch;
        }
    }
    result += L"\"";
    return result;
}

bool launchGuiWithInputs(const std::vector<std::string>& args) {
    if (args.size() <= 1) {
        return false;
    }
    for (std::size_t index = 1; index < args.size(); ++index) {
        if (!args[index].empty() && args[index].front() == '-') {
            return false;
        }
    }

    const auto guiPath = currentExecutablePath().parent_path() / L"keyconv_gui.exe";
    if (!std::filesystem::exists(guiPath)) {
        return false;
    }

    std::wstring params;
    for (std::size_t index = 1; index < args.size(); ++index) {
        if (!params.empty()) {
            params += L" ";
        }
        params += quoteShellArg(wideFromUtf8(args[index]));
    }

    const HINSTANCE result = ShellExecuteW(nullptr, L"open", guiPath.c_str(),
                                           params.empty() ? nullptr : params.c_str(),
                                           guiPath.parent_path().c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
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

long long elapsedMs(SteadyClock::time_point start, SteadyClock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
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
    std::vector<std::filesystem::path> inputs;
    std::optional<int> source;
    std::optional<int> target;
    std::optional<std::filesystem::path> out;
    std::optional<std::filesystem::path> outDir;
    std::optional<std::filesystem::path> inputList;
    keyconv::ConversionStyle style = keyconv::ConversionStyle::Playable;
    keyconv::CollisionPolicy collision = keyconv::CollisionPolicy::ShiftNearest;
    keyconv::OptimizerKind optimizer = keyconv::OptimizerKind::Greedy;
    keyconv::CompressPolicy compressPolicy = keyconv::CompressPolicy::Auto;
    keyconv::DistancePolicy distancePolicy = keyconv::DistancePolicy::AimodSafe;
    keyconv::ExpansionPolicy expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    bool expansionPolicyProvided = false;
    keyconv::EchoPolicy echoPolicy = keyconv::EchoPolicy::Off;
    keyconv::StreamEchoProfile streamEchoProfile = keyconv::StreamEchoProfile::Conservative;
    keyconv::StreamTransformPolicy streamTransformPolicy = keyconv::StreamTransformPolicy::Off;
    keyconv::TenKeyPlannerPolicy tenKeyPlannerPolicy = keyconv::TenKeyPlannerPolicy::Auto;
    unsigned int seed = 0;
    keyconv::JackPreservePolicy jackPreservePolicy = keyconv::JackPreservePolicy::PreservePlayable;
    bool gestureRailEnabled = true;
    bool preserveLaneDrift = false;
    bool echoDiagnostics = false;
    bool dpMode = false;
    bool tenKFullFieldRemix = false;
    double tenKFullFieldRemixDensityCeiling = 1.6;
    int tenKFullFieldRemixPhaseStep = 2;
    int beamWidth = 16;
    int sameTimeEpsilonMs = 2;
    int minObjectGapMs = 16;
    int sameLaneMinGapMs = 20;
    int jackWindowMs = 500;
    int strictJackWindowMs = 500;
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
    bool batchMode = false;
    bool batchQuiet = false;
    std::optional<int> jobs;
    std::optional<std::filesystem::path> onnxPolicy;
    bool autoOnnxPolicy = true;
    bool onnxPolicyAutoSelected = false;
    std::string onnxProvider = "cuda";
    bool onnxPolicyStrict = false;
    int onnxDeviceId = 0;
    int onnxMaxBatchNotes = 512;
    bool onnxMaxBatchNotesProvided = false;
    double onnxAdvisoryWeight = 18.0;
    bool preserveConvert = false;
    keyconv::nk2::Engine engine = keyconv::nk2::Engine::Classic;
    keyconv::nk2::Mode nk2Mode = keyconv::nk2::Mode::Native;
    double nk2NativeWeight = 0.5;
    double nk2RemixWeight = 0.5;
    int nk2LayoutWeightPanel = 3;
    int nk2LayoutWeightBridge = 2;
    int nk2LayoutWeightFullField = 6;
    bool verbose = false;
    bool help = false;
};

void printHelp(std::ostream& out) {
    out << kToolName << " " << kToolVersion << "\n";
    out << "Usage: KeyWeaver <input.osu|input.bms> [more inputs...] --target <number> [options]\n";
    out << "       keyconv_gui.exe accepts dragged files and uses the GUI Target field for conversion.\n\n";
    out << "Options:\n";
    out << "  --source <number>       Source key count. Overrides CircleSize/BMS key-count inference.\n";
    out << "  --target <number>       Target key count. Required.\n";
    out << "  --out <path>            Output path. Defaults beside input with the KeyWeaver mode marker.\n";
    out << "  --out-dir <path>        Batch output directory. Defaults beside each input chart.\n";
    out << "  --style <style>         direct | expand | compress | playable | faithful | training | dp.\n";
    out << "  --collision <policy>    keep | shift-nearest | merge | drop. Default: shift-nearest.\n";
    out << "  --compress-policy <p>   auto | preserve-strict | no-overlap-drop | no-overlap-roll | no-overlap-hybrid | training-simplify.\n";
    out << "  --distance-policy <p>   off | warn | aimod-safe | strict. Default: aimod-safe.\n";
    out << "  --min-gap <ms>          Minimum positive object distance. Default: 16.\n";
    out << "  --same-lane-min-gap <ms> Minimum positive same-lane distance. Default: 20.\n";
    out << "  --jack-preserve-policy <p> preserve-strict | preserve-playable | avoid-new-jacks | smooth-all. Default: preserve-playable.\n";
    out << "  --ten-key-planner <p> auto | legacy | staged-7-9-10 | staged-7-14-10. Default: auto.\n";
    out << "                          Auto uses the staged 7K -> 9K -> 10K planner for 7K-to-10K playable conversion.\n";
    out << "  --ten-k-fullfield-remix Enable gated 10K Full-Field Mirror-Remix mode.\n";
    out << "  --ten-k-remix-density-ceiling <n> Total-note density ceiling for that mode. Default: 1.6.\n";
    out << "  --ten-k-remix-phase-step <n> Echo phase rotation step for 5-lane zones, 2 or 3. Default: 2.\n";
    out << "  --jack-window-ms <ms>   Window for repeat/jack detection. Default: 500.\n";
    out << "  --strict-jack-window-ms <ms> Strict jack reference window. Default: 500.\n";
    out << "  --max-jack-split-lanes <n> Max target lanes counted as split jack. Default: 2.\n";
    out << "  --no-playable-jack-split Disallow playable jack split accounting.\n";
    out << "  --gesture-rail <on|off> Preserve detected stair/trill/jack gesture rails. Default: on.\n";
    out << "  --snap-roll             Snap rolled notes to timing grid. Default.\n";
    out << "  --no-snap-roll          Allow raw-ms roll candidates and report unsnapped rolled notes.\n";
    out << "  --snap-tolerance <ms>   Snap validation tolerance. Default: 2.\n";
    out << "  --max-roll-ms <ms>      Maximum roll distance from original time. Default: 64.\n";
    out << "  --expansion-policy <p>  auto-more | auto-normal | auto-low | preserve | preserve-tap-plus | chord-fill | echo | training-scaffold | harder-remix | seeded-random.\n";
    out << "                          Default auto-normal: preserve when target <= source, preserve-tap-plus when target > source.\n";
    out << "                          high-key presets target low/normal/more added notes at 10%/15%/20%.\n";
    out << "  --max-added-ratio <n>   Max added notes as source-note ratio. Default: 0.45.\n";
    out << "  --max-added-per-slice <n> Max added notes per source slice. Default: 2.\n";
    out << "  --max-added-per-measure <n> Max added notes per approximate measure. Default: 16.\n";
    out << "  --preserve-convert     Faithful mapping, strict source-jack preservation, and no generated notes.\n";
    out << "  --preserve-lane-drift  Allow Preserve Convert to move stable phrases into adjacent safe lanes.\n";
    out << "  --no-preserve-lane-drift Disable Preserve Convert lane drift.\n";
    out << "  --expansion-min-gap <ms> Minimum positive object gap for added notes. Default: 16.\n";
    out << "  --expansion-same-lane-min-gap <ms> Minimum same-lane gap for added notes. Default: 20.\n";
    out << "  --snap-added-notes      Require added notes to be timing-grid snapped. Default.\n";
    out << "  --no-snap-added-notes   Allow unsnapped added notes and report them.\n";
    out << "  --expansion-snap-tolerance <ms> Snap validation tolerance for added notes. Default: 2.\n";
    out << "  --echo-policy <p>       off | stair | trill | stream | stair-trill | stair-trill-stream | auto.\n";
    out << "  --stream-echo-profile <p> conservative | balanced | training | experimental. Default: conservative.\n";
    out << "  --stream-transform <p>  off | superrandom | full-jitter | super-symmetry.\n";
    out << "                          Superrandom/full-jitter are classic-engine stream transforms; super-symmetry is NK2-only.\n";
    out << "  --seed <n>              Deterministic random seed for stream transforms. Default: 0.\n";
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
    out << "  --batch                 Treat positional chart inputs as a batch. Outputs default beside each input.\n";
    out << "  --input-list <path>     Read batch input chart paths from a UTF-8 newline-delimited list.\n";
    out << "  --batch-quiet           Reduce batch stdout to progress, failures, and summary.\n";
    out << "  --jobs <number>         Batch worker count override. Default: detected CPU thread count.\n";
    out << "  --onnx-policy <path>    Batch-only ONNX lane policy model. Requires KEYWEAVER_WITH_ONNXRUNTIME.\n";
    out << "                          Target-10 batch auto-loads models/" << kDefaultBatchOnnxPolicyModel
        << " when bundled.\n";
    out << "  --no-auto-onnx-policy   Disable bundled ONNX lane policy auto-load.\n";
    out << "  --onnx-provider cuda    ONNX Runtime execution provider. Only cuda is supported in this build path.\n";
    out << "  --onnx-policy-strict    Fail instead of falling back to deterministic batch when CUDA policy fails.\n";
    out << "  --onnx-device <number>  CUDA device id for ONNX Runtime. Default: 0.\n";
    out << "  --onnx-max-batch-notes <number> Split CUDA inference into note chunks. Default: 512; use 0 for all notes.\n";
    out << "  --onnx-advisory-weight <n> Lane-score weight for ONNX advisory lanes. Default: 18.\n";
    out << "  --report <path>         Write conversion report JSON.\n";
    out << "  --target-profile <json> Use a Target-K reference profile JSON for K-likeness scoring.\n";
    out << "                          Target 10 auto-loads profiles/keyweaver_10k_broad_style_v1.json when bundled.\n";
    out << "  --engine <engine>       classic | nk2. Default: classic. NK2 supports experimental 1K..18K conversion.\n";
    out << "  --nk2-mode <mode>       native | faithful | harder | transform | report. Report is analysis-only.\n";
    out << "  --nk2-native-weight <n> Native authorship weight. Default: 0.5.\n";
    out << "  --nk2-remix-weight <n>  Remix expansion weight. Default: 0.5.\n";
    out << "  --nk2-layout-weight-panel <n> target-layout panel weight. Default: 3.\n";
    out << "  --nk2-layout-weight-bridge <n> target-layout bridge weight. Default: 2.\n";
    out << "  --nk2-layout-weight-fullfield <n> target full-field weight. Default: 6.\n";
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

std::string difficultyExpansionTag(const keyconv::ConvertOptions& options) {
    if (options.targetKeyCount <= options.sourceKeyCount) {
        return {};
    }
    switch (options.expansionPolicy) {
        case keyconv::ExpansionPolicy::PreserveTapPlusMore:
            return "more";
        case keyconv::ExpansionPolicy::PreserveTapPlus:
            return "normal";
        case keyconv::ExpansionPolicy::PreserveTapPlusLow:
            return "low";
        default:
            return {};
    }
}

std::string difficultyStreamTag(keyconv::StreamTransformPolicy policy) {
    switch (policy) {
        case keyconv::StreamTransformPolicy::SuperRandom:
            return "sRan";
        case keyconv::StreamTransformPolicy::FullJitter:
            return "jitter";
        case keyconv::StreamTransformPolicy::SuperSymmetry:
            return "sSym";
        case keyconv::StreamTransformPolicy::Off:
            return {};
    }
    return {};
}

std::string keyWeaverConversionMarker(const keyconv::ConvertOptions& options) {
    std::string marker = keyWeaverDifficultyMarker(options.targetKeyCount);
    const auto streamTag = difficultyStreamTag(options.streamTransformPolicy);
    if (!streamTag.empty()) {
        marker += "-";
        marker += streamTag;
    }
    const auto expansionTag = difficultyExpansionTag(options);
    if (!expansionTag.empty()) {
        marker += " (";
        marker += expansionTag;
        marker += ")";
    }
    return marker;
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

std::optional<std::string> convertedMarkerReason(std::string_view field, std::string_view text) {
    const auto kind = keyconv::convertedChartMarkerKind(text);
    if (kind == keyconv::ConvertedChartMarkerKind::None) {
        return std::nullopt;
    }
    std::string reason(field);
    reason += " has ";
    reason += keyconv::convertedChartMarkerLabel(kind);
    return reason;
}

std::optional<std::string> convertedPathMarkerReason(const std::filesystem::path& input) {
    return convertedMarkerReason("filename", displayPath(input.filename()));
}

std::optional<std::string> convertedMetadataMarkerReason(const keyconv::Chart& chart) {
    if (chart.meta.creator.has_value()) {
        if (auto reason = convertedMarkerReason("creator", *chart.meta.creator)) {
            return reason;
        }
    }
    if (chart.meta.version.has_value()) {
        if (auto reason = convertedMarkerReason("difficulty", *chart.meta.version)) {
            return reason;
        }
    }
    return std::nullopt;
}

void throwIfConvertedInput(const std::filesystem::path& input) {
    if (auto reason = convertedPathMarkerReason(input)) {
        throw ConvertedInputError("already-converted chart marker detected: " + *reason);
    }
}

void throwIfConvertedInput(const std::filesystem::path& input, const keyconv::Chart& chart) {
    throwIfConvertedInput(input);
    if (auto reason = convertedMetadataMarkerReason(chart)) {
        throw ConvertedInputError("already-converted chart marker detected: " + *reason);
    }
}

std::filesystem::path defaultChartExtension(const std::filesystem::path& input, int targetKeys) {
    if (isBmsPath(input) && (targetKeys == 9 || targetKeys == 18)) {
        return std::filesystem::path(".pms");
    }
    return input.has_extension() ? input.extension() : std::filesystem::path(".osu");
}

std::filesystem::path defaultOutputPath(const std::filesystem::path& input,
                                        const keyconv::ConvertOptions& options,
                                        const std::optional<std::filesystem::path>& outputDir = std::nullopt) {
    const auto parent = outputDir.has_value()
                            ? *outputDir
                            : input.has_parent_path() ? input.parent_path() : std::filesystem::path(".");
    const auto extension = defaultChartExtension(input, options.targetKeyCount);
    const auto marker = keyWeaverConversionMarker(options);

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

std::filesystem::path defaultNk2OutputPath(const std::filesystem::path& input,
                                           int targetKeys,
                                           keyconv::StreamTransformPolicy streamTransformPolicy,
                                           const std::optional<std::filesystem::path>& outputDir = std::nullopt) {
    const auto parent = outputDir.has_value()
                            ? *outputDir
                            : input.has_parent_path() ? input.parent_path() : std::filesystem::path(".");
    const auto extension = defaultChartExtension(input, targetKeys);
    std::string marker = "KeyWeaverNK2-" + std::to_string(targetKeys) + "K";
    const auto streamTag = difficultyStreamTag(streamTransformPolicy);
    if (!streamTag.empty()) {
        marker += "-";
        marker += streamTag;
    }

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

std::string trimInputListLine(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::vector<std::filesystem::path> readInputListFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Input list not found: " + displayPath(path));
    }

    std::vector<std::filesystem::path> paths;
    std::string line;
    while (std::getline(in, line)) {
        if (paths.empty() && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto trimmed = trimInputListLine(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        paths.push_back(pathFromArgument(trimmed));
    }
    return paths;
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
        } else if (arg == "--out-dir") {
            options.outDir = pathFromArgument(requireValue(i, args, arg));
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
        } else if (arg == "--ten-key-planner") {
            const auto value = requireValue(i, args, arg);
            const auto policy = keyconv::parseTenKeyPlannerPolicy(value);
            if (!policy.has_value()) {
                throw std::runtime_error("Invalid 10K planner: " + value);
            }
            options.tenKeyPlannerPolicy = *policy;
        } else if (arg == "--ten-k-fullfield-remix") {
            options.tenKFullFieldRemix = true;
        } else if (arg == "--ten-k-remix-density-ceiling") {
            options.tenKFullFieldRemixDensityCeiling = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--ten-k-remix-phase-step") {
            options.tenKFullFieldRemixPhaseStep = parseInt(requireValue(i, args, arg), arg);
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
            if (value == "auto" || value == "auto-normal") {
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
        } else if (arg == "--stream-transform") {
            const auto value = requireValue(i, args, arg);
            const auto policy = keyconv::parseStreamTransformPolicy(value);
            if (!policy.has_value()) {
                throw std::runtime_error("Invalid stream transform: " + value);
            }
            options.streamTransformPolicy = *policy;
        } else if (arg == "--seed") {
            const int value = parseInt(requireValue(i, args, arg), arg);
            if (value < 0) {
                throw std::runtime_error("Invalid seed: seed must be non-negative");
            }
            options.seed = static_cast<unsigned int>(value);
        } else if (arg == "--echo-diagnostics") {
            options.echoDiagnostics = true;
        } else if (arg == "--max-added-ratio") {
            options.maxAddedNoteRatio = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--max-added-per-slice") {
            options.maxAddedPerSlice = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--max-added-per-measure") {
            options.maxAddedPerMeasure = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--preserve-convert") {
            options.preserveConvert = true;
            options.style = keyconv::ConversionStyle::Faithful;
            options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
            options.expansionPolicyProvided = true;
            options.jackPreservePolicy = keyconv::JackPreservePolicy::PreserveStrict;
            options.allowPlayableJackSplit = false;
            options.maxAddedNoteRatio = 0.0;
            options.preserveLaneDrift = true;
        } else if (arg == "--preserve-lane-drift") {
            options.preserveLaneDrift = true;
        } else if (arg == "--no-preserve-lane-drift") {
            options.preserveLaneDrift = false;
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
        } else if (arg == "--batch") {
            options.batchMode = true;
        } else if (arg == "--input-list") {
            options.inputList = pathFromArgument(requireValue(i, args, arg));
        } else if (arg == "--batch-quiet") {
            options.batchQuiet = true;
        } else if (arg == "--jobs") {
            options.jobs = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--onnx-policy") {
            options.onnxPolicy = pathFromArgument(requireValue(i, args, arg));
            options.onnxPolicyAutoSelected = false;
            options.batchMode = true;
        } else if (arg == "--no-auto-onnx-policy") {
            options.autoOnnxPolicy = false;
        } else if (arg == "--onnx-provider") {
            options.onnxProvider = requireValue(i, args, arg);
        } else if (arg == "--onnx-policy-strict") {
            options.onnxPolicyStrict = true;
        } else if (arg == "--onnx-device") {
            options.onnxDeviceId = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--onnx-max-batch-notes") {
            options.onnxMaxBatchNotes = parseInt(requireValue(i, args, arg), arg);
            options.onnxMaxBatchNotesProvided = true;
        } else if (arg == "--onnx-advisory-weight") {
            options.onnxAdvisoryWeight = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--report") {
            options.report = pathFromArgument(requireValue(i, args, arg));
        } else if (arg == "--target-profile") {
            options.targetProfile = pathFromArgument(requireValue(i, args, arg));
        } else if (arg == "--engine") {
            options.engine = keyconv::nk2::parseEngineOrThrow(requireValue(i, args, arg));
        } else if (arg == "--nk2-mode") {
            options.nk2Mode = keyconv::nk2::parseModeOrThrow(requireValue(i, args, arg));
        } else if (arg == "--nk2-native-weight") {
            options.nk2NativeWeight = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--nk2-remix-weight") {
            options.nk2RemixWeight = parseDouble(requireValue(i, args, arg), arg);
        } else if (arg == "--nk2-layout-weight-panel") {
            options.nk2LayoutWeightPanel = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--nk2-layout-weight-bridge") {
            options.nk2LayoutWeightBridge = parseInt(requireValue(i, args, arg), arg);
        } else if (arg == "--nk2-layout-weight-fullfield") {
            options.nk2LayoutWeightFullField = parseInt(requireValue(i, args, arg), arg);
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
            options.inputs.push_back(options.input);
        } else {
            options.inputs.push_back(pathFromArgument(arg));
        }
    }

    if (options.inputList.has_value()) {
        auto listedInputs = readInputListFile(*options.inputList);
        options.inputs.insert(options.inputs.end(), listedInputs.begin(), listedInputs.end());
        options.batchMode = true;
    }
    if (options.input.empty() && !options.inputs.empty()) {
        options.input = options.inputs.front();
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

std::optional<int> detectSourceKeyCount(const std::filesystem::path& input) {
    const auto inputText = readFile(input);
    const bool bmsInput = isBmsPath(input);
    const keyconv::ParseOptions parseOptions{};
    const auto chart = bmsInput ? keyconv::parseBms(inputText, parseOptions)
                                : keyconv::parseOsu(inputText, parseOptions);
    if (chart.meta.sourceKeyCount <= 0) {
        return std::nullopt;
    }
    return chart.meta.sourceKeyCount;
}

void throwIfBatchSourceMismatch(const CliOptions& cli, const std::filesystem::path& input) {
    if (!cli.source.has_value()) {
        return;
    }
    const auto detected = detectSourceKeyCount(input);
    if (detected.has_value() && *detected == *cli.source) {
        return;
    }

    std::ostringstream message;
    message << "source key filter " << *cli.source << "K did not match ";
    if (detected.has_value()) {
        message << *detected << "K";
    } else {
        message << "unknown source";
    }
    throw SourceKeyFilterSkip(message.str());
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
    bucket.centerBridgeRate = loadTargetKFeatureStat(*featuresBody, "centerBridgeRate");
    bucket.centerSplitBalance = loadTargetKFeatureStat(*featuresBody, "centerSplitBalance");
    bucket.splitChordRate = loadTargetKFeatureStat(*featuresBody, "splitChordRate");
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
    if (const auto value = jsonNumberField(text, "desiredCenterBridgeRate"); value.has_value()) {
        profile.desiredCenterBridgeRate = *value;
    }
    if (const auto value = jsonNumberField(text, "desiredCenterSplitBalance"); value.has_value()) {
        profile.desiredCenterSplitBalance = *value;
    }
    if (const auto value = jsonNumberField(text, "desiredSplitChordRate"); value.has_value()) {
        profile.desiredSplitChordRate = *value;
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

std::vector<std::filesystem::path> bundledSearchRoots() {
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
    return roots;
}

std::optional<std::filesystem::path> bundledBatchOnnxPolicyPath(int targetKeyCount) {
#if defined(KEYWEAVER_WITH_ONNXRUNTIME)
    if (targetKeyCount != 10) {
        return std::nullopt;
    }

    const std::vector<std::filesystem::path> relativeCandidates = {
        std::filesystem::path("models") / kDefaultBatchOnnxPolicyModel,
        std::filesystem::path("models") / kLegacyBatchOnnxPolicyModel,
        std::filesystem::path(kDefaultBatchOnnxPolicyModel),
        std::filesystem::path("build") / kDefaultBatchOnnxPolicyModel,
    };

    for (const auto& root : bundledSearchRoots()) {
        for (const auto& relativePath : relativeCandidates) {
            const auto candidate = root / relativePath;
            std::error_code existsError;
            if (std::filesystem::is_regular_file(candidate, existsError)) {
                return candidate;
            }
        }
    }
#else
    (void)targetKeyCount;
#endif
    return std::nullopt;
}

void applyDefaultBatchOnnxPolicy(CliOptions& options) {
#if defined(KEYWEAVER_WITH_ONNXRUNTIME)
    const bool batch = options.batchMode || options.inputs.size() > 1;
    if (options.help || !options.autoOnnxPolicy || options.onnxPolicy.has_value() ||
        !options.target.has_value() || !batch || options.engine != keyconv::nk2::Engine::Classic) {
        return;
    }

    const auto policyPath = bundledBatchOnnxPolicyPath(*options.target);
    if (!policyPath.has_value()) {
        return;
    }

    options.onnxPolicy = *policyPath;
    options.onnxPolicyAutoSelected = true;
    options.onnxPolicyStrict = true;
    if (!options.onnxMaxBatchNotesProvided) {
        options.onnxMaxBatchNotes = 0;
    }
#else
    (void)options;
#endif
}

void validateOptions(const CliOptions& options) {
    if (options.help) {
        return;
    }
    if (options.inputs.empty()) {
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
    if (options.engine == keyconv::nk2::Engine::NK2 &&
        *options.target > keyconv::nk2::kMaxSupportedKeyCount) {
        throw std::runtime_error("NK2 --target must be between 1 and 18");
    }
    if (options.engine == keyconv::nk2::Engine::NK2 && options.source.has_value() &&
        *options.source > keyconv::nk2::kMaxSupportedKeyCount) {
        throw std::runtime_error("NK2 --source must be between 1 and 18");
    }
    if (options.tenKFullFieldRemix && *options.target != 10) {
        throw std::runtime_error("--ten-k-fullfield-remix requires --target 10");
    }
    if (options.comparePolicies.has_value() && options.comparePolicies->empty()) {
        throw std::runtime_error("--compare-policies must not be empty");
    }
    const bool batch = options.batchMode || options.inputs.size() > 1;
    if (options.engine == keyconv::nk2::Engine::NK2 && batch &&
        options.nk2Mode == keyconv::nk2::Mode::Report) {
        throw std::runtime_error("--nk2-mode report is single-input only");
    }
    if (options.engine == keyconv::nk2::Engine::NK2 && options.comparePolicies.has_value()) {
        throw std::runtime_error("--compare-policies is only supported by the classic engine");
    }
    if (options.engine == keyconv::nk2::Engine::NK2 && options.reportCsv.has_value()) {
        throw std::runtime_error("--report-csv is only supported by classic policy comparison");
    }
    if (options.engine == keyconv::nk2::Engine::NK2 &&
        (options.streamTransformPolicy == keyconv::StreamTransformPolicy::SuperRandom ||
         options.streamTransformPolicy == keyconv::StreamTransformPolicy::FullJitter)) {
        throw std::runtime_error("--stream-transform superrandom/full-jitter are only supported by the classic engine");
    }
    if (options.engine != keyconv::nk2::Engine::NK2 &&
        options.streamTransformPolicy == keyconv::StreamTransformPolicy::SuperSymmetry) {
        throw std::runtime_error("--stream-transform super-symmetry requires --engine nk2");
    }
    if (options.engine == keyconv::nk2::Engine::NK2 &&
        options.nk2Mode == keyconv::nk2::Mode::Report &&
        (options.out.has_value() || options.outDir.has_value())) {
        throw std::runtime_error("--nk2-mode report is analysis-only and does not write chart output");
    }
    if (options.out.has_value() && options.outDir.has_value()) {
        throw std::runtime_error("--out and --out-dir cannot be used together");
    }
    if (batch && options.out.has_value()) {
        throw std::runtime_error("--out is only valid for one input; batch outputs default beside each input");
    }
    if (batch && options.report.has_value()) {
        throw std::runtime_error("--report is only valid for one input; batch mode writes chart outputs only");
    }
    if (batch && options.reportCsv.has_value()) {
        throw std::runtime_error("--report-csv is only valid for one input");
    }
    if (batch && options.comparePolicies.has_value()) {
        throw std::runtime_error("--compare-policies is only valid for one input");
    }
    if (options.jobs.has_value() && *options.jobs < 1) {
        throw std::runtime_error("--jobs must be at least 1");
    }
    if (!batch && options.jobs.has_value()) {
        throw std::runtime_error("--jobs is only valid for batch mode");
    }
    if (!batch && options.batchQuiet) {
        throw std::runtime_error("--batch-quiet is only valid for batch mode");
    }
    if (options.onnxPolicy.has_value()) {
        if (!batch) {
            throw std::runtime_error("--onnx-policy is only valid for batch mode");
        }
        if (options.engine != keyconv::nk2::Engine::Classic) {
            throw std::runtime_error("--onnx-policy is only supported by the classic engine");
        }
        if (options.onnxProvider != "cuda") {
            throw std::runtime_error("--onnx-provider currently supports cuda only");
        }
        if (options.onnxDeviceId < 0) {
            throw std::runtime_error("--onnx-device must be non-negative");
        }
        if (options.onnxMaxBatchNotes < 0) {
            throw std::runtime_error("--onnx-max-batch-notes must be non-negative");
        }
        if (options.onnxAdvisoryWeight < 0.0) {
            throw std::runtime_error("--onnx-advisory-weight must be non-negative");
        }
#if !defined(KEYWEAVER_WITH_ONNXRUNTIME)
        throw std::runtime_error("--onnx-policy requires a build configured with KEYWEAVER_WITH_ONNXRUNTIME=ON");
#endif
    }
    if (!batch && isBmsPath(options.input) && options.out.has_value() && !isBmsPath(*options.out)) {
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
    if (options.tenKFullFieldRemixDensityCeiling < 1.0) {
        throw std::runtime_error("--ten-k-remix-density-ceiling must be at least 1.0");
    }
    if (options.tenKFullFieldRemixPhaseStep != 2 && options.tenKFullFieldRemixPhaseStep != 3) {
        throw std::runtime_error("--ten-k-remix-phase-step must be 2 or 3");
    }
    if (options.nk2NativeWeight < 0.0 || options.nk2RemixWeight < 0.0) {
        throw std::runtime_error("--nk2-native-weight and --nk2-remix-weight must be non-negative");
    }
    if (options.nk2LayoutWeightPanel < 0 || options.nk2LayoutWeightBridge < 0 ||
        options.nk2LayoutWeightFullField < 0) {
        throw std::runtime_error("--nk2-layout weights must be non-negative");
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

keyconv::ConvertOptions makeConvertOptions(const CliOptions& cli, const keyconv::Chart& chart) {
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
    convertOptions.streamTransformPolicy = cli.streamTransformPolicy;
    convertOptions.tenKeyPlannerPolicy = cli.tenKeyPlannerPolicy;
    convertOptions.seed = cli.seed;
    convertOptions.jackPreservePolicy = cli.jackPreservePolicy;
    convertOptions.gestureRailEnabled = cli.gestureRailEnabled;
    convertOptions.preserveLaneDrift = cli.preserveLaneDrift;
    convertOptions.echoDiagnostics = cli.echoDiagnostics;
    convertOptions.dpMode = cli.dpMode;
    convertOptions.tenKFullFieldRemix = cli.tenKFullFieldRemix;
    convertOptions.tenKFullFieldRemixDensityCeiling = cli.tenKFullFieldRemixDensityCeiling;
    convertOptions.tenKFullFieldRemixPhaseStep = cli.tenKFullFieldRemixPhaseStep;
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
    convertOptions.advisoryLaneWeight = cli.onnxAdvisoryWeight;
    return convertOptions;
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
    if (normalized == "auto-more" || normalized == "tap-plus-more" ||
        normalized == "preserve-tap-plus-more") {
        options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;
        options.echoPolicy = keyconv::EchoPolicy::Off;
        options.streamEchoProfile = keyconv::StreamEchoProfile::Conservative;
        return options;
    }
    if (normalized == "auto-low" || normalized == "tap-plus-low" ||
        normalized == "preserve-tap-plus-low") {
        options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusLow;
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
           "laneEntropy,centerBridgeRate,centerSplitBalance,splitChordRate,"
           "centerBridgeScore,centerSplitBalanceScore,splitChordScore,"
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
            << q.centerBridgeRate << ","
            << q.centerSplitBalance << ","
            << q.splitChordRate << ","
            << q.centerBridgeScore << ","
            << q.centerSplitBalanceScore << ","
            << q.splitChordScore << ","
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
        out << "      \"streamTransformPolicy\": \"" << jsonEscape(q.streamTransformPolicy) << "\",\n";
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
        out << "      \"centerBridgeRate\": " << q.centerBridgeRate << ",\n";
        out << "      \"centerSplitBalance\": " << q.centerSplitBalance << ",\n";
        out << "      \"splitChordRate\": " << q.splitChordRate << ",\n";
        out << "      \"centerBridgeScore\": " << q.centerBridgeScore << ",\n";
        out << "      \"centerSplitBalanceScore\": " << q.centerSplitBalanceScore << ",\n";
        out << "      \"splitChordScore\": " << q.splitChordScore << ",\n";
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

int runSingleConversion(const CliOptions& cli,
                        const std::filesystem::path& input,
                        std::ostream& out = std::cout,
                        std::mutex* outputMutex = nullptr) {
    const auto totalStart = SteadyClock::now();
    long long readTimeMs = 0;
    long long parseTimeMs = 0;
    long long profileTimeMs = 0;
    long long convertTimeMs = 0;
    long long exportTimeMs = 0;
    long long writeTimeMs = 0;
    long long reportWriteTimeMs = 0;

    throwIfConvertedInput(input);
    const auto readStart = SteadyClock::now();
    const auto inputText = readFile(input);
    readTimeMs = elapsedMs(readStart, SteadyClock::now());
    const bool bmsInput = isBmsPath(input);
    if (bmsInput && cli.out.has_value() && !isBmsPath(*cli.out)) {
        throw std::runtime_error("BMS input can only write BMS-family output (.bms, .bme, .bml, .pms)");
    }

    const keyconv::ParseOptions parseOptions{cli.source};
    const auto parseStart = SteadyClock::now();
    auto chart = bmsInput ? keyconv::parseBms(inputText, parseOptions)
                          : keyconv::parseOsu(inputText, parseOptions);
    parseTimeMs = elapsedMs(parseStart, SteadyClock::now());
    throwIfConvertedInput(input, chart);

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
    convertOptions.streamTransformPolicy = cli.streamTransformPolicy;
    convertOptions.tenKeyPlannerPolicy = cli.tenKeyPlannerPolicy;
    convertOptions.seed = cli.seed;
    convertOptions.jackPreservePolicy = cli.jackPreservePolicy;
    convertOptions.gestureRailEnabled = cli.gestureRailEnabled;
    convertOptions.preserveLaneDrift = cli.preserveLaneDrift;
    convertOptions.echoDiagnostics = cli.echoDiagnostics;
    convertOptions.dpMode = cli.dpMode;
    convertOptions.tenKFullFieldRemix = cli.tenKFullFieldRemix;
    convertOptions.tenKFullFieldRemixDensityCeiling = cli.tenKFullFieldRemixDensityCeiling;
    convertOptions.tenKFullFieldRemixPhaseStep = cli.tenKFullFieldRemixPhaseStep;
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

    if (cli.engine == keyconv::nk2::Engine::NK2) {
        keyconv::nk2::NK2Options nk2Options;
        nk2Options.sourceKeyCount = convertOptions.sourceKeyCount;
        nk2Options.targetKeyCount = convertOptions.targetKeyCount;
        nk2Options.mode = cli.nk2Mode;
        nk2Options.nativeWeight = cli.nk2NativeWeight;
        nk2Options.remixWeight = cli.nk2RemixWeight;
        nk2Options.layoutWeights.panel = cli.nk2LayoutWeightPanel;
        nk2Options.layoutWeights.bridge = cli.nk2LayoutWeightBridge;
        nk2Options.layoutWeights.fullField = cli.nk2LayoutWeightFullField;
        nk2Options.sameTimeEpsilonMs = convertOptions.sameTimeEpsilonMs;
        nk2Options.superSymmetry =
            cli.streamTransformPolicy == keyconv::StreamTransformPolicy::SuperSymmetry;

        const auto nk2Start = SteadyClock::now();
        const auto nk2Result = keyconv::nk2::convertChart(chart, nk2Options);
        convertTimeMs = elapsedMs(nk2Start, SteadyClock::now());
        auto outputPath = cli.out;
        if (!cli.dryRun && nk2Result.report.chartMutated && !outputPath.has_value()) {
            outputPath = defaultNk2OutputPath(input,
                                              convertOptions.targetKeyCount,
                                              cli.streamTransformPolicy,
                                              cli.outDir);
        }

        if (!cli.batchQuiet) {
            out << kToolName << " " << kToolVersion << "\n";
            out << "Input: " << displayPath(input) << "\n";
            out << "Mode: "
                << (bmsInput ? "BMS NK2 " : "osu!mania NK2 ")
                << (nk2Result.report.chartMutated ? "prototype" : "report-only") << "\n";
            out << keyconv::nk2::reportToText(nk2Result.report) << "\n";
        }

        if (!cli.dryRun && nk2Result.report.chartMutated) {
            const auto exportStart = SteadyClock::now();
            const auto outputText = bmsInput ? keyconv::exportBms(nk2Result.chart, convertOptions.targetKeyCount)
                                             : keyconv::exportOsu(nk2Result.chart, convertOptions.targetKeyCount);
            exportTimeMs = elapsedMs(exportStart, SteadyClock::now());
            const auto writeStart = SteadyClock::now();
            writeFile(*outputPath, outputText);
            writeTimeMs = elapsedMs(writeStart, SteadyClock::now());
            if (!cli.batchQuiet) {
                out << "Output written: " << displayPath(*outputPath) << "\n";
            }
        } else if (!cli.batchQuiet && nk2Result.report.chartMutated) {
            out << "Dry run: output file not written\n";
        } else if (!cli.batchQuiet) {
            out << "No chart output written\n";
        }

        if (cli.report.has_value()) {
            const auto reportWriteStart = SteadyClock::now();
            writeFile(*cli.report, keyconv::nk2::reportToJson(nk2Result.report));
            reportWriteTimeMs = elapsedMs(reportWriteStart, SteadyClock::now());
            if (!cli.batchQuiet) {
                out << "NK2 report written: " << displayPath(*cli.report) << "\n";
            }
        }
        if (!cli.batchQuiet) {
            const auto totalMs = elapsedMs(totalStart, SteadyClock::now());
            out << "Timing: read=" << readTimeMs
                << " ms parse=" << parseTimeMs
                << " ms nk2=" << convertTimeMs
                << " ms";
            if (cli.report.has_value()) {
                out << " report=" << reportWriteTimeMs << " ms";
            }
            if (!cli.dryRun && nk2Result.report.chartMutated) {
                out << " export=" << exportTimeMs << " ms"
                    << " write=" << writeTimeMs << " ms";
            }
            out << " total=" << totalMs << " ms\n";
        }
        return 0;
    }

    {
        const auto profileStart = SteadyClock::now();
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
        profileTimeMs = elapsedMs(profileStart, SteadyClock::now());
    }

    if (cli.comparePolicies.has_value()) {
        const auto rows = runPolicyComparison(chart, convertOptions, *cli.comparePolicies);
        out << kToolName << " " << kToolVersion << "\n";
        out << "Input: " << displayPath(input) << "\n";
        out << "Mode: " << (bmsInput ? "BMS policy comparison" : "osu!mania policy comparison") << "\n";
        out << "Source keys: " << convertOptions.sourceKeyCount << "\n";
        out << "Target keys: " << convertOptions.targetKeyCount << "\n\n";
        if (convertOptions.targetKProfile.has_value()) {
            out << "Target profile: " << convertOptions.targetKProfile->profileName << " / "
                << convertOptions.targetKProfile->sourceName << " ("
                << convertOptions.targetKProfile->sampleCount << " charts)\n";
        }
        printPolicyComparison(rows, cli.emitFeelReport, cli.emitDiffReport, out);

        if (cli.report.has_value()) {
            writeFile(*cli.report,
                      comparisonToJson(rows, convertOptions.sourceKeyCount, convertOptions.targetKeyCount));
            out << "Comparison report written: " << displayPath(*cli.report) << "\n";
        }
        if (cli.reportCsv.has_value()) {
            writeFile(*cli.reportCsv, comparisonToCsv(rows));
            out << "Comparison CSV written: " << displayPath(*cli.reportCsv) << "\n";
        }

        if (cli.verbose) {
            for (const auto& row : rows) {
                if (row.report.warnings.empty()) {
                    continue;
                }
                out << "\nVerbose warnings for " << row.policy << ":\n";
                for (const auto& warning : row.report.warnings) {
                    out << "- " << warning << "\n";
                }
            }
        }

        return 0;
    }

    const keyconv::Converter converter;
    const auto convertStart = SteadyClock::now();
    const auto result = converter.convert(chart, convertOptions);
    convertTimeMs = elapsedMs(convertStart, SteadyClock::now());
    auto outputPath = cli.out;
    if (!cli.dryRun && !outputPath.has_value() && outputMutex == nullptr) {
        outputPath = defaultOutputPath(input, convertOptions, cli.outDir);
    }

    if (!cli.batchQuiet) {
        out << kToolName << " " << kToolVersion << "\n";
        out << "Input: " << displayPath(input) << "\n";
        out << "Mode: " << (bmsInput ? "BMS" : "osu!mania") << "\n";
        out << "Source keys: " << convertOptions.sourceKeyCount << "\n";
        out << "Target keys: " << convertOptions.targetKeyCount << "\n";
        out << "Style: " << keyconv::toString(convertOptions.style) << "\n";
        out << "Collision policy: " << keyconv::toString(convertOptions.collisionPolicy) << "\n";
        out << "Compress policy: " << keyconv::toString(convertOptions.compressPolicy) << "\n";
        out << "Distance policy: " << keyconv::toString(convertOptions.distancePolicy) << "\n";
        out << "Expansion policy: " << keyconv::toString(convertOptions.expansionPolicy) << "\n";
        out << "Echo policy: " << keyconv::toString(convertOptions.echoPolicy) << "\n";
        out << "Stream echo profile: " << keyconv::toString(convertOptions.streamEchoProfile) << "\n";
        out << "Stream transform: " << keyconv::toString(convertOptions.streamTransformPolicy) << "\n";
        out << "Seed: " << convertOptions.seed << "\n";
        out << "Echo diagnostics: " << (convertOptions.echoDiagnostics ? "yes" : "no") << "\n";
        out << "Optimizer: " << keyconv::toString(convertOptions.optimizer) << "\n";
        out << "Same-time epsilon: " << convertOptions.sameTimeEpsilonMs << " ms\n";
        out << "Min object gap: " << convertOptions.minObjectGapMs << " ms\n";
        out << "Same-lane min gap: " << convertOptions.sameLaneMinGapMs << " ms\n";
        out << "Jack preserve policy: " << keyconv::toString(convertOptions.jackPreservePolicy) << "\n";
        out << "10K planner: " << keyconv::toString(convertOptions.tenKeyPlannerPolicy) << "\n";
        out << "10K full-field remix: " << (convertOptions.tenKFullFieldRemix ? "on" : "off") << "\n";
        if (convertOptions.tenKFullFieldRemix) {
            out << "10K remix density ceiling: " << convertOptions.tenKFullFieldRemixDensityCeiling << "\n";
            out << "10K remix phase step: " << convertOptions.tenKFullFieldRemixPhaseStep << "\n";
        }
        out << "Gesture rail: " << (convertOptions.gestureRailEnabled ? "on" : "off") << "\n";
        out << "Jack window: " << convertOptions.jackWindowMs << " ms\n";
        out << "Strict jack window: " << convertOptions.strictJackWindowMs << " ms\n";
        out << "Playable jack split: " << (convertOptions.allowPlayableJackSplit ? "yes" : "no") << "\n";
        out << "Max jack split lanes: " << convertOptions.maxJackSplitLanes << "\n";
        out << "Snap rolled notes: " << (convertOptions.snapRolledNotes ? "yes" : "no") << "\n";
        out << "Snap tolerance: " << convertOptions.snapToleranceMs << " ms\n";
        out << "Max roll: " << convertOptions.maxRollMs << " ms\n";
        out << "Max added ratio: " << convertOptions.maxAddedNoteRatio << "\n";
        out << "Max added per slice: " << convertOptions.maxAddedPerSlice << "\n";
        out << "Max added per measure: " << convertOptions.maxAddedPerMeasure << "\n";
        out << "Expansion min gap: " << convertOptions.expansionMinGapMs << " ms\n";
        out << "Expansion same-lane min gap: " << convertOptions.expansionSameLaneMinGapMs << " ms\n";
        out << "Snap added notes: " << (convertOptions.snapAddedNotes ? "yes" : "no") << "\n";
        out << "Expansion snap tolerance: " << convertOptions.expansionSnapToleranceMs << " ms\n";
        out << "Max echo ratio: " << convertOptions.maxEchoAddedRatio << "\n";
        out << "Max echo per pattern: " << convertOptions.maxEchoPerPattern << "\n";
        out << "Max echo per measure: " << convertOptions.maxEchoPerMeasure << "\n";
        out << "Max echo per slice: " << convertOptions.maxEchoPerSlice << "\n";
        out << "Echo min gap: " << convertOptions.echoMinGapMs << " ms\n";
        out << "Echo same-lane min gap: " << convertOptions.echoSameLaneMinGapMs << " ms\n";
        out << "Echo max local NPS: " << convertOptions.echoMaxLocalNps << "\n";
        out << "Preserve convert: " << (cli.preserveConvert ? "yes" : "no") << "\n";
        out << "Preserve lane drift: " << (convertOptions.preserveLaneDrift ? "yes" : "no") << "\n";
        if (convertOptions.targetKProfile.has_value()) {
            out << "Target profile: " << convertOptions.targetKProfile->profileName << " / "
                << convertOptions.targetKProfile->sourceName << " ("
                << convertOptions.targetKProfile->sampleCount << " charts";
            if (!convertOptions.targetKProfile->authorToken.empty()) {
                out << ", author " << convertOptions.targetKProfile->authorToken;
            }
            out << ")\n";
        }
        if (convertOptions.dpMode) {
            out << "DP mode: requested\n";
        }
        out << "\n";
        out << keyconv::reportToText(result.report) << "\n";
    }
    if (!cli.batchQuiet && cli.echoDiagnostics) {
        printEchoDiagnostics(result.report, out);
        out << "\n";
    }

    if (!cli.dryRun) {
        const auto exportStart = SteadyClock::now();
        const auto outputText = bmsInput ? keyconv::exportBms(result.chart, convertOptions.targetKeyCount)
                                         : keyconv::exportOsu(result.chart, convertOptions.targetKeyCount);
        exportTimeMs = elapsedMs(exportStart, SteadyClock::now());
        const auto writeStart = SteadyClock::now();
        if (!outputPath.has_value() && outputMutex != nullptr) {
            std::lock_guard<std::mutex> lock(*outputMutex);
            outputPath = defaultOutputPath(input, convertOptions, cli.outDir);
            writeFile(*outputPath, outputText);
        } else {
            writeFile(*outputPath, outputText);
        }
        writeTimeMs = elapsedMs(writeStart, SteadyClock::now());
        if (!cli.batchQuiet) {
            out << "Output written: " << displayPath(*outputPath) << "\n";
        }
    } else if (!cli.batchQuiet) {
        out << "Dry run: output file not written\n";
    }

    if (cli.report.has_value()) {
        const auto reportWriteStart = SteadyClock::now();
        writeFile(*cli.report, keyconv::reportToJson(result.report));
        reportWriteTimeMs = elapsedMs(reportWriteStart, SteadyClock::now());
        if (!cli.batchQuiet) {
            out << "Report written: " << displayPath(*cli.report) << "\n";
        }
    }

    if (!cli.batchQuiet) {
        const auto totalTimeMs = elapsedMs(totalStart, SteadyClock::now());
        out << "Timing: total=" << totalTimeMs << " ms"
            << " read=" << readTimeMs << " ms"
            << " parse=" << parseTimeMs << " ms";
        if (profileTimeMs > 0) {
            out << " profile=" << profileTimeMs << " ms";
        }
        out << " convert=" << convertTimeMs << " ms";
        if (!cli.dryRun) {
            out << " export=" << exportTimeMs << " ms"
                << " write=" << writeTimeMs << " ms";
        }
        if (cli.report.has_value()) {
            out << " report=" << reportWriteTimeMs << " ms";
        }
        out << "\n";
    }

    if (!cli.batchQuiet && cli.verbose && !result.report.warnings.empty()) {
        out << "\nVerbose warnings:\n";
        for (const auto& warning : result.report.warnings) {
            out << "- " << warning << "\n";
        }
    }

    return 0;
}

std::size_t batchWorkerCount(std::size_t jobCount, const std::optional<int>& requestedJobs) {
    if (jobCount <= 1) {
        return jobCount;
    }
    if (requestedJobs.has_value()) {
        return std::clamp<std::size_t>(static_cast<std::size_t>(*requestedJobs), 1, jobCount);
    }

    const unsigned hardware = std::thread::hardware_concurrency();
    const std::size_t workers = hardware == 0 ? 4 : static_cast<std::size_t>(hardware);
    return std::clamp<std::size_t>(workers, 1, jobCount);
}

struct BatchItemResult {
    std::size_t index = 0;
    std::filesystem::path input;
    int exitCode = 1;
    bool skipped = false;
    std::string output;
    std::string error;
};

BatchItemResult runBatchItem(const CliOptions& cli,
                             std::size_t index,
                             const std::filesystem::path& input,
                             std::mutex& outputMutex) {
    BatchItemResult result;
    result.index = index;
    result.input = input;

    try {
        if (cli.batchMode || cli.inputs.size() > 1) {
            throwIfBatchSourceMismatch(cli, input);
        }
        CliOptions item = cli;
        item.input = input;
        item.inputs = {input};
        item.batchMode = false;
        item.out.reset();
        item.report.reset();
        item.reportCsv.reset();
        item.comparePolicies.reset();

        std::ostringstream out;
        result.exitCode = runSingleConversion(item, input, out, &outputMutex);
        result.output = out.str();
    } catch (const ConvertedInputError& error) {
        result.exitCode = 0;
        result.skipped = true;
        result.output = "Skipped already-converted chart: " + displayPath(input) +
                        " (" + error.what() + ")\n";
    } catch (const SourceKeyFilterSkip& error) {
        result.exitCode = 0;
        result.skipped = true;
        result.output = "Skipped source-key mismatch: " + displayPath(input) +
                        " (" + error.what() + ")\n";
    } catch (const std::exception& error) {
        result.exitCode = 1;
        result.error = "Batch item failed: " + displayPath(input) + ": " + error.what();
    }

    return result;
}

int runBatchConversion(const CliOptions& cli);

struct ParsedBatchChart {
    std::filesystem::path input;
    bool bmsInput = false;
    keyconv::Chart chart;
    keyconv::ConvertOptions convertOptions;
};

struct OnnxBatchItemResult {
    std::size_t index = 0;
    std::filesystem::path input;
    int exitCode = 1;
    std::string output;
    std::string error;
};

std::optional<keyconv::TargetKProfile> batchTargetProfile(const CliOptions& cli) {
    auto profilePath = cli.targetProfile;
    if (!profilePath.has_value()) {
        profilePath = bundledTargetProfilePath(*cli.target);
    }
    if (!profilePath.has_value()) {
        return std::nullopt;
    }
    auto profile = loadTargetKProfile(*profilePath);
    if (profile.targetKeys != *cli.target) {
        throw std::runtime_error("Target profile key count does not match --target");
    }
    return profile;
}

ParsedBatchChart parseBatchChartForOnnx(const CliOptions& cli,
                                        const std::filesystem::path& input,
                                        const std::optional<keyconv::TargetKProfile>& targetProfile) {
    throwIfConvertedInput(input);
    throwIfBatchSourceMismatch(cli, input);
    const auto inputText = readFile(input);
    const bool bmsInput = isBmsPath(input);
    const keyconv::ParseOptions parseOptions{cli.source};
    auto chart = bmsInput ? keyconv::parseBms(inputText, parseOptions)
                          : keyconv::parseOsu(inputText, parseOptions);
    throwIfConvertedInput(input, chart);

    auto convertOptions = makeConvertOptions(cli, chart);
    if (targetProfile.has_value()) {
        convertOptions.targetKProfile = *targetProfile;
    }
    return {input, bmsInput, std::move(chart), std::move(convertOptions)};
}

OnnxBatchItemResult runOnnxBatchItem(const CliOptions& cli,
                                     std::size_t index,
                                     const ParsedBatchChart& item) {
    OnnxBatchItemResult result;
    result.index = index;
    result.input = item.input;

    try {
        const keyconv::Converter converter;
        const auto converted = converter.convert(item.chart, item.convertOptions);
        std::ostringstream out;
        if (!cli.dryRun) {
            const auto outputText = item.bmsInput
                                        ? keyconv::exportBms(converted.chart,
                                                             item.convertOptions.targetKeyCount)
                                        : keyconv::exportOsu(converted.chart,
                                                             item.convertOptions.targetKeyCount);
            const auto outputPath = defaultOutputPath(item.input, item.convertOptions, cli.outDir);
            writeFile(outputPath, outputText);
            if (!cli.batchQuiet) {
                out << "Output written: " << displayPath(outputPath) << "\n";
            }
        } else if (!cli.batchQuiet) {
            out << "Dry run: " << displayPath(item.input) << "\n";
        }
        result.exitCode = 0;
        result.output = out.str();
    } catch (const std::exception& error) {
        result.exitCode = 1;
        result.error = "Batch item failed: " + displayPath(item.input) + ": " + error.what();
    }

    return result;
}

int runDeterministicBatchFallback(const CliOptions& cli, const std::string& reason) {
    if (cli.onnxPolicyStrict) {
        std::cerr << "ONNX CUDA batch failed: " << reason << "\n";
        return 1;
    }
    std::cerr << "ONNX CUDA batch failed, falling back to deterministic batch: " << reason << "\n";
    CliOptions fallback = cli;
    fallback.onnxPolicy.reset();
    fallback.onnxPolicyStrict = false;
    fallback.onnxPolicyAutoSelected = false;
    return runBatchConversion(fallback);
}

int runOnnxBatchConversion(const CliOptions& cli) {
#if !defined(KEYWEAVER_WITH_ONNXRUNTIME)
    return runDeterministicBatchFallback(cli, "binary was built without KEYWEAVER_WITH_ONNXRUNTIME");
#else
    int succeeded = 0;
    int failed = 0;
    int skipped = 0;

    std::cout << kToolName << " " << kToolVersion << "\n";
    std::cout << "Batch mode: " << cli.inputs.size() << " input(s)\n";
    std::cout << "Target keys: " << *cli.target << "\n";
    std::cout << "ONNX lane policy: " << displayPath(*cli.onnxPolicy)
              << (cli.onnxPolicyAutoSelected ? " (auto)" : "") << "\n";
    std::cout << "ONNX provider: cuda";
    if (cli.onnxDeviceId != 0) {
        std::cout << " device=" << cli.onnxDeviceId;
    }
    std::cout << "\n";
    std::cout << "Output: "
              << (cli.outDir.has_value() ? displayPath(*cli.outDir) : "beside each input chart") << "\n";
    if (cli.batchQuiet) {
        std::cout << "Batch quiet: on\n";
    }

    std::vector<ParsedBatchChart> parsed;
    parsed.reserve(cli.inputs.size());
    try {
        const auto targetProfile = batchTargetProfile(cli);
        for (std::size_t index = 0; index < cli.inputs.size(); ++index) {
            const auto& input = cli.inputs[index];
            try {
                parsed.push_back(parseBatchChartForOnnx(cli, input, targetProfile));
            } catch (const ConvertedInputError& error) {
                ++skipped;
                if (!cli.batchQuiet) {
                    std::cout << "Skipped already-converted chart: " << displayPath(input)
                              << " (" << error.what() << ")\n";
                }
            } catch (const SourceKeyFilterSkip& error) {
                ++skipped;
                if (!cli.batchQuiet) {
                    std::cout << "Skipped source-key mismatch: " << displayPath(input)
                              << " (" << error.what() << ")\n";
                }
            } catch (const std::exception& error) {
                ++failed;
                std::cerr << "Batch item failed: " << displayPath(input) << ": "
                          << error.what() << "\n";
            }

            const std::size_t done = index + 1;
            const bool showProgress = !cli.batchQuiet || done == cli.inputs.size() ||
                                      (done % std::max<std::size_t>(1, cli.inputs.size() / 100)) == 0;
            if (showProgress) {
                const std::size_t percent = (done * 100) / cli.inputs.size();
                std::cout << "Prepare: " << percent << "% done, "
                          << (cli.inputs.size() - done) << " left\n";
            }
        }
    } catch (const std::exception& error) {
        return runDeterministicBatchFallback(cli, error.what());
    }

    if (parsed.empty()) {
        std::cout << "\nBatch summary: succeeded=0 failed=" << failed
                  << " skipped=" << skipped << "\n";
        return failed == 0 ? 0 : 1;
    }

    std::vector<keyconv::onnx::LanePolicyRequest> requests;
    requests.reserve(parsed.size());
    for (const auto& item : parsed) {
        requests.push_back({&item.chart,
                            item.convertOptions.sourceKeyCount,
                            item.convertOptions.targetKeyCount});
    }

    keyconv::onnx::LanePolicyResult onnxResult;
    try {
        keyconv::onnx::LanePolicyOptions policyOptions;
        policyOptions.modelPath = *cli.onnxPolicy;
        policyOptions.cudaDeviceId = cli.onnxDeviceId;
        policyOptions.maxBatchNotes = static_cast<std::size_t>(cli.onnxMaxBatchNotes);
        onnxResult = keyconv::onnx::runCudaLanePolicy(requests, policyOptions);
    } catch (const std::exception& error) {
        return runDeterministicBatchFallback(cli, error.what());
    }

    std::cout << "ONNX CUDA inference: notes=" << onnxResult.totalNotes
              << " run(s)=" << onnxResult.runCount
              << " input=" << onnxResult.inputName
              << " output=" << onnxResult.outputName << "\n";

    for (std::size_t index = 0; index < parsed.size(); ++index) {
        parsed[index].convertOptions.advisoryTargetLanes = onnxResult.lanesByChart.at(index);
        parsed[index].convertOptions.advisoryLaneWeight = cli.onnxAdvisoryWeight;
    }

    const int baseFailed = failed;
    const int baseSkipped = skipped;
    const std::size_t baseCompleted = static_cast<std::size_t>(baseFailed + baseSkipped);
    const std::size_t workerCount = batchWorkerCount(parsed.size(), cli.jobs);
    std::cout << "Workers: " << workerCount << (cli.jobs.has_value() ? "" : " (auto)") << "\n";
    if (workerCount > 1) {
        std::cout << "Log order: completion order\n";
    }

    std::atomic<std::size_t> nextIndex{0};
    std::atomic<std::size_t> parallelCompleted{0};
    std::atomic<int> parallelSucceeded{0};
    std::atomic<int> parallelFailed{0};
    std::mutex logMutex;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    auto runWorker = [&]() {
        for (;;) {
            const std::size_t index = nextIndex.fetch_add(1);
            if (index >= parsed.size()) {
                break;
            }
            auto result = runOnnxBatchItem(cli, index, parsed[index]);
            if (result.exitCode == 0) {
                ++parallelSucceeded;
            } else {
                ++parallelFailed;
            }

            const std::size_t done = parallelCompleted.fetch_add(1) + 1;
            const std::size_t completed = baseCompleted + done;
            const std::size_t percent = (completed * 100) / cli.inputs.size();
            const bool showItem = !cli.batchQuiet || result.exitCode != 0;
            const bool showProgress = !cli.batchQuiet || done == parsed.size() ||
                                      (done % std::max<std::size_t>(1, parsed.size() / 100)) == 0;

            std::lock_guard<std::mutex> lock(logMutex);
            if (showItem) {
                std::cout << "\n[" << (result.index + 1) << "/" << parsed.size() << "] "
                          << displayPath(result.input) << "\n";
                if (!result.output.empty()) {
                    std::cout << result.output;
                    if (result.output.back() != '\n') {
                        std::cout << "\n";
                    }
                }
            }
            if (result.exitCode != 0 && !result.error.empty()) {
                std::cerr << result.error << "\n";
            }
            if (showProgress) {
                std::cout << "Progress: " << percent << "% done, "
                          << (cli.inputs.size() - completed) << " left\n";
            }
        }
    };

    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back(runWorker);
    }
    for (auto& worker : workers) {
        worker.join();
    }

    succeeded += parallelSucceeded.load();
    failed += parallelFailed.load();

    std::cout << "\nBatch summary: succeeded=" << succeeded << " failed=" << failed
              << " skipped=" << skipped << "\n";
    return failed == 0 ? 0 : 1;
#endif
}

int runBatchConversion(const CliOptions& cli) {
    if (cli.onnxPolicy.has_value()) {
        return runOnnxBatchConversion(cli);
    }

    int succeeded = 0;
    int failed = 0;
    int skipped = 0;
    const std::size_t workerCount = batchWorkerCount(cli.inputs.size(), cli.jobs);
    std::cout << kToolName << " " << kToolVersion << "\n";
    std::cout << "Batch mode: " << cli.inputs.size() << " input(s)\n";
    std::cout << "Target keys: " << *cli.target << "\n";
    std::cout << "Output: "
              << (cli.outDir.has_value() ? displayPath(*cli.outDir) : "beside each input chart") << "\n";
    std::cout << "Workers: " << workerCount << (cli.jobs.has_value() ? "" : " (auto)") << "\n";
    if (cli.batchQuiet) {
        std::cout << "Batch quiet: on\n";
    }
    if (workerCount > 1) {
        std::cout << "Log order: completion order\n";
    }

    if (workerCount <= 1) {
        for (std::size_t index = 0; index < cli.inputs.size(); ++index) {
            const auto& input = cli.inputs[index];
            if (!cli.batchQuiet) {
                std::cout << "\n[" << (index + 1) << "/" << cli.inputs.size() << "] "
                          << displayPath(input) << "\n";
            }
            try {
                CliOptions item = cli;
                item.input = input;
                item.inputs = {input};
                item.batchMode = false;
                item.out.reset();
                item.report.reset();
                item.reportCsv.reset();
                item.comparePolicies.reset();
                runSingleConversion(item, input);
                ++succeeded;
            } catch (const ConvertedInputError& error) {
                ++skipped;
                if (!cli.batchQuiet) {
                    std::cout << "Skipped already-converted chart: " << displayPath(input)
                              << " (" << error.what() << ")\n";
                }
            } catch (const std::exception& error) {
                ++failed;
                std::cerr << "Batch item failed: " << displayPath(input) << ": "
                          << error.what() << "\n";
            }
            const std::size_t done = index + 1;
            const std::size_t percent = (done * 100) / cli.inputs.size();
            const bool showProgress = !cli.batchQuiet || done == cli.inputs.size() ||
                                      (done % std::max<std::size_t>(1, cli.inputs.size() / 100)) == 0;
            if (showProgress) {
                std::cout << "Progress: " << percent << "% done, "
                          << (cli.inputs.size() - done) << " left\n";
            }
        }

        std::cout << "\nBatch summary: succeeded=" << succeeded << " failed=" << failed
                  << " skipped=" << skipped << "\n";
        return failed == 0 ? 0 : 1;
    }

    std::atomic<std::size_t> nextIndex{0};
    std::atomic<std::size_t> parallelCompleted{0};
    std::atomic<int> parallelSucceeded{0};
    std::atomic<int> parallelFailed{0};
    std::atomic<int> parallelSkipped{0};
    std::mutex outputMutex;
    std::mutex logMutex;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&]() {
            for (;;) {
                const std::size_t index = nextIndex.fetch_add(1);
                if (index >= cli.inputs.size()) {
                    break;
                }
                auto result = runBatchItem(cli, index, cli.inputs[index], outputMutex);
                if (result.skipped) {
                    ++parallelSkipped;
                } else if (result.exitCode == 0) {
                    ++parallelSucceeded;
                } else {
                    ++parallelFailed;
                }
                const std::size_t done = parallelCompleted.fetch_add(1) + 1;
                const std::size_t percent = (done * 100) / cli.inputs.size();
                const bool showItem = !cli.batchQuiet || result.exitCode != 0;
                const bool showProgress = !cli.batchQuiet || done == cli.inputs.size() ||
                                          (done % std::max<std::size_t>(1, cli.inputs.size() / 100)) == 0;

                std::lock_guard<std::mutex> lock(logMutex);
                if (showItem) {
                    std::cout << "\n[" << (result.index + 1) << "/" << cli.inputs.size() << "] "
                              << displayPath(result.input) << "\n";
                    if (!result.output.empty()) {
                        std::cout << result.output;
                        if (result.output.back() != '\n') {
                            std::cout << "\n";
                        }
                    }
                }
                if (result.exitCode != 0 && !result.error.empty()) {
                    std::cerr << result.error << "\n";
                }
                if (showProgress) {
                    std::cout << "Progress: " << percent << "% done, "
                              << (cli.inputs.size() - done) << " left\n";
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    succeeded = parallelSucceeded.load();
    failed = parallelFailed.load();
    skipped = parallelSkipped.load();

    std::cout << "\nBatch summary: succeeded=" << succeeded << " failed=" << failed
              << " skipped=" << skipped << "\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = commandLineArgs(argc, argv);
#if defined(_WIN32)
        if (launchGuiForNoArgs(static_cast<int>(args.size()))) {
            return 0;
        }
        if (launchGuiWithInputs(args)) {
            return 0;
        }
#endif

        auto cli = parseArgs(args);
        applyDefaultBatchOnnxPolicy(cli);
        validateOptions(cli);

        if (cli.help) {
            printHelp(std::cout);
            return 0;
        }

        if (cli.batchMode || cli.inputs.size() > 1) {
            return runBatchConversion(cli);
        }

        return runSingleConversion(cli, cli.input);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        std::cerr << "Run with --help for usage.\n";
        return 1;
    }
}
