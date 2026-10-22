param(
    [string]$Version = "1.3.1",
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $ScriptDir "..")
Push-Location $Root

function Convert-ToPackageLogSafeText {
    param(
        [AllowEmptyString()][string]$Text,
        [hashtable]$Replacements
    )

    $Safe = $Text
    foreach ($entry in $Replacements.GetEnumerator()) {
        $Needle = [string]$entry.Key
        if ([string]::IsNullOrWhiteSpace($Needle)) {
            continue
        }
        $Replacement = [string]$entry.Value
        $Safe = $Safe.Replace($Needle, $Replacement)
        $Safe = $Safe.Replace($Needle.Replace("\", "/"), $Replacement)
    }
    return $Safe
}

function Sanitize-PackageTextFile {
    param(
        [string]$Path,
        [hashtable]$Replacements
    )

    if (-not (Test-Path $Path)) {
        return
    }
    $Text = Get-Content -LiteralPath $Path -Raw
    $Safe = Convert-ToPackageLogSafeText -Text $Text -Replacements $Replacements
    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Safe, $Utf8NoBom)
}

try {
    if ([string]::IsNullOrWhiteSpace($BuildDir)) {
        $BuildDir = Join-Path $Root "build\release-v$Version"
    }
    if ([string]::IsNullOrWhiteSpace($OutDir)) {
        $OutDir = Join-Path $Root "dist\release"
    }

    $BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
    $OutDir = [System.IO.Path]::GetFullPath($OutDir)
    New-Item -ItemType Directory -Force $BuildDir | Out-Null
    New-Item -ItemType Directory -Force $OutDir | Out-Null

    if (-not $SkipBuild) {
        & cmake -S $Root -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
        & cmake --build $BuildDir --parallel 4
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
    }

    & ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed" }

    $Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $PackageName = "KeyWeaver-v$Version-win64-$Stamp"
    $PackageDir = Join-Path $OutDir $PackageName
    if (Test-Path $PackageDir) {
        throw "Package directory already exists: $PackageDir"
    }
    New-Item -ItemType Directory -Force $PackageDir | Out-Null

    foreach ($exe in @("KeyWeaver.exe", "keyconv.exe", "keyconv_gui.exe")) {
        Copy-Item -LiteralPath (Join-Path $BuildDir $exe) -Destination $PackageDir
    }

    $Compiler = Get-Command "c++.exe" -ErrorAction SilentlyContinue
    if (-not $Compiler) {
        throw "Could not locate c++.exe in PATH; build first from a MinGW shell or add the compiler to PATH."
    }
    $RuntimeDir = Split-Path -Parent $Compiler.Source
    $RuntimeDlls = @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")
    foreach ($dll in $RuntimeDlls) {
        $dllPath = Join-Path $RuntimeDir $dll
        if (-not (Test-Path $dllPath)) {
            throw "Missing runtime DLL: $dllPath"
        }
        Copy-Item -LiteralPath $dllPath -Destination $PackageDir
    }

    foreach ($file in @("CHANGELOG.md", "LICENSE")) {
        Copy-Item -LiteralPath (Join-Path $Root $file) -Destination $PackageDir
    }
    Get-ChildItem -LiteralPath $Root -Filter "README*.md" -File |
        Copy-Item -Destination $PackageDir
    # An allowlist keeps private local experiments and generated songs out of
    # publishable packages, and also works from a source ZIP without Git.
    foreach ($Entry in Get-Content -LiteralPath (Join-Path $ScriptDir "release_files.txt")) {
        $Relative = $Entry.Trim()
        if (-not $Relative -or $Relative.StartsWith("#")) { continue }
        $Source = [System.IO.Path]::GetFullPath((Join-Path $Root $Relative))
        $Destination = [System.IO.Path]::GetFullPath((Join-Path $PackageDir $Relative))
        if (-not $Source.StartsWith("$Root\", [System.StringComparison]::OrdinalIgnoreCase) -or
            -not $Destination.StartsWith("$PackageDir\", [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Package entry must stay inside its source and destination roots"
        }
        New-Item -ItemType Directory -Force (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination
    }
    $BundledModels = "(none)"
    $PackageModelsDir = Join-Path $PackageDir "models"
    if (Test-Path $PackageModelsDir) {
        $BundledModels = (Get-ChildItem -LiteralPath $PackageModelsDir -Filter "*.onnx" -File |
            Select-Object -ExpandProperty Name) -join "`n"
        if ([string]::IsNullOrWhiteSpace($BundledModels)) {
            $BundledModels = "(none)"
        }
    }

    @"
KeyWeaver v$Version Windows x64 package

Run keyconv_gui.exe for the GUI. Double-clicking KeyWeaver.exe also opens keyconv_gui.exe when both files are in this folder.
Run KeyWeaver.exe from a terminal for CLI usage.
osu!mania outputs default beside the source chart when --out is omitted.
Drag files onto keyconv_gui.exe or KeyWeaver.exe to load them in the GUI first; set Target, then press Convert or Batch.
The GUI Target selector supports 1K through 18K; 10K GUI Classic conversions use Full-Field Mirror-Remix by default.
Localized README files are included: README.md, README.en.md, README.ko.md, README.ja.md, README.zh-CN.md, README.zh-TW.md, README.ru.md.
GUI Batch converts dropped charts or a selected songs/root folder and shows percent-done progress with remaining chart count.
Source can be auto or 1K-18K in the GUI; numeric Source filters batch inputs to matching source-key charts.
Experimental NK2 supports every 1K-18K source/target pair, including downward conversion and same-K transform relaning.
GUI NK2 Auto (7K presets): source 7K uses B for 4K/5K and E for 8K/10K; other pairs use faithful.
Select A-F to compare manual variants. B adds no notes; E adds taps/LNs only with full original-LN coverage for generated holds.
All manual NK2 A-F presets support upward and downward 1K-18K pairs. C-F append only safe notes without rearranging the base output; safe additions may be zero. Same-key A-F stays unchanged; use transform for same-key relaning.
CLI NK2 without explicit mode/search/note options uses the same source-7K defaults and native fallback for other pairs.
Phrase outputs must pass the independent safety audit before being saved. See docs/nk2-phrase-search.md.
Dropping files onto an already-open GUI window uses the current Target field; multi-file drops stay loaded for Batch.
CLI batch: pass multiple input charts plus explicit --target; outputs default beside each input chart and auto-detects CPU worker count.
BMS-family inputs stay BMS-family outputs (.bms, .bme, .bml, .pms); BMS to .osu output is intentionally rejected.
The GUI accepts osu!mania and BMS-family charts and preserves the BMS-family output extension.
Gesture Rail is on by default; use --gesture-rail off to compare older lane scoring.
Preserve Tap Plus uses key-growth budgets and 10K hand-zone balancing.
With --target-profile, Adaptive Growth Budget uses 1000 ms densityBuckets.low/mid/high/chordHeavy/jackRisk windows for Composer pressure.
Use --expansion-policy auto-low/auto-normal/auto-more for high-key generated-note budgets of 10%/15%/20%.
8K+ generated notes prefer 8th-beat source slices, avoid both-edge trill reinforcement, and favor mirror-lane symmetry; target-10 adds extra quarter/eighth-beat density pressure.
GUI target-10 default: --ten-key-planner staged-7-14-10 --ten-k-fullfield-remix, with the mode-local 1.6x total-density ceiling.
Use --preserve-convert for faithful mapping, strict source-jack preservation, no generated notes, and adjacent safe-lane drift.
Use --stream-transform superrandom for deterministic per-note random lane assignment, or full-jitter for 1-15 ms per-note zure-style timing spread.
Use --seed to vary deterministic stream-transform output.
Bundled profile: profiles/keyweaver_10k_broad_style_v1.json
Target-10 conversions auto-load the bundled profile; pass --target-profile to override it.
ONNX builds auto-load models\lane_policy_student_mlp_u_e_circusgalop.onnx for target-10 CLI/GUI batch when bundled; pass --no-auto-onnx-policy to force deterministic batch.
Normal-mode algorithm contract: docs/algorithm-lock-v0.6.0.md
10K Full-Field Mirror-Remix design lock: docs/design-10k-fullfield-remix.md

Bundled ONNX lane-policy models:
$BundledModels

Bundled MinGW runtime DLLs:
$($RuntimeDlls -join "`n")

Build verification: Release CMake build, all CTest suites (including NK2 safety, all-key phrase/default and batch regressions), public header smoke, GUI smoke, CLI batch/auto-low/auto-more/preserve-convert/stream-transform dry-run smokes, NK2 4K-to-18K and 7K-to-4K dry-run smokes, osu!mania sample conversion/report, KeyWeaver mode-marker smoke, reconversion guard smoke, BMS-to-BMS sample conversion/report, broad profile dry-run smoke, packaged algorithm-lock doc, and BMS-to-.osu guard smoke.
"@ | Set-Content -LiteralPath (Join-Path $PackageDir "PACKAGE_CONTENTS.txt") -Encoding UTF8

    @"
Bundled MinGW runtime DLLs:
$($RuntimeDlls -join "`n")
"@ | Set-Content -LiteralPath (Join-Path $PackageDir "DLL_DEPENDENCIES.txt") -Encoding UTF8

    $SmokeDir = Join-Path $PackageDir "smoke"
    New-Item -ItemType Directory -Force $SmokeDir | Out-Null
    $Exe = Join-Path $PackageDir "KeyWeaver.exe"
    $Gui = Join-Path $PackageDir "keyconv_gui.exe"

    $HelpText = & $Exe --help
    if ($LASTEXITCODE -ne 0 -or $HelpText[0] -ne "KeyWeaver v$Version") {
        throw "Packaged executable version does not match the requested release"
    }
    $HelpText | Set-Content -LiteralPath (Join-Path $SmokeDir "help.txt") -Encoding UTF8

    # Exercise the shipped executable as well as the build-tree CTest targets.
    foreach ($Target in @(4, 5, 8, 10)) {
        $ReportPath = Join-Path $SmokeDir "nk2_auto_7k_to_$($Target)k.json"
        & $Exe (Join-Path $PackageDir "samples\simple_7k_ln.osu") --target $Target --engine nk2 --dry-run --report $ReportPath *> (Join-Path $SmokeDir "nk2_auto_$($Target)k.txt")
        if ($LASTEXITCODE -ne 0) { throw "Packaged NK2 automatic preset smoke failed" }
        $Report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
        if (-not $Report.independentSafety.passed) { throw "Packaged NK2 preset audit failed" }
    }
    foreach ($Target in @(5, 7)) {
        foreach ($Variant in @("B", "E")) {
            $VariantArgs = @("--nk2-mode", "faithful", "--nk2-search", "phrase")
            if ($Variant -eq "E") {
                $VariantArgs += @("--nk2-phrase-support", "--nk2-phrase-dense-ln", "--nk2-phrase-source-ln")
            }
            $ReportPath = Join-Path $SmokeDir "nk2_4k_$($Variant)_$($Target)k.json"
            & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --target $Target --engine nk2 @VariantArgs --dry-run --report $ReportPath *> (Join-Path $SmokeDir "nk2_4k_$($Variant)_$($Target)k.txt")
            if ($LASTEXITCODE -ne 0) { throw "Packaged NK2 4K experiment smoke failed" }
            $Report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
            if (-not $Report.independentSafety.passed) { throw "Packaged NK2 4K experiment audit failed" }
        }
    }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --source 4 --target 10 --dry-run --report (Join-Path $SmokeDir "sample_4k_to_10k.report.json") *> (Join-Path $SmokeDir "sample_4k_to_10k.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "osu!mania sample smoke failed" }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") (Join-Path $PackageDir "samples\simple_7k_ln.osu") --target 10 --dry-run *> (Join-Path $SmokeDir "batch_cli.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "CLI batch dry-run smoke failed" }
    $BatchSmokeText = Get-Content -LiteralPath (Join-Path $SmokeDir "batch_cli.console.txt") -Raw
    if ($BatchSmokeText -notmatch "Batch summary: succeeded=2 failed=0 skipped=0") {
        throw "CLI batch dry-run smoke failed without the expected summary"
    }
    if ($BatchSmokeText -notmatch "Progress: 100% done, 0 left") {
        throw "CLI batch dry-run smoke failed without the expected progress output"
    }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --target 10 --expansion-policy auto-low --dry-run *> (Join-Path $SmokeDir "auto_low.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "auto-low dry-run smoke failed" }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --target 10 --expansion-policy auto-more --dry-run *> (Join-Path $SmokeDir "auto_more.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "auto-more dry-run smoke failed" }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --target 10 --preserve-convert --dry-run *> (Join-Path $SmokeDir "preserve_convert.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "preserve-convert dry-run smoke failed" }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --target 4 --stream-transform full-jitter --dry-run *> (Join-Path $SmokeDir "stream_full_jitter.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "stream full-jitter dry-run smoke failed" }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --target 4 --stream-transform superrandom --seed 7 --dry-run *> (Join-Path $SmokeDir "stream_superrandom.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "stream superrandom dry-run smoke failed" }

    $DifficultyMarkerOut = Join-Path $SmokeDir "difficulty_marker.osu"
    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --source 4 --target 10 --expansion-policy auto-more --stream-transform superrandom --seed 7 --out $DifficultyMarkerOut *> (Join-Path $SmokeDir "difficulty_marker.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "difficulty marker smoke failed" }
    $DifficultyMarkerText = Get-Content -LiteralPath $DifficultyMarkerOut -Raw
    if ($DifficultyMarkerText -notmatch "Version:4K KeyWeaver10K-sRan \(more\)") {
        throw "difficulty marker smoke failed without the expected Version marker"
    }

    $ReconvertGuardLog = Join-Path $SmokeDir "reconvert_guard_single.console.txt"
    $ReconvertGuardStdout = Join-Path $SmokeDir "reconvert_guard_single.stdout.tmp"
    $ReconvertGuardStderr = Join-Path $SmokeDir "reconvert_guard_single.stderr.tmp"
    $QuotedDifficultyMarkerOut = '"' + ($DifficultyMarkerOut -replace '"', '\"') + '"'
    $ReconvertGuardProcess = Start-Process -FilePath $Exe `
        -ArgumentList @($QuotedDifficultyMarkerOut, "--target", "7", "--dry-run") `
        -WorkingDirectory $PackageDir `
        -Wait `
        -PassThru `
        -NoNewWindow `
        -RedirectStandardOutput $ReconvertGuardStdout `
        -RedirectStandardError $ReconvertGuardStderr
    $ReconvertGuardExit = $ReconvertGuardProcess.ExitCode
    $ReconvertGuardText = ((Get-Content -LiteralPath $ReconvertGuardStdout -Raw -ErrorAction SilentlyContinue) +
                           (Get-Content -LiteralPath $ReconvertGuardStderr -Raw -ErrorAction SilentlyContinue))
    Set-Content -LiteralPath $ReconvertGuardLog -Value $ReconvertGuardText -Encoding UTF8
    Remove-Item -LiteralPath $ReconvertGuardStdout, $ReconvertGuardStderr -Force -ErrorAction SilentlyContinue
    if ($ReconvertGuardExit -eq 0) { throw "reconversion guard single smoke unexpectedly succeeded" }
    if ($ReconvertGuardText -notmatch "already-converted chart marker") {
        throw "reconversion guard single smoke failed without the expected guard message"
    }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") $DifficultyMarkerOut --target 10 --dry-run *> (Join-Path $SmokeDir "reconvert_guard_batch.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "reconversion guard batch smoke failed" }
    $ReconvertGuardBatchText = Get-Content -LiteralPath (Join-Path $SmokeDir "reconvert_guard_batch.console.txt") -Raw
    if ($ReconvertGuardBatchText -notmatch "Batch summary: succeeded=1 failed=0 skipped=1") {
        throw "reconversion guard batch smoke failed without the expected skip summary"
    }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --source 4 --target 10 --expansion-policy preserve-tap-plus --dry-run --report (Join-Path $SmokeDir "profile_4k_to_10k.report.json") *> (Join-Path $SmokeDir "profile_4k_to_10k.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "auto-profile sample smoke failed" }

    & $Exe (Join-Path $PackageDir "samples\simple_4k.osu") --source 4 --target 18 --engine nk2 --nk2-mode native --dry-run *> (Join-Path $SmokeDir "nk2_4k_to_18k.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "NK2 4K-to-18K dry-run smoke failed" }

    & $Exe (Join-Path $PackageDir "samples\simple_7k_ln.osu") --source 7 --target 4 --engine nk2 --nk2-mode native --dry-run *> (Join-Path $SmokeDir "nk2_7k_to_4k.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "NK2 7K-to-4K dry-run smoke failed" }

    & $Exe (Join-Path $PackageDir "samples\simple_bms_4k.bms") --source 4 --target 10 --dry-run --report (Join-Path $SmokeDir "sample_bms_4k.report.json") *> (Join-Path $SmokeDir "sample_bms_4k.console.txt")
    if ($LASTEXITCODE -ne 0) { throw "BMS sample smoke failed" }

    $PreviousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Exe (Join-Path $PackageDir "samples\simple_bms_4k.bms") --source 4 --target 10 --out (Join-Path $SmokeDir "blocked.osu") --dry-run *> (Join-Path $SmokeDir "bms_to_osu_guard.txt")
    $BmsGuardExitCode = $LASTEXITCODE
    $ErrorActionPreference = $PreviousErrorActionPreference
    if ($BmsGuardExitCode -eq 0) { throw "BMS-to-osu guard smoke unexpectedly succeeded" }
    $BmsGuardText = Get-Content -LiteralPath (Join-Path $SmokeDir "bms_to_osu_guard.txt") -Raw
    if ($BmsGuardText -notmatch "BMS input can only write BMS-family output") {
        throw "BMS-to-osu guard smoke failed without the expected error message"
    }
    @(
        "BMS-to-osu guard smoke: passed",
        "Expected nonzero exit code: $BmsGuardExitCode",
        "error: BMS input can only write BMS-family output (.bms, .bme, .bml, .pms)."
    ) | Set-Content -LiteralPath (Join-Path $SmokeDir "bms_to_osu_guard.txt") -Encoding UTF8

    & $Gui --smoke (Join-Path $PackageDir "samples\simple_4k.osu") (Join-Path $SmokeDir "gui") *> (Join-Path $SmokeDir "gui_smoke.txt")
    if ($LASTEXITCODE -ne 0) { throw "GUI smoke failed" }

    $LogReplacements = @{}
    $LogReplacements[$PackageDir] = "<package>"
    $LogReplacements[$Root] = "<repo>"
    $LogReplacements[$OutDir] = "<release>"
    $LogReplacements[$BuildDir] = "<build>"
    Get-ChildItem -LiteralPath $SmokeDir -Recurse -File |
        Where-Object { $_.Extension -in @(".txt", ".json") } |
        ForEach-Object { Sanitize-PackageTextFile -Path $_.FullName -Replacements $LogReplacements }

    $ZipPath = Join-Path $OutDir "$PackageName.zip"
    Compress-Archive -LiteralPath $PackageDir -DestinationPath $ZipPath
    $Hash = Get-FileHash -Algorithm SHA256 -LiteralPath $ZipPath
    "$($Hash.Hash)  $([System.IO.Path]::GetFileName($ZipPath))" |
        Set-Content -LiteralPath "$ZipPath.sha256" -Encoding ASCII

    Write-Host "package=$ZipPath"
    Write-Host "sha256=$($Hash.Hash)"
    Write-Host "dir=$PackageDir"
}
finally {
    Pop-Location
}
