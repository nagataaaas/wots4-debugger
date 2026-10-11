// Declarations shared inside error_log.
// These functions are also called while handling exceptions, so they do not use the heap and work only with static storage.
#pragma once
#include "../core/core.h"

namespace debugger::error_log {

// [error_log] minidump in config.ini. Detail level of the minidump written on a crash.
enum class DumpLevel { off, basic, standard, full };  // basic is small in the settings (small is a macro in the Windows headers)

// Set once during initialization, read-only afterwards.
struct State {
    DumpLevel dumpLevel = DumpLevel::standard;
    wchar_t logPath[1024] = {};   // logs\error_<date-time>_<process ID>.log. Created on the first record
    wchar_t dumpPath[1024] = {};  // logs\crash_<same date-time and process ID>.dmp. Created only on a crash
    char header[2048] = {};       // Written at the top of the log: the game's exe information, etc.
};
extern State state;

enum class ReportKind { firstChance, fatal };

// Writes a detailed record of one exception. For fatal, also writes the notes, the module list and the minidump result.
void writeReport(ReportKind kind, EXCEPTION_POINTERS* info, unsigned long occurrence);
// Writes an exception repeated at the same place as one line.
void writeRepeat(EXCEPTION_POINTERS* info, unsigned long occurrence);

// Writes one line (adds the trailing newline). Buffered, not written to the file, until flushLog is called.
void writeLine(const char* text);
void flushLog();

// Writes the minidump. Returns a short description of the result for the log.
const char* writeMinidump(EXCEPTION_POINTERS* info);
bool loadDumpWriter();

const char* dumpLevelName(DumpLevel level);

}  // namespace debugger::error_log
