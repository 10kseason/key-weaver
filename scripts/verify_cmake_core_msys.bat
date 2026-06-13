@echo off
setlocal

set "REPO_ROOT=%~dp0.."
for %%I in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fI"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=classic"
if /I "%MODE%"=="--help" goto :help
if /I "%MODE%"=="help" goto :help
if /I "%MODE%"=="classic" goto :mode_ok
if /I "%MODE%"=="onnx" goto :mode_ok
echo ERROR: Unknown mode "%MODE%".
goto :help_error

:mode_ok
set "BUILD_DIR=C:\tmp\keyweaver-core-verify-msys"
if /I "%MODE%"=="onnx" set "BUILD_DIR=C:\tmp\keyweaver-core-verify-msys-onnx"

set "CMAKE_EXE=C:\msys64\mingw64\bin\cmake.exe"
set "NINJA_EXE=C:\msys64\mingw64\bin\ninja.exe"
set "NINJA_CMAKE=C:/msys64/mingw64/bin/ninja.exe"
set "SAMPLE=%REPO_ROOT%\samples\simple_4k.osu"
set "SAMPLE_BATCH=%REPO_ROOT%\samples\simple_7k_ln.osu"
set "MODEL=%REPO_ROOT%\models\u_e_circusgalop_chart_dataset_lane_policy.onnx"
set "KEYWEAVER_EXE=%BUILD_DIR%\KeyWeaver.exe"
set "TEST_EXE=%BUILD_DIR%\keyconv_tests.exe"
set "HEADER_SMOKE_EXE=%BUILD_DIR%\keyconv_public_header_smoke.exe"

echo [KeyWeaver core CMake verify]
echo Repo:  "%REPO_ROOT%"
echo Build: "%BUILD_DIR%"
echo Mode:  "%MODE%"
echo CMake: "%CMAKE_EXE%"
echo Ninja: "%NINJA_EXE%"
echo.

if not exist "%CMAKE_EXE%" (
  echo ERROR: CMake not found: "%CMAKE_EXE%"
  exit /b 1
)

if not exist "%NINJA_EXE%" (
  echo ERROR: Ninja not found: "%NINJA_EXE%"
  exit /b 1
)

if not exist "%SAMPLE%" (
  echo ERROR: Sample chart not found: "%SAMPLE%"
  exit /b 1
)

if /I "%MODE%"=="onnx" (
  call :resolve_onnx_root
  if errorlevel 1 exit /b %errorlevel%
)

if not exist "C:\tmp" mkdir "C:\tmp"
if errorlevel 1 exit /b %errorlevel%

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%

echo [1/5] Configure with MSYS2 Ninja...
if /I "%MODE%"=="onnx" (
  "%CMAKE_EXE%" -S "%REPO_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_CMAKE%" -DKEYWEAVER_WITH_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT="%ONNXRUNTIME_ROOT%"
) else (
  "%CMAKE_EXE%" -S "%REPO_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_CMAKE%"
)
if errorlevel 1 (
  echo Configure failed.
  exit /b %errorlevel%
)

echo.
echo [2/5] Build core targets...
"%NINJA_EXE%" -C "%BUILD_DIR%" KeyWeaver keyconv_tests keyconv_public_header_smoke
if errorlevel 1 (
  echo Build failed.
  exit /b %errorlevel%
)

if not exist "%KEYWEAVER_EXE%" (
  echo ERROR: KeyWeaver executable not found after build: "%KEYWEAVER_EXE%"
  exit /b 1
)
if not exist "%TEST_EXE%" (
  echo ERROR: Test executable not found after build: "%TEST_EXE%"
  exit /b 1
)
if not exist "%HEADER_SMOKE_EXE%" (
  echo ERROR: Header smoke executable not found after build: "%HEADER_SMOKE_EXE%"
  exit /b 1
)

echo.
echo [3/5] Run unit and header smoke tests...
"%TEST_EXE%"
if errorlevel 1 (
  echo Unit tests failed.
  exit /b %errorlevel%
)
"%HEADER_SMOKE_EXE%"
if errorlevel 1 (
  echo Header smoke failed.
  exit /b %errorlevel%
)

echo.
echo [4/5] Run CLI smoke...
pushd "%REPO_ROOT%" >nul
"%KEYWEAVER_EXE%" --help >nul
if errorlevel 1 (
  popd >nul
  echo CLI help smoke failed.
  exit /b %errorlevel%
)
"%KEYWEAVER_EXE%" "%SAMPLE%" --target 10 --dry-run
if errorlevel 1 (
  popd >nul
  echo CLI dry-run smoke failed.
  exit /b %errorlevel%
)

echo.
echo [5/5] Optional ONNX smoke...
if /I "%MODE%"=="onnx" (
  if not exist "%MODEL%" (
    popd >nul
    echo ERROR: ONNX mode selected but model file was not found: "%MODEL%"
    exit /b 1
  )
  "%KEYWEAVER_EXE%" "%SAMPLE%" "%SAMPLE_BATCH%" --target 10 --batch --onnx-policy "%MODEL%" --onnx-provider cpu --onnx-policy-strict --dry-run
  if errorlevel 1 (
    popd >nul
    echo ONNX strict batch smoke failed.
    exit /b %errorlevel%
  )
) else (
  echo Skipped. Run "%~nx0 onnx" to verify the ONNX Runtime build path.
)
popd >nul

echo.
echo OK: KeyWeaver core CMake verification passed.
exit /b 0

:resolve_onnx_root
if defined ONNXRUNTIME_ROOT goto :onnx_root_ready
set "NUGET_ORT_ROOT=%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime"
if exist "%NUGET_ORT_ROOT%" (
  for /f "delims=" %%V in ('dir /b /ad /o-n "%NUGET_ORT_ROOT%" 2^>nul') do (
    if not defined ONNXRUNTIME_ROOT set "ONNXRUNTIME_ROOT=%NUGET_ORT_ROOT%\%%V"
  )
)

:onnx_root_ready
if not defined ONNXRUNTIME_ROOT (
  echo ERROR: ONNX mode requires ONNXRUNTIME_ROOT or a NuGet Microsoft.ML.OnnxRuntime package.
  echo Example:
  echo   set "ONNXRUNTIME_ROOT=%%USERPROFILE%%\.nuget\packages\microsoft.ml.onnxruntime\1.22.0"
  exit /b 1
)
echo ONNX Runtime root: "%ONNXRUNTIME_ROOT%"
exit /b 0

:help
echo Usage:
echo   scripts\verify_cmake_core_msys.bat
echo   scripts\verify_cmake_core_msys.bat classic
echo   scripts\verify_cmake_core_msys.bat onnx
echo.
echo classic: configure, build, run unit/header/CLI smoke checks without ONNX Runtime.
echo onnx:    same checks with KEYWEAVER_WITH_ONNXRUNTIME=ON, then strict ONNX batch smoke.
exit /b 0

:help_error
call :help
exit /b 1
