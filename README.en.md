# debugger

[日本語](README.md) | [Changelog](CHANGELOG.en.md)

A debugging tool for making Mods for Way of the Samurai 4. It is a Mod that bundles several features, and you can choose whether to use each feature in `config.ini`.

| Feature | What it does | Default |
| --- | --- | --- |
| [Error log](#error-log-error_log) (`error_log`) | Records exceptions, and also writes a minidump when the game crashes | On |
| [Hitch log](#hitch-log-jank) (`jank`) | Finds frames that took longer than usual, and writes what the game was doing during them | Off |
| [Voice log](#voice-log-voice_trace) (`voice_trace`) | Each time a character voice is played, writes whether it played and, if not, why | Off |

Whichever features you use, other Mods can leave clues with [notes](#leaving-clues-from-other-mods-notes).

## Error log (error_log)

When an exception (such as an access violation) occurs in the game, it is recorded in `logs/` in the game folder (the folder containing `WayOfTheSamurai4.exe`). If no exception occurs between starting and quitting the game, neither the file nor the `logs/` folder is created.

```
logs/error_<date-time>_<process ID>.log   ... error log (text)
logs/crash_<date-time>_<process ID>.dmp   ... minidump (only when the game crashes)
```

An error log and a minidump from the same run have the same date-time and process ID.

The error log has two kinds of records.

| Heading | When it is written |
| --- | --- |
| `==== Exception #n (first chance …)` | When the exception occurs. This includes exceptions the game handles itself and keeps running after. For an exception at the same address, from the second time on, a single line `---- Exception … again at the same address …: occurrence n` is written only on the 2nd, 4th, 8th, … time |
| `==== Fatal exception` | When nothing handles the exception and the game crashes. When looking for the cause, check this section first |

Each record contains:

- The exception code and address. Inside the game's exe, it also writes the address converted to the exe's default base address (`00400000`), as in `WayOfTheSamurai4.exe+1234 .text (default VA 00401234)`
- For an access violation, the address it tried to read or write, and the state of that memory (such as `free (released, or never allocated)` if it was released)
- The bytes of the instruction that raised the exception
- All registers and where each one points (the memory state and the first 32 bytes. If the value of the first 4 bytes points to read-only data, it may be a virtual function table, so `(possibly a vtable)` is written)
- The top of the stack, and values on the stack that look like return addresses (ones that point right after a call). They are picked up by scanning the stack, so unrelated values are mixed in

For a fatal exception, it also writes:

- Notes (clues left by other Mods; see below)
- The list of loaded modules
- The result of the minidump

The top of the error log has the file size and SHA-256 of the game's exe. You can use them to check which version of the exe the record is from.

- It does not swallow exceptions or modify the game's code or saves. It does not send anything over the network.
- The error log and the minidump contain the paths of the exe and DLLs. The minidump also contains part of the game's memory (all of it with `full`). Be careful when sharing them.

## Hitch log (jank)

A feature for finding the cause when the game's picture stops for a moment (a frame is shown late). Use it when optimizing the game. It is off by default. Set `enabled=1` in `[jank]` of `config.ini` to use it.

It measures the time of each frame of the game (from showing one picture to showing the next), and when a frame took longer than usual, records it in `logs/` in the game folder.

```
logs/jank_<date-time>_<process ID>.log
```

To do this, it stops the thread doing the game's work (the main thread) for a very short time (a few microseconds) to see what it is doing, every millisecond (sampling). It counts the samples taken during a slow frame and shows where the time went as shares. This makes the game a little heavier, so do not use it while just playing.

For each slow frame, it writes a `==== Slow frame #<frame number>: <time taken> ms` section.

| Line / heading | Contents |
| --- | --- |
| `Timing` | The time of the work for the frame shown (the `Frame` call), and the time before it (waiting for the next frame, window messages) |
| `Main thread` | How long the main thread ran on a CPU, and the number of page faults of the process. If the CPU time is much shorter than the frame time, it was waiting for something (such as reading a file) |
| `Game` | The game's state (the task's state function, whether a script is running and its kind, whether a map is loading, whether game time is stopped). If it changed during the frame, both before and after |
| `Steps of the main loop` | Which step of the main loop it was in (waiting for the next frame, input, the game's update, rendering, display [Present], task switch, and so on) |
| `Calls in game code that were running` | Calls in the game's code (return addresses) that were on the stack: the share of time spent inside each call. For a direct call, the address of the function called is written too |
| `Where the main thread was executing` | The code that was running. In the game, the function (its start address is estimated from the padding before it, marked with `~`); in a DLL, `<DLL name>!<nearby export name>+<offset>` |
| `Most common stacks` | The most frequent stacks (from the inside out) |

Addresses are converted to the game exe's default base address (`00400000`). Samples are picked up by scanning the stack, so unrelated values (left over from earlier calls) can be mixed in. Treat entries with only a few samples as noise.

In addition, at a fixed interval (60 seconds by default), it writes one line with the number of frames, the FPS and the distribution of frame times (median p50, p95, p99, maximum) for that period. Use it to compare before and after an optimization.

- By default, a frame counts as slow when it takes at least 1.5 times the median of recent frames (at least the median + 5 ms). That is 50 ms at 30 FPS and 25 ms at 60 FPS.
- Up to 20 frames per minute are written in detail. Frames beyond that are only counted, and the count is written in the next section and in the summary line.
- It does not modify the game's code or saves. It only points one entry of the table through which the game calls its frame function at a function that measures the time.

## Voice log (voice_trace)

A feature for finding the cause when a character voice (shopkeepers and talk, battle cries, and so on) does not play. It is off by default. Set `enabled=1` in `[voice_trace]` of `config.ini` to use it.

It writes one line per voice to `logs/` in the game folder.

```
logs/voice_<date-time>_<process ID>.log
```

- Voices that scripts play (shopkeepers and talk) are written whether they played or not.
- Other voices (battle cries and so on) are written only when they did not play (with `log_played_game_voices=1`, also when they played). Up to `max_lines_per_minute` lines per minute; beyond that, only the count is written.
- A line is also written when the setting that keeps character voices from playing is switched.

Each line has the time, the character (the ID that scripts use, the NPC number and the voice set), the voice number and the result. When the voice did not play, it also has the check that stopped it (the character was made voiceless, character voices are set not to play, there is no such voice, all the streams that can play at once are in use, and so on) and the state at that moment (how many streams and sound slots were in use).

```
18:18:04.015 PlayVoiceSE: character ID 7 (NPC 1006), voice set 42, voice 38: not played: the voice is streamed and 5 streams are already in use [caller 0x00568e19, forced=yes; muted=no, streams=5/5, slots in use=5/24, record=streamed]
```

- It does not modify the game's code or saves. It only reads the state and records it at the start of the three functions that play a voice.
- It writes a line to the log each time a voice is played, so leave it off while just playing.

## Settings (config.ini)

`mods/debugger/config.ini` has a section for each feature (`[<feature name>]`). `enabled=1` turns that feature on, and `0` turns it off. If the file or an entry is missing, the "Default" in the table above and the default value of each entry below are used.

```ini
[error_log]
enabled=1
minidump=standard

[jank]
enabled=0
threshold_ms=0
sample_interval_us=1000
max_reports_per_minute=20
summary_seconds=60

[voice_trace]
enabled=0
log_played_game_voices=0
max_lines_per_minute=120
```

`minidump` in `[error_log]` is the detail level of the minidump written on a crash. The default is `standard`.

| minidump | Contents | Approximate size |
| --- | --- | --- |
| `off` | Not written | None |
| `small` | Stacks and registers of all threads, and the module list | A few hundred KB |
| `standard` | `small`, plus global variables, memory that registers and stacks point to, and memory allocation state | A few MB to a few tens of MB |
| `full` | All process memory | Hundreds of MB to several GB |

If you write an invalid value, the default value is used and this is written to `mod_loader.log`.
If the minidump cannot be written with `standard` or `full`, it is written again with `small`. The level actually written is shown in "the result of the minidump" in the error log.
Minidumps can be opened in WinDbg or Visual Studio.

Entries in `[jank]`:

| Entry | Contents | Default |
| --- | --- | --- |
| `threshold_ms` | Record frames that take at least this many milliseconds. `0` means automatic (1.5 times the median of recent frames, at least the median + 5 ms) | `0` |
| `sample_interval_us` | Interval between looks at the main thread (microseconds, 250 to 10000). Shorter gives finer detail but makes the game heavier | `1000` |
| `max_reports_per_minute` | Frames written in detail per minute (0 to 600) | `20` |
| `summary_seconds` | Interval of the frame time summary (seconds, 0 to 3600). `0` means none | `60` |

Entries in `[voice_trace]`:

| Entry | Meaning | Default |
| --- | --- | --- |
| `log_played_game_voices` | `1` also writes voices other than scripts' (battle cries and so on) when they played | `0` |
| `max_lines_per_minute` | Lines per minute about voices other than scripts' (0 to 6000). Script voices are not counted and are always written | `120` |

## Leaving clues from other Mods (notes)

Other Mods can leave short messages (notes) with debugger. debugger keeps the last 64, and writes them to the error log when the game crashes (if the error log is turned off, they are not written anywhere). Use them to find out things like "which scene was in progress when it crashed". If debugger is not installed, nothing happens.

Copy [include/wots4/debugger.h](include/wots4/debugger.h) into your Mod and use it like this.

```cpp
#include <wots4/debugger.h>

extern "C" BOOL WINAPI Wots4ModInitialize(const wots4::ModContext* context) {
    wots4::debugger::connect(context);  // Call once, during initialization
    // ...
}

// From anywhere (any thread). Up to 160 bytes per note
wots4::debugger::note("scene change started");
```

Notes are written to the error log as they are, so write them in English (the error log is kept English only). They appear in the error log like this (inside `[ ]` is the folder name of the Mod that left the note).

```
Notes from other Mods (last 64):
  20:14:36.902 thread 12392 [example] scene change started
```

## Installation

### Automatic installation (recommended)

1. Close the game.
2. Download and run [WayOfTheSamurai4ModInstaller.exe](https://github.com/nagataaaas/wots4-mods/releases/latest/download/WayOfTheSamurai4ModInstaller.exe).
   - If "Windows protected your PC" appears, click "More info" → "Run anyway".
   - When asked whether to allow it to make changes as administrator, click "Yes" (it writes to the game folder).
3. Follow the screen and choose `debugger` in the list of Mods. Move with Up and Down, choose with Space and confirm with Enter.

The ModLoader is installed along with it. The same steps update the Mod, and your settings (`config.ini`) are kept.

<details>
<summary>Manual installation</summary>

> [!WARNING]
> Install [wots4-mods (ModLoader)](https://github.com/nagataaaas/wots4-mods) first.

1. Close the game.
2. Download `wots4-mod-debugger-<version>.zip` from [Releases](../../releases) and extract it.
3. Copy the extracted `mods` folder into the game's installation folder (the folder containing `WayOfTheSamurai4.exe`). If a `mods` folder already exists, merge the contents into it.
4. Confirm that `{game installation folder}/mods/debugger/mod.dll` exists, then start the game.

</details>

## Uninstallation

1. Close the game.
2. Delete the `{game installation folder}/mods/debugger/` folder. The settings in it (`config.ini`) are deleted too.

If you do not need the records, also delete `error_*.log`, `crash_*.dmp`, `jank_*.log` and `voice_*.log` in `{game installation folder}/logs/`.

## Building

Requirements: Visual Studio 2022 or later (Desktop development with C++), CMake 3.25 or later

You need [wots4-mods](https://github.com/nagataaaas/wots4-mods) (the ModLoader and Mod SDK). Build in one of these ways.

- **Build inside wots4-mods:** Place this repository at `wots4-mods/mods/debugger/` and build following the steps in the wots4-mods README.
- **Build only this Mod:** Pass the path of wots4-mods in `WOTS4_SDK_DIR`.

```bash
cmake -S . -B build -A Win32 -DWOTS4_SDK_DIR=<path to wots4-mods>
cmake --build build --config Release
ctest --test-dir build -C Release -LE needs_exe
```
