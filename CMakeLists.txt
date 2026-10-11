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

wots4_add_mod(debugger
    SOURCES src/mod.cpp
        src/core/session.cpp src/core/notes.cpp
        src/error_log/error_log.cpp src/error_log/report.cpp src/error_log/minidump.cpp
    DEF src/mod.def
    LIBS bcrypt.lib
    FILES config.ini)
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
endif()
