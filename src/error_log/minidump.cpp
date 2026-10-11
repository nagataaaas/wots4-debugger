// Minidump on a crash. The detail level is chosen with [error_log] minidump in config.ini.
// dbghelp.dll is loaded before any crash (during initialization).
#include "internal.h"
#include <dbghelp.h>
#include <cstdio>
#include <cwchar>

namespace {

using namespace debugger;
using namespace debugger::error_log;

using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
WriteDump writeDump;

MINIDUMP_TYPE dumpType(DumpLevel level) {
    // small: stacks and registers of all threads, and the module list
    int type = MiniDumpNormal | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo;
    if (level == DumpLevel::basic) return static_cast<MINIDUMP_TYPE>(type);
    // standard: also each module's global variables, memory that registers and stacks point to,
    // and the list of memory allocation state (which regions are freed)
    type |= MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemoryInfo |
        MiniDumpWithProcessThreadData | MiniDumpWithHandleData;
    // full: also all process memory (hundreds of MB to several GB)
    if (level == DumpLevel::full) type |= MiniDumpWithFullMemory;
    return static_cast<MINIDUMP_TYPE>(type);
}

const wchar_t* fileName(const wchar_t* path) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

}  // namespace

namespace debugger::error_log {

const char* dumpLevelName(DumpLevel level) {
    switch (level) {
    case DumpLevel::off: return "off";
    case DumpLevel::basic: return "small";
    case DumpLevel::standard: return "standard";
    case DumpLevel::full: return "full";
    }
    return "?";
}

bool loadDumpWriter() {
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    writeDump = dbghelp ? reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    return writeDump != nullptr;
}

const char* writeMinidump(EXCEPTION_POINTERS* info) {
    static char result[1400];
    DumpLevel level = state.dumpLevel;
    if (level == DumpLevel::off) return "not written (minidump=off in config.ini)";
    if (!writeDump) return "not written (could not load dbghelp.dll)";
    createLogDirectory();
    HANDLE file = CreateFileW(state.dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        sprintf_s(result, "not written (could not create the file; Windows error %lu)", GetLastError());
        return result;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception = {GetCurrentThreadId(), info, FALSE};
    BOOL written = writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, dumpType(level), &exception,
        nullptr, nullptr);
    DWORD error = written ? 0 : GetLastError();
    CloseHandle(file);
    char name[1100];
    WideCharToMultiByte(CP_UTF8, 0, fileName(state.dumpPath), -1, name, sizeof(name), nullptr, nullptr);
    if (written) {
        sprintf_s(result, "written to %s (level=%s)", name, dumpLevelName(level));
    } else {
        sprintf_s(result, "failed to write %s (level=%s; error %08lx)", name, dumpLevelName(level), error);
    }
    return result;
}

}  // namespace debugger::error_log
