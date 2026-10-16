// Declarations shared inside jank.
// Three threads take part:
// - the game's main thread presents frames and pushes one FrameRecord per presented frame (frame_hook.cpp)
// - the sampler thread suspends the main thread every few hundred microseconds and pushes one Sample (monitor.cpp)
// - the analyzer thread reads both, finds slow frames and writes the jank log (monitor.cpp, report.cpp)
// Each ring has a single producer, so pushing never waits.
#pragma once
#include "../core/core.h"
#include <atomic>
#include <cstdint>

namespace debugger::jank {

// [jank] in config.ini.
struct Settings {
    unsigned thresholdMs = 0;          // 0: automatic (from the median of recent frames)
    unsigned sampleIntervalUs = 1000;  // Interval between samples of the main thread
    unsigned maxReportsPerMinute = 20;
    unsigned summarySeconds = 60;      // 0: no summary lines
};

// What the game was doing, read on the main thread when a frame is presented.
struct GameState {
    uint32_t stateFunction;  // Default VA of the game task's current state function. 0 if there is none
    uint8_t scriptRunning;   // 1 while an event script runs
    int8_t scriptKind;       // Folder of the last script (0 cutin, 1 scenario, ...). -1 if none
    uint8_t mapLoading;      // 1 while LoadWaitMap loads a map
    uint8_t gameTimeRunning; // 0 while menus and choices stop the game time
};

struct FrameRecord {
    int64_t callStart;     // QPC when the Frame call that presented this frame began
    int64_t presented;     // QPC right after the frame was presented
    uint64_t cpuCycles;    // CPU cycles the main thread has used so far
    uint32_t pageFaults;   // Page faults of the process so far
    GameState state;
};

// Return addresses of the main loop's own calls (default VA), one per nesting level.
// The scan keeps the outermost value of each level: a stale value can only be left in that level's
// own return slot, which is inside the live stack only while that call is running.
enum MarkerLevel { runLevel, frameLevel, updateLevel, markerLevelCount };

constexpr int maxCallSites = 24;  // Innermost return addresses into game code kept per sample
constexpr int stackTopWords = 48; // Words from esp kept for naming the callers outside game code

struct Sample {
    int64_t time;   // QPC when the main thread was suspended
    uint32_t eip;   // Actual address the main thread was executing
    uint32_t markers[markerLevelCount];   // Default VA, 0 if the level was not on the stack
    uint32_t callSiteCount;
    uint32_t callSites[maxCallSites];     // Default VAs of return addresses into game code, innermost first
    uint32_t stackTop[stackTopWords];     // Raw words from esp up (fewer if the stack ends earlier, rest 0)
};

// Single producer, single consumer. The consumer copies items and then discards those the producer
// may have overwritten meanwhile, so neither side waits.
template <typename T, uint32_t capacity>
class Ring {
    static_assert((capacity & (capacity - 1)) == 0, "capacity must be a power of 2");

public:
    void push(const T& item) {
        const uint32_t index = written.load(std::memory_order_relaxed);
        items[index & (capacity - 1)] = item;
        written.store(index + 1, std::memory_order_release);
    }
    uint32_t count() const { return written.load(std::memory_order_acquire); }
    // Item number. May be torn if the producer is overwriting it; copy() tells which items stayed intact.
    const T& at(uint32_t number) const { return items[number & (capacity - 1)]; }
    // Copies the items numbered [first, end) that are still intact. Returns the first number copied.
    uint32_t copy(uint32_t first, uint32_t end, T* out) const;
    static constexpr uint32_t size() { return capacity; }

private:
    T items[capacity];
    std::atomic<uint32_t> written{0};
};

template <typename T, uint32_t capacity>
uint32_t Ring<T, capacity>::copy(uint32_t first, uint32_t end, T* out) const {
    // Keep clear of the slots the producer is about to write while copying.
    constexpr uint32_t margin = capacity / 16;
    if (end - first > capacity - margin) first = end - (capacity - margin);
    for (uint32_t number = first; number != end; ++number) out[number - first] = items[number & (capacity - 1)];
    const uint32_t now = written.load(std::memory_order_acquire);
    const uint32_t oldestIntact = now > capacity ? now - capacity : 0;
    return first < oldestIntact ? oldestIntact : first;
}

// ---------------------------------------------------------------- analysis.cpp (no Windows calls; tested alone)

// Which part of the main loop a sample was in.
enum class Step : uint8_t {
    limiterWait,       // Waiting for the next frame (0x686bd0)
    windowMessages,    // PeekMessage loop between frames
    beginFrame,
    updateInput,
    updateFrameHooks,  // 0x50e870 and the ModLoader's onFrame callbacks
    updateGameState,   // The game task's state function: the game's own update
    updateOther,
    render,
    endFrame,
    present,
    taskSwitch,        // Switching to the next game task (title, map, ...)
    frameOther,        // Inside the Frame call but between its steps
    outsideMainLoop,   // Not under the main loop, or the stack was too deep to reach it
    count
};

const char* stepName(Step step);
// The level of a default VA if it is one of the main loop's return addresses, otherwise -1.
int markerLevel(uint32_t defaultVa);
Step classify(const Sample& sample);

// Rolling view of recent frame times for the automatic threshold.
class JankDetector {
public:
    explicit JankDetector(unsigned fixedThresholdMs) : fixedThresholdMs(fixedThresholdMs) {}
    // Threshold for the next frame in ms. 0 while too few frames are known for the automatic one.
    double threshold() const;
    double median() const { return medianMs; }
    void add(double frameMs);

private:
    static constexpr int window = 120;
    static constexpr int warmUp = 30;
    unsigned fixedThresholdMs;
    double recent[window] = {};
    int recentCount = 0, next = 0;
    double medianMs = 0;
};

// Value at fraction (0..1) of values, reordering them. 0 for no values.
double percentile(double* values, int count, double fraction);

// Counts of the samples of one slow frame.
struct Profile {
    int sampleCount = 0;
    int steps[static_cast<int>(Step::count)] = {};

    // depthSum: the positions from the innermost it was seen at, added up. Among equal samples the one
    // nearer the inner end is the more specific, so it is listed first.
    struct Count { uint32_t key; int samples; uint32_t depthSum; };
    // Hash tables while counting, then the used entries at the front.
    // Much larger than the samples of one frame, so practically nothing is dropped.
    static constexpr int maxEntries = 4096;
    // Return addresses into game code, each counted once per sample that has it on the stack (inclusive).
    // The main loop's own calls are left out; the steps already show them.
    Count callSites[maxEntries];
    int callSiteCount = 0;
    // Where the main thread was executing (actual address)
    Count leaves[maxEntries];
    int leafCount = 0;
    // Stacks: the innermost call sites (up to stackDepth), with the index of the first sample that had them
    static constexpr int stackDepth = 6;
    struct Stack { uint32_t hash; int firstSample; int samples; };
    Stack stacks[maxEntries];
    int stackCount = 0;
};

void buildProfile(const Sample* samples, int count, Profile* profile);
// Sorts by samples, most first.
void sortCounts(Profile::Count* counts, int count);
void sortStacks(Profile::Stack* stacks, int count);

// ---------------------------------------------------------------- monitor.cpp

// The game exe in memory. Set by startMonitoring, read-only afterwards.
struct GameImage {
    uintptr_t base = 0, end = 0;
    uintptr_t textStart = 0, textEnd = 0;  // The code section
    uintptr_t preferredBase = 0;           // The base the default VAs are written against
};
extern GameImage gameImage;

// Default VA of an actual address in the game exe, or 0 if it is outside the exe.
inline uint32_t toDefaultVa(uintptr_t address) {
    if (address < gameImage.base || address >= gameImage.end) return 0;
    return static_cast<uint32_t>(address - gameImage.base + gameImage.preferredBase);
}

// Finds the game exe in memory (gameImage) and gets ready for framePresented. Call first.
bool prepareMonitoring();
// Starts the sampler and analyzer threads. They wait until the first frame is presented.
bool startMonitoring(const Settings& settings);
// Called on the main thread right after a frame is presented. callStart is the QPC when that Frame call began.
void framePresented(int64_t callStart, const GameState& state);
// QPC ticks per millisecond.
double ticksPerMs();

// ---------------------------------------------------------------- frame_hook.cpp

// Checks that this is the supported game exe and starts timing its frames. Returns false with a reason in why.
bool installFrameHook(const char** why);
// Points the Frame slot back at the game's function and releases the reservation.
void removeFrameHook();

// ---------------------------------------------------------------- report.cpp

struct JankReport {
    const FrameRecord* previous;  // The frame before the slow one
    const FrameRecord* slow;
    uint64_t frameNumber;
    double frameMs, thresholdMs, medianMs;
    double cpuMs;                  // < 0 if unknown
    const Sample* samples;
    int sampleCount;
    bool samplesCutShort;          // The ring did not hold the whole frame
    unsigned suppressedBefore;     // Slow frames since the last report that were only counted
};

struct Summary {
    double seconds;
    uint64_t frames;
    double p50, p95, p99, maxMs;
    unsigned janks, reported;
};

// Remembers what the log header says. The file is created on the first write.
void prepareJankLog(const Settings& settings, const char* timerKind, int64_t startTicks);
void writeJankReport(const JankReport& report);
void writeSummary(const Summary& summary);

}  // namespace debugger::jank
