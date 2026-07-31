cmake_minimum_required(VERSION 3.20)

project(KeyWeaver VERSION 1.2.0 LANGUAGES CXX)

option(KEYWEAVER_WITH_ONNXRUNTIME "Enable experimental ONNX Runtime lane policy support" OFF)
set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime package root")

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
    src/nk2/intent_graph.cpp
    src/nk2/layout_model.cpp
    src/nk2/nk2_convert.cpp
    src/nk2/nk2_report.cpp
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

set(KEYWEAVER_CLI_LIBS keyconv_core)

if(KEYWEAVER_WITH_ONNXRUNTIME)
    set(_onnxruntime_roots)
    if(ONNXRUNTIME_ROOT)
        list(APPEND _onnxruntime_roots "${ONNXRUNTIME_ROOT}")
    endif()
    if(DEFINED ENV{USERPROFILE})
        file(GLOB _onnxruntime_nuget_roots
            "$ENV{USERPROFILE}/.nuget/packages/microsoft.ml.onnxruntime/*"
            "$ENV{USERPROFILE}/.nuget/packages/microsoft.ml.onnxruntime.gpu/*"
            "$ENV{USERPROFILE}/.nuget/packages/microsoft.ml.onnxruntime.gpu.windows/*")
        list(APPEND _onnxruntime_roots ${_onnxruntime_nuget_roots})
    endif()

    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        PATHS ${_onnxruntime_roots}
        PATH_SUFFIXES include build/native/include buildTransitive/native/include
    )
    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        PATHS ${_onnxruntime_roots}
        PATH_SUFFIXES lib runtimes/win-x64/native runtimes/linux-x64/native runtimes/osx-x64/native
    )

    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR
            "KEYWEAVER_WITH_ONNXRUNTIME requires ONNXRUNTIME_ROOT or a local Microsoft.ML.OnnxRuntime package")
    endif()

    add_library(keyconv_onnx
        src/onnx/onnx_cuda_policy.cpp
    )
    target_compile_features(keyconv_onnx PUBLIC cxx_std_20)
    target_include_directories(keyconv_onnx
        PUBLIC
            include
        PRIVATE
            src
            ${ONNXRUNTIME_INCLUDE_DIR}
    )
    target_link_libraries(keyconv_onnx PUBLIC keyconv_core ${ONNXRUNTIME_LIBRARY})
    target_compile_definitions(keyconv_onnx PUBLIC KEYWEAVER_WITH_ONNXRUNTIME=1)
    list(APPEND KEYWEAVER_CLI_LIBS keyconv_onnx)
endif()

add_executable(keyconv src/cli.cpp)
target_link_libraries(keyconv PRIVATE ${KEYWEAVER_CLI_LIBS})

add_executable(KeyWeaver src/cli.cpp)
target_link_libraries(KeyWeaver PRIVATE ${KEYWEAVER_CLI_LIBS})

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
