// Turns samples into counts. Pure computation, so tests can feed it synthetic samples.
#include "internal.h"
#include <algorithm>

namespace debugger::jank {
namespace {

struct StepMarker {
    uint32_t returnAddress;  // Default VA right after the call
    MarkerLevel level;
    Step step;
};

// The supported Steam build's main loop:
//   Run 0x4dd1b0: loops PumpMessages (vtable +0x20) and Frame (vtable +0x24)
//   Frame 0x4dd4f0: limiter, BeginFrame, update 0x4dd5b0, render, EndFrame, DisplayFrame, task switch
//   update 0x4dd5b0: input, 0x50e870, then the task's state function through 0x4ef1a0
const StepMarker stepMarkers[] = {
    {0x4dd1dd, runLevel, Step::windowMessages},
    {0x4dd1f7, runLevel, Step::windowMessages},
    {0x4dd1ea, runLevel, Step::frameOther},
    {0x4dd503, frameLevel, Step::limiterWait},
    {0x4dd52a, frameLevel, Step::beginFrame},
    {0x4dd539, frameLevel, Step::updateOther},
    {0x4dd546, frameLevel, Step::render},
    {0x4dd551, frameLevel, Step::endFrame},
    {0x4dd572, frameLevel, Step::present},
    {0x4dd593, frameLevel, Step::taskSwitch},
    {0x4dd59a, frameLevel, Step::taskSwitch},
    {0x4dd5c8, updateLevel, Step::updateInput},
    {0x4dd5e2, updateLevel, Step::updateInput},
    {0x4dd5fb, updateLevel, Step::updateInput},
    {0x4dd6c2, updateLevel, Step::updateFrameHooks},
    {0x4dd6cc, updateLevel, Step::updateGameState},
};

const StepMarker* findMarker(uint32_t defaultVa) {
    for (const StepMarker& marker : stepMarkers) {
        if (marker.returnAddress == defaultVa) return &marker;
    }
    return nullptr;
}

// Open addressing over the Count arrays of Profile, then compacted to the front.
constexpr uint32_t tableSize = Profile::maxEntries;
static_assert((tableSize & (tableSize - 1)) == 0, "table size must be a power of 2");

uint32_t mix(uint32_t key) {
    key ^= key >> 16;
    key *= 0x7feb352d;
    key ^= key >> 15;
    return key;
}

// Adds one sample to key. Keys that no longer fit are dropped (the table is far larger than the samples of a frame).
void countKey(Profile::Count* table, uint32_t key, uint32_t depth) {
    if (!key) return;
    for (uint32_t probe = 0, slot = mix(key); probe < tableSize; ++probe, ++slot) {
        Profile::Count& entry = table[slot & (tableSize - 1)];
        if (entry.key == key) {
            ++entry.samples;
            entry.depthSum += depth;
            return;
        }
        if (!entry.key) {
            entry = {key, 1, depth};
            return;
        }
    }
}

int compact(Profile::Count* table) {
    int used = 0;
    for (uint32_t slot = 0; slot < tableSize; ++slot) {
        if (table[slot].key) table[used++] = table[slot];
    }
    return used;
}

uint32_t stackHash(const Sample& sample) {
    uint32_t hash = 2166136261u;  // FNV-1a
    const uint32_t depth = std::min<uint32_t>(sample.callSiteCount, Profile::stackDepth);
    for (uint32_t i = 0; i < depth; ++i) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (sample.callSites[i] >> shift) & 0xff;
            hash *= 16777619u;
        }
    }
    return hash | 1;  // 0 marks an empty slot
}

void countStack(Profile* profile, const Sample& sample, int index) {
    const uint32_t hash = stackHash(sample);
    for (uint32_t probe = 0, slot = mix(hash); probe < tableSize; ++probe, ++slot) {
        Profile::Stack& entry = profile->stacks[slot & (tableSize - 1)];
        if (entry.hash == hash) {
            ++entry.samples;
            return;
        }
        if (!entry.hash) {
            entry = {hash, index, 1};
            return;
        }
    }
}

bool alreadyCounted(const Sample& sample, uint32_t upTo) {
    for (uint32_t i = 0; i < upTo; ++i) {
        if (sample.callSites[i] == sample.callSites[upTo]) return true;
    }
    return false;
}

}  // namespace

const char* stepName(Step step) {
    switch (step) {
    case Step::limiterWait: return "waiting for the next frame (frame limiter)";
    case Step::windowMessages: return "window messages (between frames)";
    case Step::beginFrame: return "begin frame (renderer)";
    case Step::updateInput: return "update: input";
    case Step::updateFrameHooks: return "update: 0x50e870 and Mods' onFrame";
    case Step::updateGameState: return "update: game task state (game logic)";
    case Step::updateOther: return "update: other";
    case Step::render: return "render (scene and 2D)";
    case Step::endFrame: return "end frame (renderer)";
    case Step::present: return "present (display the frame)";
    case Step::taskSwitch: return "game task switch (title, map, ...)";
    case Step::frameOther: return "frame: between steps";
    case Step::outsideMainLoop: return "outside the main loop (or stack too deep)";
    default: return "?";
    }
}

int markerLevel(uint32_t defaultVa) {
    const StepMarker* marker = findMarker(defaultVa);
    return marker ? marker->level : -1;
}

// Walks the levels from the outside in, because a stale return address of an inner level can be left in
// the stack while the outer level is in a different call.
Step classify(const Sample& sample) {
    const StepMarker* run = findMarker(sample.markers[runLevel]);
    if (!run) return Step::outsideMainLoop;
    if (run->step == Step::windowMessages) return run->step;
    const StepMarker* frame = findMarker(sample.markers[frameLevel]);
    if (!frame) return Step::frameOther;
    if (frame->step != Step::updateOther) return frame->step;
    const StepMarker* update = findMarker(sample.markers[updateLevel]);
    return update ? update->step : Step::updateOther;
}

double JankDetector::threshold() const {
    if (fixedThresholdMs) return fixedThresholdMs;
    if (recentCount < warmUp) return 0;
    // 1.5 times the median catches a skipped frame at 30 FPS (50 ms). At high frame rates the margin of
    // 5 ms keeps ordinary timer jitter from counting.
    return std::max(medianMs * 1.5, medianMs + 5.0);
}

void JankDetector::add(double frameMs) {
    recent[next] = frameMs;
    next = (next + 1) % window;
    if (recentCount < window) ++recentCount;
    double sorted[window];
    std::copy(recent, recent + recentCount, sorted);
    medianMs = percentile(sorted, recentCount, 0.5);
}

double percentile(double* values, int count, double fraction) {
    if (count <= 0) return 0;
    int index = static_cast<int>(fraction * (count - 1) + 0.5);
    index = std::clamp(index, 0, count - 1);
    std::nth_element(values, values + index, values + count);
    return values[index];
}

void buildProfile(const Sample* samples, int count, Profile* profile) {
    *profile = Profile{};
    profile->sampleCount = count;
    for (int index = 0; index < count; ++index) {
        const Sample& sample = samples[index];
        ++profile->steps[static_cast<int>(classify(sample))];
        countKey(profile->leaves, sample.eip, 0);
        for (uint32_t i = 0; i < sample.callSiteCount; ++i) {
            if (findMarker(sample.callSites[i]) || alreadyCounted(sample, i)) continue;
            countKey(profile->callSites, sample.callSites[i], i);
        }
        countStack(profile, sample, index);
    }
    profile->callSiteCount = compact(profile->callSites);
    profile->leafCount = compact(profile->leaves);
    int stacks = 0;
    for (uint32_t slot = 0; slot < tableSize; ++slot) {
        if (profile->stacks[slot].hash) profile->stacks[stacks++] = profile->stacks[slot];
    }
    profile->stackCount = stacks;
    sortCounts(profile->callSites, profile->callSiteCount);
    sortCounts(profile->leaves, profile->leafCount);
    sortStacks(profile->stacks, profile->stackCount);
}

void sortCounts(Profile::Count* counts, int count) {
    std::sort(counts, counts + count, [](const Profile::Count& a, const Profile::Count& b) {
        if (a.samples != b.samples) return a.samples > b.samples;
        // Average depth a.depthSum / a.samples against b's, without dividing
        const uint64_t depthA = uint64_t(a.depthSum) * b.samples, depthB = uint64_t(b.depthSum) * a.samples;
        return depthA != depthB ? depthA < depthB : a.key < b.key;
    });
}

void sortStacks(Profile::Stack* stacks, int count) {
    std::sort(stacks, stacks + count, [](const Profile::Stack& a, const Profile::Stack& b) {
        return a.samples != b.samples ? a.samples > b.samples : a.firstSample < b.firstSample;
    });
}

}  // namespace debugger::jank
