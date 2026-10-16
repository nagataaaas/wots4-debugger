// voice_trace on a copy of the exe mapped into this process (sections and base relocations only; no entry point,
// Steam, saves or game assets). Installs the hooks, then runs the game's own PlayVoiceSE body (0x568d00), character
// voice (0x49ab50) and voice call (0x5a2d30) with a synthetic character, sound manager, voice table and slot table.
// Every case stops before 0x5a2900, so no sound device is opened.
//   voice_trace_native_probe <decrypted exe>
#include "voice_trace/voice_trace.h"
#include "core/core.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ---------------------------------------------------------------- Stand-in for debugger's core

namespace debugger {
Session session;
}

namespace {

std::string hostLines;
struct Claim {
    uintptr_t address;
    uint32_t length;
};
std::vector<Claim> claims;

}  // namespace

namespace debugger {

void hostLog(const char* message) {
    hostLines += message;
    hostLines += "\n";
    printf("  log: %s\n", message);
}
void readConfig(const wchar_t*, const wchar_t*, const wchar_t* fallback, wchar_t* out, DWORD size) {
    wcsncpy_s(out, size, fallback, _TRUNCATE);
}
bool readConfigFlag(const wchar_t*, const wchar_t*, bool fallback) { return fallback; }
unsigned readConfigNumber(const wchar_t*, const wchar_t*, unsigned fallback, unsigned, unsigned) { return fallback; }
bool claimGameMemory(uintptr_t address, uint32_t length) {
    claims.push_back({address, length});
    return true;
}
void releaseGameMemory() { claims.clear(); }
void createLogDirectory() { CreateDirectoryW(session.logDirectory, nullptr); }
void visitNotes(void (*)(const char*, void*), void*) {}

}  // namespace debugger

namespace {

using debugger::session;

int passes, failures;

void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
        ++passes;
    else
        ++failures;
}

// ---------------------------------------------------------------- The exe copy

uintptr_t imageBase;
uintptr_t va(uintptr_t defaultVa) { return imageBase + defaultVa - 0x400000; }

bool mapImage(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.size() < 4096) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || static_cast<size_t>(dos->e_lfanew) > bytes.size() - sizeof(IMAGE_NT_HEADERS32))
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.ImageBase != 0x400000) return false;
    auto* base = static_cast<unsigned char*>(VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT,
        PAGE_EXECUTE_READWRITE));
    if (!base) return false;
    imageBase = reinterpret_cast<uintptr_t>(base);
    memcpy(base, bytes.data(), nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& section = sections[i];
        if (static_cast<size_t>(section.PointerToRawData) + section.SizeOfRawData > bytes.size() ||
            static_cast<size_t>(section.VirtualAddress) + section.SizeOfRawData > nt->OptionalHeader.SizeOfImage)
            return false;
        memcpy(base + section.VirtualAddress, bytes.data() + section.PointerToRawData, section.SizeOfRawData);
    }
    const IMAGE_DATA_DIRECTORY relocations = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    for (unsigned at = relocations.VirtualAddress; at < relocations.VirtualAddress + relocations.Size;) {
        const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base + at);
        if (block->SizeOfBlock < 8 || at + block->SizeOfBlock > nt->OptionalHeader.SizeOfImage) return false;
        const auto* entries = reinterpret_cast<const WORD*>(block + 1);
        for (unsigned i = 0; i < (block->SizeOfBlock - 8) / 2; ++i) {
            if ((entries[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
                *reinterpret_cast<uint32_t*>(base + block->VirtualAddress + (entries[i] & 0xfff)) +=
                    static_cast<uint32_t>(imageBase - 0x400000);
            else if (entries[i] >> 12)
                return false;
        }
        at += block->SizeOfBlock;
    }
    return true;
}

template <typename T>
T& at(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(base) + offset);
}
template <typename T>
T& global(uintptr_t defaultVa) {
    return *reinterpret_cast<T*>(va(defaultVa));
}

// ---------------------------------------------------------------- Synthetic game state

constexpr int32_t characterId = 7, npcNumber = 1006, voiceSet = 42, voiceNumber = 38;
constexpr uint32_t streamedRecordFlags = 0x2;

unsigned char characterData[0x400], character[0x200], model[0x100], modelNode[0x100];
unsigned char soundManager[0x80], soundInfo[0x40], voiceTable[0x10];
uint32_t voicePairs[2];
unsigned char voiceBlock[4 + 0x18 * 2];  // One record, then a zeroed one that 0x59f810 may read as the end
// A one-node tree for 0x465590: list +4 is the end node, +8 the root; node +8/+0xc children, +0x10 ID, +0x14 value.
unsigned char characterList[0x10], characterNode[0x18];

void buildGame() {
    at<int32_t>(characterData, 0x04) = characterId;
    at<int32_t>(characterData, 0x9c) = npcNumber;
    at<int32_t>(characterData, 0x2e4) = voiceSet;
    at<void*>(character, 0x0c) = characterData;
    at<void*>(character, 0x18) = model;
    at<int32_t>(character, 0x1d0) = -1;  // No voice playing, so 0x49add0 stops nothing
    at<void*>(model, 0x5c) = modelNode;  // 0x49b6c0: the position is node + 0x88
    global<void*>(0x9ebb1c) = soundManager;

    at<void*>(soundInfo, 0x24) = voiceTable;
    at<void*>(voiceTable, 0) = voiceBlock;
    at<uint32_t>(voiceTable, 4) = 1;
    at<void*>(voiceTable, 8) = voicePairs;
    voicePairs[0] = voiceSet;
    voicePairs[1] = 0;
    at<uint32_t>(voiceBlock, 0) = 1;
    at<int16_t>(voiceBlock, 4) = voiceNumber;
    at<int8_t>(voiceBlock, 4 + 4) = 1;  // One variation, so no random number is drawn
    at<uint16_t>(voiceBlock, 4 + 6) = streamedRecordFlags;
    at<float>(voiceBlock, 4 + 8) = 1.0f;
    global<void*>(0x9ebb18) = soundInfo;

    at<void*>(characterNode, 0x08) = nullptr;
    at<void*>(characterNode, 0x0c) = nullptr;
    at<int32_t>(characterNode, 0x10) = characterId;
    at<void*>(characterNode, 0x14) = character;
    at<void*>(characterList, 0x08) = characterNode;
    global<void*>(0x9e9658) = characterList;
}

// The slot table with this many streams in use (kind 2 at +4, in use at +0x34).
void fillStreams(unsigned count) {
    for (unsigned i = 0; i < 24; ++i) {
        auto* slot = reinterpret_cast<unsigned char*>(va(0x9f02c0 + i * 0x74));
        memset(slot, 0, 0x74);
        if (i >= count) continue;
        at<uint32_t>(slot, 0x04) = 2;
        at<uint8_t>(slot, 0x34) = 1;
    }
}

// ---------------------------------------------------------------- Calling the game

struct Registers {
    uintptr_t ebx, esi, edi, esp;
};

// PlayVoiceSE's body, as the script binding calls it (0x568e14). Also records the callee-saved registers and esp.
void playScriptVoice(int32_t id, int32_t number, Registers* before, Registers* after) {
    const uintptr_t function = va(0x568d00);
    uintptr_t ebxBefore, esiBefore, ediBefore, espBefore, ebxAfter, esiAfter, ediAfter, espAfter;
    __asm {
        push ebx
        push esi
        push edi
        mov ebx, 0x11111111
        mov esi, 0x22222222
        mov edi, 0x33333333
        mov ebxBefore, ebx
        mov esiBefore, esi
        mov ediBefore, edi
        mov espBefore, esp
        push 0                  // Volume 0.0f: the forced path through 0x49ab50
        mov eax, id
        mov ecx, number
        call function
        add esp, 4
        mov ebxAfter, ebx
        mov esiAfter, esi
        mov ediAfter, edi
        mov espAfter, esp
        pop edi
        pop esi
        pop ebx
    }
    *before = {ebxBefore, esiBefore, ediBefore, espBefore};
    *after = {ebxAfter, esiAfter, ediAfter, espAfter};
}

// 0x5a2d30 straight from game code, as the torture scenes do (0x5a4736).
int32_t callVoiceDirectly(Registers* before, Registers* after) {
    const uintptr_t function = va(0x5a2d30);
    const uintptr_t position = reinterpret_cast<uintptr_t>(modelNode) + 0x88;
    const uintptr_t manager = reinterpret_cast<uintptr_t>(soundManager);
    int32_t handle;
    uintptr_t ebxBefore, esiBefore, ediBefore, espBefore, ebxAfter, esiAfter, ediAfter, espAfter;
    __asm {
        push ebx
        push esi
        push edi
        mov ebx, voiceNumber
        mov esi, 0x22222222
        mov edi, 0x33333333
        mov ebxBefore, ebx
        mov esiBefore, esi
        mov ediBefore, edi
        mov espBefore, esp
        push position
        push manager
        mov ecx, voiceSet
        call function
        mov handle, eax
        mov ebxAfter, ebx
        mov esiAfter, esi
        mov ediAfter, edi
        mov espAfter, esp
        pop edi
        pop esi
        pop ebx
    }
    *before = {ebxBefore, esiBefore, ediBefore, espBefore};
    *after = {ebxAfter, esiAfter, ediAfter, espAfter};
    return handle;
}

bool sameRegisters(const Registers& before, const Registers& after) {
    return before.ebx == after.ebx && before.esi == after.esi && before.edi == after.edi && before.esp == after.esp;
}

// ---------------------------------------------------------------- The log

wchar_t workDirectory[MAX_PATH];

std::string readVoiceLog() {
    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, L"%svoice_*.log", session.logDirectory);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return {};
    FindClose(search);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s%s", session.logDirectory, found.cFileName);
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), {});
}

size_t lineCount(const std::string& text) {
    size_t count = 0;
    for (char c : text) count += c == '\n';
    return count;
}

// The last line written since the previous call.
std::string lastLog;
std::string newLines() {
    const std::string text = readVoiceLog();
    std::string added = text.substr(lastLog.size() <= text.size() ? lastLog.size() : 0);
    lastLog = text;
    printf("  voice log: %s", added.empty() ? "(nothing)\n" : added.c_str());
    return added;
}

bool has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !mapImage(argv[1])) {
        printf("FAIL usage: voice_trace_native_probe <decrypted exe> (the exe could not be mapped)\n");
        return 1;
    }
    GetTempPathW(MAX_PATH, workDirectory);
    swprintf_s(session.logDirectory, L"%svoice_trace_probe_%lu\\", workDirectory, GetCurrentProcessId());
    wcscpy_s(session.stamp, L"probe");
    session.gameBase = imageBase;
    session.gamePreferredBase = 0x400000;
    buildGame();

    check(debugger::voice_trace::initialize(), "voice_trace starts on the supported exe");
    auto claimed = [](uintptr_t address, uint32_t length) {
        for (const Claim& claim : claims)
            if (claim.address == va(address) && claim.length == length) return true;
        return false;
    };
    check(claims.size() == 3 && claimed(0x568d00, 6) && claimed(0x49ab50, 5) && claimed(0x5a2d30, 6),
        "claims 0x568d00 (6 bytes), 0x49ab50 (5 bytes) and 0x5a2d30 (6 bytes) and nothing else");
    check(has(hostLines, "voice_trace: tracing character voices"), "mod_loader.log says the trace started");

    Registers before, after;

    // 1. Five streams in use: the streamed voice is refused at 0x5a2d75.
    fillStreams(5);
    playScriptVoice(characterId, voiceNumber, &before, &after);
    std::string line = newLines();
    check(sameRegisters(before, after), "PlayVoiceSE: ebx, esi, edi and esp are kept through all three hooks");
    check(has(line, "PlayVoiceSE: character ID 7 (NPC 1006), voice set 42, voice 38: not played: the voice is streamed "
                    "and 5 streams are already in use"),
        "the stream limit is named, with the character ID, NPC and voice set");
    check(has(line, "streams=5/5") && has(line, "record=streamed") && has(line, "forced=yes"),
        "the line has the stream count, the record kind and forced");
    check(has(line, "[caller 0x"), "the line names the caller");
    check(global<uint8_t>(0x9f02c0 + 0x34) == 1 && global<uint32_t>(0x9f02c0 + 4) == 2,
        "the slot table is only read, not changed");

    // 2. Character voices muted by SetNoPlay.
    fillStreams(0);
    global<uint8_t>(0x9ecfea) = 1;
    playScriptVoice(characterId, voiceNumber, &before, &after);
    line = newLines();
    check(has(line, "character voices were muted since the last voice"), "a change of 0x9ecfea gets its own line");
    check(has(line, "not played: character voices are muted (0x9ecfea, Sound.SetNoPlay)") && has(line, "muted=yes"),
        "muted voices are named before the voice table");
    global<uint8_t>(0x9ecfea) = 0;

    // 3. The character marked voiceless: 0x49ab50 returns before 0x5a2d30.
    at<uint32_t>(characterData, 0x300) = 0x800000;
    playScriptVoice(characterId, voiceNumber, &before, &after);
    line = newLines();
    check(sameRegisters(before, after), "the early return of 0x49ab50 keeps the registers too");
    check(has(line, "not played: the character is marked voiceless (+0x300 bit 0x800000, set by SetAction 13)") &&
              !has(line, "streams="),
        "the voiceless flag is named, without state that 0x5a2d30 never read");
    at<uint32_t>(characterData, 0x300) = 0;

    // 4. A voice number the voice set does not have.
    fillStreams(5);
    playScriptVoice(characterId, voiceNumber + 1, &before, &after);
    line = newLines();
    check(has(line, "voice 39: not played: the voice set has no voice with this number") && has(line, "record=missing"),
        "a missing voice record is named");

    // 5. PlayVoiceSE with no character behind the ID.
    playScriptVoice(-99, voiceNumber, &before, &after);
    line = newLines();
    check(sameRegisters(before, after), "PlayVoiceSE that finds no character keeps the registers");
    check(has(line, "PlayVoiceSE: character ID -99, voice 38, volume 0.00: not played: the character ID is negative"),
        "a negative character ID (RegisterEventChara found no one) is named");
    playScriptVoice(characterId + 1, voiceNumber, &before, &after);
    line = newLines();
    check(has(line, "character ID 8, voice 38, volume 0.00: not played: no character has this ID now"),
        "an ID with no character is named");

    // 6. 0x5a2d30 straight from other game code.
    const int32_t handle = callVoiceDirectly(&before, &after);
    line = newLines();
    check(handle == -1, "the direct call returns the game's -1 unchanged");
    check(sameRegisters(before, after), "the direct call keeps ebx, esi, edi and esp (ret 8)");
    check(has(line, "direct voice call: voice set 42, voice 38: not played: the voice is streamed and 5 streams") &&
              has(line, "[caller 0x"),
        "a failed direct call is written with its voice set");

    const std::string text = readVoiceLog();
    bool ascii = true;
    for (unsigned char c : text) ascii &= c < 0x80;
    check(ascii, "the voice log is ASCII only");
    check(lineCount(text) == 9, "one line per voice, plus one for each SetNoPlay change (9 lines)");

    printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
