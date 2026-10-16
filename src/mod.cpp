// Entry point of debugger. Sets up the foundation (core), then initializes the features in order.
// To add a feature, create src/<feature name>/ and add it to features below. Its setting is [<feature name>] enabled=1/0 in config.ini.
#include "core/core.h"
#include "error_log/error_log.h"
#include "jank/jank.h"
#include "voice_trace/voice_trace.h"
#include <wots4/mod_api.h>
#include <cstdio>

namespace {

using namespace debugger;

struct Feature {
    const wchar_t* name;      // Section name in config.ini
    const char* logName;      // Name written to mod_loader.log
    bool enabledByDefault;
    bool (*initialize)();
};

const Feature features[] = {
    {L"error_log", "error_log", true, error_log::initialize},
    {L"jank", "jank", false, jank::initialize},
    {L"voice_trace", "voice_trace", false, voice_trace::initialize},
};

volatile LONG initialized;
bool accepted;

}  // namespace

extern "C" BOOL WINAPI Wots4ModInitialize(const wots4::ModContext* context) {
    if (!context || context->size < sizeof(*context) || context->version != wots4::modApiVersion ||
        !context->directory || !context->module || !context->game)
        return FALSE;
    if (InterlockedCompareExchange(&initialized, 1, 0)) return accepted;
    if (!initializeSession(context)) return FALSE;

    // Features are also called while handling exceptions, so this DLL stays loaded until the process exits.
    HMODULE pinned;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Wots4ModInitialize), &pinned))
        return FALSE;

    // A failure in one feature does not stop the other features or the notes.
    for (const Feature& feature : features) {
        char message[160];
        if (!readConfigFlag(feature.name, L"enabled", feature.enabledByDefault)) {
            sprintf_s(message, "%s: disabled in config.ini", feature.logName);
            hostLog(message);
        } else if (!feature.initialize()) {
            sprintf_s(message, "%s: failed to start", feature.logName);
            hostLog(message);
        }
    }
    accepted = true;
    return TRUE;
}

BOOL WINAPI DllMain(HMODULE, DWORD, void*) {
    return TRUE;
}
