cmake_minimum_required(VERSION 3.20)

project(KeyWeaver VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(keyconv_core
    src/core/assignment.cpp
    src/core/collision.cpp
    src/core/compress.cpp
    src/core/converter.cpp
    src/core/convert.cpp
    src/core/distance.cpp
    src/core/expansion.cpp
    src/core/gesture.cpp
    src/core/mapping.cpp
    src/core/optimizer.cpp
    src/core/pattern.cpp
    src/core/quality.cpp
    src/core/repeat.cpp
    src/core/report.cpp
    src/core/slice.cpp
    src/exporter/bms.cpp
    src/exporter/osu.cpp
    src/parser/bms.cpp
    src/parser/osu.cpp
)

target_include_directories(keyconv_core
    PUBLIC
        include
    PRIVATE
        src
)
target_compile_features(keyconv_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(keyconv_core PRIVATE /W4)
else()
    target_compile_options(keyconv_core PRIVATE -Wall -Wextra -pedantic)
endif()

add_executable(keyconv src/cli.cpp)
target_link_libraries(keyconv PRIVATE keyconv_core)

add_executable(KeyWeaver src/cli.cpp)
target_link_libraries(KeyWeaver PRIVATE keyconv_core)

if(WIN32)
    target_link_libraries(keyconv PRIVATE shell32 user32)
    target_link_libraries(KeyWeaver PRIVATE shell32 user32)
endif()

if(WIN32)
    add_executable(keyconv_gui src/gui_win32.cpp)
    target_compile_features(keyconv_gui PRIVATE cxx_std_20)
    target_include_directories(keyconv_gui PRIVATE include)
    target_link_libraries(keyconv_gui PRIVATE comdlg32 shell32 ole32 user32 gdi32)
endif()

enable_testing()

add_executable(keyconv_tests tests/test_main.cpp)
target_link_libraries(keyconv_tests PRIVATE keyconv_core)
target_include_directories(keyconv_tests PRIVATE src)
add_test(NAME keyconv_tests COMMAND keyconv_tests)

add_executable(keyconv_public_header_smoke tests/public_header_smoke.cpp)
target_link_libraries(keyconv_public_header_smoke PRIVATE keyconv_core)
add_test(NAME keyconv_public_header_smoke COMMAND keyconv_public_header_smoke)
