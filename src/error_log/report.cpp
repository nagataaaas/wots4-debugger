// Builds the record of one exception. Called while handling an exception, so it does not use the heap,
// and checks memory that may not be readable before reading it (and catches exceptions while reading, too).
#include "internal.h"
#define PSAPI_VERSION 2  // Make EnumProcessModules the kernel32 K32EnumProcessModules
#include <psapi.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

using namespace debugger;
using namespace debugger::error_log;

// ---------------------------------------------------------------- Log file

char pending[32 * 1024];  // Buffered until flushLog
size_t pendingLength;
HANDLE logFile = INVALID_HANDLE_VALUE;

// Creates the file on the first record. A run with no exceptions leaves no log.
bool openLog() {
    if (logFile != INVALID_HANDLE_VALUE) return true;
    createLogDirectory();
    logFile = CreateFileW(state.logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(logFile, state.header, static_cast<DWORD>(strlen(state.header)), &written, nullptr);
    return true;
}

void append(const char* text, size_t length) {
    while (length) {
        if (pendingLength == sizeof(pending)) flushLog();
        size_t chunk = sizeof(pending) - pendingLength;
        if (chunk > length) chunk = length;
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
    int length = _vsnprintf_s(text, _TRUNCATE, format, arguments);
    va_end(arguments);
    append(text, length >= 0 ? static_cast<size_t>(length) : strlen(text));
}

// ---------------------------------------------------------------- Memory

bool readableProtection(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool readable(uintptr_t address, size_t size) {
    uintptr_t end = address + size;
    if (end < address) return false;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) return false;
        if (region.State != MEM_COMMIT || !readableProtection(region.Protect)) return false;
        address = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

// Another thread may free the memory after the check, so also catch exceptions while reading.
bool copyMemory(void* out, uintptr_t address, size_t size) {
    if (!readable(address, size)) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

const char* protectionName(DWORD protect) {
    switch (protect & 0xff) {
    case PAGE_NOACCESS: return "no access";
    case PAGE_READONLY: return "read-only";
    case PAGE_READWRITE: return "read/write";
    case PAGE_WRITECOPY: return "write-copy";
    case PAGE_EXECUTE: return "execute";
    case PAGE_EXECUTE_READ: return "execute/read";
    case PAGE_EXECUTE_READWRITE: return "execute/read/write";
    case PAGE_EXECUTE_WRITECOPY: return "execute/write-copy";
    default: return "unknown protection";
    }
}

// ---------------------------------------------------------------- Modules

struct Image {
    uintptr_t base;
    uint32_t size;
    char name[64];
};

Image images[96];
int imageCount;

bool readHeaders(uintptr_t base, IMAGE_NT_HEADERS32* nt, uintptr_t* sections) {
    IMAGE_DOS_HEADER dos;
    if (!copyMemory(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    uintptr_t at = base + static_cast<uint32_t>(dos.e_lfanew);
    if (!copyMemory(nt, at, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (sections)
        *sections = at + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
    return true;
}

// The module (exe or DLL) containing address. Its name is cached once looked up.
bool findImage(uintptr_t address, Image* out) {
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) || region.Type != MEM_IMAGE)
        return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(region.AllocationBase);
    for (int i = 0; i < imageCount; ++i) {
        if (images[i].base == base) {
            *out = images[i];
            return true;
        }
    }
    IMAGE_NT_HEADERS32 nt;
    if (!readHeaders(base, &nt, nullptr)) return false;
    Image image = {base, nt.OptionalHeader.SizeOfImage, {}};
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path)) {
        const wchar_t* file = wcsrchr(path, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, file ? file + 1 : path, -1, image.name, sizeof(image.name),
            nullptr, nullptr);
    } else {
        sprintf_s(image.name, "image@%08lx", static_cast<unsigned long>(base));
    }
    if (imageCount < ARRAYSIZE(images)) images[imageCount++] = image;
    *out = image;
    return true;
}

// Name and characteristics of the section containing rva.
bool findSection(const Image& image, uint32_t rva, char* name, DWORD* characteristics) {
    IMAGE_NT_HEADERS32 nt;
    uintptr_t sections;
    if (!readHeaders(image.base, &nt, &sections)) return false;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections && i < 96; ++i) {
        IMAGE_SECTION_HEADER section;
        if (!copyMemory(&section, sections + i * sizeof(section), sizeof(section))) return false;
        DWORD size = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
        if (rva >= section.VirtualAddress && rva < section.VirtualAddress + size) {
            memcpy(name, section.Name, 8);
            name[8] = '\0';
            *characteristics = section.Characteristics;
            return true;
        }
    }
    return false;
}

// Writes where a value points, as part of a line.
// Examples: "WayOfTheSamurai4.exe+1234 .text (default VA 00401234)", "free (...)", "read/write private"
void describe(uintptr_t value) {
    if (value < 0x10000) {
        append("near NULL (never accessible)");
        return;
    }
    Image image;
    if (findImage(value, &image)) {
        uint32_t rva = static_cast<uint32_t>(value - image.base);
        appendf("%s+%lx", image.name, static_cast<unsigned long>(rva));
        char section[9];
        DWORD characteristics;
        if (findSection(image, rva, section, &characteristics)) appendf(" %s", section);
        if (image.base == session.gameBase && session.gamePreferredBase)
            appendf(" (default VA %08lx)", static_cast<unsigned long>(session.gamePreferredBase + rva));
        return;
    }
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery(reinterpret_cast<void*>(value), &region, sizeof(region))) {
        append("unknown address");
        return;
    }
    if (region.State == MEM_FREE) {
        append("free (released, or never allocated)");
    } else if (region.State == MEM_RESERVE) {
        append("reserved, not committed (released, or never allocated)");
    } else {
        appendf("%s%s %s", protectionName(region.Protect), (region.Protect & PAGE_GUARD) ? " guard" : "",
            region.Type == MEM_PRIVATE ? "private" : "mapped");
    }
}

// If a value points to a module's read-only data, it may be a C++ virtual function table.
bool pointsToReadOnlyData(uintptr_t value) {
    Image image;
    char section[9];
    DWORD characteristics;
    return findImage(value, &image) &&
        findSection(image, static_cast<uint32_t>(value - image.base), section, &characteristics) &&
        !(characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE));
}

bool pointsToCode(uintptr_t value) {
    Image image;
    char section[9];
    DWORD characteristics;
    return findImage(value, &image) &&
        findSection(image, static_cast<uint32_t>(value - image.base), section, &characteristics) &&
        (characteristics & IMAGE_SCN_MEM_EXECUTE);
}

// Whether a call instruction comes right before address. Used as a hint that a stack value is a return address.
bool followsCall(uintptr_t address) {
    uint8_t bytes[8];  // bytes[8 - k] is the byte at address - k
    if (!copyMemory(bytes, address - 8, sizeof(bytes))) return false;
    if (bytes[3] == 0xe8) return true;  // call rel32
    static const int lengths[] = {2, 3, 4, 6, 7};
    for (int length : lengths) {  // call r/m32 (FF /2)
        uint8_t opcode = bytes[8 - length], modrm = bytes[8 - length + 1];
        if (opcode == 0xff && ((modrm >> 3) & 7) == 2) return true;
    }
    return false;
}

void hexLine(uintptr_t address, size_t size) {
    uint8_t bytes[16];
    if (size > sizeof(bytes)) size = sizeof(bytes);
    if (!copyMemory(bytes, address, size)) return;
    appendf("      %08lx:", static_cast<unsigned long>(address));
    for (size_t i = 0; i < size; ++i) appendf(" %02x", bytes[i]);
    append("\r\n");
}

// ---------------------------------------------------------------- Describing the exception

const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "floating-point invalid operation";
    case EXCEPTION_FLT_OVERFLOW: return "floating-point overflow";
    case EXCEPTION_FLT_STACK_CHECK: return "floating-point stack check";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case 0xc0000374: return "heap corruption";
    case 0xc0000409: return "stack buffer overrun";
    case 0xe06d7363: return "C++ exception (throw)";
    default: return "";
    }
}

void writeTime() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    appendf("%04u-%02u-%02u %02u:%02u:%02u.%03u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds);
}

void writeCause(const EXCEPTION_RECORD* record) {
    appendf("Code: %08lx %s", record->ExceptionCode, exceptionName(record->ExceptionCode));
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2) {
        ULONG_PTR operation = record->ExceptionInformation[0];
        uintptr_t target = record->ExceptionInformation[1];
        const char* verb = operation == 0 ? "read" : operation == 1 ? "write" : operation == 8 ? "execute" : "access";
        appendf(" (%s at %08lx: ", verb, static_cast<unsigned long>(target));
        describe(target);
        append(")");
    }
    append("\r\n");
}

struct Register {
    const char* name;
    DWORD value;
};

void writeRegisters(const CONTEXT* context) {
    appendf("Registers: eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx\r\n"
            "           ebp=%08lx esp=%08lx eip=%08lx eflags=%08lx\r\n",
        context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi,
        context->Ebp, context->Esp, context->Eip, context->EFlags);

    Register pointers[] = {
        {"eax", context->Eax}, {"ebx", context->Ebx}, {"ecx", context->Ecx}, {"edx", context->Edx},
        {"esi", context->Esi}, {"edi", context->Edi}, {"ebp", context->Ebp},
    };
    append("Where the registers point:\r\n");
    for (const Register& item : pointers) {
        appendf("  %s %08lx: ", item.name, item.value);
        describe(item.value);
        append("\r\n");
        if (item.value < 0x10000 || !readable(item.value, 4)) continue;
        hexLine(item.value, 16);
        hexLine(item.value + 16, 16);
        DWORD first;
        if (copyMemory(&first, item.value, sizeof(first)) && pointsToReadOnlyData(first)) {
            appendf("      first 4 bytes %08lx -> ", first);
            describe(first);
            append(" (possibly a vtable)\r\n");
        }
    }
}

// Picks values on the stack that point right after a call (likely return addresses).
// Some functions do not use a frame pointer, so this scans instead of walking frames. False positives can appear.
void writeStack(const CONTEXT* context) {
    uintptr_t esp = context->Esp;
    append("Top of the stack:\r\n");
    for (int i = 0; i < 4; ++i) hexLine(esp + i * 16, 16);

    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    uintptr_t end = esp + 16 * 1024;
    uintptr_t stackBase = reinterpret_cast<uintptr_t>(tib->StackBase);
    if (esp < stackBase && end > stackBase) end = stackBase;
    append("Likely return addresses on the stack (up to 16 KB above esp; values right after a call):\r\n");
    int found = 0;
    for (uintptr_t at = esp & ~uintptr_t(3); at + 4 <= end && found < 48; at += 4) {
        DWORD value;
        if (!copyMemory(&value, at, sizeof(value))) break;
        if (!pointsToCode(value) || !followsCall(value)) continue;
        appendf("  esp+%04lx %08lx ", static_cast<unsigned long>(at - esp), value);
        describe(value);
        append("\r\n");
        ++found;
    }
    if (!found) append("  none found\r\n");
}

void writeModules() {
    HMODULE modules[256];
    DWORD needed = 0;
    append("Loaded modules:\r\n");
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        appendf("  could not list them (Windows error %lu)\r\n", GetLastError());
        return;
    }
    DWORD count = needed / sizeof(HMODULE);
    if (count > ARRAYSIZE(modules)) count = ARRAYSIZE(modules);
    for (DWORD i = 0; i < count; ++i) {
        uintptr_t base = reinterpret_cast<uintptr_t>(modules[i]);
        IMAGE_NT_HEADERS32 nt = {};
        readHeaders(base, &nt, nullptr);
        wchar_t path[MAX_PATH] = L"?";
        GetModuleFileNameW(modules[i], path, ARRAYSIZE(path));
        char utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
        appendf("  %08lx-%08lx timestamp %08lx %s\r\n", static_cast<unsigned long>(base),
            static_cast<unsigned long>(base + nt.OptionalHeader.SizeOfImage), nt.FileHeader.TimeDateStamp, utf8);
    }
}

void writePlace(uintptr_t ip) {
    appendf("Address: %08lx ", static_cast<unsigned long>(ip));
    describe(ip);
    append("\r\n");
    uint8_t bytes[16];
    if (copyMemory(bytes, ip, sizeof(bytes))) {
        append("Instruction bytes:");
        for (uint8_t byte : bytes) appendf(" %02x", byte);
        append("\r\n");
    }
}

}  // namespace

namespace debugger::error_log {

void writeLine(const char* text) {
    append(text);
    append("\r\n");
}

void flushLog() {
    if (pendingLength && openLog()) {
        DWORD written;
        WriteFile(logFile, pending, static_cast<DWORD>(pendingLength), &written, nullptr);
        FlushFileBuffers(logFile);
    }
    pendingLength = 0;
}

void writeReport(ReportKind kind, EXCEPTION_POINTERS* info, unsigned long occurrence) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const CONTEXT* context = info->ContextRecord;
    append("\r\n");
    if (kind == ReportKind::fatal) {
        append("==== Fatal exception (not handled anywhere)");
    } else {
        appendf("==== Exception #%lu (first chance; the game may handle it)", occurrence);
    }
    append("  ");
    writeTime();
    appendf("  thread %lu\r\n", GetCurrentThreadId());
    writeCause(record);
    writePlace(reinterpret_cast<uintptr_t>(record->ExceptionAddress));
    writeRegisters(context);
    writeStack(context);
    if (kind == ReportKind::fatal) {
        visitNotes([](const char* line, void*) { writeLine(line); }, nullptr);
        writeModules();
        flushLog();  // Keep everything so far even if the minidump stops partway
        appendf("Minidump: %s\r\n", writeMinidump(info));
    }
    flushLog();
}

void writeRepeat(EXCEPTION_POINTERS* info, unsigned long occurrence) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    appendf("---- Exception %08lx again at the same address ", record->ExceptionCode);
    describe(reinterpret_cast<uintptr_t>(record->ExceptionAddress));
    appendf(": occurrence %lu  ", occurrence);
    writeTime();
    appendf("  thread %lu\r\n", GetCurrentThreadId());
    flushLog();
}

}  // namespace debugger::error_log
