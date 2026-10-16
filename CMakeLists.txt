cmake_minimum_required(VERSION 3.25)
project(wots4_mod_debugger LANGUAGES C CXX)

# For a standalone build, pass -DWOTS4_SDK_DIR=<path to wots4-mods>
if(NOT COMMAND wots4_add_mod)
    set(WOTS4_SDK_DIR "" CACHE PATH "Path to wots4-mods (ModLoader)")
    if(NOT EXISTS "${WOTS4_SDK_DIR}/cmake/Wots4.cmake")
        message(FATAL_ERROR "Set WOTS4_SDK_DIR to the path of wots4-mods")
    endif()
    include(${WOTS4_SDK_DIR}/cmake/Wots4.cmake)
    enable_testing()
    set(WOTS4_BUILD_TESTS ON)
endif()

wots4_use_minhook()

wots4_add_mod(debugger
    SOURCES src/mod.cpp
        src/core/session.cpp src/core/notes.cpp
        src/error_log/error_log.cpp src/error_log/report.cpp src/error_log/minidump.cpp
        src/jank/jank.cpp src/jank/frame_hook.cpp src/jank/monitor.cpp src/jank/analysis.cpp src/jank/report.cpp
        src/voice_trace/voice_trace.cpp
    DEF src/mod.def
    LIBS bcrypt.lib wots4_minhook
    FILES config.ini
    LICENSES docs/licenses/minhook.txt)
target_include_directories(mod_debugger PRIVATE include)

if(WOTS4_BUILD_TESTS)
    # Check that it is loaded through the ModLoader, logs exceptions, and still lets them reach the app's own exception handling
    add_executable(debugger_probe tests/debugger_probe.cpp)
    target_include_directories(debugger_probe PRIVATE include)
    target_link_libraries(debugger_probe PRIVATE wots4::sdk dxguid.lib)
    wots4_add_loader()  # For a standalone build, also build the ModLoader for the test
    add_test(NAME debugger_probe COMMAND ${CMAKE_COMMAND}
        -DPROBE=$<TARGET_FILE:debugger_probe>
        -DLOADER=$<TARGET_FILE:wots4_loader>
        -DMOD=$<TARGET_FILE:mod_debugger>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/debugger_tree
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_debugger_probe.cmake)

    # jank's counting, with synthetic samples
    add_executable(jank_analysis_test tests/jank_analysis_test.cpp src/jank/analysis.cpp)
    target_include_directories(jank_analysis_test PRIVATE src)
    add_test(NAME jank_analysis_test COMMAND jank_analysis_test)

    # voice_trace's reasons for a silent voice, with synthetic attempts
    add_executable(voice_trace_diagnosis_test tests/voice_trace_diagnosis_test.cpp)
    target_include_directories(voice_trace_diagnosis_test PRIVATE src)
    add_test(NAME voice_trace_diagnosis_test COMMAND voice_trace_diagnosis_test)

    # voice_trace's hooks on a copy of the exe: runs the game's own voice code with a synthetic character and
    # sound tables (the game is not started)
    set(WOTS4_UNPACKED_EXE "${WOTS4_ROOT}/research/bin/WayOfTheSamurai4.unpacked.exe" CACHE FILEPATH "Decrypted exe")
    file(TO_NATIVE_PATH "${WOTS4_UNPACKED_EXE}" unpacked_native)
    wots4_add_probe(voice_trace_native_probe SOURCES tests/voice_trace_native_probe.cpp src/voice_trace/voice_trace.cpp
        LIBS wots4_minhook
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
        LINK_OPTIONS /BASE:0x10000000 /DYNAMICBASE
        ARGS ${unpacked_native} NEEDS_EXE)

    # jank's threads and log without the game: the probe exe stands in for the game and presents frames itself
    add_executable(jank_probe tests/jank_probe.cpp
        src/core/session.cpp src/core/notes.cpp
        src/jank/monitor.cpp src/jank/analysis.cpp src/jank/report.cpp)
    target_include_directories(jank_probe PRIVATE src)
    target_link_libraries(jank_probe PRIVATE wots4::sdk)
    add_test(NAME jank_probe COMMAND ${CMAKE_COMMAND}
        -DPROBE=$<TARGET_FILE:jank_probe>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/jank_tree
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_jank_probe.cmake)
endif()
