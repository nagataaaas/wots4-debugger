// voice_trace: hooks the three steps a character voice goes through and writes one line per voice.
//   0x568d00  PlayVoiceSE (script and timetable): finds the character by ID, then calls 0x49ab50
//   0x49ab50  Character voice: checks the character and the sound manager, then calls 0x5a2d30
//   0x5a2d30  Looks up the voice in the voice table and takes a sound slot
// Nothing in the game is changed: each hook reads some state, calls the original and reads its result.
// Voices play on the main thread only, so the state shared by the hooks is not locked.
#include "voice_trace.h"
#include "diagnosis.h"
#include "../core/core.h"
#include <MinHook.h>
#include <intrin.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

using namespace debugger;
using namespace debugger::voice_trace;

// ---------------------------------------------------------------- Game

constexpr DWORD supportedTimestamp = 0x683d42eb;
constexpr uintptr_t scriptVoice = 0x568d00;     // eax = character ID, ecx = voice number, (float volume), cdecl
constexpr uint32_t scriptVoiceHookLength = 6;   // push ebp; mov ebp,esp; push esi; mov esi,eax
constexpr uintptr_t characterVoice = 0x49ab50;  // (character, voice number, forced), stdcall
constexpr uint32_t characterVoiceHookLength = 5;  // push ebp; mov ebp,esp; push -1
constexpr uintptr_t voiceCall = 0x5a2d30;       // ecx = voice set, ebx = voice number, (sound manager, position), ret 8
constexpr uint32_t voiceCallHookLength = 6;     // push ebp; mov ebp,esp; sub esp,8
constexpr uintptr_t findCharacter = 0x465590;   // ecx = character list, esi = ID; returns the character or 0

constexpr uintptr_t soundManagerGlobal = 0x9ebb1c;
constexpr uint32_t flatVoicePathOffset = 0x2c;  // Byte: 0x49ab50 uses 0x5a2e00 instead of 0x5a2d30 (0x49abe8)
constexpr uintptr_t soundInfoGlobal = 0x9ebb18;
constexpr uint32_t voiceTableOffset = 0x24;     // Character voice table (0x5a2d55)
constexpr uintptr_t voicesMutedGlobal = 0x9ecfea;  // Byte (0x5a2d36)
constexpr uintptr_t characterListGlobal = 0x9e9658;
constexpr uintptr_t soundSlots = 0x9f02c0;
constexpr uint32_t soundSlotSize = 0x74, slotKindOffset = 0x04, slotInUseOffset = 0x34;
constexpr uint32_t streamSlotKind = 2;

constexpr uint32_t characterDataOffset = 0x0c;  // Character -> its data
// Character data.
constexpr uint32_t dataIdOffset = 0x04;          // The ID scripts use (RegisterEventChara, 0x54a111)
constexpr uint32_t dataNpcOffset = 0x9c;         // NPC number (RegisterEventChara compares it, 0x4a0746)
constexpr uint32_t dataVoiceSetOffset = 0x2e4;   // Passed to 0x5a2d30 in ecx (0x49ad23)
constexpr uint32_t dataFlagsOffset = 0x300;
constexpr uint32_t voicelessFlag = 0x800000;     // Tested at 0x49ab7e
// Voice table: +0 base, +4 count, +8 (voice set, offset from base) pairs. At base + offset: count, then records
// of 0x18 bytes (s16 voice number, ..., u16 flags at +6). See 0x59f520.
constexpr uint32_t voiceRecordSize = 0x18, recordFlagsOffset = 6;
constexpr uint16_t recordStreamedFlag = 0x2;
constexpr uint32_t maxVoiceSets = 4096, maxVoicesPerSet = 4096;  // Bounds for a damaged table

uintptr_t va(uintptr_t defaultVa) {
    return defaultVa - session.gamePreferredBase + session.gameBase;
}

// The hooks read game memory that a damaged pointer could make unreadable. A failed read gives false, not a crash.
bool readMemory(uintptr_t address, void* out, size_t size) {
    if (!address) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

template <typename T>
T readGame(uintptr_t address, T fallback = T()) {
    T value;
    return readMemory(address, &value, sizeof(value)) ? value : fallback;
}

struct CodeSignature {
    uintptr_t address;
    const unsigned char* bytes;
    size_t length;
};

// Only bytes that ASLR does not relocate. Absolute addresses in the code are compared after relocation below.
constexpr unsigned char scriptVoicePrologue[] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf0, 0x57, 0x8b, 0xf9, 0x85, 0xf6,
    0x78, 0x5f};                                           // ...; mov edi,ecx; test esi,esi; js (negative ID)
constexpr unsigned char scriptVoiceCall[] = {0x6a, 0x01, 0xdd, 0xd8, 0x57, 0x51, 0xe8};  // push 1 (forced); ...; call
constexpr unsigned char characterVoicePrologue[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
constexpr unsigned char voicelessTest[] = {0x8b, 0x43, 0x0c, 0xf7, 0x80, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x00};                                                 // mov eax,[ebx+0xc]; test [eax+0x300],0x800000
constexpr unsigned char voiceSetLoad[] = {0x8b, 0x43, 0x0c, 0x8b, 0xb0, 0xe4, 0x02, 0x00, 0x00};  // esi = [data+0x2e4]
constexpr unsigned char voiceCallPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08, 0x80, 0x3d};
constexpr unsigned char streamTest[] = {0xf6, 0x46, 0x06, 0x02, 0x74, 0x0a, 0xe8};  // test [esi+6],2; je; call count
constexpr unsigned char streamLimitTest[] = {0x83, 0xf8, 0x05};                    // cmp eax,5
constexpr unsigned char findCharacterCode[] = {0x8b, 0x41, 0x08, 0x83, 0xc1, 0x04};

const CodeSignature signatures[] = {
    {scriptVoice, scriptVoicePrologue, sizeof(scriptVoicePrologue)},
    {0x568d61, scriptVoiceCall, sizeof(scriptVoiceCall)},
    {characterVoice, characterVoicePrologue, sizeof(characterVoicePrologue)},
    {0x49ab7b, voicelessTest, sizeof(voicelessTest)},
    {0x49ad20, voiceSetLoad, sizeof(voiceSetLoad)},
    {voiceCall, voiceCallPrologue, sizeof(voiceCallPrologue)},
    {0x5a2d6a, streamTest, sizeof(streamTest)},
    {0x5a2d75, streamLimitTest, sizeof(streamLimitTest)},
    {findCharacter, findCharacterCode, sizeof(findCharacterCode)},
};

uintptr_t callTarget(uintptr_t call) {
    return call + 5 + readGame<int32_t>(va(call + 1));
}

// Why the code is not the known version, or nullptr when it is.
const char* checkGameCode() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(session.gameBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(session.gameBase + dos->e_lfanew);
    if (!session.gamePreferredBase || nt->FileHeader.TimeDateStamp != supportedTimestamp)
        return "the game exe is not the supported version";
    for (const CodeSignature& code : signatures) {
        if (memcmp(reinterpret_cast<const void*>(va(code.address)), code.bytes, code.length) != 0)
            return "the voice code is not the known version";
    }
    if (readGame<uintptr_t>(va(0x5a2d38)) != va(voicesMutedGlobal) || callTarget(0x568d67) != characterVoice ||
        callTarget(0x49ad3a) != voiceCall || callTarget(0x568d13) != findCharacter)
        return "the voice code does not call or read what is expected";
    return nullptr;
}

unsigned countSlots(bool streamsOnly) {
    unsigned count = 0;
    for (unsigned i = 0; i < soundSlotCount; ++i) {
        const uintptr_t slot = va(soundSlots) + i * soundSlotSize;
        if (!readGame<uint8_t>(slot + slotInUseOffset)) continue;
        if (streamsOnly && readGame<uint32_t>(slot + slotKindOffset) != streamSlotKind) continue;
        ++count;
    }
    return count;
}

// The same lookup as 0x59f520, without picking a variation (that one draws a random number).
void lookUpVoice(int32_t voiceSet, int32_t voiceNumber, VoiceAttempt* attempt) {
    const uintptr_t info = readGame<uintptr_t>(va(soundInfoGlobal));
    const uintptr_t table = info ? readGame<uintptr_t>(info + voiceTableOffset) : 0;
    if (!table) {
        attempt->voiceTableMissing = true;
        return;
    }
    const uintptr_t base = readGame<uintptr_t>(table);
    const uint32_t setCount = readGame<uint32_t>(table + 4);
    const uintptr_t pairs = readGame<uintptr_t>(table + 8);
    for (uint32_t set = 0; set < setCount && set < maxVoiceSets; ++set) {
        if (readGame<int32_t>(pairs + set * 8, -1) != voiceSet) continue;
        const uintptr_t block = base + readGame<uint32_t>(pairs + set * 8 + 4);
        const uint32_t voiceCount = readGame<uint32_t>(block);
        for (uint32_t voice = 0; voice < voiceCount && voice < maxVoicesPerSet; ++voice) {
            const uintptr_t record = block + 4 + voice * voiceRecordSize;
            if (readGame<int16_t>(record, -1) != voiceNumber) continue;
            attempt->recordFound = true;
            attempt->recordStreamed = (readGame<uint16_t>(record + recordFlagsOffset) & recordStreamedFlag) != 0;
            return;
        }
        return;
    }
}

uintptr_t callFindCharacter(int32_t id) {
    const uintptr_t list = readGame<uintptr_t>(va(characterListGlobal));
    if (!list) return 0;
    const uintptr_t function = va(findCharacter);
    uintptr_t character;
    __asm {
        push esi
        mov ecx, list
        mov esi, id
        call function
        pop esi
        mov character, eax
    }
    return character;
}

// ---------------------------------------------------------------- Settings and log

struct Settings {
    bool logPlayedGameVoices = false;  // Also write voices from game code that played (scripts' are always written)
    unsigned maxLinesPerMinute = 120;  // For voices from game code; script voices are always written
};
Settings settings;

constexpr unsigned maxLinesLimit = 6000;
constexpr ULONGLONG minuteMs = 60 * 1000;

wchar_t logPath[1024];
HANDLE logFile = INVALID_HANDLE_VALUE;
bool openFailed;
ULONGLONG minuteStart;
unsigned linesThisMinute, skippedThisMinute;

bool openLog() {
    if (logFile != INVALID_HANDLE_VALUE) return true;
    if (openFailed) return false;
    createLogDirectory();
    logFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (logFile == INVALID_HANDLE_VALUE) {
        openFailed = true;
        char message[160];
        sprintf_s(message, "voice_trace: could not create the voice log in logs\\ (Windows error %lu); nothing will be written",
            GetLastError());
        hostLog(message);
        return false;
    }
    return true;
}

// Written at once rather than buffered, so the lines before a crash or a forced quit are kept. Voices come a few
// times a second at most, and a cached WriteFile takes microseconds.
void writeLine(const char* format, ...) {
    if (!openLog()) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[640];
    int length = sprintf_s(line, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list arguments;
    va_start(arguments, format);
    const int body = _vsnprintf_s(line + length, sizeof(line) - length - 2, _TRUNCATE, format, arguments);
    va_end(arguments);
    length = body >= 0 ? length + body : static_cast<int>(strlen(line));
    line[length++] = '\r';
    line[length++] = '\n';
    DWORD written;
    WriteFile(logFile, line, static_cast<DWORD>(length), &written, nullptr);
}

// Script voices are few and are what a silent talk is about, so only voices from game code (battle cries and such)
// are limited.
bool allowGameVoiceLine() {
    const ULONGLONG now = GetTickCount64();
    if (now - minuteStart >= minuteMs) {
        if (skippedThisMinute)
            writeLine("%u more line(s) about voices from game code were not written in the last minute "
                      "(max_lines_per_minute=%u)", skippedThisMinute, settings.maxLinesPerMinute);
        minuteStart = now;
        linesThisMinute = skippedThisMinute = 0;
    }
    if (linesThisMinute < settings.maxLinesPerMinute) {
        ++linesThisMinute;
        return true;
    }
    ++skippedThisMinute;
    return false;
}

// SetNoPlay may change between voices; a line when it did shows where it happened.
bool lastVoicesMuted;
void noteVoicesMutedChange() {
    const bool muted = readGame<uint8_t>(va(voicesMutedGlobal)) != 0;
    if (muted == lastVoicesMuted) return;
    lastVoicesMuted = muted;
    writeLine("character voices were %s since the last voice (0x9ecfea, Sound.SetNoPlay)",
        muted ? "muted" : "unmuted");
}

// ---------------------------------------------------------------- Hooks

// The PlayVoiceSE call in progress. Lets 0x49ab50 name its source and the character ID the script gave.
struct ScriptVoiceCall {
    bool active = false;
    bool reachedCharacterVoice = false;
    int32_t characterId = 0;
    int32_t voiceNumber = 0;
    float volume = 0.0f;
    uintptr_t returnAddress = 0;
};
ScriptVoiceCall scriptCall;

// The 0x49ab50 call in progress, filled in by the 0x5a2d30 hook.
VoiceAttempt* currentAttempt;

void* originalScriptVoice;
uint32_t(__stdcall* originalCharacterVoice)(uintptr_t character, int32_t voiceNumber, uint32_t forced);
void* originalVoiceCall;

uintptr_t defaultVa(uintptr_t address) {
    return address - session.gameBase + session.gamePreferredBase;
}

void describeCharacter(uintptr_t character, char* out, size_t size) {
    const uintptr_t data = readGame<uintptr_t>(character + characterDataOffset);
    if (!data) {
        sprintf_s(out, size, "character at 0x%08lx with no data", static_cast<unsigned long>(character));
        return;
    }
    sprintf_s(out, size, "character ID %ld (NPC %ld), voice set %ld", static_cast<long>(readGame<int32_t>(data + dataIdOffset)),
        static_cast<long>(readGame<int32_t>(data + dataNpcOffset)),
        static_cast<long>(readGame<int32_t>(data + dataVoiceSetOffset)));
}

void writeAttempt(const char* source, uintptr_t caller, uintptr_t character, int32_t voiceNumber,
    const VoiceAttempt& attempt, Outcome outcome) {
    char who[128];
    describeCharacter(character, who, sizeof(who));
    char result[48] = "";
    if (outcome == Outcome::played) sprintf_s(result, " (handle %ld)", static_cast<long>(attempt.handle));
    char state[160] = "";
    if (attempt.reachedVoiceCall)
        sprintf_s(state, "; muted=%s, streams=%u/%u, slots in use=%u/%u, record=%s", attempt.voicesMuted ? "yes" : "no",
            attempt.streamsInUse, streamLimit, attempt.slotsInUse, soundSlotCount,
            wasPlayed(outcome) ? "-" : !attempt.recordFound ? "missing" : attempt.recordStreamed ? "streamed" : "sample");
    writeLine("%s: %s, voice %ld: %s%s [caller 0x%08lx, forced=%s%s]", source, who, static_cast<long>(voiceNumber),
        describe(outcome), result, static_cast<unsigned long>(defaultVa(caller)), attempt.forced ? "yes" : "no", state);
}

uint32_t __stdcall characterVoiceHook(uintptr_t character, int32_t voiceNumber, uint32_t forced) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool fromScript = scriptCall.active;
    if (fromScript) scriptCall.reachedCharacterVoice = true;
    noteVoicesMutedChange();

    VoiceAttempt attempt;
    attempt.forced = (forced & 0xff) != 0;
    const uintptr_t data = readGame<uintptr_t>(character + characterDataOffset);
    attempt.characterVoiceless = (readGame<uint32_t>(data + dataFlagsOffset) & voicelessFlag) != 0;
    const uintptr_t manager = readGame<uintptr_t>(va(soundManagerGlobal));
    attempt.soundManagerMissing = !manager;
    attempt.flatVoicePath = manager && readGame<uint8_t>(manager + flatVoicePathOffset);

    VoiceAttempt* outer = currentAttempt;
    currentAttempt = &attempt;
    const uint32_t result = originalCharacterVoice(character, voiceNumber, forced);
    currentAttempt = outer;

    if (attempt.reachedVoiceCall && attempt.handle < 0)
        lookUpVoice(readGame<int32_t>(data + dataVoiceSetOffset), voiceNumber, &attempt);
    const Outcome outcome = diagnose(attempt);
    if (fromScript) {
        writeAttempt("PlayVoiceSE", scriptCall.returnAddress, character, voiceNumber, attempt, outcome);
    } else if ((!wasPlayed(outcome) || settings.logPlayedGameVoices) && allowGameVoiceLine()) {
        writeAttempt("game voice", caller, character, voiceNumber, attempt, outcome);
    }
    return result;
}

void __cdecl beforeVoiceCall() {
    if (!currentAttempt) return;
    currentAttempt->reachedVoiceCall = true;
    currentAttempt->voicesMuted = readGame<uint8_t>(va(voicesMutedGlobal)) != 0;
    currentAttempt->streamsInUse = countSlots(true);
    currentAttempt->slotsInUse = countSlots(false);
}

// Inside 0x49ab50 the result goes to its line. Otherwise 0x5a2d30 was called by other game code (the torture
// scenes call it directly), and only failures are written.
void __cdecl afterVoiceCall(int32_t handle, int32_t voiceSet, int32_t voiceNumber, uintptr_t caller) {
    if (currentAttempt) {
        currentAttempt->handle = handle;
        return;
    }
    if (handle >= 0 || !allowGameVoiceLine()) return;
    VoiceAttempt attempt;
    attempt.forced = true;
    attempt.reachedVoiceCall = true;
    attempt.voicesMuted = readGame<uint8_t>(va(voicesMutedGlobal)) != 0;
    attempt.streamsInUse = countSlots(true);
    attempt.slotsInUse = countSlots(false);
    lookUpVoice(voiceSet, voiceNumber, &attempt);
    char state[160];
    sprintf_s(state, "muted=%s, streams=%u/%u, slots in use=%u/%u, record=%s", attempt.voicesMuted ? "yes" : "no",
        attempt.streamsInUse, streamLimit, attempt.slotsInUse, soundSlotCount,
        !attempt.recordFound ? "missing" : attempt.recordStreamed ? "streamed" : "sample");
    writeLine("direct voice call: voice set %ld, voice %ld: %s [caller 0x%08lx; %s]", static_cast<long>(voiceSet),
        static_cast<long>(voiceNumber), describe(diagnose(attempt)), static_cast<unsigned long>(defaultVa(caller)), state);
}

// ecx and ebx are 0x5a2d30's arguments, so they reach the original unchanged; the C functions keep ebx.
__declspec(naked) void voiceCallHook() {
    __asm {
        push ecx
        call beforeVoiceCall
        pop ecx
        push ebx                     // For afterVoiceCall: voice number
        push ecx                     //                     voice set
        push dword ptr [esp + 0x10]  // Position
        push dword ptr [esp + 0x10]  // Sound manager
        call dword ptr [originalVoiceCall]
        pop ecx
        pop edx
        push eax
        push dword ptr [esp + 4]     // Caller
        push edx
        push ecx
        push eax
        call afterVoiceCall
        add esp, 0x10
        pop eax
        ret 8
    }
}

void __cdecl beforeScriptVoice(int32_t characterId, int32_t voiceNumber, float volume, uintptr_t caller) {
    scriptCall.active = true;
    scriptCall.reachedCharacterVoice = false;
    scriptCall.characterId = characterId;
    scriptCall.voiceNumber = voiceNumber;
    scriptCall.volume = volume;
    scriptCall.returnAddress = caller;
}

// 0x568d00 returns without a line from 0x49ab50 when it found no character, or when a volume sent the voice to
// 0x5a2e00 (0x568d46).
void __cdecl afterScriptVoice() {
    scriptCall.active = false;
    if (scriptCall.reachedCharacterVoice) return;
    noteVoicesMutedChange();
    const char* why;
    if (scriptCall.characterId < 0) {
        why = "not played: the character ID is negative (RegisterEventChara found no character)";
    } else {
        const uintptr_t character = callFindCharacter(scriptCall.characterId);
        if (!character)
            why = "not played: no character has this ID now";
        else if (!readGame<uintptr_t>(character + characterDataOffset))
            why = "not played: the character has no data";
        else
            why = "sent to 0x5a2e00 with a volume (not followed by this trace)";
    }
    writeLine("PlayVoiceSE: character ID %ld, voice %ld, volume %.2f: %s [caller 0x%08lx]",
        static_cast<long>(scriptCall.characterId), static_cast<long>(scriptCall.voiceNumber), scriptCall.volume, why,
        static_cast<unsigned long>(defaultVa(scriptCall.returnAddress)));
}

// eax and ecx are 0x568d00's arguments; they are restored before the original runs.
__declspec(naked) void scriptVoiceHook() {
    __asm {
        push eax
        push ecx
        push dword ptr [esp + 8]     // Caller
        push dword ptr [esp + 0x10]  // Volume
        push ecx
        push eax
        call beforeScriptVoice
        add esp, 0x10
        pop ecx
        pop eax
        push dword ptr [esp + 4]     // Volume
        call dword ptr [originalScriptVoice]
        add esp, 4
        call afterScriptVoice
        ret
    }
}

// ---------------------------------------------------------------- Start-up

struct Hook {
    uintptr_t address;
    uint32_t length;
    void* detour;
    void** original;
};

const char* installHooks() {
    const Hook hooks[] = {
        {voiceCall, voiceCallHookLength, reinterpret_cast<void*>(&voiceCallHook), &originalVoiceCall},
        {characterVoice, characterVoiceHookLength, reinterpret_cast<void*>(&characterVoiceHook),
            reinterpret_cast<void**>(&originalCharacterVoice)},
        {scriptVoice, scriptVoiceHookLength, reinterpret_cast<void*>(&scriptVoiceHook), &originalScriptVoice},
    };
    for (const Hook& hook : hooks) {
        if (!claimGameMemory(va(hook.address), hook.length)) {
            releaseGameMemory();
            return "another Mod already changes one of the voice functions";
        }
    }
    if (MH_Initialize() != MH_OK) return "MinHook could not start";
    for (const Hook& hook : hooks) {
        if (MH_CreateHook(reinterpret_cast<void*>(va(hook.address)), hook.detour, hook.original) != MH_OK) {
            MH_Uninitialize();
            return "a voice function could not be hooked";
        }
    }
    // One switch for all three, so a voice is never traced halfway.
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_Uninitialize();
        return "the voice hooks could not be turned on";
    }
    return nullptr;
}

void readSettings() {
    settings.logPlayedGameVoices = readConfigFlag(L"voice_trace", L"log_played_game_voices", settings.logPlayedGameVoices);
    settings.maxLinesPerMinute = readConfigNumber(L"voice_trace", L"max_lines_per_minute", settings.maxLinesPerMinute,
        0, maxLinesLimit);
}

}  // namespace

namespace debugger::voice_trace {

bool initialize() {
    readSettings();
    char message[200];
    if (const char* why = checkGameCode()) {
        sprintf_s(message, "voice_trace: %s; voices are not traced", why);
        hostLog(message);
        return false;
    }
    if (swprintf_s(logPath, L"%svoice_%s.log", session.logDirectory, session.stamp) < 0) return false;
    if (const char* why = installHooks()) {
        sprintf_s(message, "voice_trace: %s; voices are not traced", why);
        hostLog(message);
        return false;
    }
    minuteStart = GetTickCount64();
    sprintf_s(message, "voice_trace: tracing character voices (log_played_game_voices=%d, max_lines_per_minute=%u); "
        "writing logs\\voice_*.log", settings.logPlayedGameVoices ? 1 : 0, settings.maxLinesPerMinute);
    hostLog(message);
    return true;
}

}  // namespace debugger::voice_trace
