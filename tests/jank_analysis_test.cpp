// Checks jank's counting with synthetic samples: which main loop step a stack is in, the automatic
// threshold, percentiles, and the counts of a profile. Runs without the game or any threads.
#include "jank/internal.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using namespace debugger::jank;

int passed, failed;

void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ++(ok ? passed : failed);
}

bool closeTo(double value, double expected) {
    return std::fabs(value - expected) < 1e-6;
}

// The main loop's return addresses (default VA), as frame_hook.cpp's signatures pin them.
constexpr uint32_t runCallsPump = 0x4dd1dd, runCallsFrame = 0x4dd1ea;
constexpr uint32_t frameCallsLimiter = 0x4dd503, frameCallsUpdate = 0x4dd539, frameCallsRender = 0x4dd546;
constexpr uint32_t frameCallsPresent = 0x4dd572, frameCallsTaskSwitch = 0x4dd593;
constexpr uint32_t updateCallsHooks = 0x4dd6c2, updateCallsState = 0x4dd6cc;

Sample stackWith(uint32_t run, uint32_t frame, uint32_t update) {
    Sample sample = {};
    sample.markers[runLevel] = run;
    sample.markers[frameLevel] = frame;
    sample.markers[updateLevel] = update;
    return sample;
}

void testMarkerLevels() {
    check(markerLevel(runCallsFrame) == runLevel, "the return address of Frame in Run is the run level");
    check(markerLevel(frameCallsRender) == frameLevel, "the return address of render in Frame is the frame level");
    check(markerLevel(updateCallsHooks) == updateLevel, "the return address of 0x50e870 in update is the update level");
    check(markerLevel(0x401000) == -1, "other game addresses are not markers");
}

void testClassify() {
    check(classify(stackWith(0, 0, 0)) == Step::outsideMainLoop, "a stack without Run is outside the main loop");
    check(classify(stackWith(runCallsPump, frameCallsPresent, 0)) == Step::windowMessages,
        "in PumpMessages, a stale present return address below it is ignored");
    check(classify(stackWith(runCallsFrame, frameCallsRender, updateCallsState)) == Step::render,
        "in render, a stale update return address below it is ignored");
    check(classify(stackWith(runCallsFrame, frameCallsUpdate, updateCallsState)) == Step::updateGameState,
        "in update, the state function call is the game logic");
    check(classify(stackWith(runCallsFrame, frameCallsUpdate, updateCallsHooks)) == Step::updateFrameHooks,
        "in update, 0x50e870 is the frame hooks step");
    check(classify(stackWith(runCallsFrame, frameCallsUpdate, 0)) == Step::updateOther,
        "update's own code is update: other");
    check(classify(stackWith(runCallsFrame, 0, 0)) == Step::frameOther, "Frame's own code is frame: between steps");
    check(classify(stackWith(runCallsFrame, frameCallsLimiter, 0)) == Step::limiterWait, "the limiter is the wait");
    check(classify(stackWith(runCallsFrame, frameCallsTaskSwitch, 0)) == Step::taskSwitch, "0x50f350 is a task switch");
    check(classify(stackWith(0, frameCallsRender, 0)) == Step::outsideMainLoop,
        "a frame level marker without Run (stack not reached) is not trusted");
}

void testDetector() {
    JankDetector automatic(0);
    for (int i = 0; i < 29; ++i) automatic.add(1000.0 / 30);
    check(automatic.threshold() == 0, "no automatic threshold before 30 frames");
    for (int i = 0; i < 200; ++i) automatic.add(1000.0 / 30);
    check(closeTo(automatic.threshold(), 50.0), "at 30 FPS the automatic threshold is 50 ms (1.5 x 33.3)");
    for (int i = 0; i < 120; ++i) automatic.add(1000.0 / 120);
    check(closeTo(automatic.threshold(), 1000.0 / 120 + 5), "at 120 FPS the threshold is the median + 5 ms");
    automatic.add(500);
    check(closeTo(automatic.median(), 1000.0 / 120), "one slow frame does not move the median");

    JankDetector fixed(40);
    check(fixed.threshold() == 40, "a fixed threshold applies from the first frame");
}

void testPercentile() {
    double values[100];
    for (int i = 0; i < 100; ++i) values[i] = 100 - i;  // Unsorted on purpose
    check(percentile(values, 100, 0.0) == 1, "p0 is the smallest");
    check(percentile(values, 100, 1.0) == 100, "p100 is the largest");
    check(percentile(values, 100, 0.5) == 51, "p50 of 1..100 is 51 (rounded index)");
    check(percentile(values, 0, 0.5) == 0, "no values give 0");
}

void testProfile() {
    constexpr uint32_t siteA = 0x512345, siteB = 0x598765, siteC = 0x5a1234;
    constexpr uint32_t leafX = 0x10001000, leafY = 0x10002000;
    static Sample samples[10];
    for (int i = 0; i < 10; ++i) {
        Sample& sample = samples[i];
        sample = stackWith(runCallsFrame, i < 6 ? frameCallsUpdate : frameCallsRender, i < 6 ? updateCallsState : 0);
        sample.time = i;
        sample.eip = i < 7 ? leafX : leafY;
        if (i < 6) {
            // Recursion: A is on the stack twice. The main loop's own call is shown as a step instead.
            const uint32_t sites[] = {siteA, siteB, siteA, frameCallsUpdate};
            memcpy(sample.callSites, sites, sizeof(sites));
            sample.callSiteCount = 4;
        } else {
            sample.callSites[0] = siteC;
            sample.callSiteCount = 1;
        }
    }
    static Profile profile;
    buildProfile(samples, 10, &profile);
    check(profile.sampleCount == 10, "the profile counts every sample");
    check(profile.steps[static_cast<int>(Step::updateGameState)] == 6 && profile.steps[static_cast<int>(Step::render)] == 4,
        "steps are counted per sample");
    check(profile.callSiteCount == 3, "three distinct call sites; the main loop's own call is left out");
    check(profile.callSites[0].key == siteA && profile.callSites[0].samples == 6,
        "a call site on the stack twice counts once per sample");
    check(profile.callSites[1].key == siteB && profile.callSites[1].samples == 6,
        "among equal samples the call site nearer the inner end comes first");
    check(profile.callSites[2].key == siteC && profile.callSites[2].samples == 4, "call sites are sorted by samples");
    check(profile.leafCount == 2 && profile.leaves[0].key == leafX && profile.leaves[0].samples == 7,
        "leaves are counted by address, most first");
    check(profile.stackCount == 2 && profile.stacks[0].samples == 6 && profile.stacks[0].firstSample == 0 &&
        profile.stacks[1].samples == 4 && profile.stacks[1].firstSample == 6,
        "stacks are grouped by their call sites and keep their first sample");
}

}  // namespace

int main() {
    testMarkerLevels();
    testClassify();
    testDetector();
    testPercentile();
    testProfile();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
