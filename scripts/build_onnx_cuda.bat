@echo off
setlocal EnableExtensions

rem KeyWeaver CUDA ONNX Runtime CMake build helper.
rem Override with:
rem   set KW_BUILD_DIR=C:\keyweaver-cmake
rem   set ONNXRUNTIME_ROOT=%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.gpu\1.26.0
rem   set KW_RUN_TESTS=0
rem   set KW_RUN_SMOKE=1
rem   set KW_MODEL=C:\path\to\lane_policy.onnx
rem   set KW_ALLOW_CPU_ORT=1  (compile-only fallback; CUDA smoke will still fail)

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"

if not defined KW_GENERATOR set "KW_GENERATOR=Ninja"
if not defined KW_TARGETS set "KW_TARGETS=KeyWeaver keyconv_tests"
if not defined KW_RUN_TESTS set "KW_RUN_TESTS=1"
if not defined KW_RUN_SMOKE set "KW_RUN_SMOKE=0"

if not defined KW_BUILD_DIR set "KW_BUILD_DIR=C:\keyweaver-cmake"

if not defined ONNXRUNTIME_ROOT (
    if exist "%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.gpu.windows" (
        for /f "delims=" %%D in ('dir /b /ad /o-n "%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.gpu.windows" 2^>nul') do (
            if not defined ONNXRUNTIME_ROOT set "ONNXRUNTIME_ROOT=%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.gpu.windows\%%D"
        )
    )
)

if not defined ONNXRUNTIME_ROOT (
    if exist "%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.gpu" (
        for /f "delims=" %%D in ('dir /b /ad /o-n "%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.gpu" 2^>nul') do (
            if not defined ONNXRUNTIME_ROOT set "ONNXRUNTIME_ROOT=%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.gpu\%%D"
        )
    )
)

if not defined ONNXRUNTIME_ROOT (
    if exist "%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime" (
        if "%KW_ALLOW_CPU_ORT%"=="1" (
            for /f "delims=" %%D in ('dir /b /ad /o-n "%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime" 2^>nul') do (
                if not defined ONNXRUNTIME_ROOT set "ONNXRUNTIME_ROOT=%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime\%%D"
            )
        )
    )
)

if not defined ONNXRUNTIME_ROOT (
    echo error: ONNXRUNTIME_ROOT is not set and no NuGet ONNX Runtime package was found.
    echo Install or point ONNXRUNTIME_ROOT at Microsoft.ML.OnnxRuntime.Gpu for CUDA.
    exit /b 1
)

if exist "%ONNXRUNTIME_ROOT%\build\native\include\onnxruntime_cxx_api.h" (
    set "ORT_INCLUDE=%ONNXRUNTIME_ROOT%\build\native\include"
) else if exist "%ONNXRUNTIME_ROOT%\buildTransitive\native\include\onnxruntime_cxx_api.h" (
    set "ORT_INCLUDE=%ONNXRUNTIME_ROOT%\buildTransitive\native\include"
) else (
    echo error: ONNXRUNTIME_ROOT does not look like a native ONNX Runtime package:
    echo   %ONNXRUNTIME_ROOT%
    exit /b 1
)

set "ORT_NATIVE=%ONNXRUNTIME_ROOT%\runtimes\win-x64\native"
if not exist "%ORT_NATIVE%\onnxruntime.lib" (
    echo error: ONNX Runtime native library was not found:
    echo   %ORT_NATIVE%\onnxruntime.lib
    exit /b 1
)

if not exist "%ORT_NATIVE%\onnxruntime_providers_cuda.dll" (
    if not "%KW_ALLOW_CPU_ORT%"=="1" (
        echo error: CUDA provider DLL was not found:
        echo   %ORT_NATIVE%\onnxruntime_providers_cuda.dll
        echo Use Microsoft.ML.OnnxRuntime.Gpu or set KW_ALLOW_CPU_ORT=1 for compile-only checks.
        exit /b 1
    )
    echo warning: CUDA provider DLL was not found; this is compile-only and CUDA smoke will fail.
)

echo Source: %REPO_ROOT%
echo Build:  %KW_BUILD_DIR%
echo ORT:    %ONNXRUNTIME_ROOT%
echo.

cmake -S "%REPO_ROOT%" -B "%KW_BUILD_DIR%" -G "%KW_GENERATOR%" -DKEYWEAVER_WITH_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT="%ONNXRUNTIME_ROOT%"
if errorlevel 1 exit /b %errorlevel%

cmake --build "%KW_BUILD_DIR%" --target %KW_TARGETS%
if errorlevel 1 exit /b %errorlevel%

if exist "%ORT_NATIVE%\*.dll" (
    echo Copying ONNX Runtime DLLs into build directory...
    copy /Y "%ORT_NATIVE%\*.dll" "%KW_BUILD_DIR%\" >nul
)

if exist "%REPO_ROOT%\models\*.onnx" (
    echo Copying bundled ONNX models into build directory...
    if not exist "%KW_BUILD_DIR%\models" mkdir "%KW_BUILD_DIR%\models"
    copy /Y "%REPO_ROOT%\models\*.onnx" "%KW_BUILD_DIR%\models\" >nul
    if exist "%REPO_ROOT%\models\*.onnx.data" copy /Y "%REPO_ROOT%\models\*.onnx.data" "%KW_BUILD_DIR%\models\" >nul
)

if exist "%SCRIPT_DIR%copy_cuda_runtime_deps.ps1" (
    echo Copying CUDA runtime DLLs from NuGet cache when available...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%copy_cuda_runtime_deps.ps1" -BuildDir "%KW_BUILD_DIR%"
    if errorlevel 1 exit /b %errorlevel%
)

if "%KW_RUN_TESTS%"=="1" (
    if exist "%KW_BUILD_DIR%\keyconv_tests.exe" (
        "%KW_BUILD_DIR%\keyconv_tests.exe"
        if errorlevel 1 exit /b %errorlevel%
    ) else (
        echo warning: keyconv_tests.exe was not found; skipping tests.
    )
)

if "%KW_RUN_SMOKE%"=="1" (
    if not defined KW_MODEL (
        set "KW_MODEL=%KW_BUILD_DIR%\models\lane_policy_student_mlp_u_e_circusgalop.onnx"
        set "KW_SMOKE_AUTO_ONNX=1"
    )
    if not exist "%KW_MODEL%" (
        echo error: KW_MODEL was not found:
        echo   %KW_MODEL%
        exit /b 1
    )
    if "%KW_SMOKE_AUTO_ONNX%"=="1" (
        "%KW_BUILD_DIR%\KeyWeaver.exe" "%REPO_ROOT%\samples\simple_4k.osu" "%REPO_ROOT%\samples\simple_7k_ln.osu" --target 10 --batch --dry-run --verbose
        if errorlevel 1 exit /b %errorlevel%
    ) else (
        "%KW_BUILD_DIR%\KeyWeaver.exe" "%REPO_ROOT%\samples\simple_4k.osu" "%REPO_ROOT%\samples\simple_7k_ln.osu" --target 10 --batch --onnx-policy "%KW_MODEL%" --onnx-provider cuda --onnx-policy-strict --dry-run --verbose
        if errorlevel 1 exit /b %errorlevel%
    )
)

echo.
echo done
echo Build output: %KW_BUILD_DIR%
exit /b 0
