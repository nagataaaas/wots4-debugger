# debugger

[日本語](README.md)

A debugging tool for making Mods for Way of the Samurai 4. It is a Mod that bundles several features, and you can choose whether to use each feature in `config.ini`.

| Feature | What it does | Default |
| --- | --- | --- |
| [Error log](#error-log-error_log) (`error_log`) | Records exceptions, and also writes a minidump when the game crashes | On |

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

## Settings (config.ini)

`mods/debugger/config.ini` has a section for each feature (`[<feature name>]`). `enabled=1` turns that feature on, and `0` turns it off. If the file or an entry is missing, the "Default" in the table above and the default value of each entry below are used.

```ini
[error_log]
enabled=1
minidump=standard
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

This Mod requires the ModLoader.

1. Download the ModLoader (`wots4-modloader-<version>.zip`) from the [wots4-mods Releases](https://github.com/nagataaaas/wots4-mods/releases).
2. Download `wots4-mod-debugger-<version>.zip` from this repository's [Releases](../../releases).
3. Copy the contents of both zips into the same folder as `WayOfTheSamurai4.exe`.

For details, see [Installation](https://github.com/nagataaaas/wots4-mods/blob/master/docs/INSTALL.md).
To uninstall, delete `mods/debugger/`. If you do not need the error logs and minidumps, also delete `error_*.log` and `crash_*.dmp` in `logs/`.

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
