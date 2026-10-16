# Builds a test game folder (dinput8.dll and mods/debugger/mod.dll), has debugger_probe raise
# exceptions on purpose, and checks logs/error_*.log and logs/crash_*.dmp.
# file(READ) drops the CR of line breaks, so the regular expressions use only \n. All output (logs and test messages) is English only.
set(passed 0)
# expect(<if() condition...> <description>)
function(expect)
    set(condition ${ARGV})
    list(POP_BACK condition what)
    if(${condition})
        math(EXPR count "${passed} + 1")
        set(passed ${count} PARENT_SCOPE)
        message(STATUS "PASS ${what}")
    else()
        message(FATAL_ERROR "FAIL ${what}\ncondition: ${condition}\n${text}")
    endif()
endfunction()

# Runs the probe once with the given contents of [error_log] in config.ini (if empty, no config.ini is placed).
function(run_probe mode config)
    file(REMOVE_RECURSE ${WORK})
    file(MAKE_DIRECTORY ${WORK}/mods/debugger)
    file(COPY_FILE ${LOADER} ${WORK}/dinput8.dll)
    file(COPY_FILE ${MOD} ${WORK}/mods/debugger/mod.dll)
    if(NOT config STREQUAL "")
        file(WRITE ${WORK}/mods/debugger/config.ini "[error_log]\n${config}\n")
    endif()
    file(TO_NATIVE_PATH ${WORK}/dinput8.dll loader)
    execute_process(COMMAND ${PROBE} ${loader} ${mode} WORKING_DIRECTORY ${WORK}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output ENCODING UTF-8)
    message(STATUS "${output}")
    set(result "${result}" PARENT_SCOPE)
    if(NOT mode STREQUAL "die" AND NOT result EQUAL 0)
        message(FATAL_ERROR "FAIL the probe failed (${result})")
    endif()
    string(REGEX MATCHALL "(^|\n)PASS " probe_passes "${output}")  # The probe's own checks
    list(LENGTH probe_passes probe_count)
    math(EXPR total "${passed} + ${probe_count}")
    set(passed ${total} PARENT_SCOPE)
    file(GLOB logs ${WORK}/logs/error_*.log)
    file(GLOB dumps ${WORK}/logs/crash_*.dmp)
    file(GLOB misplaced ${WORK}/mods/debugger/*.log ${WORK}/mods/debugger/*.dmp)
    set(misplaced "${misplaced}" PARENT_SCOPE)
    set(text "")
    list(LENGTH logs count)
    if(count EQUAL 1)
        file(READ ${logs} text)
    endif()
    file(READ ${WORK}/mod_loader.log loader_log)
    set(logs ${logs} PARENT_SCOPE)
    set(dumps ${dumps} PARENT_SCOPE)
    set(text "${text}" PARENT_SCOPE)
    set(loader_log "${loader_log}" PARENT_SCOPE)
endfunction()

# 1. No exceptions, no log. Without config.ini, standard
run_probe(quiet "")
list(LENGTH logs count)
expect(count EQUAL 0 "no error_*.log without exceptions")
expect(NOT EXISTS ${WORK}/logs "no logs folder without exceptions")
string(FIND "${loader_log}" "[debugger] error_log: recording exceptions (minidump=standard)" at)
expect(NOT at EQUAL -1 "error_log is enabled with minidump=standard without config.ini")
string(FIND "${loader_log}" "[debugger] jank: disabled in config.ini" at)
expect(NOT at EQUAL -1 "jank is off without config.ini")
string(FIND "${loader_log}" "[debugger] voice_trace: disabled in config.ini" at)
expect(NOT at EQUAL -1 "voice_trace is off without config.ini")

# 2. standard: records of first-chance and fatal exceptions, and the minidump
run_probe(crash "enabled=1\nminidump=standard")
list(LENGTH logs count)
expect(count EQUAL 1 "one error_*.log in the logs folder of the game folder")
expect(NOT misplaced "nothing is written to mods/debugger")
expect(text MATCHES "SHA-256 [0-9a-f]+\n" "the header has the SHA-256 of the game exe")
expect(text MATCHES "base [0-9a-f]+ \\(default 00400000\\)" "the header has the default base read from the exe file")
expect(text MATCHES "==== Exception #1 \\(first chance" "first-chance exceptions are recorded")
expect(text MATCHES "Code: c0000005 access violation \\(read at [0-9a-f]+: free" "a released address is described as free")
expect(text MATCHES "Address: [0-9a-f]+ debugger_probe.exe\\+([0-9a-f]+) \\.text \\(default VA 004[0-9a-f]+\\)"
    "the default VA is computed from the default base, not the ASLR base")
expect(text MATCHES "Exception c0000005 again at the same address debugger_probe.exe\\+[0-9a-f]+ \\.text \\(default VA [0-9a-f]+\\): occurrence 2"
    "the second exception at the same address is a single line")
string(REGEX MATCHALL "==== Exception #" reports "${text}")
list(LENGTH reports report_count)
expect(report_count EQUAL 2 "one full record per address (released read and NULL write)")
expect(text MATCHES "Instruction bytes: 8b 80 88 00 00 00" "the instruction bytes are recorded")
expect(text MATCHES "esi [0-9a-f]+: [^\n]*\n[^\n]*\n[^\n]*\n      first 4 bytes [0-9a-f]+ -> debugger_probe.exe\\+[0-9a-f]+ \\.rdata \\(default VA [0-9a-f]+\\) \\(possibly a vtable\\)"
    "the vtable of an object in a register is recognized")
expect(text MATCHES "esp\\+[0-9a-f]+ [0-9a-f]+ debugger_probe.exe\\+[0-9a-f]+ \\.text" "return addresses are found on the stack")
expect(text MATCHES "==== Fatal exception" "the unhandled exception filter records the fatal exception")
expect(text MATCHES "\\[debugger_probe.exe\\] probe note 1" "notes are recorded")
expect(text MATCHES "Loaded modules:\n[^=]*mods\\\\debugger\\\\mod.dll" "the module list is recorded")
# Memory freed while dbghelp reads it is left out ("; N memory ranges ..."), but the level stays standard
expect(text MATCHES "Minidump: written to crash_[0-9_]+\\.dmp \\(level=standard[;)]" "the minidump result is recorded")
list(LENGTH dumps count)
expect(count EQUAL 1 "one crash_*.dmp")
string(REGEX MATCH "error_([0-9_]+)\\.log$" matched "${logs}")
expect(dumps MATCHES "/crash_${CMAKE_MATCH_1}\\.dmp$" "the minidump has the same time and process ID as the error log")
file(READ ${dumps} signature LIMIT 4 HEX)
expect(signature STREQUAL "4d444d50" "the minidump starts with MDMP")
file(SIZE ${dumps} standard_size)
string(REGEX MATCH "[^\t\n\r -~]" non_ascii "${text}${loader_log}")
expect(NOT non_ascii "the error log and mod_loader.log are ASCII only")

# 3. small: smaller than standard
run_probe(crash "minidump=small")
file(SIZE ${dumps} small_size)
expect(small_size LESS standard_size "small is smaller than standard")

# 4. off: no minidump
run_probe(crash "minidump=off")
list(LENGTH dumps count)
expect(count EQUAL 0 "no minidump with minidump=off")
expect(text MATCHES "Minidump: not written \\(minidump=off in config.ini\\)" "minidump=off is recorded")

# 5. Invalid value: use standard and report it in mod_loader.log
run_probe(quiet "minidump=bogus")
string(FIND "${loader_log}" "config.ini: [error_log] minidump=bogus is not a valid value; using standard" at)
expect(NOT at EQUAL -1 "an invalid minidump value is reported in mod_loader.log")

# 6. enabled=0: error_log is off. Notes and the other filters still work
run_probe(crash "enabled=0")
list(LENGTH logs count)
expect(count EQUAL 0 "no error log with enabled=0")
expect(NOT EXISTS ${WORK}/logs "no logs folder with enabled=0")
string(FIND "${loader_log}" "[debugger] error_log: disabled in config.ini" at)
expect(NOT at EQUAL -1 "enabled=0 is reported in mod_loader.log")

# 7. Invalid enabled value: use 1 and report it in mod_loader.log
run_probe(quiet "enabled=yes")
string(FIND "${loader_log}" "config.ini: [error_log] enabled=yes is not a valid value (use 1 or 0); using 1" at)
expect(NOT at EQUAL -1 "an invalid enabled value is reported in mod_loader.log")
string(FIND "${loader_log}" "error_log: recording exceptions" at)
expect(NOT at EQUAL -1 "error_log starts with an invalid enabled value")

# 8. jank: an invalid number falls back, and an exe that is not the game is refused without changing anything
run_probe(quiet "enabled=1\n[jank]\nenabled=1\nsample_interval_us=50")
string(FIND "${loader_log}" "config.ini: [jank] sample_interval_us=50 is not a valid value (use 250 to 10000); using 1000" at)
expect(NOT at EQUAL -1 "an out-of-range jank number is reported in mod_loader.log")
string(FIND "${loader_log}" "[debugger] jank: this game exe is not the supported version; slow frames are not recorded" at)
expect(NOT at EQUAL -1 "jank refuses an exe that is not the supported game")
file(GLOB jank_logs ${WORK}/logs/jank_*.log)
expect(NOT jank_logs "no jank log when jank is refused")
string(FIND "${loader_log}" "error_log: recording exceptions" at)
expect(NOT at EQUAL -1 "error_log still starts when jank is refused")

# 9. Even a process that really crashes with an unhandled exception is recorded by the filter Windows calls
run_probe(die "minidump=standard")
expect(result STREQUAL "Access violation" "the probe ends with an access violation")  # CMake returns the exception's name
expect(text MATCHES "==== Fatal exception" "a crashing process records the fatal exception")
list(LENGTH dumps count)
expect(count EQUAL 1 "a crashing process writes a minidump")

file(REMOVE_RECURSE ${WORK})
message(STATUS "${passed} passed, 0 failed")
