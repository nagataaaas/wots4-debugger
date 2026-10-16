// voice_trace: records each character voice the game tries to play, and the check that refused it if it did not
// play. Writes logs\voice_*.log. Used to find why a voice goes silent.
#pragma once

namespace debugger::voice_trace {

// Reads [voice_trace] from config.ini and hooks the voice functions. Returns false on failure.
bool initialize();

}  // namespace debugger::voice_trace
