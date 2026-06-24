param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'

$packageRoot = Join-Path $env:USERPROFILE '.nuget\packages'
if (-not (Test-Path -LiteralPath $packageRoot)) {
    Write-Warning "NuGet package cache was not found: $packageRoot"
    exit 0
}

$dllNames = @(
    'cublasLt64_12.dll',
    'cublas64_12.dll',
    'cufft64_11.dll',
    'cudart64_12.dll',
    'cudnn64_9.dll',
    'cudnn_adv64_9.dll',
    'cudnn_cnn64_9.dll',
    'cudnn_engines_precompiled64_9.dll',
    'cudnn_engines_runtime_compiled64_9.dll',
    'cudnn_graph64_9.dll',
    'cudnn_heuristic64_9.dll',
    'cudnn_ops64_9.dll'
)

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

foreach ($name in $dllNames) {
    $hit = Get-ChildItem -LiteralPath $packageRoot -Recurse -Filter $name -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Length -gt 0 } |
        Sort-Object Length -Descending |
        Select-Object -First 1

    if ($null -eq $hit) {
        Write-Warning "CUDA runtime DLL not found in NuGet cache: $name"
        continue
    }

    Copy-Item -LiteralPath $hit.FullName -Destination $BuildDir -Force
    Write-Host "Copied $name"
}
