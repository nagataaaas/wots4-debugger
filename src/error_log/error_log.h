// error_log: the feature that records exceptions. Writes an error log and a minidump to logs\ in the game folder.
#pragma once

namespace debugger::error_log {

// Reads [error_log] from config.ini and starts watching for exceptions. Returns false on failure.
bool initialize();

}  // namespace debugger::error_log
