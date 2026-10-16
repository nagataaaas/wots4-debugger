// Foundation of debugger. Holds what the features (error_log, etc.) share.
// Features live in src/<feature name>/ and are initialized from the list in mod.cpp. Their settings are [<feature name>] in config.ini.
// The functions here may be called while handling an exception, so they do not use the heap.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX  // Keep std::min and std::max usable
#endif
#include <windows.h>
#include <cstdint>

namespace wots4 {
struct ModContext;
}

namespace debugger {

// Set once during initialization, read-only afterwards.
struct Session {
    HMODULE self = nullptr;
    uintptr_t gameBase = 0;           // Actual base (ASLR)
    uintptr_t gamePreferredBase = 0;  // Preferred base written in the exe file. 0 if it cannot be read
    wchar_t gamePath[1024] = {};      // The game's exe
    wchar_t configPath[1024] = {};    // mods\debugger\config.ini
    wchar_t logDirectory[1024] = {};  // logs\ in the game folder (ends with \)
    wchar_t stamp[64] = {};           // <date-time>_<process ID>. Used in the names of files written in the same run
};
extern Session session;

// Builds the Session. Returns false on failure (debugger then refuses to load).
bool initializeSession(const wots4::ModContext* context);

// Writes one line to mod_loader.log. Write in English.
void hostLog(const char* message);

// [section] key in config.ini, or fallback if missing.
void readConfig(const wchar_t* section, const wchar_t* key, const wchar_t* fallback, wchar_t* out, DWORD size);
// A 1 / 0 setting. Any other value falls back to fallback and is reported in mod_loader.log.
bool readConfigFlag(const wchar_t* section, const wchar_t* key, bool fallback);
// A whole number from minimum to maximum. Any other value falls back to fallback and is reported in mod_loader.log.
unsigned readConfigNumber(const wchar_t* section, const wchar_t* key, unsigned fallback, unsigned minimum,
    unsigned maximum);

// Reserves [address, address + length) of the game for a change (HostApi::claim). False if another Mod has it.
bool claimGameMemory(uintptr_t address, uint32_t length);
// Releases every reservation of debugger (HostApi::releaseClaims).
void releaseGameMemory();

// Creates logs\ if missing. Call right before writing a file (a run that writes nothing creates no folder).
void createLogDirectory();

// Passes the notes (clues left by other Mods) to visit one line at a time, oldest first. Includes the heading line.
void visitNotes(void (*visit)(const char* line, void* context), void* context);

}  // namespace debugger
