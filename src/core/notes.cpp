// Notes: keeps the last 64 short clues left by other Mods (such as "started scene X").
// Features (error_log, etc.) write them all out on a crash.
// Lock-free, so nothing waits even if the crashing thread was in the middle of writing one.
#include "core.h"
#include <cstdio>
#include <cstring>

namespace {

constexpr LONG noteCount = 64;  // Power of 2
constexpr size_t noteTextSize = 160;

struct Note {
    volatile LONG sequence;  // Set to the sequence number once written. 0 while being written
    DWORD thread;
    FILETIME time;
    HMODULE owner;
    char text[noteTextSize];
};

Note notes[noteCount];
volatile LONG nextSequence;

// <name> for "mods\<name>\mod.dll", otherwise the file name.
void ownerName(HMODULE owner, char* out, size_t size) {
    wchar_t path[1024];
    DWORD length = owner ? GetModuleFileNameW(owner, path, ARRAYSIZE(path)) : 0;
    if (!length || length >= ARRAYSIZE(path)) {
        sprintf_s(out, size, "%p", static_cast<void*>(owner));
        return;
    }
    wchar_t* file = wcsrchr(path, L'\\');
    const wchar_t* name = file ? file + 1 : path;
    if (file && _wcsicmp(name, L"mod.dll") == 0) {
        *file = L'\0';
        wchar_t* folder = wcsrchr(path, L'\\');
        name = folder ? folder + 1 : path;
    }
    WideCharToMultiByte(CP_UTF8, 0, name, -1, out, static_cast<int>(size), nullptr, nullptr);
}

}  // namespace

// Called by other Mods (include/wots4/debugger.h). Any thread is fine.
extern "C" void WINAPI Wots4DebuggerNote(HMODULE owner, const char* text) {
    if (!text) return;
    LONG sequence = InterlockedIncrement(&nextSequence);
    Note& note = notes[(sequence - 1) & (noteCount - 1)];
    InterlockedExchange(&note.sequence, 0);
    note.thread = GetCurrentThreadId();
    GetSystemTimeAsFileTime(&note.time);
    note.owner = owner;
    strncpy_s(note.text, text, _TRUNCATE);
    InterlockedExchange(&note.sequence, sequence);
}

namespace debugger {

void visitNotes(void (*visit)(const char* line, void* context), void* context) {
    LONG last = nextSequence;
    LONG first = last > noteCount ? last - noteCount + 1 : 1;
    char line[512];
    sprintf_s(line, "Notes from other Mods (last %ld):", noteCount);
    visit(line, context);
    if (last == 0) {
        visit("  none", context);
        return;
    }
    for (LONG sequence = first; sequence <= last; ++sequence) {
        const Note& note = notes[(sequence - 1) & (noteCount - 1)];
        if (note.sequence != sequence) continue;  // Being written, or overwritten by a newer note
        FILETIME local;
        SYSTEMTIME time = {};
        FileTimeToLocalFileTime(&note.time, &local);
        FileTimeToSystemTime(&local, &time);
        char owner[128];
        ownerName(note.owner, owner, sizeof(owner));
        sprintf_s(line, "  %02u:%02u:%02u.%03u thread %lu [%s] %s", time.wHour, time.wMinute,
            time.wSecond, time.wMilliseconds, note.thread, owner, note.text);
        visit(line, context);
    }
}

}  // namespace debugger
