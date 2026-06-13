@echo off
setlocal

set "REPO_ROOT=%~dp0.."
for %%I in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fI"

set "BUILD_DIR=C:\tmp\keyweaver-build-verify-msys"
set "CMAKE_EXE=C:\msys64\mingw64\bin\cmake.exe"
set "NINJA_EXE=C:\msys64\mingw64\bin\ninja.exe"
set "NINJA_CMAKE=C:/msys64/mingw64/bin/ninja.exe"

echo [KeyWeaver GUI build verify]
echo Repo:  "%REPO_ROOT%"
echo Build: "%BUILD_DIR%"
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

if not exist "C:\tmp" mkdir "C:\tmp"
if errorlevel 1 exit /b %errorlevel%

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%

echo [1/2] Configure with MSYS2 Ninja...
"%CMAKE_EXE%" -S "%REPO_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_CMAKE%"
if errorlevel 1 (
  echo Configure failed.
  exit /b %errorlevel%
)

echo.
echo [2/2] Build KeyWeaver and keyconv_gui...
"%NINJA_EXE%" -C "%BUILD_DIR%" KeyWeaver keyconv_gui
if errorlevel 1 (
  echo Build failed.
  exit /b %errorlevel%
)

echo.
echo OK: KeyWeaver and keyconv_gui build verification passed.
exit /b 0
