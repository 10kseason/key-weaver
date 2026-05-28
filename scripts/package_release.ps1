param(
    [string]$Version = "0.6.0",
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
        & cmake --build $BuildDir --target KeyWeaver keyconv keyconv_gui keyconv_tests keyconv_public_header_smoke
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
    }

    & (Join-Path $BuildDir "keyconv_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "keyconv_tests failed" }
    & (Join-Path $BuildDir "keyconv_public_header_smoke.exe")
    if ($LASTEXITCODE -ne 0) { throw "keyconv_public_header_smoke failed" }

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

    foreach ($file in @("README.md", "CHANGELOG.md", "LICENSE")) {
        Copy-Item -LiteralPath (Join-Path $Root $file) -Destination $PackageDir
    }
    foreach ($dir in @("samples", "profiles", "docs")) {
        Copy-Item -LiteralPath (Join-Path $Root $dir) -Destination (Join-Path $PackageDir $dir) -Recurse
    }
    Get-ChildItem -LiteralPath (Join-Path $PackageDir "samples") -Filter "* KeyWeaver*.osu" -File |
        Remove-Item -Force
    $PackageScriptsDir = Join-Path $PackageDir "scripts"
    New-Item -ItemType Directory -Force $PackageScriptsDir | Out-Null
    foreach ($scriptFile in @("build_target_k_profile.py", "package_release.ps1", "u_e_10k_curated_patterns.txt")) {
        Copy-Item -LiteralPath (Join-Path $Root "scripts\$scriptFile") -Destination $PackageScriptsDir
    }

    @"
KeyWeaver v$Version Windows x64 package

Run keyconv_gui.exe for the GUI. Double-clicking KeyWeaver.exe also opens keyconv_gui.exe when both files are in this folder.
Run KeyWeaver.exe from a terminal for CLI usage.
osu!mania outputs default beside the source chart when --out is omitted.
Drag files onto keyconv_gui.exe or KeyWeaver.exe to load them in the GUI first; set Target, then press Convert or Batch.
GUI Batch converts dropped charts or a selected songs/root folder and shows percent-done progress with remaining chart count.
Source override is passed to GUI conversions.
Dropping files onto an already-open GUI window uses the current Target field; multi-file drops stay loaded for Batch.
CLI batch: pass multiple input charts plus explicit --target; outputs default beside each input chart and auto-detects CPU worker count.
BMS-family inputs stay BMS-family outputs (.bms, .bme, .bml, .pms); BMS to .osu output is intentionally rejected.
The GUI accepts osu!mania and BMS-family charts and preserves the BMS-family output extension.
Gesture Rail is on by default; use --gesture-rail off to compare older lane scoring.
Preserve Tap Plus uses key-growth budgets and 10K hand-zone balancing.
With --target-profile, Adaptive Growth Budget uses 1000 ms densityBuckets.low/mid/high/chordHeavy/jackRisk windows for Composer pressure.
Use --expansion-policy auto-low/auto-normal/auto-more for high-key generated-note budgets of 10%/15%/20%.
8K+ generated notes prefer 8th-beat source slices, avoid both-edge trill reinforcement, and favor mirror-lane symmetry; target-10 adds extra quarter/eighth-beat density pressure.
Use --preserve-convert for faithful mapping, strict source-jack preservation, no generated notes, and adjacent safe-lane drift.
Use --stream-transform superrandom for deterministic per-note random lane assignment, or full-jitter for 1-15 ms per-note zure-style timing spread.
Use --seed to vary deterministic stream-transform output.
Bundled profile: profiles/keyweaver_10k_broad_style_v1.json
Target-10 conversions auto-load the bundled profile; pass --target-profile to override it.
Normal-mode algorithm contract: docs/algorithm-lock-v0.6.0.md

Bundled MinGW runtime DLLs:
$($RuntimeDlls -join "`n")

Build verification: Release CMake build, unit tests, public header smoke, GUI smoke, CLI batch/auto-low/auto-more/preserve-convert/stream-transform dry-run smokes, osu!mania sample conversion/report, KeyWeaver mode-marker smoke, reconversion guard smoke, BMS-to-BMS sample conversion/report, broad profile dry-run smoke, packaged algorithm-lock doc, and BMS-to-.osu guard smoke.
"@ | Set-Content -LiteralPath (Join-Path $PackageDir "PACKAGE_CONTENTS.txt") -Encoding UTF8

    @"
Bundled MinGW runtime DLLs:
$($RuntimeDlls -join "`n")
"@ | Set-Content -LiteralPath (Join-Path $PackageDir "DLL_DEPENDENCIES.txt") -Encoding UTF8

    $SmokeDir = Join-Path $PackageDir "smoke"
    New-Item -ItemType Directory -Force $SmokeDir | Out-Null
    $Exe = Join-Path $PackageDir "KeyWeaver.exe"
    $Gui = Join-Path $PackageDir "keyconv_gui.exe"

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
