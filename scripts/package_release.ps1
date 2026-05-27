param(
    [string]$Version = "0.5.5",
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
BMS-family inputs stay BMS-family outputs (.bms, .bme, .bml, .pms); BMS to .osu output is intentionally rejected.
The GUI accepts osu!mania and BMS-family charts and preserves the BMS-family output extension.
Gesture Rail is on by default; use --gesture-rail off to compare older lane scoring.
Preserve Tap Plus uses key-growth budgets and 10K hand-zone balancing.
With --target-profile, Adaptive Growth Budget uses 1000 ms densityBuckets.low/mid/high/chordHeavy/jackRisk windows for Composer pressure.
Bundled profile: profiles/keyweaver_10k_broad_style_v1.json
Target-10 conversions auto-load the bundled profile; pass --target-profile to override it.
Frozen algorithm contract: docs/algorithm-lock-v0.5.5.md

Bundled MinGW runtime DLLs:
$($RuntimeDlls -join "`n")

Build verification: Release CMake build, unit tests, public header smoke, GUI smoke, osu!mania sample conversion/report, BMS-to-BMS sample conversion/report, broad profile dry-run smoke, packaged algorithm-lock doc, and BMS-to-.osu guard smoke.
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
