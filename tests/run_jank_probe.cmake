# Runs jank_probe in a fresh folder and checks logs/jank_*.log: the one slow frame is found and its time
# is put on the call inside the probe's slowFrame.
# file(READ) drops the CR of line breaks, so the regular expressions use only \n. All output is English only.
# expect() takes its condition as a list, so a ";" in a regular expression is matched with "." instead.
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

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK}/mods/debugger)
get_filename_component(probe_name ${PROBE} NAME)
file(COPY_FILE ${PROBE} ${WORK}/${probe_name})
file(TO_NATIVE_PATH ${WORK} work_native)
execute_process(COMMAND ${WORK}/${probe_name} ${work_native} WORKING_DIRECTORY ${WORK}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output ENCODING UTF-8)
message(STATUS "${output}")
expect(result EQUAL 0 "the probe ran to the end")
string(REGEX MATCH "SLOW_CALL_SITE=([0-9a-f]+)" matched "${output}")
set(call_site ${CMAKE_MATCH_1})
string(REGEX MATCH "SLOW_STATE=([0-9a-f]+)" matched "${output}")
set(slow_state ${CMAKE_MATCH_1})
expect(call_site MATCHES "^004[0-9a-f]+$" "the probe reports its call site as a default VA")

file(GLOB logs ${WORK}/logs/jank_*.log)
list(LENGTH logs count)
expect(count EQUAL 1 "one jank_*.log in the logs folder")
set(text "")
if(count EQUAL 1)
    file(READ ${logs} text)
endif()
expect(text MATCHES "^debugger jank log" "the log starts with its header")
expect(text MATCHES "Threshold: automatic" "the header has the threshold")

# The slow frame is the one of about 150 ms. Normal frames can also cross 15 ms on a busy machine, so look
# for the section of the slow one rather than counting sections. Frames are numbered from the first present,
# so the 61st present is frame #60.
string(REGEX MATCH "==== Slow frame #60: 1[4-9][0-9]\\.[0-9] ms[^=]*" slow "${text}")
expect(slow MATCHES "^==== Slow frame" "frame 60 (the slow one) is reported with about 150 ms")
expect(slow MATCHES "threshold 15\\.[0-9] ms, median 1[0-1]\\.[0-9] ms"
    "the automatic threshold follows the 10 ms frames (median + 5 ms)")
expect(slow MATCHES "Game: state function [0-9a-f]+. no script\n   -> state function ${slow_state}. script running \\(scenario\\)"
    "the game state before and after the frame is written when it changed")
expect(slow MATCHES "Samples: [0-9]+ \\(one every [0-9.]+ ms" "the samples of the frame are counted")
string(REGEX MATCH "Samples: ([0-9]+)" matched "${slow}")
expect(CMAKE_MATCH_1 GREATER 50 "more than 50 samples in a 150 ms frame at 1 ms intervals")
expect(slow MATCHES "Steps of the main loop:\n +[0-9.]+ ms +(9[0-9]|100)\\.[0-9]%  outside the main loop"
    "without the game's main loop every sample is outside it")
expect(slow MATCHES "Calls in game code that were running[^\n]*\n +[0-9.]+ ms +(9[0-9]|100)\\.[0-9]%  ${call_site} \\(calls [0-9a-f]+\\)"
    "the call inside slowFrame is the top running call with nearly all samples")
expect(slow MATCHES "Where the main thread was executing:\n +[0-9.]+ ms +[0-9.]+%  game "
    "the code executing most is in the exe")
expect(slow MATCHES "Most common stacks[^\n]*\n +[0-9.]+ ms +[0-9.]+%  [^\n]*${call_site}"
    "the most common stack goes through the call inside slowFrame")
expect(text MATCHES "\n---- [0-9-]+ [0-9:.]+  last [0-9.]+ s: [0-9]+ frames \\([0-9.]+ FPS\\). frame time p50 [0-9.]+ ms, p95 [0-9.]+ ms, p99 [0-9.]+ ms, max [0-9.]+ ms. [0-9]+ slow frames"
    "a summary line is written")
string(REGEX MATCH "[^\t\n\r -~]" non_ascii "${text}")
expect(NOT non_ascii "the jank log is ASCII only")

file(REMOVE_RECURSE ${WORK})
message(STATUS "${passed} passed, 0 failed")
