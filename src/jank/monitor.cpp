// The sampler and analyzer threads, and the main thread's side of the frame records.
#include "internal.h"
#define PSAPI_VERSION 2  // Make GetProcessMemoryInfo the kernel32 K32GetProcessMemoryInfo
#include <psapi.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace debugger::jank {
GameImage gameImage;
}

namespace {

using namespace debugger;
using namespace debugger::jank;

Settings settings;
LARGE_INTEGER frequency;

Ring<FrameRecord, 1024> frames;   // About 8 seconds at 120 FPS, far more than the analyzer falls behind
Ring<Sample, 8192> samples;       // About 8 seconds at the default interval: the longest frame fully sampled
std::atomic<int64_t> latestSampleTime{0};

// ---------------------------------------------------------------- Main thread

HANDLE mainThread;
uintptr_t mainStackBase;
HANDLE mainThreadKnown;  // Manual-reset event, set on the first presented frame

// The sampler needs a real handle to the main thread, so it is opened from the main thread itself.
void attachMainThread() {
    mainThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
        FALSE, GetCurrentThreadId());
    mainStackBase = reinterpret_cast<uintptr_t>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    if (!mainThread) hostLog("jank: could not open the main thread; slow frames are timed but not sampled");
    SetEvent(mainThreadKnown);
}

// ---------------------------------------------------------------- Sampler

constexpr size_t maxStackCopy = 64 * 1024;  // Deeper than the main loop's stack ever gets in practice
constexpr uintptr_t maxStackSize = 64 * 1024 * 1024;  // An esp further than this from the base is not the stack
alignas(16) unsigned char stackCopy[maxStackCopy];

// Kept in its own function: __try cannot share a function with objects that need unwinding.
size_t copyStack(uintptr_t esp) {
    if (esp >= mainStackBase || mainStackBase - esp > maxStackSize) return 0;
    const size_t length = std::min<size_t>(mainStackBase - esp, maxStackCopy);
    __try {
        memcpy(stackCopy, reinterpret_cast<const void*>(esp), length);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return length;
}

// Whether a call instruction ends right at address in the game's code (a return address, not just a code pointer).
bool followsCallInGame(uintptr_t address) {
    if (address - 8 < gameImage.textStart) return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(address - 8);  // bytes[8 - k] is the byte at address - k
    if (bytes[3] == 0xe8) return true;                                // call rel32
    static const int lengths[] = {2, 3, 4, 6, 7};
    for (int length : lengths) {                                      // call r/m32 (FF /2)
        if (bytes[8 - length] == 0xff && ((bytes[8 - length + 1] >> 3) & 7) == 2) return true;
    }
    return false;
}

// The main loop's return addresses are all in 0x4dd1dd..0x4dd6cc; checking the range first keeps the scan cheap.
constexpr uint32_t firstMarker = 0x4dd1dd, lastMarker = 0x4dd6cc;

void scanStack(size_t length, Sample* sample) {
    const auto* words = reinterpret_cast<const uint32_t*>(stackCopy);
    const size_t wordCount = length / 4;
    const size_t topCount = std::min<size_t>(wordCount, stackTopWords);
    memcpy(sample->stackTop, words, topCount * 4);
    memset(sample->stackTop + topCount, 0, (stackTopWords - topCount) * 4);
    for (size_t i = 0; i < wordCount; ++i) {
        const uintptr_t value = words[i];
        if (value < gameImage.textStart || value >= gameImage.textEnd) continue;
        const uint32_t defaultVa = toDefaultVa(value);
        if (defaultVa >= firstMarker && defaultVa <= lastMarker) {
            const int level = markerLevel(defaultVa);
            if (level >= 0) sample->markers[level] = defaultVa;  // Later words are further out: keeps the outermost
        }
        if (sample->callSiteCount < maxCallSites && followsCallInGame(value))
            sample->callSites[sample->callSiteCount++] = defaultVa;
    }
}

// Between SuspendThread and ResumeThread the main thread may hold any lock (the heap, the loader, the CRT),
// so only GetThreadContext and a memcpy run there. Everything else waits until it runs again.
bool takeSample(Sample* sample) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(mainThread) == static_cast<DWORD>(-1)) return false;
    const bool haveContext = GetThreadContext(mainThread, &context) != 0;
    const size_t length = haveContext ? copyStack(context.Esp) : 0;
    ResumeThread(mainThread);
    if (!haveContext) return false;

    memset(sample->markers, 0, sizeof(sample->markers));
    sample->time = now.QuadPart;
    sample->eip = context.Eip;
    sample->callSiteCount = 0;
    scanStack(length, sample);
    return true;
}

const char* timerKind = "standard waitable timer";

HANDLE createSampleTimer() {
    // High resolution timers (Windows 10 1803 and later) wake within tens of microseconds; others follow the
    // system timer resolution (1 ms when the game raises it, otherwise 15.6 ms).
    constexpr DWORD highResolution = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, highResolution, TIMER_ALL_ACCESS);
    if (timer) {
        timerKind = "high resolution waitable timer";
        return timer;
    }
    return CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
}

HANDLE sampleTimer;

DWORD WINAPI sampleMainThread(void*) {
    WaitForSingleObject(mainThreadKnown, INFINITE);
    if (!mainThread) return 0;
    Sample sample;
    // Each wait aims at the next point of a fixed schedule, so the time spent sampling and waking late does not
    // stretch the interval. After a long stall the schedule starts again from now instead of catching up.
    const int64_t intervalTicks = static_cast<int64_t>(settings.sampleIntervalUs * ticksPerMs() / 1000);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t nextSample = now.QuadPart + intervalTicks;
    for (;;) {
        QueryPerformanceCounter(&now);
        if (nextSample <= now.QuadPart) nextSample = now.QuadPart + intervalTicks;
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((nextSample - now.QuadPart) * 10000 / ticksPerMs());  // 100 ns units
        nextSample += intervalTicks;
        if (!SetWaitableTimer(sampleTimer, &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(sampleTimer, INFINITE) != WAIT_OBJECT_0) {
            hostLog("jank: the sample timer failed; sampling stopped (slow frames are still timed)");
            return 0;
        }
        if (!takeSample(&sample)) continue;  // The thread is gone or not stopped; try the next time
        samples.push(sample);
        latestSampleTime.store(sample.time, std::memory_order_release);
    }
}

// ---------------------------------------------------------------- Analyzer

constexpr DWORD analyzeIntervalMs = 50;
constexpr DWORD sampleCatchUpMs = 100;  // How long a report waits for the samples up to the slow frame's end
constexpr int64_t minuteSeconds = 60;

double cyclesPerMs;  // 0 if not calibrated

// QueryThreadCycleTime counts the time stamp counter, so its rate is measured against QPC once.
void calibrateCycles() {
    LARGE_INTEGER startTicks, endTicks;
    QueryPerformanceCounter(&startTicks);
    const uint64_t startCycles = __rdtsc();
    Sleep(100);
    const uint64_t endCycles = __rdtsc();
    QueryPerformanceCounter(&endTicks);
    const double ms = (endTicks.QuadPart - startTicks.QuadPart) / ticksPerMs();
    if (ms > 0) cyclesPerMs = (endCycles - startCycles) / ms;
}

struct SummaryWindow {
    std::vector<double> frameMs;
    int64_t start = 0;
    unsigned janks = 0, reported = 0;
};

struct Analyzer {
    JankDetector detector{settings.thresholdMs};
    FrameRecord previous = {};
    bool havePrevious = false;
    uint32_t nextFrame = 0;
    uint64_t frameNumber = 0;
    int64_t minuteStart = 0;
    unsigned reportsThisMinute = 0, suppressed = 0;
    SummaryWindow summary;
    std::vector<Sample> frameSamples;
};

// Copies the samples taken between two QPC times. Returns whether the start of that time was already overwritten.
bool collectSamples(int64_t from, int64_t to, std::vector<Sample>* out) {
    out->clear();
    const uint32_t end = samples.count();
    const uint32_t oldest = end > samples.size() ? end - samples.size() : 0;
    uint32_t low = oldest, high = end;
    while (low < high) {  // First sample at or after from
        const uint32_t middle = low + (high - low) / 2;
        if (samples.at(middle).time < from) low = middle + 1;
        else high = middle;
    }
    uint32_t last = low;
    while (last < end && samples.at(last).time <= to) ++last;
    out->resize(last - low);
    const uint32_t intact = out->empty() ? low : samples.copy(low, last, out->data());
    if (intact > low) out->erase(out->begin(), out->begin() + (intact - low));
    const bool ringWrapped = oldest > 0;
    return (ringWrapped && low == oldest) || intact > low;
}

void waitForSamplesUpTo(int64_t time) {
    if (!mainThread) return;
    for (DWORD waited = 0; waited < sampleCatchUpMs; waited += 5) {
        if (latestSampleTime.load(std::memory_order_acquire) >= time) return;
        Sleep(5);
    }
}

double cpuMsBetween(const FrameRecord& from, const FrameRecord& to) {
    if (!cyclesPerMs || !from.cpuCycles || !to.cpuCycles) return -1;
    return (to.cpuCycles - from.cpuCycles) / cyclesPerMs;
}

void reportSlowFrame(Analyzer& analyzer, const FrameRecord& slow, double frameMs, double thresholdMs) {
    const int64_t minuteTicks = static_cast<int64_t>(minuteSeconds * 1000 * ticksPerMs());
    if (slow.presented - analyzer.minuteStart >= minuteTicks) {
        analyzer.minuteStart = slow.presented;
        analyzer.reportsThisMinute = 0;
    }
    ++analyzer.summary.janks;
    if (analyzer.reportsThisMinute >= settings.maxReportsPerMinute) {
        ++analyzer.suppressed;
        return;
    }
    ++analyzer.reportsThisMinute;
    ++analyzer.summary.reported;
    waitForSamplesUpTo(slow.presented);
    const bool cutShort = collectSamples(analyzer.previous.presented, slow.presented, &analyzer.frameSamples);

    JankReport report = {};
    report.previous = &analyzer.previous;
    report.slow = &slow;
    report.frameNumber = analyzer.frameNumber;
    report.frameMs = frameMs;
    report.thresholdMs = thresholdMs;
    report.medianMs = analyzer.detector.median();
    report.cpuMs = cpuMsBetween(analyzer.previous, slow);
    report.samples = analyzer.frameSamples.data();
    report.sampleCount = static_cast<int>(analyzer.frameSamples.size());
    report.samplesCutShort = cutShort;
    report.suppressedBefore = analyzer.suppressed;
    writeJankReport(report);
    analyzer.suppressed = 0;
}

void finishSummary(Analyzer& analyzer, int64_t now) {
    SummaryWindow& window = analyzer.summary;
    Summary summary = {};
    summary.seconds = (now - window.start) / ticksPerMs() / 1000;
    summary.frames = window.frameMs.size();
    double* values = window.frameMs.data();
    const int count = static_cast<int>(window.frameMs.size());
    summary.maxMs = count ? *std::max_element(values, values + count) : 0;
    summary.p50 = percentile(values, count, 0.50);
    summary.p95 = percentile(values, count, 0.95);
    summary.p99 = percentile(values, count, 0.99);
    summary.janks = window.janks;
    summary.reported = window.reported;
    writeSummary(summary);
    window.frameMs.clear();
    window.janks = window.reported = 0;
    window.start = now;
}

void analyzeFrame(Analyzer& analyzer, const FrameRecord& frame) {
    if (!analyzer.havePrevious) {
        analyzer.previous = frame;
        analyzer.havePrevious = true;
        analyzer.minuteStart = analyzer.summary.start = frame.presented;
        return;
    }
    ++analyzer.frameNumber;
    const double frameMs = (frame.presented - analyzer.previous.presented) / ticksPerMs();
    const double thresholdMs = analyzer.detector.threshold();
    analyzer.detector.add(frameMs);
    analyzer.summary.frameMs.push_back(frameMs);
    if (thresholdMs > 0 && frameMs >= thresholdMs) reportSlowFrame(analyzer, frame, frameMs, thresholdMs);
    const int64_t summaryTicks = static_cast<int64_t>(settings.summarySeconds * 1000.0 * ticksPerMs());
    if (settings.summarySeconds && frame.presented - analyzer.summary.start >= summaryTicks)
        finishSummary(analyzer, frame.presented);
    analyzer.previous = frame;
}

DWORD WINAPI analyzeFrames(void*) {
    WaitForSingleObject(mainThreadKnown, INFINITE);
    calibrateCycles();
    static Analyzer analyzer;  // Large; kept off this thread's stack
    analyzer.summary.frameMs.reserve(4096);
    FrameRecord frame;
    for (;;) {
        Sleep(analyzeIntervalMs);
        const uint32_t end = frames.count();
        if (end - analyzer.nextFrame > frames.size()) analyzer.havePrevious = false;  // Fell behind; restart
        for (uint32_t number = end - std::min(end - analyzer.nextFrame, frames.size()); number != end; ++number) {
            if (frames.copy(number, number + 1, &frame) != number) {
                analyzer.havePrevious = false;
                continue;
            }
            analyzeFrame(analyzer, frame);
        }
        analyzer.nextFrame = end;
    }
}

// The exe's in-memory headers are only read here, during initialization.
bool findGameImage() {
    const uintptr_t base = session.gameBase;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    gameImage.base = base;
    gameImage.end = base + nt->OptionalHeader.SizeOfImage;
    gameImage.preferredBase = session.gamePreferredBase ? session.gamePreferredBase : base;
    // The largest executable section is the game's code (the Steam build also has a small loader section).
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    DWORD largest = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& section = sections[i];
        const DWORD size = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || size <= largest) continue;
        largest = size;
        gameImage.textStart = base + section.VirtualAddress;
        gameImage.textEnd = gameImage.textStart + size;
    }
    return largest != 0;
}

}  // namespace

namespace debugger::jank {

double ticksPerMs() {
    return frequency.QuadPart / 1000.0;
}

bool prepareMonitoring() {
    QueryPerformanceFrequency(&frequency);
    mainThreadKnown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return mainThreadKnown && findGameImage();
}

bool startMonitoring(const Settings& chosen) {
    settings = chosen;
    sampleTimer = createSampleTimer();
    if (!sampleTimer) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    prepareJankLog(settings, timerKind, now.QuadPart);

    HANDLE sampler = CreateThread(nullptr, 0, sampleMainThread, nullptr, 0, nullptr);
    if (!sampler) return false;
    // Above the game's threads so samples keep coming while the game is busy; it sleeps almost all the time.
    SetThreadPriority(sampler, THREAD_PRIORITY_HIGHEST);
    CloseHandle(sampler);
    HANDLE analyzer = CreateThread(nullptr, 0, analyzeFrames, nullptr, 0, nullptr);
    if (!analyzer) return false;
    SetThreadPriority(analyzer, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(analyzer);
    return true;
}

// Runs on the main thread once per presented frame: two cheap system calls and a push, no allocation or locks.
void framePresented(int64_t callStart, const GameState& state) {
    static bool attached;
    if (!attached) {
        attached = true;
        attachMainThread();
    }
    FrameRecord frame;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    frame.callStart = callStart;
    frame.presented = now.QuadPart;
    ULONG64 cycles = 0;
    frame.cpuCycles = QueryThreadCycleTime(GetCurrentThread(), &cycles) ? cycles : 0;
    PROCESS_MEMORY_COUNTERS memory = {};
    memory.cb = sizeof(memory);
    frame.pageFaults = GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)) ? memory.PageFaultCount : 0;
    frame.state = state;
    frames.push(frame);
}

}  // namespace debugger::jank
