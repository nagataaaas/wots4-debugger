cmake_minimum_required(VERSION 3.25)
project(wots4_mod_debugger LANGUAGES C CXX)

# 単体でビルドするときは -DWOTS4_SDK_DIR=<wots4-mods のパス> を渡す
if(NOT COMMAND wots4_add_mod)
    set(WOTS4_SDK_DIR "" CACHE PATH "wots4-mods（ModLoader）のパス")
    if(NOT EXISTS "${WOTS4_SDK_DIR}/cmake/Wots4.cmake")
        message(FATAL_ERROR "WOTS4_SDK_DIR に wots4-mods のパスを指定してください")
    endif()
    include(${WOTS4_SDK_DIR}/cmake/Wots4.cmake)
    enable_testing()
    set(WOTS4_BUILD_TESTS ON)
endif()

wots4_add_mod(debugger SOURCES src/crash_log.cpp DEF src/crash_log.def)

if(WOTS4_BUILD_TESTS)
    # ModLoader 経由で読み込まれ、例外が記録され、アプリ側の例外処理にも届くことを確かめる
    add_executable(crash_log_loader_probe tests/crash_log_loader_probe.cpp)
    target_link_libraries(crash_log_loader_probe PRIVATE dxguid.lib)
    if(TARGET wots4_loader)
        add_test(NAME crash_log_loader_probe COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:crash_log_loader_probe>
            -DLOADER=$<TARGET_FILE:wots4_loader>
            -DMOD=$<TARGET_FILE:mod_debugger>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/crash_log_tree
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_crash_log_probe.cmake)
    endif()
endif()
