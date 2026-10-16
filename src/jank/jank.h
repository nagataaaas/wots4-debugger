// jank: finds what made a frame slow. Times every presented frame, samples the main thread's stack, and
// writes each slow frame's breakdown (main loop step, running calls, executing code) to logs\jank_*.log.
#pragma once

namespace debugger::jank {

// Reads [jank] from config.ini and starts timing frames. Returns false on failure.
bool initialize();

}  // namespace debugger::jank
