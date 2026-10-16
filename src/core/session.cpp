// Session, settings, and writing to mod_loader.log.
#include "core.h"
#include <wots4/mod_api.h>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace debugger {
Session session;
}

namespace {

using namespace debugger;

const wots4::HostApi* host;

// Preferred base written in the exe file. The in-memory header after loading cannot be used, because
// Windows rewrites it to the actual base when ASLR moves the image. 0 if it cannot be read.
uintptr_t preferredBaseFromFile(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    unsigned char headers[4096];
    DWORD read = 0;
    BOOL ok = ReadFile(file, headers, sizeof(headers), &read, nullptr);
    CloseHandle(file);
    if (!ok || read < sizeof(IMAGE_DOS_HEADER)) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(headers);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<DWORD>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > read)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(headers + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    return nt->OptionalHeader.ImageBase;
}

// logs\ in the game folder (the folder containing the ModLoader's dinput8.dll).
bool findLogDirectory(HMODULE loader) {
    wchar_t path[ARRAYSIZE(session.logDirectory)];
    DWORD length = loader ? GetModuleFileNameW(loader, path, ARRAYSIZE(path)) : 0;
    if (!length || length >= ARRAYSIZE(path)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = L'\0';
    return swprintf_s(session.logDirectory, L"%slogs\\", path) >= 0;
}

}  // namespace

namespace debugger {

bool initializeSession(const wots4::ModContext* context) {
    host = context->host;
    session.self = context->module;
    session.gameBase = reinterpret_cast<uintptr_t>(context->game);
    DWORD length = GetModuleFileNameW(context->game, session.gamePath, ARRAYSIZE(session.gamePath));
    if (!length || length >= ARRAYSIZE(session.gamePath)) return false;
    session.gamePreferredBase = preferredBaseFromFile(session.gamePath);
    if (swprintf_s(session.configPath, L"%sconfig.ini", context->directory) < 0) return false;
    if (!findLogDirectory(context->loader)) return false;
    SYSTEMTIME now;
    GetLocalTime(&now);
    return swprintf_s(session.stamp, L"%04u%02u%02u_%02u%02u%02u_%03u_%lu", now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId()) >= 0;
}

void hostLog(const char* message) {
    if (host && host->log) host->log(session.self, message);
}

void readConfig(const wchar_t* section, const wchar_t* key, const wchar_t* fallback, wchar_t* out, DWORD size) {
    GetPrivateProfileStringW(section, key, fallback, out, size, session.configPath);
}

bool readConfigFlag(const wchar_t* section, const wchar_t* key, bool fallback) {
    wchar_t value[16];
    readConfig(section, key, fallback ? L"1" : L"0", value, ARRAYSIZE(value));
    if (wcscmp(value, L"1") == 0) return true;
    if (wcscmp(value, L"0") == 0) return false;
    char message[256];
    sprintf_s(message, "config.ini: [%ls] %ls=%ls is not a valid value (use 1 or 0); using %d", section, key,
        value, fallback ? 1 : 0);
    hostLog(message);
    return fallback;
}

unsigned readConfigNumber(const wchar_t* section, const wchar_t* key, unsigned fallback, unsigned minimum,
    unsigned maximum) {
    wchar_t value[32];
    readConfig(section, key, L"", value, ARRAYSIZE(value));
    if (!value[0]) return fallback;
    wchar_t* end = nullptr;
    const unsigned long number = wcstoul(value, &end, 10);
    const bool onlyDigits = iswdigit(value[0]) && end && *end == L'\0';
    if (onlyDigits && number >= minimum && number <= maximum) return static_cast<unsigned>(number);
    char message[256];
    sprintf_s(message, "config.ini: [%ls] %ls=%ls is not a valid value (use %u to %u); using %u", section, key,
        value, minimum, maximum, fallback);
    hostLog(message);
    return fallback;
}

bool claimGameMemory(uintptr_t address, uint32_t length) {
    return host && host->claim && host->claim(session.self, address, length);
}

void releaseGameMemory() {
    if (host && host->releaseClaims) host->releaseClaims(session.self);
}

void createLogDirectory() {
    CreateDirectoryW(session.logDirectory, nullptr);  // Simply fails if it already exists
}

}  // namespace debugger
