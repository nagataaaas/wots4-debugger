// Header for leaving clues (notes) with the debugger Mod.
// Only the last 64 notes are kept; they are written to the error log (logs\error_*.log) when the game crashes.
// Does nothing if debugger is not installed.
//
//   #include <wots4/debugger.h>
//
//   BOOL WINAPI Wots4ModInitialize(const wots4::ModContext* context) {
//       wots4::debugger::connect(context);  // Call once, during initialization
//       ...
//   }
//   wots4::debugger::note("scene change started");  // Any thread is fine. Up to 160 bytes. Write in English
#pragma once
#include <wots4/mod_api.h>
#include <cwchar>

namespace wots4::debugger {

using NoteFunction = void(WINAPI*)(HMODULE owner, const char* text);

inline NoteFunction noteFunction;
inline HMODULE noteOwner;

// Looks for mods\debugger\mod.dll next to mods\<this Mod>\. The ModLoader loads every mod.dll
// before calling Wots4ModInitialize, so during initialization debugger is already loaded.
inline bool connect(const ModContext* context) {
    if (!context || !context->directory) return false;
    wchar_t path[1024];
    if (wcscpy_s(path, context->directory) != 0) return false;
    size_t length = wcslen(path);
    if (length && path[length - 1] == L'\\') path[--length] = L'\0';
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = L'\0';
    if (wcscat_s(path, L"debugger\\mod.dll") != 0) return false;
    HMODULE debugger = GetModuleHandleW(path);
    noteFunction = debugger ? reinterpret_cast<NoteFunction>(GetProcAddress(debugger, "Wots4DebuggerNote")) : nullptr;
    noteOwner = context->module;
    return noteFunction != nullptr;
}

inline void note(const char* text) {
    if (noteFunction && text) noteFunction(noteOwner, text);
}

}  // namespace wots4::debugger
