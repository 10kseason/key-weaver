#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
    std::wstring expansionPolicy = L"auto";
    std::wstring compressPolicy = L"auto";
    std::wstring streamEchoProfile = L"conservative";
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
    HWND detectedLabel = nullptr;
    HWND summaryList = nullptr;
    HWND logEdit = nullptr;
    HFONT uiFont = nullptr;
    std::filesystem::path lastOutputPath;
    std::filesystem::path lastReportPath;
    std::wstring lastCommand;
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

bool isBmsFamilyPath(const std::filesystem::path& path) {
    const auto extension = lowerAscii(path.extension().wstring());
    return extension == L".bms" || extension == L".bme" || extension == L".bml" || extension == L".pms";
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

std::filesystem::path makeOutputBase(const ToolOptions& options, const std::wstring& suffix) {
    const std::wstring stem = options.inputFile.stem().wstring();
    const auto outputDir = options.outputDir.empty() ? options.inputFile.parent_path() : options.outputDir;
    const auto chartExtension = chartOutputExtension(options);
    for (int index = 1;; ++index) {
        std::wstring name = stem + L" KeyWeaver" + options.targetKeys + L"K";
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
        if (!std::filesystem::exists(chart) && !std::filesystem::exists(json) && !std::filesystem::exists(csv)) {
            return base;
        }
    }
}

void appendArg(std::wstring& command, const std::wstring& arg) {
    command += L" ";
    command += arg;
}

std::wstring buildSingleCommand(const ToolOptions& options, OutputPaths& paths) {
    const auto base = makeOutputBase(options, L"");
    paths.outputChart = base;
    paths.outputChart += chartOutputExtension(options);
    paths.reportJson = base;
    paths.reportJson += L".json";

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
    if (options.expansionPolicy != L"auto") {
        appendArg(command, L"--expansion-policy");
        appendArg(command, options.expansionPolicy);
    }
    if (options.expansionPolicy == L"echo" || options.expansionPolicy == L"harder-remix") {
        appendArg(command, L"--echo-policy");
        appendArg(command, L"stair-trill-stream");
        appendArg(command, L"--stream-echo-profile");
        appendArg(command, options.streamEchoProfile);
    }
    appendArg(command, L"--out");
    appendArg(command, quoteArg(paths.outputChart));
    appendArg(command, L"--report");
    appendArg(command, quoteArg(paths.reportJson));
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
    options.streamEchoProfile = comboText(state.streamProfileCombo);
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

std::optional<std::filesystem::path> browseFolder(HWND owner) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = L"Select output folder";
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
    if (!options.outputDir.empty()) {
        std::filesystem::create_directories(options.outputDir);
    }
    OutputPaths paths;
    const std::wstring command = buildSingleCommand(options, paths);
    state.lastCommand = command;
    appendLog(state, L"\r\n> " + command + L"\r\n");
    const auto result = runProcess(command, options.keyconvExe.parent_path());
    appendLog(state, widen(result.output) + L"\r\n");
    if (result.exitCode != 0) {
        MessageBoxW(state.hwnd, L"Convert failed. See log output.", L"KeyWeaver GUI", MB_ICONERROR);
        return;
    }
    state.lastOutputPath = paths.outputChart;
    state.lastReportPath = paths.reportJson;
    showReportSummary(state, parseReportSummary(paths.reportJson));
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
    appendLog(state, L"\r\n> " + command + L"\r\n");
    const auto result = runProcess(command, options.keyconvExe.parent_path());
    appendLog(state, widen(result.output) + L"\r\n");
    if (result.exitCode != 0) {
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
    for (const auto* item :
         {L"auto", L"preserve", L"preserve-tap-plus", L"chord-fill", L"training-scaffold", L"echo", L"harder-remix"}) {
        addComboItem(state.expansionCombo, item);
    }
    setComboSelection(state.expansionCombo, L"auto");

    makeControl(state, L"STATIC", L"Compress", 0, 320, y + 4, 70, 20, -1);
    state.compressCombo = makeControl(state, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 392, y, 180, 180,
                                      kComboCompress);
    for (const auto* item : {L"auto", L"preserve-strict", L"no-overlap-drop", L"no-overlap-roll",
                            L"no-overlap-hybrid", L"training-simplify"}) {
        addComboItem(state.compressCombo, item);
    }
    setComboSelection(state.compressCombo, L"auto");

    makeControl(state, L"STATIC", L"Stream", 0, 584, y + 4, 58, 20, -1);
    state.streamProfileCombo = makeControl(state, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                           642, y, 120, 140, kComboStreamProfile);
    for (const auto* item : {L"conservative", L"balanced", L"training", L"experimental"}) {
        addComboItem(state.streamProfileCombo, item);
    }
    setComboSelection(state.streamProfileCombo, L"conservative");
    y += 40;

    makeControl(state, L"BUTTON", L"Convert", BS_PUSHBUTTON, editX, y, 110, 28, kButtonConvert);
    makeControl(state, L"BUTTON", L"Matrix", BS_PUSHBUTTON, editX + 118, y, 92, 28, kButtonMatrix);
    makeControl(state, L"BUTTON", L"Open Output", BS_PUSHBUTTON, editX + 218, y, 120, 28, kButtonOpenOutput);
    makeControl(state, L"BUTTON", L"Open Report", BS_PUSHBUTTON, editX + 346, y, 112, 28, kButtonOpenReport);
    makeControl(state, L"BUTTON", L"Copy CLI", BS_PUSHBUTTON, editX + 466, y, 100, 28, kButtonCopyCommand);
    y += 42;

    makeControl(state, L"STATIC", L"Report / Matrix", 0, labelX, y + 4, 100, 20, -1);
    state.summaryList = makeControl(state, L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL,
                                    editX, y, 644, 130, kListSummary, WS_EX_CLIENTEDGE);
    y += 142;

    makeControl(state, L"STATIC", L"Log", 0, labelX, y + 4, 100, 20, -1);
    state.logEdit = makeControl(state, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                                   ES_READONLY | WS_VSCROLL | WS_HSCROLL,
                                editX, y, 644, 190, kEditLog, WS_EX_CLIENTEDGE);

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
            return 0;
        }
        case WM_COMMAND:
            if (!state) {
                break;
            }
            switch (LOWORD(wParam)) {
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
                        setWindowText(state->inputEdit, path->wstring());
                        syncOutputDirToInput(*state);
                        updateDetectedSource(*state);
                    }
                    return 0;
                }
                case kButtonBrowseOutputDir: {
                    const auto path = browseFolder(hwnd);
                    if (path.has_value()) {
                        setWindowText(state->outputDirEdit, path->wstring());
                    }
                    return 0;
                }
                case kButtonConvert:
                    executeSingleConvert(*state);
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
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int runGui() {
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
                                L"KeyWeaver v0.5.5 Playtest Tool",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                810,
                                650,
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
    return runGui();
}
