// voice_trace: decides why a character voice did or did not play, from what the hooks saw. It does not touch the
// game, so tests feed it synthetic values.
#pragma once
#include <cstdint>

namespace debugger::voice_trace {

// 0x5a2d30 refuses a streamed voice while this many streams are in use (0x5a2d75: cmp eax, 5 / jge).
constexpr unsigned streamLimit = 5;
constexpr unsigned soundSlotCount = 24;  // Slot table 0x9f02c0

// One call of the character voice function 0x49ab50 (or of 0x5a2d30 directly from other game code).
struct VoiceAttempt {
    // Seen by 0x49ab50 before it plays anything.
    bool characterVoiceless = false;  // [data + 0x300] bit 0x800000, set by SetAction(<character>, 13) (0x54dbfe)
    bool soundManagerMissing = false;
    bool flatVoicePath = false;       // [sound manager + 0x2c]: 0x49ab50 calls 0x5a2e00, which keeps no handle
    bool forced = false;              // 3rd argument: skip the distance check from the player (0x49ac45)
    // 0x5a2d30, the call that looks up the voice and takes a sound slot.
    bool reachedVoiceCall = false;
    int32_t handle = -1;              // What 0x5a2d30 returned (negative: not played)
    // Read right before 0x5a2d30 ran.
    bool voicesMuted = false;         // [0x9ecfea], written by Sound.SetNoPlay(<on>, 0) and timetable type 94
    unsigned streamsInUse = 0;
    unsigned slotsInUse = 0;
    // The voice table entry, looked up only when the voice did not play.
    bool voiceTableMissing = false;
    bool recordFound = false;
    bool recordStreamed = false;      // Record flags bit 1
};

enum class Outcome {
    played,
    playedUntraced,      // Went through 0x5a2e00, which this trace does not follow
    characterVoiceless,
    noSoundManager,
    tooFar,
    stoppedEarly,        // Returned before 0x5a2d30 for a reason this trace does not know
    voicesMuted,
    noVoiceTable,
    noVoiceRecord,
    streamLimit,
    playbackFailed,      // Passed every check of 0x5a2d30, but 0x5a2900 did not take a slot
};

// Follows the order of the checks in 0x49ab50 and 0x5a2d30, so the first one that refused is named.
inline Outcome diagnose(const VoiceAttempt& attempt) {
    if (attempt.characterVoiceless) return Outcome::characterVoiceless;
    if (attempt.soundManagerMissing) return Outcome::noSoundManager;
    if (attempt.flatVoicePath) return Outcome::playedUntraced;
    if (!attempt.reachedVoiceCall) return attempt.forced ? Outcome::stoppedEarly : Outcome::tooFar;
    if (attempt.handle >= 0) return Outcome::played;
    if (attempt.voicesMuted) return Outcome::voicesMuted;
    if (attempt.voiceTableMissing) return Outcome::noVoiceTable;
    if (!attempt.recordFound) return Outcome::noVoiceRecord;
    if (attempt.recordStreamed && attempt.streamsInUse >= streamLimit) return Outcome::streamLimit;
    return Outcome::playbackFailed;
}

inline bool wasPlayed(Outcome outcome) {
    return outcome == Outcome::played || outcome == Outcome::playedUntraced;
}

inline const char* describe(Outcome outcome) {
    switch (outcome) {
    case Outcome::played: return "played";
    case Outcome::playedUntraced:
        return "sent to the flat voice path (sound manager +0x2c is set; 0x5a2e00 keeps no handle, so the result is unknown)";
    case Outcome::characterVoiceless:
        return "not played: the character is marked voiceless (+0x300 bit 0x800000, set by SetAction 13)";
    case Outcome::noSoundManager: return "not played: there is no sound manager";
    case Outcome::tooFar: return "not played: the character is too far from the player";
    case Outcome::stoppedEarly: return "not played: 0x49ab50 returned before the voice call for an unknown reason";
    case Outcome::voicesMuted: return "not played: character voices are muted (0x9ecfea, Sound.SetNoPlay)";
    case Outcome::noVoiceTable: return "not played: the voice table is not loaded";
    case Outcome::noVoiceRecord: return "not played: the voice set has no voice with this number";
    case Outcome::streamLimit: return "not played: the voice is streamed and 5 streams are already in use";
    case Outcome::playbackFailed: return "not played: no sound slot was taken (0x5a2900)";
    }
    return "unknown";
}

}  // namespace debugger::voice_trace
