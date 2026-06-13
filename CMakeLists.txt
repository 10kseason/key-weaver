cmake_minimum_required(VERSION 3.20)

project(KeyWeaver VERSION 1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KEYWEAVER_WITH_ONNXRUNTIME "Enable experimental ONNX Runtime lane-policy inference" OFF)
set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime install root containing include/ and lib/")

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
    src/core/model_policy.cpp
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

if(KEYWEAVER_WITH_ONNXRUNTIME)
    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        HINTS
            "${ONNXRUNTIME_ROOT}/include"
            "${ONNXRUNTIME_ROOT}/build/native/include"
            "$ENV{ONNXRUNTIME_ROOT}/include"
            "$ENV{ONNXRUNTIME_ROOT}/build/native/include"
    )
    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        HINTS
            "${ONNXRUNTIME_ROOT}/lib"
            "${ONNXRUNTIME_ROOT}/lib/x64"
            "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native"
            "${ONNXRUNTIME_ROOT}/runtimes/linux-x64/native"
            "$ENV{ONNXRUNTIME_ROOT}/lib"
            "$ENV{ONNXRUNTIME_ROOT}/lib/x64"
            "$ENV{ONNXRUNTIME_ROOT}/runtimes/win-x64/native"
            "$ENV{ONNXRUNTIME_ROOT}/runtimes/linux-x64/native"
    )
    find_file(ONNXRUNTIME_DLL
        NAMES onnxruntime.dll
        HINTS
            "${ONNXRUNTIME_ROOT}/bin"
            "${ONNXRUNTIME_ROOT}/lib"
            "${ONNXRUNTIME_ROOT}/lib/x64"
            "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native"
            "$ENV{ONNXRUNTIME_ROOT}/bin"
            "$ENV{ONNXRUNTIME_ROOT}/lib"
            "$ENV{ONNXRUNTIME_ROOT}/lib/x64"
            "$ENV{ONNXRUNTIME_ROOT}/runtimes/win-x64/native"
    )
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR "KEYWEAVER_WITH_ONNXRUNTIME=ON requires ONNXRUNTIME_ROOT with include/onnxruntime_cxx_api.h and lib/onnxruntime")
    endif()
    if(WIN32 AND NOT ONNXRUNTIME_DLL)
        message(FATAL_ERROR "KEYWEAVER_WITH_ONNXRUNTIME=ON requires onnxruntime.dll on Windows")
    endif()
    if(WIN32)
        get_filename_component(ONNXRUNTIME_DLL_DIR "${ONNXRUNTIME_DLL}" DIRECTORY)
        file(GLOB ONNXRUNTIME_EXTRA_DLLS CONFIGURE_DEPENDS
            "${ONNXRUNTIME_DLL_DIR}/onnxruntime_providers*.dll"
            "${ONNXRUNTIME_DLL_DIR}/DirectML.dll"
        )
        set(ONNXRUNTIME_RUNTIME_DLLS "${ONNXRUNTIME_DLL}" ${ONNXRUNTIME_EXTRA_DLLS})
        list(REMOVE_DUPLICATES ONNXRUNTIME_RUNTIME_DLLS)
    endif()
    target_compile_definitions(keyconv_core PRIVATE KEYWEAVER_WITH_ONNXRUNTIME=1)
    target_include_directories(keyconv_core PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(keyconv_core PRIVATE "${ONNXRUNTIME_LIBRARY}")
else()
    target_compile_definitions(keyconv_core PRIVATE KEYWEAVER_WITH_ONNXRUNTIME=0)
endif()

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

if(KEYWEAVER_WITH_ONNXRUNTIME AND WIN32)
    function(keyweaver_copy_onnxruntime_runtime target_name)
        foreach(runtime_dll IN LISTS ONNXRUNTIME_RUNTIME_DLLS)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${runtime_dll}"
                    "$<TARGET_FILE_DIR:${target_name}>"
                VERBATIM
            )
        endforeach()
    endfunction()

    keyweaver_copy_onnxruntime_runtime(keyconv)
    keyweaver_copy_onnxruntime_runtime(KeyWeaver)
    if(TARGET keyconv_gui)
        keyweaver_copy_onnxruntime_runtime(keyconv_gui)
    endif()
    keyweaver_copy_onnxruntime_runtime(keyconv_tests)
    keyweaver_copy_onnxruntime_runtime(keyconv_public_header_smoke)
endif()
