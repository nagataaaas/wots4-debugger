// error_log: watches for exceptions in two places.
// - First chance (vectored exception handler): writes details once per address, including exceptions the game handles itself
// - Fatal (unhandled exception filter): when nothing handles the exception and the game crashes, writes a detailed record and a minidump
// Does not modify the game's code or saves, and never swallows exceptions (always passes them on).
#include "error_log.h"
#include "internal.h"
#include <bcrypt.h>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace debugger::error_log {
State state;
}

namespace {

using namespace debugger;
using namespace debugger::error_log;

// ---------------------------------------------------------------- Writer exclusion

// The thread writing a record. Re-entry from the same thread (such as an exception raised while reading memory for a record) writes nothing.
volatile LONG writerThread;

bool lockWriter() {
    const LONG self = static_cast<LONG>(GetCurrentThreadId());
    for (int attempt = 0; attempt < 2000; ++attempt) {  // If another thread is writing, wait up to 2 seconds
        LONG owner = InterlockedCompareExchange(&writerThread, self, 0);
        if (owner == 0) return true;
        if (owner == self) return false;
        Sleep(1);
    }
    return false;
}

void unlockWriter() {
    InterlockedExchange(&writerThread, 0);
}

// ---------------------------------------------------------------- first chance

struct Site {
    DWORD code;
    uintptr_t address;
    unsigned long count;
};

constexpr int maxSites = 128;  // New addresses beyond this are not written in detail
Site sites[maxSites];
int siteCount;
bool siteLimitReported;

// Only watch exceptions whose severity is "error" and that are not application-defined (such as C++ throw).
bool watchAtFirstChance(DWORD code) {
    return (code >> 29) == 6;  // 11 (error) + 0 (Microsoft code)
}

LPTOP_LEVEL_EXCEPTION_FILTER volatile chainedFilter;  // Filter to call after this one
LPTOP_LEVEL_EXCEPTION_FILTER originalFilter;          // Filter that was set at initialization
LONG WINAPI fatalFilter(EXCEPTION_POINTERS* info);

// Even if another filter is installed later, keep this one called first. The replaced filter is called next.
void keepFilterOnTop() {
    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(fatalFilter);
    if (previous != fatalFilter) chainedFilter = previous;
}

LONG CALLBACK observe(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (!watchAtFirstChance(record->ExceptionCode) || !lockWriter()) return EXCEPTION_CONTINUE_SEARCH;
    keepFilterOnTop();
    const uintptr_t address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    Site* site = nullptr;
    for (int i = 0; i < siteCount; ++i) {
        if (sites[i].code == record->ExceptionCode && sites[i].address == address) site = &sites[i];
    }
    if (site) {
        ++site->count;
        if ((site->count & (site->count - 1)) == 0) writeRepeat(info, site->count);  // The 2nd, 4th, 8th, ... time
    } else if (siteCount < maxSites) {
        sites[siteCount++] = {record->ExceptionCode, address, 1};
        writeReport(ReportKind::firstChance, info, static_cast<unsigned long>(siteCount));
    } else if (!siteLimitReported) {
        siteLimitReported = true;
        writeLine("\r\n---- Too many exception addresses; first chances at new addresses are no longer recorded");
        flushLog();
    }
    unlockWriter();
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- Fatal exceptions

volatile LONG fatalThread;  // Thread that recorded the first fatal exception
LONG fatalDepth;            // How deep that thread is inside this filter

LPTOP_LEVEL_EXCEPTION_FILTER usable(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    return filter == fatalFilter ? nullptr : filter;
}

LONG WINAPI fatalFilter(EXCEPTION_POINTERS* info) {
    const LONG self = static_cast<LONG>(GetCurrentThreadId());
    LONG owner = InterlockedCompareExchange(&fatalThread, self, 0);
    if (owner == 0) {
        if (lockWriter()) {
            writeReport(ReportKind::fatal, info, 0);
            unlockWriter();
        }
        fatalDepth = 1;
        LPTOP_LEVEL_EXCEPTION_FILTER next = usable(chainedFilter);
        LONG result = next ? next(info) : EXCEPTION_CONTINUE_SEARCH;
        fatalDepth = 0;
        return result;
    }
    if (owner == self) {
        // The next filter called back into this one as its "previous filter". Pass on to the filter from initialization.
        if (fatalDepth != 1) return EXCEPTION_CONTINUE_SEARCH;
        fatalDepth = 2;
        LPTOP_LEVEL_EXCEPTION_FILTER next = usable(originalFilter);
        LONG result = next && next != chainedFilter ? next(info) : EXCEPTION_CONTINUE_SEARCH;
        fatalDepth = 1;
        return result;
    }
    // Another thread crashed too. Record only the first one and pass this one on.
    LPTOP_LEVEL_EXCEPTION_FILTER next = usable(chainedFilter);
    return next ? next(info) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- Initialization

void readSettings() {
    wchar_t value[32];
    readConfig(L"error_log", L"minidump", L"standard", value, ARRAYSIZE(value));
    const struct {
        const wchar_t* name;
        DumpLevel level;
    } levels[] = {{L"off", DumpLevel::off}, {L"small", DumpLevel::basic},
        {L"standard", DumpLevel::standard}, {L"full", DumpLevel::full}};
    for (const auto& item : levels) {
        if (_wcsicmp(value, item.name) == 0) {
            state.dumpLevel = item.level;
            return;
        }
    }
    char message[256];
    sprintf_s(message, "config.ini: [error_log] minidump=%ls is not a valid value; using standard", value);
    hostLog(message);
}

// Size and SHA-256 of the exe file. Used to match the log to an exe version when comparing with analysis notes.
bool hashFile(const wchar_t* path, unsigned long long* size, char* hex, size_t hexSize) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::vector<unsigned char> buffer(1 << 16);
    *size = 0;
    DWORD read = 0;
    while (ok && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read) {
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
        *size += read;
    }
    unsigned char digest[32];
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok || hexSize < 65) return false;
    for (int i = 0; i < 32; ++i) sprintf_s(hex + i * 2, hexSize - i * 2, "%02x", digest[i]);
    return true;
}

void buildHeader() {
    char path[3200];
    WideCharToMultiByte(CP_UTF8, 0, session.gamePath, -1, path, sizeof(path), nullptr, nullptr);
    unsigned long long size = 0;
    char hash[65] = "(could not compute)";
    hashFile(session.gamePath, &size, hash, sizeof(hash));

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(session.gameBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(session.gameBase + dos->e_lfanew);
    SYSTEMTIME now;
    GetLocalTime(&now);
    sprintf_s(state.header,
        "debugger error log (diagnostics only; exceptions are never handled here)\r\n"
        "Process %lu  started %04u-%02u-%02u %02u:%02u:%02u\r\n"
        "Game: %s\r\n"
        "  %llu bytes  SHA-256 %s\r\n"
        "  timestamp %08lx  base %08lx (default %08lx)\r\n"
        "Minidump: level=%s (written only when the game crashes)\r\n"
        "First-chance records include exceptions that the game handles itself. "
        "When an exception is not handled anywhere, a \"Fatal exception\" section is written.\r\n",
        GetCurrentProcessId(), now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, path,
        size, hash, nt->FileHeader.TimeDateStamp, static_cast<unsigned long>(session.gameBase),
        static_cast<unsigned long>(session.gamePreferredBase), dumpLevelName(state.dumpLevel));
}

}  // namespace

namespace debugger::error_log {

bool initialize() {
    readSettings();
    // Error log error_<date-time>_<process ID>.log and the crash minidump crash_<same>.dmp
    if (swprintf_s(state.logPath, L"%serror_%s.log", session.logDirectory, session.stamp) < 0 ||
        swprintf_s(state.dumpPath, L"%scrash_%s.dmp", session.logDirectory, session.stamp) < 0)
        return false;
    buildHeader();

    if (state.dumpLevel != DumpLevel::off && !loadDumpWriter())
        hostLog("could not load dbghelp.dll; minidumps will not be written");

    if (!AddVectoredExceptionHandler(1, observe)) return false;
    originalFilter = SetUnhandledExceptionFilter(fatalFilter);
    chainedFilter = originalFilter;

    char message[128];
    sprintf_s(message, "error_log: recording exceptions (minidump=%s)", dumpLevelName(state.dumpLevel));
    hostLog(message);
    return true;
}

}  // namespace debugger::error_log
