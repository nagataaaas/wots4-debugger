// voice_trace's diagnosis with synthetic attempts: the first check that refused a voice is the one named, in the
// order of 0x49ab50 and 0x5a2d30.
#include "voice_trace/diagnosis.h"
#include <cstdio>
#include <cstring>

namespace {

using namespace debugger::voice_trace;

int failures, passes;

void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
        ++passes;
    else
        ++failures;
}

// A voice that reached 0x5a2d30 and was refused there.
VoiceAttempt refusedInVoiceCall() {
    VoiceAttempt attempt;
    attempt.forced = true;
    attempt.reachedVoiceCall = true;
    attempt.handle = -1;
    attempt.recordFound = true;
    return attempt;
}

}  // namespace

int main() {
    VoiceAttempt played = refusedInVoiceCall();
    played.handle = 12;
    played.voicesMuted = true;  // A handle wins: the voice played whatever was read before
    check(diagnose(played) == Outcome::played, "a handle from 0x5a2d30 means played");

    VoiceAttempt voiceless = played;
    voiceless.characterVoiceless = true;
    voiceless.soundManagerMissing = true;
    check(diagnose(voiceless) == Outcome::characterVoiceless, "the voiceless flag is checked first (0x49ab7e)");

    VoiceAttempt noManager;
    noManager.soundManagerMissing = true;
    noManager.flatVoicePath = true;
    check(diagnose(noManager) == Outcome::noSoundManager, "a missing sound manager comes before the flat path");

    VoiceAttempt flat;
    flat.flatVoicePath = true;
    check(diagnose(flat) == Outcome::playedUntraced, "the flat path is reported as untraced, not as silent");
    check(wasPlayed(Outcome::playedUntraced), "the flat path counts as played (not a failure line)");

    VoiceAttempt far;
    check(diagnose(far) == Outcome::tooFar, "not forced and no voice call means too far from the player");
    VoiceAttempt early;
    early.forced = true;
    check(diagnose(early) == Outcome::stoppedEarly, "forced but no voice call is an unknown early return");

    VoiceAttempt muted = refusedInVoiceCall();
    muted.voicesMuted = true;
    muted.recordFound = false;
    check(diagnose(muted) == Outcome::voicesMuted, "SetNoPlay is checked before the voice table (0x5a2d36)");

    VoiceAttempt noTable = refusedInVoiceCall();
    noTable.voiceTableMissing = true;
    noTable.recordFound = false;
    check(diagnose(noTable) == Outcome::noVoiceTable, "a missing voice table");

    VoiceAttempt noRecord = refusedInVoiceCall();
    noRecord.recordFound = false;
    noRecord.streamsInUse = streamLimit;
    check(diagnose(noRecord) == Outcome::noVoiceRecord, "a missing record comes before the stream limit");

    VoiceAttempt streams = refusedInVoiceCall();
    streams.recordStreamed = true;
    streams.streamsInUse = streamLimit;
    check(diagnose(streams) == Outcome::streamLimit, "a streamed voice with 5 streams in use hits the limit");

    VoiceAttempt belowLimit = streams;
    belowLimit.streamsInUse = streamLimit - 1;
    check(diagnose(belowLimit) == Outcome::playbackFailed, "4 streams is below the limit (jge 5)");

    VoiceAttempt sample = streams;
    sample.recordStreamed = false;
    check(diagnose(sample) == Outcome::playbackFailed, "the stream limit does not apply to a sample");

    check(strstr(describe(Outcome::characterVoiceless), "SetAction 13") != nullptr,
        "the voiceless reason names where the flag comes from");
    bool allDescribed = true;
    for (int outcome = static_cast<int>(Outcome::played); outcome <= static_cast<int>(Outcome::playbackFailed);
         ++outcome)
        allDescribed &= strcmp(describe(static_cast<Outcome>(outcome)), "unknown") != 0;
    check(allDescribed, "every outcome has a description");

    printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
