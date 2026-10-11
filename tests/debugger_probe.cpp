// Loads debugger through the ModLoader and raises exceptions on purpose (without the game).
//   debugger_probe <dinput8.dll> quiet  ... raises no exceptions
//   debugger_probe <dinput8.dll> crash  ... raises first-chance exceptions and triggers the unhandled exception filter as in a crash
//   debugger_probe <dinput8.dll> die    ... really crashes with an unhandled exception
// run_debugger_probe.cmake checks the log contents. This checks the filter chain and the note connection.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <wots4/debugger.h>
#include <cstdio>
#include <cstring>

namespace {

int failures;

void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// Stand-ins for the game's CRT: a filter set before the ModLoader (A) and one set after it (B).
bool calledA, calledB;
LPTOP_LEVEL_EXCEPTION_FILTER beforeB;

LONG WINAPI filterA(EXCEPTION_POINTERS*) {
    calledA = true;
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI filterB(EXCEPTION_POINTERS* info) {
    calledB = true;
    return beforeB ? beforeB(info) : EXCEPTION_CONTINUE_SEARCH;
}

// An object with a virtual function table. Checks that the record of where registers point says "possibly a vtable".
struct Probe {
    virtual ~Probe() = default;
    virtual int value() { return 1; }
};
Probe probeObject;

// Reads a released page while esi holds the object (reproduces a use-after-free).
__declspec(noinline) void readFreed(void* object, const void* freed) {
    __asm {
        mov esi, object
        mov eax, freed
        mov eax, [eax + 0x88]
    }
}

__declspec(noinline) void writeNull() {
    *static_cast<volatile int*>(nullptr) = 1;
}

// Calls the current topmost unhandled exception filter, as Windows does on a crash.
LONG callTopFilter(EXCEPTION_POINTERS* info) {
    LPTOP_LEVEL_EXCEPTION_FILTER top = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(top);
    if (top) top(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool loadMods(const char* loaderPath) {
    HMODULE loader = LoadLibraryA(loaderPath);
    auto create = loader ? reinterpret_cast<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)>(
        GetProcAddress(loader, "DirectInput8Create")) : nullptr;
    IDirectInput8W* input = nullptr;
    HRESULT result = create ? create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
        reinterpret_cast<void**>(&input), nullptr) : E_FAIL;
    if (input) input->Release();
    return SUCCEEDED(result) && GetModuleHandleW(L"mod.dll");
}

// Connects to debugger as a Mod in mods\probe\, the same way other Mods do.
bool connectAsMod() {
    wchar_t directory[1024];
    DWORD length = GetCurrentDirectoryW(ARRAYSIZE(directory), directory);
    if (!length || wcscat_s(directory, L"\\mods\\probe\\") != 0) return false;
    wots4::ModContext context = {};
    context.size = sizeof(context);
    context.version = wots4::modApiVersion;
    context.module = GetModuleHandleW(nullptr);
    context.directory = directory;
    return wots4::debugger::connect(&context);
}

void crash() {
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    VirtualFree(page, 0, MEM_RELEASE);
    for (int i = 0; i < 3; ++i) {
        __try {
            readFreed(&probeObject, page);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    __try {
        writeNull();
    } __except (callTopFilter(GetExceptionInformation())) {
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    if (strcmp(argv[2], "die") == 0) {
        // Really crash with the exception unhandled. Do not show the Windows error reporting dialog.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        if (!loadMods(argv[1])) return 1;
        writeNull();
        return 1;  // Never reached
    }
    const bool crashing = strcmp(argv[2], "crash") == 0;
    SetUnhandledExceptionFilter(filterA);
    check(loadMods(argv[1]), "ModLoader loads debugger");
    if (crashing) {
        check(connectAsMod(), "connect in debugger.h finds Wots4DebuggerNote");
        wots4::debugger::note("probe note 1");
        beforeB = SetUnhandledExceptionFilter(filterB);
        crash();
        check(calledB, "filter B, set after debugger, is also called");
        check(calledA, "filter A, set before debugger, is also called");
    }
    puts("exception reaches application handler");
    return failures ? 1 : 0;
}
