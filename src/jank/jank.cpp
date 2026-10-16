// jank: settings and start-up. The work is split into frame_hook.cpp (the game), monitor.cpp (threads),
// analysis.cpp (counting) and report.cpp (the log).
#include "jank.h"
#include "internal.h"
#include <cstdio>

namespace {

using namespace debugger;
using namespace debugger::jank;

constexpr unsigned maxThresholdMs = 10000;
constexpr unsigned minSampleIntervalUs = 250, maxSampleIntervalUs = 10000;
constexpr unsigned maxReportsLimit = 600;
constexpr unsigned maxSummarySeconds = 3600;

Settings readSettings() {
    Settings settings;
    settings.thresholdMs = readConfigNumber(L"jank", L"threshold_ms", settings.thresholdMs, 0, maxThresholdMs);
    settings.sampleIntervalUs = readConfigNumber(L"jank", L"sample_interval_us", settings.sampleIntervalUs,
        minSampleIntervalUs, maxSampleIntervalUs);
    settings.maxReportsPerMinute = readConfigNumber(L"jank", L"max_reports_per_minute", settings.maxReportsPerMinute,
        0, maxReportsLimit);
    settings.summarySeconds = readConfigNumber(L"jank", L"summary_seconds", settings.summarySeconds, 0,
        maxSummarySeconds);
    return settings;
}

}  // namespace

namespace debugger::jank {

bool initialize() {
    const Settings settings = readSettings();
    char message[256];
    if (!prepareMonitoring()) {
        hostLog("jank: could not read the game exe in memory; slow frames are not recorded");
        return false;
    }
    const char* why = nullptr;
    if (!installFrameHook(&why)) {
        sprintf_s(message, "jank: %s; slow frames are not recorded", why);
        hostLog(message);
        return false;
    }
    if (!startMonitoring(settings)) {
        removeFrameHook();
        hostLog("jank: could not start the sampling threads; the Frame slot was restored and slow frames are not recorded");
        return false;
    }
    char threshold[32] = "auto";
    if (settings.thresholdMs) sprintf_s(threshold, "%u ms", settings.thresholdMs);
    sprintf_s(message, "jank: timing frames (threshold=%s, sample_interval_us=%u); writing slow frames to logs\\jank_*.log",
        threshold, settings.sampleIntervalUs);
    hostLog(message);
    return true;
}

}  // namespace debugger::jank
