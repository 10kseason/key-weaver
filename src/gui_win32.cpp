#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <keyconv/reconvert_guard.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kEditKeyconv = 100;
constexpr int kButtonBrowseKeyconv = 101;
constexpr int kEditInput = 102;
constexpr int kButtonBrowseInput = 103;
constexpr int kEditOutputDir = 104;
constexpr int kButtonBrowseOutputDir = 105;
constexpr int kEditSource = 106;
constexpr int kEditTarget = 107;
constexpr int kComboExpansion = 108;
constexpr int kComboCompress = 109;
constexpr int kComboStreamProfile = 110;
constexpr int kButtonConvert = 111;
constexpr int kButtonMatrix = 112;
constexpr int kButtonOpenOutput = 113;
constexpr int kButtonOpenReport = 114;
constexpr int kButtonCopyCommand = 115;
constexpr int kListSummary = 116;
constexpr int kEditLog = 117;
constexpr int kStaticDetected = 118;
constexpr int kButtonBatch = 119;
constexpr int kCheckPreserveConvert = 120;
constexpr int kCheckDebugReports = 121;
constexpr int kStaticStatus = 122;

struct ProcessResult {
    DWORD exitCode = 1;
    std::string output;
};

struct ToolOptions {
    std::filesystem::path keyconvExe;
    std::filesystem::path inputFile;
    std::filesystem::path outputDir;
    std::wstring sourceOverride;
    std::wstring targetKeys = L"10";
    std::wstring expansionPolicy = L"auto (normal)";
    std::wstring compressPolicy = L"auto";
    std::wstring streamTransform = L"off";
    bool preserveConvert = false;
    bool debugReports = false;
};

struct OutputPaths {
    std::filesystem::path outputChart;
    std::filesystem::path reportJson;
    std::filesystem::path reportCsv;
};

struct ReportSummary {
    int totalNotes = 0;
    int addedNotes = 0;
    double addedNoteRatio = 0.0;
    int collisionCount = 0;
    int lnConflictCount = 0;
    int nearTimeConflicts = 0;
    int unsnappedAddedNotes = 0;
    double playabilityScore = 0.0;
    bool deterministic = true;
    bool valid = false;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND keyconvEdit = nullptr;
    HWND inputEdit = nullptr;
    HWND outputDirEdit = nullptr;
    HWND sourceEdit = nullptr;
    HWND targetEdit = nullptr;
    HWND expansionCombo = nullptr;
    HWND compressCombo = nullptr;
    HWND streamProfileCombo = nullptr;
    HWND preserveConvertCheck = nullptr;
    HWND debugReportsCheck = nullptr;
    HWND batchButton = nullptr;
    HWND detectedLabel = nullptr;
    HWND statusLabel = nullptr;
    HWND summaryList = nullptr;
    HWND logEdit = nullptr;
    HFONT uiFont = nullptr;
    std::filesystem::path lastOutputPath;
    std::filesystem::path lastReportPath;
    std::wstring lastCommand;
    std::vector<std::filesystem::path> batchInputs;
    bool suppressInputChange = false;
};

std::filesystem::path directoryForOpen(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        return path;
    }
    return path.parent_path();
}

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int utf8Size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
    const UINT codePage = utf8Size > 0 ? CP_UTF8 : CP_ACP;
    const int size = MultiByteToWideChar(codePage, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(codePage, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string narrowLossy(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::vector<std::wstring> commandLineArgsWide(int argc, char** argv) {
    int wideArgc = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv != nullptr) {
        std::vector<std::wstring> args;
        args.reserve(static_cast<std::size_t>(wideArgc));
        for (int i = 0; i < wideArgc; ++i) {
            args.emplace_back(wideArgv[i]);
        }
        LocalFree(wideArgv);
        return args;
    }

    std::vector<std::wstring> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(widen(argv[i]));
    }
    return args;
}

std::wstring trim(std::wstring value) {
    auto isSpace = [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
    };
    while (!value.empty() && isSpace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::string trimAscii(std::string value) {
    auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::wstring getWindowText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    return text;
}

void setWindowText(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

bool pumpPendingUiMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void setInputText(AppState& state, const std::wstring& text) {
    state.suppressInputChange = true;
    setWindowText(state.inputEdit, text);
    state.suppressInputChange = false;
}

std::filesystem::path moduleDirectory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), buffer.data() + length)).parent_path();
}

std::filesystem::path absolutePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return path;
    }
    return std::filesystem::absolute(path);
}

std::wstring quoteArg(const std::wstring& value) {
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

std::wstring quoteArg(const std::filesystem::path& value) {
    return quoteArg(value.wstring());
}

std::wstring timestampSuffix() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &nowTime);
    std::wostringstream out;
    out << std::put_time(&local, L"%Y%m%d_%H%M%S");
    return out.str();
}

std::filesystem::path preferredKeyWeaverExe() {
    const auto keyWeaver = moduleDirectory() / L"KeyWeaver.exe";
    if (std::filesystem::exists(keyWeaver)) {
        return keyWeaver;
    }
    return moduleDirectory() / L"keyconv.exe";
}

std::wstring sanitizeToken(std::wstring value) {
    for (auto& ch : value) {
        if (!(std::iswalnum(ch) || ch == L'-' || ch == L'_')) {
            ch = L'_';
        }
    }
    return value.empty() ? L"policy" : value;
}

std::wstring lowerAscii(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        if (ch >= L'A' && ch <= L'Z') {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return ch;
    });
    return value;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    });
    return value;
}

bool isBmsFamilyPath(const std::filesystem::path& path) {
    const auto extension = lowerAscii(path.extension().wstring());
    return extension == L".bms" || extension == L".bme" || extension == L".bml" || extension == L".pms";
}

bool isOsuPath(const std::filesystem::path& path) {
    return lowerAscii(path.extension().wstring()) == L".osu";
}

bool isSupportedChartPath(const std::filesystem::path& path) {
    const auto extension = lowerAscii(path.extension().wstring());
    return extension == L".osu" || extension == L".bms" || extension == L".bme" ||
           extension == L".bml" || extension == L".pms";
}

std::optional<std::wstring> convertedMarkerReason(std::wstring_view field, std::wstring_view text) {
    const auto kind = keyconv::convertedChartMarkerKind(text);
    if (kind == keyconv::ConvertedChartMarkerKind::None) {
        return std::nullopt;
    }
    std::wstring reason(field);
    reason += L" has ";
    reason += widen(std::string(keyconv::convertedChartMarkerLabel(kind)));
    return reason;
}

std::optional<std::wstring> convertedPathMarkerReason(const std::filesystem::path& path) {
    return convertedMarkerReason(L"filename", path.filename().wstring());
}

std::optional<std::wstring> convertedMetadataMarkerReason(const std::filesystem::path& path) {
    if (!isSupportedChartPath(path)) {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    std::string line;
    int scanned = 0;
    while (std::getline(in, line) && scanned++ < 500) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string trimmed = trimAscii(line);
        if (trimmed.empty()) {
            continue;
        }
        if (isOsuPath(path)) {
            if (trimmed == "[Difficulty]" || trimmed == "[Events]" || trimmed == "[TimingPoints]" ||
                trimmed == "[HitObjects]") {
                break;
            }
            const auto colon = trimmed.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string key = lowerAscii(trimAscii(trimmed.substr(0, colon)));
            if (key != "creator" && key != "version") {
                continue;
            }
            if (auto reason = convertedMarkerReason(widen(key), widen(trimAscii(trimmed.substr(colon + 1))))) {
                return reason;
            }
        } else {
            if (trimmed.front() != '#') {
                continue;
            }
            const auto split = trimmed.find_first_of(" \t");
            const std::string key = lowerAscii(trimmed.substr(1, split == std::string::npos
                                                                    ? std::string::npos
                                                                    : split - 1));
            if (key != "subtitle" && key != "subartist") {
                continue;
            }
            const std::string value = split == std::string::npos ? std::string{} : trimAscii(trimmed.substr(split + 1));
            if (auto reason = convertedMarkerReason(widen(key), widen(value))) {
                return reason;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> convertedInputMarkerReason(const std::filesystem::path& path) {
    if (auto reason = convertedPathMarkerReason(path)) {
        return reason;
    }
    return convertedMetadataMarkerReason(path);
}

std::wstring chartOutputExtension(const ToolOptions& options) {
    if (isBmsFamilyPath(options.inputFile) && trim(options.targetKeys) == L"9") {
        return L".pms";
    }
    if (isBmsFamilyPath(options.inputFile) && options.inputFile.has_extension()) {
        return options.inputFile.extension().wstring();
    }
    return L".osu";
}

std::wstring expansionDifficultyTag(const ToolOptions& options) {
    if (options.preserveConvert) {
        return {};
    }
    if (options.expansionPolicy == L"auto (more)") {
        return L"more";
    }
    if (options.expansionPolicy == L"auto (normal)") {
        return L"normal";
    }
    if (options.expansionPolicy == L"auto (low)") {
        return L"low";
    }
    return {};
}

std::wstring streamDifficultyTag(const ToolOptions& options) {
    if (options.streamTransform == L"superrandom") {
        return L"sRan";
    }
    if (options.streamTransform == L"full-jitter") {
        return L"jitter";
    }
    return {};
}

std::wstring keyWeaverConversionMarker(const ToolOptions& options) {
    std::wstring marker = L"KeyWeaver" + options.targetKeys + L"K";
    const auto streamTag = streamDifficultyTag(options);
    if (!streamTag.empty()) {
        marker += L"-";
        marker += streamTag;
    }
    const auto expansionTag = expansionDifficultyTag(options);
    if (!expansionTag.empty()) {
        marker += L" (";
        marker += expansionTag;
        marker += L")";
    }
    return marker;
}

std::filesystem::path makeOutputBase(const ToolOptions& options,
                                     const std::wstring& suffix,
                                     std::set<std::filesystem::path>* reservedPaths = nullptr) {
    const std::wstring stem = options.inputFile.stem().wstring();
    const auto outputDir = options.outputDir.empty() ? options.inputFile.parent_path() : options.outputDir;
    const auto chartExtension = chartOutputExtension(options);
    const auto marker = keyWeaverConversionMarker(options);
    for (int index = 1;; ++index) {
        std::wstring name = stem + L" " + marker;
        if (!suffix.empty()) {
            name += L" ";
            name += suffix;
        }
        if (index > 1) {
            name += L" ";
            name += std::to_wstring(index);
        }
        const auto base = outputDir / name;
        std::filesystem::path chart = base;
        chart += chartExtension;
        std::filesystem::path json = base;
        json += L".json";
        std::filesystem::path csv = base;
        csv += L".csv";
        const bool reserved = reservedPaths != nullptr &&
                              (reservedPaths->count(chart) > 0 ||
                               reservedPaths->count(json) > 0 ||
                               reservedPaths->count(csv) > 0);
        if (!reserved && !std::filesystem::exists(chart) && !std::filesystem::exists(json) &&
            !std::filesystem::exists(csv)) {
            if (reservedPaths != nullptr) {
                reservedPaths->insert(chart);
                reservedPaths->insert(json);
                reservedPaths->insert(csv);
            }
            return base;
        }
    }
}

void appendArg(std::wstring& command, const std::wstring& arg) {
    command += L" ";
    command += arg;
}

std::wstring expansionPolicyCliValue(const std::wstring& value) {
    if (value == L"auto (more)") {
        return L"auto-more";
    }
    if (value == L"auto (normal)") {
        return L"auto-normal";
    }
    if (value == L"auto (low)") {
        return L"auto-low";
    }
    return value;
}

std::wstring buildSingleCommand(ToolOptions options,
                                OutputPaths& paths,
                                std::set<std::filesystem::path>* reservedPaths = nullptr) {
    const auto base = makeOutputBase(options, L"", reservedPaths);
    paths.outputChart = base;
    paths.outputChart += chartOutputExtension(options);
    paths.reportJson.clear();
    if (options.debugReports) {
        paths.reportJson = base;
        paths.reportJson += L".json";
    }

    std::wstring command = quoteArg(options.keyconvExe);
    appendArg(command, quoteArg(options.inputFile));
    if (!trim(options.sourceOverride).empty()) {
        appendArg(command, L"--source");
        appendArg(command, trim(options.sourceOverride));
    }
    appendArg(command, L"--target");
    appendArg(command, options.targetKeys);
    appendArg(command, L"--compress-policy");
    appendArg(command, options.compressPolicy);
    const auto expansionPolicy = expansionPolicyCliValue(options.expansionPolicy);
    if (options.preserveConvert) {
        appendArg(command, L"--preserve-convert");
    } else if (expansionPolicy != L"auto" && expansionPolicy != L"auto-normal") {
        appendArg(command, L"--expansion-policy");
        appendArg(command, expansionPolicy);
    }
    if (!options.preserveConvert && options.streamTransform != L"off") {
        appendArg(command, L"--stream-transform");
        appendArg(command, options.streamTransform);
    }
    appendArg(command, L"--out");
    appendArg(command, quoteArg(paths.outputChart));
    if (options.debugReports) {
        appendArg(command, L"--report");
        appendArg(command, quoteArg(paths.reportJson));
    }
    return command;
}

std::wstring buildMatrixCommand(const ToolOptions& options, OutputPaths& paths) {
    const auto base = makeOutputBase(options, L"compare");
    paths.reportJson = base;
    paths.reportJson += L".json";
    paths.reportCsv = base;
    paths.reportCsv += L".csv";

    std::wstring command = quoteArg(options.keyconvExe);
    appendArg(command, quoteArg(options.inputFile));
    if (!trim(options.sourceOverride).empty()) {
        appendArg(command, L"--source");
        appendArg(command, trim(options.sourceOverride));
    }
    appendArg(command, L"--target");
    appendArg(command, options.targetKeys);
    appendArg(command, L"--compress-policy");
    appendArg(command, options.compressPolicy);
    appendArg(command, L"--compare-policies");
    appendArg(command, L"preserve,preserve-tap-plus,echo-balanced,training-scaffold,harder-balanced");
    appendArg(command, L"--emit-feel-report");
    appendArg(command, L"--emit-diff-report");
    appendArg(command, L"--report");
    appendArg(command, quoteArg(paths.reportJson));
    appendArg(command, L"--report-csv");
    appendArg(command, quoteArg(paths.reportCsv));
    return command;
}

ProcessResult runProcess(const std::wstring& command, const std::filesystem::path& workingDirectory) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        return {1, "CreatePipe failed\n"};
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    const std::wstring cwd = workingDirectory.wstring();
    const BOOL ok = CreateProcessW(nullptr,
                                   mutableCommand.data(),
                                   nullptr,
                                   nullptr,
                                   TRUE,
                                   CREATE_NO_WINDOW,
                                   nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(),
                                   &startup,
                                   &process);
    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        std::ostringstream out;
        out << "CreateProcess failed: " << GetLastError() << "\n";
        return {1, out.str()};
    }

    std::string output;
    std::array<char, 4096> buffer{};
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buffer.data(), buffer.data() + bytesRead);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    return {exitCode, output};
}

std::optional<double> findJsonNumber(std::string_view text, std::string_view field) {
    const std::string needle = "\"" + std::string(field) + "\"";
    const auto fieldPos = text.find(needle);
    if (fieldPos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = text.find(':', fieldPos + needle.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto begin = colon + 1;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    auto end = begin;
    while (end < text.size()) {
        const char ch = text[end];
        if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.' ||
              ch == 'e' || ch == 'E')) {
            break;
        }
        ++end;
    }
    try {
        return std::stod(std::string(text.substr(begin, end - begin)));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> findJsonBool(std::string_view text, std::string_view field) {
    const std::string needle = "\"" + std::string(field) + "\"";
    const auto fieldPos = text.find(needle);
    if (fieldPos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = text.find(':', fieldPos + needle.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto begin = colon + 1;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    if (text.substr(begin, 4) == "true") {
        return true;
    }
    if (text.substr(begin, 5) == "false") {
        return false;
    }
    return std::nullopt;
}

ReportSummary parseReportSummary(const std::filesystem::path& reportPath) {
    std::ifstream in(reportPath, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    ReportSummary summary;
    summary.totalNotes = static_cast<int>(findJsonNumber(text, "totalNotes").value_or(0.0));
    summary.addedNotes = static_cast<int>(findJsonNumber(text, "addedNotes").value_or(0.0));
    summary.addedNoteRatio = findJsonNumber(text, "addedNoteRatio").value_or(0.0);
    summary.collisionCount = static_cast<int>(findJsonNumber(text, "collisionCount").value_or(0.0));
    summary.lnConflictCount = static_cast<int>(findJsonNumber(text, "lnConflictCount").value_or(0.0));
    summary.nearTimeConflicts = static_cast<int>(findJsonNumber(text, "nearTimeConflicts").value_or(0.0));
    summary.unsnappedAddedNotes = static_cast<int>(findJsonNumber(text, "unsnappedAddedNotes").value_or(0.0));
    summary.playabilityScore = findJsonNumber(text, "playabilityScore").value_or(0.0);
    summary.deterministic = findJsonBool(text, "deterministic").value_or(true);
    summary.valid = true;
    return summary;
}

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> cells;
    std::string current;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted && ch == '"' && i + 1 < line.size() && line[i + 1] == '"') {
            current.push_back('"');
            ++i;
        } else if (ch == '"') {
            quoted = !quoted;
        } else if (ch == ',' && !quoted) {
            cells.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    cells.push_back(current);
    return cells;
}

std::vector<std::wstring> parseMatrixRows(const std::filesystem::path& csvPath) {
    std::ifstream in(csvPath, std::ios::binary);
    std::vector<std::wstring> rows;
    if (!in) {
        return rows;
    }
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        line = trimAscii(line);
        if (line.empty()) {
            continue;
        }
        if (header) {
            header = false;
            continue;
        }
        const auto cells = splitCsvLine(line);
        if (cells.size() < 27) {
            continue;
        }
        std::ostringstream row;
        row << cells[0] << " | notes " << cells[3] << " | added " << cells[4]
            << " | ratio " << cells[6] << " | entropy " << cells[7]
            << " | pattern " << cells[8] << " | safety " << cells[26];
        if (cells.size() > 27 && !cells[27].empty()) {
            row << " | " << cells[27];
        }
        rows.push_back(widen(row.str()));
    }
    return rows;
}

std::optional<int> detectCircleSize(const std::filesystem::path& osuPath) {
    std::ifstream in(osuPath, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const auto key = trimAscii(line.substr(0, colon));
        if (key != "CircleSize") {
            continue;
        }
        try {
            return std::stoi(trimAscii(line.substr(colon + 1)));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<int> parseSourceOverrideKeyCount(const std::wstring& value) {
    const auto text = trim(value);
    if (text.empty()) {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != L'\0') || parsed < 1 || parsed > 32) {
        return -1;
    }
    return static_cast<int>(parsed);
}

struct FolderScanProgress {
    std::size_t visitedFiles = 0;
    std::size_t totalFiles = 0;
    std::size_t chartFiles = 0;
    bool counting = false;
};

using FolderScanProgressCallback = std::function<void(const FolderScanProgress&)>;

std::size_t countRegularFilesRecursively(const std::filesystem::path& root,
                                         const FolderScanProgressCallback& onProgress) {
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return 0;
    }
    if (!std::filesystem::is_directory(root, error)) {
        return std::filesystem::is_regular_file(root, error) ? 1 : 0;
    }

    std::size_t count = 0;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end; it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file(error)) {
            error.clear();
            continue;
        }
        ++count;
        if (onProgress && (count % 512) == 0) {
            onProgress({count, 0, 0, true});
        } else if ((count % 512) == 0) {
            pumpPendingUiMessages();
        }
    }
    if (onProgress) {
        onProgress({count, 0, 0, true});
    } else {
        pumpPendingUiMessages();
    }
    return count;
}

std::vector<std::filesystem::path> collectOsuFilesRecursively(
    const std::filesystem::path& root,
    const FolderScanProgressCallback& onProgress = {}) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return files;
    }
    if (!std::filesystem::is_directory(root, error)) {
        if (isOsuPath(root)) {
            files.push_back(absolutePath(root));
        }
        if (onProgress) {
            onProgress({1, 1, files.size(), false});
        }
        return files;
    }

    const std::size_t totalFiles = countRegularFilesRecursively(root, onProgress);
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::size_t visitedFiles = 0;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end; it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file(error)) {
            error.clear();
            continue;
        }
        ++visitedFiles;
        const auto path = entry.path();
        if (isOsuPath(path)) {
            files.push_back(absolutePath(path));
        }
        if (onProgress && ((visitedFiles % 128) == 0 || visitedFiles == totalFiles)) {
            onProgress({visitedFiles, totalFiles, files.size(), false});
        } else if ((visitedFiles % 256) == 0) {
            pumpPendingUiMessages();
        }
    }
    if (onProgress) {
        onProgress({visitedFiles, totalFiles, files.size(), false});
    } else {
        pumpPendingUiMessages();
    }
    std::stable_sort(files.begin(), files.end());
    return files;
}

bool matchesSourceOverride(const std::filesystem::path& path, int sourceKeyCount) {
    if (!isOsuPath(path)) {
        return true;
    }
    const auto detected = detectCircleSize(path);
    return detected.has_value() && *detected == sourceKeyCount;
}

void setChildFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND makeControl(AppState& state,
                 const wchar_t* cls,
                 const wchar_t* text,
                 DWORD style,
                 int x,
                 int y,
                 int width,
                 int height,
                 int id,
                 DWORD exStyle = 0) {
    HWND hwnd = CreateWindowExW(exStyle,
                                cls,
                                text,
                                WS_CHILD | WS_VISIBLE | style,
                                x,
                                y,
                                width,
                                height,
                                state.hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr),
                                nullptr);
    setChildFont(hwnd, state.uiFont);
    return hwnd;
}

void addComboItem(HWND combo, const wchar_t* text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

std::wstring comboText(HWND combo) {
    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0) {
        return {};
    }
    const int length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, index, 0));
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(result.data()));
    return result;
}

void setComboSelection(HWND combo, const wchar_t* value) {
    const int index = static_cast<int>(SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                                    reinterpret_cast<LPARAM>(value)));
    if (index >= 0) {
        SendMessageW(combo, CB_SETCURSEL, index, 0);
    }
}

void appendLog(AppState& state, const std::wstring& text) {
    const int length = GetWindowTextLengthW(state.logEdit);
    SendMessageW(state.logEdit, EM_SETSEL, length, length);
    SendMessageW(state.logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void clearSummary(AppState& state) {
    SendMessageW(state.summaryList, LB_RESETCONTENT, 0, 0);
}

void addSummaryLine(AppState& state, const std::wstring& text) {
    SendMessageW(state.summaryList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

std::wstring progressText(std::size_t done, std::size_t total, const wchar_t* label) {
    const std::size_t percent = total == 0 ? 100 : (done * 100) / total;
    const std::size_t remaining = done >= total ? 0 : total - done;
    std::wostringstream out;
    out << label << L": " << percent << L"% done, " << remaining << L" left";
    return out.str();
}

void setStatus(AppState& state, const std::wstring& text) {
    if (state.statusLabel == nullptr) {
        return;
    }
    setWindowText(state.statusLabel, text);
    UpdateWindow(state.statusLabel);
}

void appendProgress(AppState& state, std::size_t done, std::size_t total, const wchar_t* label) {
    const auto text = progressText(done, total, label);
    setStatus(state, text);
    appendLog(state, L"[progress] " + text + L"\r\n");
}

void showReportSummary(AppState& state, const ReportSummary& summary) {
    clearSummary(state);
    if (!summary.valid) {
        addSummaryLine(state, L"Report parse failed.");
        return;
    }
    std::wostringstream line;
    line << L"totalNotes: " << summary.totalNotes;
    addSummaryLine(state, line.str());
    line.str(L"");
    line << L"addedNotes: " << summary.addedNotes << L"  ratio: " << std::fixed << std::setprecision(4)
         << summary.addedNoteRatio;
    addSummaryLine(state, line.str());
    line.str(L"");
    line << L"collision: " << summary.collisionCount << L"  LN: " << summary.lnConflictCount
         << L"  near: " << summary.nearTimeConflicts << L"  unsnappedAdded: " << summary.unsnappedAddedNotes;
    addSummaryLine(state, line.str());
    line.str(L"");
    line << L"playability: " << std::fixed << std::setprecision(2) << summary.playabilityScore
         << L"  deterministic: " << (summary.deterministic ? L"yes" : L"no");
    addSummaryLine(state, line.str());
}

ToolOptions readToolOptions(const AppState& state) {
    ToolOptions options;
    options.keyconvExe = absolutePath(std::filesystem::path(getWindowText(state.keyconvEdit)));
    options.inputFile = absolutePath(std::filesystem::path(getWindowText(state.inputEdit)));
    const auto outputDirText = trim(getWindowText(state.outputDirEdit));
    options.outputDir = outputDirText.empty() ? options.inputFile.parent_path()
                                              : absolutePath(std::filesystem::path(outputDirText));
    options.sourceOverride = trim(getWindowText(state.sourceEdit));
    options.targetKeys = trim(getWindowText(state.targetEdit));
    options.expansionPolicy = comboText(state.expansionCombo);
    options.compressPolicy = comboText(state.compressCombo);
    options.streamTransform = comboText(state.streamProfileCombo);
    options.preserveConvert =
        SendMessageW(state.preserveConvertCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    options.debugReports =
        SendMessageW(state.debugReportsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return options;
}

bool validateToolOptions(const ToolOptions& options, HWND owner) {
    if (options.keyconvExe.empty() || !std::filesystem::exists(options.keyconvExe)) {
        MessageBoxW(owner, L"KeyWeaver executable path is invalid.", L"KeyWeaver GUI", MB_ICONERROR);
        return false;
    }
    if (options.inputFile.empty() || !std::filesystem::exists(options.inputFile)) {
        MessageBoxW(owner, L"Input chart path is invalid.", L"KeyWeaver GUI", MB_ICONERROR);
        return false;
    }
    if (options.targetKeys.empty()) {
        MessageBoxW(owner, L"Target key count is required.", L"KeyWeaver GUI", MB_ICONERROR);
        return false;
    }
    const auto sourceOverride = parseSourceOverrideKeyCount(options.sourceOverride);
    if (sourceOverride.has_value() && *sourceOverride < 0) {
        MessageBoxW(owner, L"Source override must be a key count between 1 and 32.", L"KeyWeaver GUI", MB_ICONERROR);
        return false;
    }
    return true;
}

void updateDetectedSource(AppState& state) {
    const auto input = std::filesystem::path(getWindowText(state.inputEdit));
    const auto detected = detectCircleSize(input);
    if (detected.has_value()) {
        setWindowText(state.detectedLabel, L"Source: " + std::to_wstring(*detected) + L"K");
    } else {
        setWindowText(state.detectedLabel, L"Source: auto");
    }
}

void syncOutputDirToInput(AppState& state) {
    const auto input = absolutePath(std::filesystem::path(getWindowText(state.inputEdit)));
    if (!input.empty() && input.has_parent_path()) {
        setWindowText(state.outputDirEdit, input.parent_path().wstring());
    }
}

std::optional<std::filesystem::path> browseOpenFile(HWND owner,
                                                    const wchar_t* title,
                                                    const wchar_t* filter,
                                                    const std::filesystem::path& initial) {
    std::array<wchar_t, 4096> buffer{};
    const auto initialText = initial.wstring();
    wcsncpy_s(buffer.data(), buffer.size(), initialText.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        return std::filesystem::path(buffer.data());
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> browseFolder(HWND owner, const wchar_t* title = L"Select folder") {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) {
        return std::nullopt;
    }
    std::array<wchar_t, MAX_PATH> buffer{};
    const BOOL ok = SHGetPathFromIDListW(item, buffer.data());
    CoTaskMemFree(item);
    if (!ok) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer.data());
}

void copyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void executeSingleConvert(AppState& state) {
    auto options = readToolOptions(state);
    if (!validateToolOptions(options, state.hwnd)) {
        return;
    }
    if (auto reason = convertedInputMarkerReason(options.inputFile)) {
        clearSummary(state);
        addSummaryLine(state, L"[skip] already converted: " + options.inputFile.filename().wstring());
        addSummaryLine(state, L"       " + *reason);
        setStatus(state, L"Skipped: already converted");
        appendLog(state, L"\r\n[skip] " + options.inputFile.wstring() +
                         L" already converted (" + *reason + L")\r\n");
        MessageBoxW(state.hwnd,
                    L"This chart already has a converter marker, so KeyWeaver skipped it.",
                    L"KeyWeaver GUI",
                    MB_ICONINFORMATION);
        return;
    }
    if (!options.outputDir.empty()) {
        std::filesystem::create_directories(options.outputDir);
    }
    OutputPaths paths;
    const std::wstring command = buildSingleCommand(options, paths);
    state.lastCommand = command;
    clearSummary(state);
    addSummaryLine(state, L"Converting... 0% done, 1 left");
    setStatus(state, L"Converting: 0% done, 1 left");
    appendLog(state, L"\r\n> " + command + L"\r\n");
    UpdateWindow(state.hwnd);
    const auto result = runProcess(command, options.keyconvExe.parent_path());
    appendLog(state, widen(result.output) + L"\r\n");
    if (result.exitCode != 0) {
        setStatus(state, L"Convert failed");
        clearSummary(state);
        addSummaryLine(state, L"Convert failed. See log output.");
        MessageBoxW(state.hwnd, L"Convert failed. See log output.", L"KeyWeaver GUI", MB_ICONERROR);
        return;
    }
    state.lastOutputPath = paths.outputChart;
    state.lastReportPath = paths.reportJson;
    if (options.debugReports) {
        showReportSummary(state, parseReportSummary(paths.reportJson));
    } else {
        clearSummary(state);
        addSummaryLine(state, L"Converted. Enable Debug JSON for metrics/report output.");
    }
    appendProgress(state, 1, 1, L"Convert");
    appendLog(state, L"Output: " + paths.outputChart.wstring() + L"\r\n");
}

void executeMatrix(AppState& state) {
    auto options = readToolOptions(state);
    if (!validateToolOptions(options, state.hwnd)) {
        return;
    }
    if (!options.outputDir.empty()) {
        std::filesystem::create_directories(options.outputDir);
    }
    OutputPaths paths;
    const std::wstring command = buildMatrixCommand(options, paths);
    state.lastCommand = command;
    setStatus(state, L"Matrix: running");
    appendLog(state, L"\r\n> " + command + L"\r\n");
    UpdateWindow(state.hwnd);
    const auto result = runProcess(command, options.keyconvExe.parent_path());
    appendLog(state, widen(result.output) + L"\r\n");
    if (result.exitCode != 0) {
        setStatus(state, L"Matrix failed");
        MessageBoxW(state.hwnd, L"Policy matrix failed. See log output.", L"KeyWeaver GUI", MB_ICONERROR);
        return;
    }
    state.lastOutputPath = options.outputDir;
    state.lastReportPath = paths.reportJson;
    clearSummary(state);
    const auto rows = parseMatrixRows(paths.reportCsv);
    for (const auto& row : rows) {
        addSummaryLine(state, row);
    }
    setStatus(state, L"Matrix: done");
}

std::optional<std::filesystem::path> explicitOutputDir(const AppState& state) {
    const auto outputDirText = trim(getWindowText(state.outputDirEdit));
    if (outputDirText.empty()) {
        return std::nullopt;
    }
    return absolutePath(std::filesystem::path(outputDirText));
}

std::vector<std::filesystem::path> batchInputsForState(const AppState& state) {
    if (!state.batchInputs.empty()) {
        return state.batchInputs;
    }
    const auto inputText = trim(getWindowText(state.inputEdit));
    if (inputText.empty()) {
        return {};
    }
    const auto input = absolutePath(std::filesystem::path(inputText));
    std::error_code error;
    if (std::filesystem::is_directory(input, error)) {
        return collectOsuFilesRecursively(input);
    }
    return {input};
}

std::vector<std::filesystem::path> chooseBatchFolderInputs(AppState& state) {
    const auto folder = browseFolder(state.hwnd, L"Select songs folder for recursive .osu batch");
    if (!folder.has_value()) {
        return {};
    }
    const auto root = absolutePath(*folder);
    setInputText(state, root.wstring());
    setWindowText(state.outputDirEdit, L"");
    state.batchInputs.clear();
    clearSummary(state);
    addSummaryLine(state, L"Scanning folder...");
    setStatus(state, L"Scanning: counting files");
    appendLog(state, L"\r\nScanning folder: " + root.wstring() + L"\r\n");
    UpdateWindow(state.hwnd);
    std::size_t lastLogged = 0;
    auto onProgress = [&](const FolderScanProgress& progress) {
        if (progress.counting) {
            const std::wstring text = L"Scanning: counting files, " +
                                      std::to_wstring(progress.visitedFiles) + L" seen";
            setStatus(state, text);
            if (progress.visitedFiles == 0 ||
                progress.visitedFiles - lastLogged >= 4096) {
                appendLog(state, L"[progress] " + text + L"\r\n");
                lastLogged = progress.visitedFiles;
            }
        } else {
            const std::size_t done = progress.visitedFiles;
            const std::size_t total = progress.totalFiles;
            const std::size_t percent = total == 0 ? 100 : (done * 100) / total;
            const std::size_t remaining = done >= total ? 0 : total - done;
            std::wostringstream text;
            text << L"Scanning: " << percent << L"% done, "
                 << remaining << L" files left, "
                 << progress.chartFiles << L" charts";
            setStatus(state, text.str());
            if (done == total || done - lastLogged >= 4096) {
                appendLog(state, L"[progress] " + text.str() + L"\r\n");
                lastLogged = done;
            }
        }
        UpdateWindow(state.hwnd);
        pumpPendingUiMessages();
    };
    auto files = collectOsuFilesRecursively(root, onProgress);
    std::wostringstream done;
    done << L"Scanning done: " << files.size() << L" charts";
    setStatus(state, done.str());
    appendLog(state, done.str() + L"\r\n");
    return files;
}

struct BatchJob {
    std::size_t index = 0;
    std::size_t total = 0;
    ToolOptions options;
    OutputPaths paths;
    std::wstring command;
};

struct BatchJobResult {
    std::size_t index = 0;
    std::size_t total = 0;
    std::filesystem::path inputFile;
    OutputPaths paths;
    std::wstring command;
    ProcessResult process;
    ReportSummary summary;
};

std::size_t batchWorkerCount(std::size_t jobCount) {
    if (jobCount <= 1) {
        return jobCount;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    const std::size_t workers = hardware == 0 ? 4 : static_cast<std::size_t>(hardware);
    return std::clamp<std::size_t>(workers, 1, jobCount);
}

BatchJobResult runBatchJob(BatchJob job) {
    BatchJobResult result;
    result.index = job.index;
    result.total = job.total;
    result.inputFile = job.options.inputFile;
    result.paths = job.paths;
    result.command = job.command;
    result.process = runProcess(job.command, job.options.keyconvExe.parent_path());
    if (result.process.exitCode == 0 && job.options.debugReports && !job.paths.reportJson.empty()) {
        result.summary = parseReportSummary(job.paths.reportJson);
    }
    return result;
}

void applyBatchJobResult(AppState& state, const BatchJobResult& result, int& succeeded, int& failed) {
    appendLog(state, L"\r\n[" + std::to_wstring(result.index + 1) + L"/" +
                     std::to_wstring(result.total) + L"] > " + result.command + L"\r\n");
    appendLog(state, widen(result.process.output) + L"\r\n");
    if (result.process.exitCode != 0) {
        ++failed;
        addSummaryLine(state, L"[fail] " + result.inputFile.filename().wstring());
        return;
    }

    ++succeeded;
    state.lastCommand = result.command;
    state.lastOutputPath = result.paths.outputChart;
    state.lastReportPath = result.paths.reportJson;
    std::wostringstream line;
    line << L"[ok] " << result.inputFile.filename().wstring();
    if (result.summary.valid) {
        line << L"  notes=" << result.summary.totalNotes << L" added=" << result.summary.addedNotes;
    }
    addSummaryLine(state, line.str());
    appendLog(state, L"Output: " + result.paths.outputChart.wstring() + L"\r\n");
}

void executeBatchConvert(AppState& state, bool chooseFolderFirst = false) {
    auto baseOptions = readToolOptions(state);
    auto inputs = chooseFolderFirst ? chooseBatchFolderInputs(state) : batchInputsForState(state);
    const auto inputText = trim(getWindowText(state.inputEdit));
    const auto inputPath = inputText.empty() ? std::filesystem::path() : absolutePath(std::filesystem::path(inputText));
    std::error_code inputError;
    const bool hasDirectoryInput = !inputPath.empty() && std::filesystem::is_directory(inputPath, inputError);
    if (!chooseFolderFirst && (inputs.empty() || (!hasDirectoryInput && state.batchInputs.size() <= 1))) {
        inputs = chooseBatchFolderInputs(state);
    }
    if (inputs.empty()) {
        MessageBoxW(state.hwnd, L"Select a songs folder containing .osu files, or drop multiple chart files first.",
                    L"KeyWeaver GUI", MB_ICONERROR);
        return;
    }
    if (baseOptions.keyconvExe.empty() || !std::filesystem::exists(baseOptions.keyconvExe)) {
        MessageBoxW(state.hwnd, L"KeyWeaver executable path is invalid.", L"KeyWeaver GUI", MB_ICONERROR);
        return;
    }
    if (baseOptions.targetKeys.empty()) {
        MessageBoxW(state.hwnd, L"Target key count is required.", L"KeyWeaver GUI", MB_ICONERROR);
        return;
    }
    const auto sourceFilter = parseSourceOverrideKeyCount(baseOptions.sourceOverride);
    if (sourceFilter.has_value() && *sourceFilter < 0) {
        MessageBoxW(state.hwnd, L"Source override must be a key count between 1 and 32.", L"KeyWeaver GUI", MB_ICONERROR);
        return;
    }

    const auto forcedOutputDir = explicitOutputDir(state);
    clearSummary(state);
    state.lastReportPath.clear();
    setStatus(state, L"Batch: preparing");
    appendLog(state, L"\r\nBatch convert: " + std::to_wstring(inputs.size()) +
                     L" file(s), target " + baseOptions.targetKeys + L"K\r\n");
    if (sourceFilter.has_value()) {
        appendLog(state, L"Source filter: only " + std::to_wstring(*sourceFilter) + L"K .osu charts\r\n");
    }

    int succeeded = 0;
    int failed = 0;
    int skipped = 0;
    std::vector<BatchJob> jobs;
    jobs.reserve(inputs.size());
    std::set<std::filesystem::path> reservedOutputPaths;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if ((index % 64) == 0) {
            const auto text = progressText(index, inputs.size(), L"Batch prepare");
            setStatus(state, text);
            if ((index % 512) == 0) {
                appendLog(state, L"[progress] " + text + L"\r\n");
            }
            pumpPendingUiMessages();
        }
        ToolOptions options = baseOptions;
        options.inputFile = absolutePath(inputs[index]);
        if (!std::filesystem::exists(options.inputFile) || !isSupportedChartPath(options.inputFile)) {
            ++failed;
            addSummaryLine(state, L"[fail] " + options.inputFile.filename().wstring() + L" invalid input");
            appendLog(state, L"[fail] " + options.inputFile.wstring() + L" invalid input\r\n");
            continue;
        }
        if (auto reason = convertedInputMarkerReason(options.inputFile)) {
            ++skipped;
            addSummaryLine(state, L"[skip] " + options.inputFile.filename().wstring() + L" already converted");
            appendLog(state, L"[skip] " + options.inputFile.wstring() +
                             L" already converted (" + *reason + L")\r\n");
            continue;
        }
        if (sourceFilter.has_value() && !matchesSourceOverride(options.inputFile, *sourceFilter)) {
            ++skipped;
            const auto detected = isOsuPath(options.inputFile) ? detectCircleSize(options.inputFile) : std::nullopt;
            std::wstring sourceText = detected.has_value() ? std::to_wstring(*detected) + L"K" : L"unknown";
            addSummaryLine(state, L"[skip] " + options.inputFile.filename().wstring() + L" source=" + sourceText);
            appendLog(state, L"[skip] " + options.inputFile.wstring() + L" source=" + sourceText + L"\r\n");
            continue;
        }
        options.outputDir = forcedOutputDir.has_value() ? *forcedOutputDir : options.inputFile.parent_path();
        if (!options.outputDir.empty()) {
            std::filesystem::create_directories(options.outputDir);
        }

        OutputPaths paths;
        const std::wstring command = buildSingleCommand(options, paths, &reservedOutputPaths);
        jobs.push_back({index, inputs.size(), options, paths, command});
    }
    setStatus(state, L"Batch: launching workers");
    pumpPendingUiMessages();
    if (jobs.empty()) {
        const std::wstring done = L"Batch done: ok=0 fail=" + std::to_wstring(failed) +
                                  L" skip=" + std::to_wstring(skipped);
        setStatus(state, done);
        appendLog(state, done + L"\r\n");
        return;
    }

    const std::size_t workerCount = batchWorkerCount(jobs.size());
    if (workerCount > 1) {
        appendLog(state, L"Parallel workers: " + std::to_wstring(workerCount) + L"\r\n");
    }
    if (!baseOptions.debugReports) {
        appendLog(state, L"Debug JSON off: batch writes charts only.\r\n");
    }
    appendProgress(state, static_cast<std::size_t>(skipped + failed), inputs.size(), L"Batch");
    UpdateWindow(state.hwnd);

    std::deque<std::future<BatchJobResult>> running;
    auto launchJob = [&](const BatchJob& job) {
        running.push_back(std::async(std::launch::async, runBatchJob, job));
    };
    auto collectOne = [&]() {
        while (running.front().wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
            pumpPendingUiMessages();
            Sleep(10);
        }
        auto result = running.front().get();
        running.pop_front();
        applyBatchJobResult(state, result, succeeded, failed);
        appendProgress(state,
                       static_cast<std::size_t>(succeeded + failed + skipped),
                       inputs.size(),
                       L"Batch");
        UpdateWindow(state.hwnd);
    };

    for (const auto& job : jobs) {
        while (running.size() >= workerCount) {
            collectOne();
        }
        launchJob(job);
    }
    while (!running.empty()) {
        collectOne();
    }

    std::wostringstream done;
    done << L"Batch done: ok=" << succeeded << L" fail=" << failed << L" skip=" << skipped;
    setStatus(state, done.str());
    appendLog(state, done.str() + L"\r\n");
    if (failed > 0) {
        MessageBoxW(state.hwnd, done.str().c_str(), L"KeyWeaver batch", MB_ICONWARNING);
    }
}

void loadDroppedFiles(AppState& state,
                      std::vector<std::filesystem::path> files,
                      bool runNow) {
    files.erase(std::remove_if(files.begin(), files.end(), [](const std::filesystem::path& path) {
                    return !isSupportedChartPath(path);
                }),
                files.end());
    if (files.empty()) {
        MessageBoxW(state.hwnd, L"Drop osu/BMS-family chart files.", L"KeyWeaver GUI", MB_ICONERROR);
        return;
    }

    for (auto& file : files) {
        file = absolutePath(file);
    }
    state.batchInputs = files;
    setInputText(state, files.front().wstring());
    setWindowText(state.outputDirEdit, files.front().parent_path().wstring());
    updateDetectedSource(state);
    appendLog(state, L"\r\nLoaded dropped chart(s): " + std::to_wstring(files.size()) + L"\r\n");
    if (files.size() > 1) {
        appendLog(state, L"Output field is blank: each chart writes beside its original file.\r\n");
    }

    if (!runNow) {
        return;
    }
    if (files.size() == 1) {
        executeSingleConvert(state);
    } else {
        executeBatchConvert(state, false);
    }
}

std::vector<std::filesystem::path> filesFromDrop(HDROP drop) {
    std::vector<std::filesystem::path> files;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
        DragQueryFileW(drop, index, buffer.data(), length + 1);
        buffer.resize(length);
        files.emplace_back(buffer);
    }
    return files;
}

void createUi(AppState& state) {
    state.uiFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const int labelX = 12;
    const int editX = 118;
    const int buttonX = 650;
    int y = 12;

    makeControl(state, L"STATIC", L"KeyWeaver", 0, labelX, y + 4, 96, 20, -1);
    state.keyconvEdit = makeControl(state, L"EDIT", L"", ES_AUTOHSCROLL, editX, y, 520, 24, kEditKeyconv,
                                    WS_EX_CLIENTEDGE);
    makeControl(state, L"BUTTON", L"Browse", BS_PUSHBUTTON, buttonX, y, 86, 24, kButtonBrowseKeyconv);
    y += 34;

    makeControl(state, L"STATIC", L"Input", 0, labelX, y + 4, 96, 20, -1);
    state.inputEdit = makeControl(state, L"EDIT", L"", ES_AUTOHSCROLL, editX, y, 520, 24, kEditInput,
                                  WS_EX_CLIENTEDGE);
    makeControl(state, L"BUTTON", L"Browse", BS_PUSHBUTTON, buttonX, y, 86, 24, kButtonBrowseInput);
    y += 34;

    makeControl(state, L"STATIC", L"Output", 0, labelX, y + 4, 96, 20, -1);
    state.outputDirEdit = makeControl(state, L"EDIT", L"", ES_AUTOHSCROLL, editX, y, 520, 24, kEditOutputDir,
                                      WS_EX_CLIENTEDGE);
    makeControl(state, L"BUTTON", L"Browse", BS_PUSHBUTTON, buttonX, y, 86, 24, kButtonBrowseOutputDir);
    y += 36;

    state.detectedLabel = makeControl(state, L"STATIC", L"Source: auto", 0, editX, y, 160, 20,
                                      kStaticDetected);
    makeControl(state, L"STATIC", L"Source override", 0, labelX, y + 28, 100, 20, -1);
    state.sourceEdit = makeControl(state, L"EDIT", L"", ES_AUTOHSCROLL, editX, y + 24, 70, 24, kEditSource,
                                   WS_EX_CLIENTEDGE);
    makeControl(state, L"STATIC", L"Target", 0, 210, y + 28, 50, 20, -1);
    state.targetEdit = makeControl(state, L"EDIT", L"10", ES_AUTOHSCROLL, 260, y + 24, 70, 24, kEditTarget,
                                   WS_EX_CLIENTEDGE);
    y += 64;

    makeControl(state, L"STATIC", L"Expansion", 0, labelX, y + 4, 96, 20, -1);
    state.expansionCombo = makeControl(state, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, editX, y, 180, 160,
                                       kComboExpansion);
    for (const auto* item : {L"auto (more)", L"auto (normal)", L"auto (low)"}) {
        addComboItem(state.expansionCombo, item);
    }
    setComboSelection(state.expansionCombo, L"auto (normal)");

    makeControl(state, L"STATIC", L"Compress", 0, 320, y + 4, 70, 20, -1);
    state.compressCombo = makeControl(state, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 392, y, 180, 180,
                                      kComboCompress);
    for (const auto* item : {L"auto"}) {
        addComboItem(state.compressCombo, item);
    }
    setComboSelection(state.compressCombo, L"auto");

    makeControl(state, L"STATIC", L"Stream", 0, 584, y + 4, 58, 20, -1);
    state.streamProfileCombo = makeControl(state, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                           642, y, 120, 140, kComboStreamProfile);
    for (const auto* item : {L"off", L"superrandom", L"full-jitter"}) {
        addComboItem(state.streamProfileCombo, item);
    }
    setComboSelection(state.streamProfileCombo, L"off");
    y += 38;

    state.preserveConvertCheck = makeControl(state,
                                             L"BUTTON",
                                             L"Preserve Convert",
                                             BS_AUTOCHECKBOX,
                                             editX,
                                             y,
                                             180,
                                             22,
                                             kCheckPreserveConvert);
    makeControl(state, L"STATIC", L"faithful mapping, strict source jacks", 0,
                editX + 190, y + 2, 300, 20, -1);
    state.debugReportsCheck = makeControl(state,
                                          L"BUTTON",
                                          L"Debug JSON",
                                          BS_AUTOCHECKBOX,
                                          editX + 500,
                                          y,
                                          124,
                                          22,
                                          kCheckDebugReports);
    y += 32;

    makeControl(state, L"BUTTON", L"Convert", BS_PUSHBUTTON, editX, y, 96, 28, kButtonConvert);
    state.batchButton = makeControl(state, L"BUTTON", L"Batch", BS_PUSHBUTTON, editX + 104, y, 96, 28, kButtonBatch);
    makeControl(state, L"BUTTON", L"Matrix", BS_PUSHBUTTON, editX + 208, y, 86, 28, kButtonMatrix);
    makeControl(state, L"BUTTON", L"Open Output", BS_PUSHBUTTON, editX + 302, y, 112, 28, kButtonOpenOutput);
    makeControl(state, L"BUTTON", L"Open Report", BS_PUSHBUTTON, editX + 422, y, 104, 28, kButtonOpenReport);
    makeControl(state, L"BUTTON", L"Copy CLI", BS_PUSHBUTTON, editX + 534, y, 90, 28, kButtonCopyCommand);
    y += 36;

    makeControl(state, L"STATIC", L"Status", 0, labelX, y + 4, 100, 20, -1);
    state.statusLabel = makeControl(state, L"STATIC", L"Ready", 0, editX, y + 4, 644, 20, kStaticStatus);
    y += 30;

    makeControl(state, L"STATIC", L"Report / Matrix", 0, labelX, y + 4, 100, 20, -1);
    state.summaryList = makeControl(state, L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL,
                                    editX, y, 644, 118, kListSummary, WS_EX_CLIENTEDGE);
    y += 130;

    makeControl(state, L"STATIC", L"Log", 0, labelX, y + 4, 100, 20, -1);
    state.logEdit = makeControl(state, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                                   ES_READONLY | WS_VSCROLL | WS_HSCROLL,
                                editX, y, 644, 172, kEditLog, WS_EX_CLIENTEDGE);

    const auto exe = preferredKeyWeaverExe();
    setWindowText(state.keyconvEdit, exe.wstring());
    setWindowText(state.outputDirEdit, L"");
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<AppState*>(create->lpCreateParams);
            state->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            createUi(*state);
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        }
        case WM_DROPFILES:
            if (state) {
                HDROP drop = reinterpret_cast<HDROP>(wParam);
                auto files = filesFromDrop(drop);
                DragFinish(drop);
                loadDroppedFiles(*state, std::move(files), true);
                return 0;
            }
            break;
        case WM_COMMAND: {
            if (!state) {
                break;
            }
            const auto controlId = LOWORD(wParam);
            const auto notification = HIWORD(wParam);
            if (controlId == kEditInput && notification == EN_CHANGE && !state->suppressInputChange) {
                state->batchInputs.clear();
            }
            switch (controlId) {
                case kButtonBrowseKeyconv: {
                    const auto path = browseOpenFile(hwnd, L"Select KeyWeaver.exe",
                                                     L"Executable\0*.exe\0All files\0*.*\0",
                                                     std::filesystem::path(getWindowText(state->keyconvEdit)));
                    if (path.has_value()) {
                        setWindowText(state->keyconvEdit, path->wstring());
                    }
                    return 0;
                }
                case kButtonBrowseInput: {
                    const auto path = browseOpenFile(hwnd, L"Select chart",
                                                     L"Supported charts\0*.osu;*.bms;*.bme;*.bml;*.pms\0osu! chart\0*.osu\0BMS family\0*.bms;*.bme;*.bml;*.pms\0All files\0*.*\0",
                                                     std::filesystem::path(getWindowText(state->inputEdit)));
                    if (path.has_value()) {
                        state->batchInputs = {*path};
                        setInputText(*state, path->wstring());
                        syncOutputDirToInput(*state);
                        updateDetectedSource(*state);
                    }
                    return 0;
                }
                case kButtonBrowseOutputDir: {
                    const auto path = browseFolder(hwnd, L"Select output folder");
                    if (path.has_value()) {
                        setWindowText(state->outputDirEdit, path->wstring());
                    }
                    return 0;
                }
                case kButtonConvert:
                    executeSingleConvert(*state);
                    return 0;
                case kButtonBatch:
                    executeBatchConvert(*state, true);
                    return 0;
                case kButtonMatrix:
                    executeMatrix(*state);
                    return 0;
                case kButtonOpenOutput: {
                    auto path = directoryForOpen(state->lastOutputPath);
                    if (path.empty()) {
                        path = readToolOptions(*state).outputDir;
                    }
                    if (!path.empty()) {
                        ShellExecuteW(hwnd, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    return 0;
                }
                case kButtonOpenReport:
                    if (!state->lastReportPath.empty()) {
                        ShellExecuteW(hwnd, L"open", state->lastReportPath.wstring().c_str(), nullptr, nullptr,
                                      SW_SHOWNORMAL);
                    }
                    return 0;
                case kButtonCopyCommand: {
                    if (state->lastCommand.empty()) {
                        OutputPaths paths;
                        state->lastCommand = buildSingleCommand(readToolOptions(*state), paths);
                    }
                    copyToClipboard(hwnd, state->lastCommand);
                    return 0;
                }
                default:
                    break;
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int runGui(const std::vector<std::filesystem::path>& initialInputs = {}) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AppState state;

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KeyConvPlaytestGui";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0,
                                wc.lpszClassName,
                                L"KeyWeaver v0.6.0 Playtest Tool",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                810,
                                680,
                                nullptr,
                                nullptr,
                                wc.hInstance,
                                &state);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    if (!initialInputs.empty()) {
        loadDroppedFiles(state, initialInputs, false);
        appendLog(state, L"Choose Target keys, then press Convert or Batch.\r\n");
        setStatus(state, L"Ready");
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

int runSmoke(const std::filesystem::path& input, const std::filesystem::path& outputDir) {
    ToolOptions options;
    options.keyconvExe = preferredKeyWeaverExe();
    options.inputFile = absolutePath(input);
    options.outputDir = outputDir.empty() ? options.inputFile.parent_path() : absolutePath(outputDir);
    options.debugReports = true;
    std::filesystem::create_directories(options.outputDir);

    OutputPaths singlePaths;
    const auto singleCommand = buildSingleCommand(options, singlePaths);
    const auto single = runProcess(singleCommand, options.keyconvExe.parent_path());
    if (single.exitCode != 0) {
        std::cerr << single.output;
        return 1;
    }
    const auto summary = parseReportSummary(singlePaths.reportJson);
    if (!summary.valid || summary.totalNotes <= 0) {
        std::cerr << "GUI smoke failed to parse single-convert report\n";
        return 1;
    }

    ToolOptions noReportOptions = options;
    noReportOptions.outputDir = options.outputDir / L"normal_no_json";
    noReportOptions.debugReports = false;
    std::filesystem::create_directories(noReportOptions.outputDir);
    OutputPaths noReportPaths;
    const auto noReportCommand = buildSingleCommand(noReportOptions, noReportPaths);
    const auto noReport = runProcess(noReportCommand, noReportOptions.keyconvExe.parent_path());
    if (noReport.exitCode != 0) {
        std::cerr << noReport.output;
        return 1;
    }
    if (!noReportPaths.reportJson.empty()) {
        std::cerr << "GUI smoke expected normal conversion to omit JSON report path\n";
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(noReportOptions.outputDir)) {
        if (lowerAscii(entry.path().extension().wstring()) == L".json") {
            std::cerr << "GUI smoke expected normal conversion to skip JSON report output\n";
            return 1;
        }
    }

    OutputPaths matrixPaths;
    const auto matrixCommand = buildMatrixCommand(options, matrixPaths);
    const auto matrix = runProcess(matrixCommand, options.keyconvExe.parent_path());
    if (matrix.exitCode != 0) {
        std::cerr << matrix.output;
        return 1;
    }
    const auto rows = parseMatrixRows(matrixPaths.reportCsv);
    if (rows.size() < 3) {
        std::cerr << "GUI smoke failed to parse policy matrix CSV\n";
        return 1;
    }

    std::cout << "gui smoke ok: totalNotes=" << summary.totalNotes
              << " matrixRows=" << rows.size() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = commandLineArgsWide(argc, argv);
    if (args.size() >= 3 && args[1] == L"--smoke") {
        const std::filesystem::path outputDir = args.size() >= 4 ? std::filesystem::path(args[3])
                                                                 : std::filesystem::path();
        return runSmoke(std::filesystem::path(args[2]), outputDir);
    }
    std::vector<std::filesystem::path> initialInputs;
    for (std::size_t index = 1; index < args.size(); ++index) {
        if (!args[index].empty() && args[index].front() != L'-') {
            initialInputs.emplace_back(args[index]);
        }
    }
    return runGui(initialInputs);
}
