// Times the game's frames. The main loop (Run 0x4dd1b0) calls Frame through slot +0x24 of the game system's
// vtable on every pass, so that one slot is pointed at timedFrame. Nothing in the game's code is changed.
#include "internal.h"
#include <cstring>

namespace {

using namespace debugger;
using namespace debugger::jank;

// Default VAs of the supported Steam build (exe timestamp below).
constexpr DWORD supportedTimestamp = 0x683d42eb;
constexpr uintptr_t frameFunction = 0x4dd4f0;     // CGameSystemWin32 Frame: thiscall, no arguments, returns al
constexpr uintptr_t frameSlot = 0x987294 + 0x24;  // CGameSystemWin32 vtable 0x987294, slot Frame
constexpr uintptr_t rendererGlobal = 0x9e0890;    // NiDX9Renderer*
constexpr uint32_t presentedCountOffset = 0xa0;   // Frame adds 1 after DisplayFrame succeeds (0x4dd57c)
constexpr uintptr_t rendererOperand = 0x4dd50d;   // mov ecx, [0x9e0890] in Frame
// CGameSystemWin32 +8: the current game task. Task +8: its state function (called through 0x4ef1a0).
constexpr uint32_t taskOffset = 8, stateFunctionOffset = 8;
// CScriptEventSystem*. +0x170: 1 while a script runs (0x5729b6); +0x174 bit 0x10: LoadWaitMap is loading
// (0x5603d4); +0x178: the folder of the last script (0x572896).
constexpr uintptr_t scriptSystemGlobal = 0x9eb5f8;
constexpr uint32_t scriptRunningOffset = 0x170, scriptFlagsOffset = 0x174, scriptKindOffset = 0x178;
constexpr uint32_t mapLoadingFlag = 0x10;
// CFPSTimer*. +0xc (byte): whether game time advances; menus and talk choices clear it (0x4e1228, 0x561321).
constexpr uintptr_t frameTimerGlobal = 0x9ebb90;
constexpr uint32_t gameTimeRunningOffset = 0xc;

struct CodeSignature {
    uintptr_t address;
    const unsigned char* bytes;
    size_t length;
};

// The code whose return addresses analysis.cpp names, and what this file reads. Only bytes that ASLR does
// not relocate.
constexpr unsigned char framePrologue[] = {0x55, 0x8b, 0xec, 0x51, 0x53, 0x56, 0x57, 0x8b, 0xd9, 0x8b, 0x73, 0x0c,
    0x6a, 0x00, 0xe8};                                  // ... push 0; call limiter (returns to 0x4dd503)
constexpr unsigned char framePresent[] = {0xff, 0xd2, 0xc7, 0x06, 0, 0, 0, 0, 0x84, 0xc0, 0x74, 0x06,
    0xff, 0x87, 0xa0, 0, 0, 0};                         // call DisplayFrame; ...; inc [edi + 0xa0]
constexpr unsigned char runCallsSlot[] = {0x8b, 0x06, 0x8b, 0x50, 0x24, 0x8b, 0xce, 0xff, 0xd2};  // Frame
constexpr unsigned char runCallsPump[] = {0x8b, 0x06, 0x8b, 0x50, 0x20, 0x8b, 0xce, 0xff, 0xd2};  // PumpMessages
constexpr unsigned char updateCallsHooks[] = {0xe8};                  // call 0x50e870 (returns to 0x4dd6c2)
constexpr unsigned char updateCallsState[] = {0x8b, 0x42, 0x08, 0xff, 0xd0};  // call task vtable +8 (0x4dd6cc)

template <size_t length>
constexpr CodeSignature signature(uintptr_t address, const unsigned char (&bytes)[length]) {
    return {address, bytes, length};
}

const CodeSignature signatures[] = {
    signature(frameFunction, framePrologue),
    signature(0x4dd570, framePresent),
    signature(0x4dd1e1, runCallsSlot),
    signature(0x4dd1d4, runCallsPump),
    signature(0x4dd1ee, runCallsPump),
    signature(0x4dd6bd, updateCallsHooks),
    signature(0x4dd6c7, updateCallsState),
};

uintptr_t va(uintptr_t defaultVa) {
    return defaultVa - gameImage.preferredBase + gameImage.base;
}

template <typename T>
T readGame(uintptr_t address) {
    T value;
    memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

const char* checkGame() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameImage.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(gameImage.base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != supportedTimestamp) return "this game exe is not the supported version";
    for (const CodeSignature& code : signatures) {
        if (memcmp(reinterpret_cast<const void*>(va(code.address)), code.bytes, code.length) != 0)
            return "the game's main loop is not as expected";
    }
    if (readGame<uintptr_t>(va(frameSlot)) != va(frameFunction)) return "the Frame slot was already changed";
    if (readGame<uintptr_t>(va(rendererOperand)) != va(rendererGlobal)) return "the renderer is not where expected";
    return nullptr;
}

// ---------------------------------------------------------------- Per frame

using FrameFunction = bool(__thiscall*)(void* system);
FrameFunction originalFrame;

uint32_t presentedCount() {
    const uintptr_t renderer = readGame<uintptr_t>(va(rendererGlobal));
    return renderer ? readGame<uint32_t>(renderer + presentedCountOffset) : 0;
}

GameState readGameState(const void* system) {
    GameState state = {};
    const uintptr_t task = readGame<uintptr_t>(reinterpret_cast<uintptr_t>(system) + taskOffset);
    if (task) state.stateFunction = toDefaultVa(readGame<uintptr_t>(task + stateFunctionOffset));
    state.scriptKind = -1;
    if (const uintptr_t scripts = readGame<uintptr_t>(va(scriptSystemGlobal))) {
        state.scriptRunning = readGame<uint32_t>(scripts + scriptRunningOffset) == 1;
        state.scriptKind = static_cast<int8_t>(readGame<int32_t>(scripts + scriptKindOffset));
        state.mapLoading = (readGame<uint32_t>(scripts + scriptFlagsOffset) & mapLoadingFlag) != 0;
    }
    const uintptr_t timer = readGame<uintptr_t>(va(frameTimerGlobal));
    state.gameTimeRunning = timer ? readGame<uint8_t>(timer + gameTimeRunningOffset) != 0 : 1;
    return state;
}

// While the limiter waits, Run calls Frame thousands of times per frame and Frame returns at once, so this
// costs one QPC and two reads per call; the rest runs only when a frame was presented.
bool __fastcall timedFrame(void* system, void* /*edx: unused by thiscall*/) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const uint32_t before = presentedCount();
    const bool result = originalFrame(system);
    if (presentedCount() != before) framePresented(start.QuadPart, readGameState(system));
    return result;
}

// Run reads the slot on every pass from the main thread; one aligned 4-byte write switches it at once.
bool writeFrameSlot(uintptr_t target) {
    auto* slot = reinterpret_cast<LONG*>(va(frameSlot));
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return false;
    InterlockedExchange(slot, static_cast<LONG>(target));
    VirtualProtect(slot, sizeof(*slot), protection, &protection);
    return true;
}

}  // namespace

namespace debugger::jank {

bool installFrameHook(const char** why) {
    if ((*why = checkGame())) return false;
    if (!claimGameMemory(va(frameSlot), sizeof(uintptr_t))) {
        *why = "another Mod already uses the Frame slot";
        return false;
    }
    originalFrame = reinterpret_cast<FrameFunction>(va(frameFunction));
    if (!writeFrameSlot(reinterpret_cast<uintptr_t>(&timedFrame))) {
        releaseGameMemory();
        *why = "could not make the vtable writable";
        return false;
    }
    return true;
}

void removeFrameHook() {
    writeFrameSlot(va(frameFunction));
    releaseGameMemory();
}

}  // namespace debugger::jank
