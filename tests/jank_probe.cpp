// Runs jank's sampler, analyzer and log without the game. This exe stands in for the game exe: it presents
// "frames" of about 10 ms itself, with one frame that spends 150 ms in slowFrame.
//   jank_probe <work folder>   (writes <work folder>\logs\jank_*.log)
// Prints the default VA of the call inside slowFrame, and run_jank_probe.cmake looks for it in the log.
#include "jank/internal.h"
#include <wots4/mod_api.h>
#include <intrin.h>
#include <cstdio>

namespace {

using namespace debugger;
using namespace debugger::jank;

constexpr double normalFrameMs = 10, slowFrameMs = 150;
constexpr int framesBefore = 60, framesAfter = 60;

LARGE_INTEGER frequency;

double nowMs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000.0 / frequency.QuadPart;
}

volatile uint32_t sink;
uintptr_t slowCallSite;  // Where spin returns to in slowFrame

// Mostly arithmetic, so most samples land in this function rather than in QueryPerformanceCounter.
__declspec(noinline) void spin(double untilMs, uintptr_t* returnAddress) {
    if (returnAddress) *returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    uint32_t value = sink;
    while (nowMs() < untilMs) {
        for (int i = 0; i < 20000; ++i) value = value * 1664525u + 1013904223u;
        sink = value;
    }
}

__declspec(noinline) void normalFrame(double untilMs) {
    spin(untilMs, nullptr);
}

__declspec(noinline) void slowFrame(double untilMs) {
    spin(untilMs, &slowCallSite);
    sink = sink + 1;  // Keeps the call above from becoming a tail jump
}

void presentFrames(int count, void (*work)(double), double frameMs, const GameState& state) {
    for (int i = 0; i < count; ++i) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        work(nowMs() + frameMs);
        framePresented(start.QuadPart, state);
    }
}

void WINAPI printLog(HMODULE, const char* message) {
    printf("[debugger] %s\n", message);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    QueryPerformanceFrequency(&frequency);
    wchar_t work[1024], directory[1100];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, work, ARRAYSIZE(work));
    swprintf_s(directory, L"%s\\mods\\debugger\\", work);

    // The session puts logs\ next to the "ModLoader", which here is this exe in the work folder.
    wots4::HostApi host = {};
    host.size = sizeof(host);
    host.version = wots4::modApiVersion;
    host.log = printLog;
    wots4::ModContext context = {};
    context.size = sizeof(context);
    context.version = wots4::modApiVersion;
    context.loader = GetModuleHandleW(nullptr);
    context.module = GetModuleHandleW(nullptr);
    context.game = GetModuleHandleW(nullptr);
    context.directory = directory;
    context.host = &host;
    if (!initializeSession(&context) || !prepareMonitoring()) {
        printf("FAIL the session could not be set up\n");
        return 1;
    }
    Settings settings;
    settings.summarySeconds = 1;
    if (!startMonitoring(settings)) {
        printf("FAIL the threads did not start\n");
        return 1;
    }

    GameState playing = {};
    playing.stateFunction = toDefaultVa(reinterpret_cast<uintptr_t>(&normalFrame));
    playing.scriptKind = -1;
    playing.gameTimeRunning = 1;
    GameState event = playing;
    event.stateFunction = toDefaultVa(reinterpret_cast<uintptr_t>(&slowFrame));
    event.scriptRunning = 1;
    event.scriptKind = 1;  // scenario

    presentFrames(framesBefore, normalFrame, normalFrameMs, playing);
    presentFrames(1, slowFrame, slowFrameMs, event);
    presentFrames(framesAfter, normalFrame, normalFrameMs, playing);
    Sleep(500);  // The analyzer looks every 50 ms and may wait for the last samples

    printf("SLOW_CALL_SITE=%08lx\n", static_cast<unsigned long>(toDefaultVa(slowCallSite)));
    printf("SLOW_STATE=%08lx\n", static_cast<unsigned long>(event.stateFunction));
    printf("PASS the probe presented %d frames\n", framesBefore + 1 + framesAfter);
    return 0;
}
