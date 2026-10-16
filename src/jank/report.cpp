// Writes logs\jank_<date-time>_<process ID>.log. Runs on the analyzer thread only.
#include "internal.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

using namespace debugger;
using namespace debugger::jank;

// ---------------------------------------------------------------- Log file

wchar_t logPath[1024];
char header[2048];
HANDLE logFile = INVALID_HANDLE_VALUE;
bool openFailed;
char pending[64 * 1024];
size_t pendingLength;
int64_t startTicks;     // QPC when monitoring started
FILETIME startTime;     // The same moment as UTC

bool openLog() {
    if (logFile != INVALID_HANDLE_VALUE) return true;
    if (openFailed) return false;
    createLogDirectory();
    logFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile == INVALID_HANDLE_VALUE) {
        openFailed = true;
        char message[160];
        sprintf_s(message, "jank: could not create the jank log in logs\\ (Windows error %lu); nothing will be written",
            GetLastError());
        hostLog(message);
        return false;
    }
    DWORD written;
    WriteFile(logFile, header, static_cast<DWORD>(strlen(header)), &written, nullptr);
    return true;
}

void flush() {
    if (pendingLength && openLog()) {
        DWORD written;
        WriteFile(logFile, pending, static_cast<DWORD>(pendingLength), &written, nullptr);
    }
    pendingLength = 0;
}

void append(const char* text, size_t length) {
    while (length) {
        if (pendingLength == sizeof(pending)) flush();
        const size_t chunk = std::min(sizeof(pending) - pendingLength, length);
        memcpy(pending + pendingLength, text, chunk);
        pendingLength += chunk;
        text += chunk;
        length -= chunk;
    }
}

void append(const char* text) {
    append(text, strlen(text));
}

void appendf(const char* format, ...) {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    const int length = _vsnprintf_s(text, _TRUNCATE, format, arguments);
    va_end(arguments);
    append(text, length >= 0 ? static_cast<size_t>(length) : strlen(text));
}

void writeTimeOf(int64_t ticks) {
    ULARGE_INTEGER utc;
    utc.LowPart = startTime.dwLowDateTime;
    utc.HighPart = startTime.dwHighDateTime;
    utc.QuadPart += static_cast<ULONGLONG>((ticks - startTicks) / ticksPerMs() * 10000);  // 100 ns units
    FILETIME at = {utc.LowPart, utc.HighPart}, local;
    SYSTEMTIME time = {};
    FileTimeToLocalFileTime(&at, &local);
    FileTimeToSystemTime(&local, &time);
    appendf("%04u-%02u-%02u %02u:%02u:%02u.%03u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds);
}

// ---------------------------------------------------------------- Naming addresses

// Memory of other modules can go away (a DLL unloaded), so reads outside the game exe are guarded.
bool copyMemory(void* out, uintptr_t address, size_t size) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool inExecutableImage(uintptr_t address, uintptr_t* base) {
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || region.Type != MEM_IMAGE) return false;
    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(region.Protect & executable)) return false;
    *base = reinterpret_cast<uintptr_t>(region.AllocationBase);
    return true;
}

bool followsCall(uintptr_t address) {
    uint8_t bytes[8];  // bytes[8 - k] is the byte at address - k
    if (!copyMemory(bytes, address - 8, sizeof(bytes))) return false;
    if (bytes[3] == 0xe8) return true;  // call rel32
    static const int lengths[] = {2, 3, 4, 6, 7};
    for (int length : lengths) {  // call r/m32 (FF /2)
        if (bytes[8 - length] == 0xff && ((bytes[8 - length + 1] >> 3) & 7) == 2) return true;
    }
    return false;
}

// MSVC pads between functions with int3 up to a 16-byte boundary, so the nearest such boundary below an
// address is usually where its function starts. Functions that end exactly on a boundary have no padding,
// which makes this land on an earlier function; the "~" in the log marks the estimate.
uintptr_t estimateFunctionStart(uintptr_t address) {
    constexpr uintptr_t functionAlignment = 16, searchLimit = 64 * 1024;
    constexpr uint8_t int3 = 0xcc;
    const uintptr_t lowest = std::max(gameImage.textStart, address > searchLimit ? address - searchLimit : 0);
    for (uintptr_t at = address & ~(functionAlignment - 1); at > lowest; at -= functionAlignment) {
        if (*reinterpret_cast<const uint8_t*>(at - 1) == int3) return at;
    }
    return 0;
}

void moduleName(uintptr_t base, char* out, size_t size) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) {
        sprintf_s(out, size, "image@%08lx", static_cast<unsigned long>(base));
        return;
    }
    const wchar_t* file = wcsrchr(path, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, file ? file + 1 : path, -1, out, static_cast<int>(size), nullptr, nullptr);
}

// The named export with the highest address at or below rva. Internal functions get the export before them,
// so the offset after "+" tells how far it is.
bool nearestExport(uintptr_t base, uint32_t rva, char* name, size_t nameSize, uint32_t* exportRva) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    if (!copyMemory(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (!copyMemory(&nt, base + dos.e_lfanew, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_DATA_DIRECTORY& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY exports;
    if (!directory.VirtualAddress || !copyMemory(&exports, base + directory.VirtualAddress, sizeof(exports))) return false;
    uint32_t best = 0;
    DWORD bestName = 0;
    for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
        DWORD nameRva;
        WORD ordinal;
        DWORD functionRva;
        if (!copyMemory(&nameRva, base + exports.AddressOfNames + i * 4, 4) ||
            !copyMemory(&ordinal, base + exports.AddressOfNameOrdinals + i * 2, 2) ||
            !copyMemory(&functionRva, base + exports.AddressOfFunctions + ordinal * 4, 4))
            return false;
        if (functionRva <= rva && functionRva > best) {
            best = functionRva;
            bestName = nameRva;
        }
    }
    if (!bestName) return false;
    size_t length = 0;
    while (length + 1 < nameSize && copyMemory(&name[length], base + bestName + length, 1) && name[length]) ++length;
    name[length] = '\0';
    *exportRva = best;
    return length != 0;
}

// "module.dll!Export+1c", or "module.dll+1234" when no export is below it.
void nameOutsideGame(uintptr_t address, char* out, size_t size) {
    uintptr_t base;
    if (!inExecutableImage(address, &base)) {
        sprintf_s(out, size, "%08lx (not in a module)", static_cast<unsigned long>(address));
        return;
    }
    char module[MAX_PATH];
    moduleName(base, module, sizeof(module));
    const uint32_t rva = static_cast<uint32_t>(address - base);
    char exportName[128];
    uint32_t exportRva;
    if (nearestExport(base, rva, exportName, sizeof(exportName), &exportRva)) {
        sprintf_s(out, size, "%s!%s+%lx", module, exportName, static_cast<unsigned long>(rva - exportRva));
    } else {
        sprintf_s(out, size, "%s+%lx", module, static_cast<unsigned long>(rva));
    }
}

// Groups the place a sample ran at: by estimated function in the game, by export elsewhere.
void namePlace(uintptr_t address, char* out, size_t size) {
    if (!toDefaultVa(address)) {
        nameOutsideGame(address, out, size);
        return;
    }
    const uintptr_t start = address >= gameImage.textStart && address < gameImage.textEnd ?
        estimateFunctionStart(address) : 0;
    if (start) {
        sprintf_s(out, size, "game function ~%08lx", static_cast<unsigned long>(toDefaultVa(start)));
    } else {
        sprintf_s(out, size, "game %08lx", static_cast<unsigned long>(toDefaultVa(address)));
    }
}

uintptr_t toActual(uint32_t defaultVa) {
    return defaultVa - gameImage.preferredBase + gameImage.base;
}

// "004dd539 (calls 004dd5b0)" for a direct call, "004dd546 (indirect call)" otherwise.
void nameCallSite(uint32_t defaultVa, char* out, size_t size) {
    const uintptr_t returnAddress = toActual(defaultVa);
    uint8_t call[5];
    if (copyMemory(call, returnAddress - 5, sizeof(call)) && call[0] == 0xe8) {
        int32_t displacement;
        memcpy(&displacement, call + 1, sizeof(displacement));
        sprintf_s(out, size, "%08lx (calls %08lx)", static_cast<unsigned long>(defaultVa),
            static_cast<unsigned long>(toDefaultVa(returnAddress + displacement)));
    } else {
        sprintf_s(out, size, "%08lx (indirect call)", static_cast<unsigned long>(defaultVa));
    }
}

// ---------------------------------------------------------------- Sections of a report

constexpr int maxCallSitesShown = 15, maxPlacesShown = 12, maxStacksShown = 5;
constexpr int maxOutsideCallers = 3;  // Callers outside game code shown before the first game call site

const char* scriptKindName(int kind) {
    static const char* const names[] = {"cutin", "scenario", "shop", "init", "torture", "ts", "other"};
    return kind >= 0 && kind < static_cast<int>(ARRAYSIZE(names)) ? names[kind] : "none";
}

void appendState(const GameState& state) {
    appendf("state function %08lx; ", static_cast<unsigned long>(state.stateFunction));
    if (state.scriptRunning) appendf("script running (%s)", scriptKindName(state.scriptKind));
    else append("no script");
    if (state.mapLoading) append("; loading a map");
    if (!state.gameTimeRunning) append("; game time stopped");
}

bool sameState(const GameState& a, const GameState& b) {
    return a.stateFunction == b.stateFunction && a.scriptRunning == b.scriptRunning && a.scriptKind == b.scriptKind &&
        a.mapLoading == b.mapLoading && a.gameTimeRunning == b.gameTimeRunning;
}

// Share of the frame, with the time it stands for.
void appendShare(int samples, int total, double frameMs) {
    const double share = total ? static_cast<double>(samples) / total : 0;
    appendf("  %7.1f ms %5.1f%%  ", share * frameMs, share * 100);
}

void writeSteps(const Profile& profile, double frameMs) {
    append("Steps of the main loop:\r\n");
    int order[static_cast<int>(Step::count)];
    for (int i = 0; i < static_cast<int>(Step::count); ++i) order[i] = i;
    std::sort(order, order + static_cast<int>(Step::count),
        [&](int a, int b) { return profile.steps[a] > profile.steps[b]; });
    for (int step : order) {
        if (!profile.steps[step]) break;
        appendShare(profile.steps[step], profile.sampleCount, frameMs);
        appendf("%s\r\n", stepName(static_cast<Step>(step)));
    }
}

void writeCallSites(const Profile& profile, double frameMs) {
    append("Calls in game code that were running (inclusive; default VA of the return address):\r\n");
    const int shown = std::min(profile.callSiteCount, maxCallSitesShown);
    for (int i = 0; i < shown; ++i) {
        char name[64];
        nameCallSite(profile.callSites[i].key, name, sizeof(name));
        appendShare(profile.callSites[i].samples, profile.sampleCount, frameMs);
        appendf("%s\r\n", name);
    }
    if (!shown) append("  none\r\n");
}

struct Place {
    char name[192];
    int samples;
    uint32_t hottest;      // The address with the most samples in this place
    int hottestSamples;
};

void writePlaces(const Profile& profile, double frameMs) {
    static Place places[Profile::maxEntries];
    int placeCount = 0;
    for (int i = 0; i < profile.leafCount; ++i) {  // Leaves are sorted, so a place's first leaf is its hottest
        char name[192];
        namePlace(profile.leaves[i].key, name, sizeof(name));
        Place* place = nullptr;
        for (int j = 0; j < placeCount && !place; ++j) {
            if (strcmp(places[j].name, name) == 0) place = &places[j];
        }
        if (!place) {
            place = &places[placeCount++];
            strcpy_s(place->name, name);
            place->samples = 0;
            place->hottest = profile.leaves[i].key;
            place->hottestSamples = profile.leaves[i].samples;
        }
        place->samples += profile.leaves[i].samples;
    }
    std::sort(places, places + placeCount, [](const Place& a, const Place& b) { return a.samples > b.samples; });
    append("Where the main thread was executing:\r\n");
    const int shown = std::min(placeCount, maxPlacesShown);
    for (int i = 0; i < shown; ++i) {
        const Place& place = places[i];
        appendShare(place.samples, profile.sampleCount, frameMs);
        append(place.name);
        if (const uint32_t hottest = toDefaultVa(place.hottest)) {
            appendf(" (most at %08lx: %d samples)", static_cast<unsigned long>(hottest), place.hottestSamples);
        }
        append("\r\n");
    }
}

// Callers outside game code (Windows, the graphics driver, Mods) between the leaf and the first game call site.
void appendOutsideCallers(const Sample& sample) {
    int shown = 0;
    for (int i = 0; i < stackTopWords && shown < maxOutsideCallers; ++i) {
        const uintptr_t value = sample.stackTop[i];
        if (toDefaultVa(value)) break;  // Reached game code; the call sites follow
        uintptr_t base;
        if (!value || !inExecutableImage(value, &base) || !followsCall(value)) continue;
        char name[192];
        nameOutsideGame(value, name, sizeof(name));
        appendf(" <- %s", name);
        ++shown;
    }
}

void writeStacks(const Profile& profile, const Sample* samples, double frameMs) {
    append("Most common stacks (innermost first; values left over on the stack can appear too):\r\n");
    const int shown = std::min(profile.stackCount, maxStacksShown);
    for (int i = 0; i < shown; ++i) {
        const Sample& sample = samples[profile.stacks[i].firstSample];
        char leaf[192];
        if (const uint32_t inGame = toDefaultVa(sample.eip)) {
            sprintf_s(leaf, "%08lx", static_cast<unsigned long>(inGame));
        } else {
            nameOutsideGame(sample.eip, leaf, sizeof(leaf));
        }
        appendShare(profile.stacks[i].samples, profile.sampleCount, frameMs);
        append(leaf);
        if (!toDefaultVa(sample.eip)) appendOutsideCallers(sample);
        const uint32_t depth = std::min<uint32_t>(sample.callSiteCount, Profile::stackDepth + 2);
        for (uint32_t j = 0; j < depth; ++j) appendf(" <- %08lx", static_cast<unsigned long>(sample.callSites[j]));
        append("\r\n");
    }
}

void writeTiming(const JankReport& report) {
    const double workMs = (report.slow->presented - report.slow->callStart) / ticksPerMs();
    appendf("Timing: the Frame call that presented it took %.1f ms; %.1f ms passed before that call "
            "(waiting for the frame and window messages)\r\n",
        workMs, report.frameMs - workMs);
    append("Main thread: ");
    if (report.cpuMs >= 0) appendf("on a CPU for about %.1f ms of the %.1f ms", report.cpuMs, report.frameMs);
    else append("CPU time unknown");
    appendf("; %lu page faults in the process\r\n",
        static_cast<unsigned long>(report.slow->pageFaults - report.previous->pageFaults));
    append("Game: ");
    if (!sameState(report.previous->state, report.slow->state)) {
        appendState(report.previous->state);
        append("\r\n   -> ");
    }
    appendState(report.slow->state);
    append("\r\n");
}

void writeSampleCount(const JankReport& report) {
    if (!report.sampleCount) {
        append("Samples: none (sampling is not running, or the main thread could not be stopped)\r\n");
        return;
    }
    appendf("Samples: %d (one every %.2f ms on average)", report.sampleCount, report.frameMs / report.sampleCount);
    if (report.samplesCutShort) append("; the start of the frame was not kept (the frame was too long)");
    append("\r\n");
}

}  // namespace

namespace debugger::jank {

void prepareJankLog(const Settings& settings, const char* timerKind, int64_t ticks) {
    startTicks = ticks;
    GetSystemTimeAsFileTime(&startTime);
    swprintf_s(logPath, L"%sjank_%s.log", session.logDirectory, session.stamp);
    char path[3200];
    WideCharToMultiByte(CP_UTF8, 0, session.gamePath, -1, path, sizeof(path), nullptr, nullptr);
    char threshold[128];
    if (settings.thresholdMs) sprintf_s(threshold, "%u ms", settings.thresholdMs);
    else strcpy_s(threshold, "automatic (1.5 x the median of recent frames, at least the median + 5 ms)");
    sprintf_s(header,
        "debugger jank log: frames that took longer than usual, and what the main thread was doing\r\n"
        "Process %lu\r\n"
        "Game: %s\r\n"
        "  base %08lx (default %08lx); addresses below are default VAs unless a module is named\r\n"
        "Threshold: %s\r\n"
        "Sampling: the main thread every %u us (%s)\r\n"
        "A frame is the time from one present to the next. Shares come from samples of the main thread's stack,\r\n"
        "so a share below a few samples is noise. \"~\" marks a function start estimated from the code's padding.\r\n",
        GetCurrentProcessId(), path, static_cast<unsigned long>(gameImage.base),
        static_cast<unsigned long>(gameImage.preferredBase), threshold, settings.sampleIntervalUs, timerKind);
}

void writeJankReport(const JankReport& report) {
    static Profile profile;  // Large; kept off the analyzer's stack
    buildProfile(report.samples, report.sampleCount, &profile);
    appendf("\r\n==== Slow frame #%llu: %.1f ms (threshold %.1f ms, median %.1f ms)  ",
        static_cast<unsigned long long>(report.frameNumber), report.frameMs, report.thresholdMs, report.medianMs);
    writeTimeOf(report.slow->presented);
    append("\r\n");
    if (report.suppressedBefore) {
        appendf("(%u slow frames before this one were only counted: max_reports_per_minute was reached)\r\n",
            report.suppressedBefore);
    }
    writeTiming(report);
    writeSampleCount(report);
    if (report.sampleCount) {
        writeSteps(profile, report.frameMs);
        writeCallSites(profile, report.frameMs);
        writePlaces(profile, report.frameMs);
        writeStacks(profile, report.samples, report.frameMs);
    }
    flush();
}

void writeSummary(const Summary& summary) {
    append("\r\n---- ");
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    writeTimeOf(now.QuadPart);
    const double fps = summary.seconds > 0 ? summary.frames / summary.seconds : 0;
    appendf("  last %.0f s: %llu frames (%.1f FPS); frame time p50 %.1f ms, p95 %.1f ms, p99 %.1f ms, max %.1f ms; "
            "%u slow frames (%u written above)\r\n",
        summary.seconds, static_cast<unsigned long long>(summary.frames), fps, summary.p50, summary.p95, summary.p99,
        summary.maxMs, summary.janks, summary.reported);
    flush();
}

}  // namespace debugger::jank
