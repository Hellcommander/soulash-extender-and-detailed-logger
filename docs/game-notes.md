# Soulash 2 game notes (static analysis)

Findings about `Soulash 2.exe` that the extender depends on. Everything here
came from reading files only: the executable was never run, patched, or
loaded for this analysis. Addresses are RVAs (add the runtime image base; the
image uses ASLR). **Every RVA is valid only for the exact executable hash it
is listed under.** Reproduce them with `tools/pe_xrefs.py`.

| Build | `version.txt` | SHA-256 (first 8) | Seen in |
| --- | --- | --- | --- |
| current | 0.10.3.4 | `d10cec5f` | installed game, analysed below |
| older | unknown | `7cec802f` | `proxy-crash.log` (2026-09-28), 23 crashes |
| older | unknown | `09565e8d` | `proxy-crash.log`, 1 crash |

Only the current build is available for disassembly, so crash addresses from
older builds cannot be mapped onto it.

## Load point (the first BugSplat call)

Verified statically for build `d10cec5f`:

- `BugSplat64.dll` is a **static import** of the game, so the loader maps it
  (and the proxy's `DllMain` runs) before the executable's entry point
  (`0x80B7A0`). The proxy does no work in `DllMain`, by design.
- The CRT start-up path reaches a wide-to-narrow `argv` wrapper at `0x80BBE0`,
  which calls the game's `main` at **`0x6E0D70`** (call site `0x80BD0B`).
- `main`'s first action is `IsDebuggerPresent()` (`0x6E0D93`). Only when it
  returns 0 does `main` build a path string and construct the crash sender:
  `MiniDmpSender::MiniDmpSender` at **`0x6E0FB5`**. That is the first proxy API
  call and therefore the point at which the extender starts.
- After the constructor `main` installs CRT handlers (`set_terminate`,
  `_set_purecall_handler`, `_set_invalid_parameter_handler`,
  `_set_new_handler`, `signal`, `set_unexpected`), then calls
  `sendAdditionalFile` (`0x6E10F0`) and `setGuardByteBufferSize` with `0x1400000`
  (`0x6E110C`). The destructor is reached through the game's own vtable
  (`0x6E1250` is its scalar deleting destructor); the game overwrites the
  object's vtable pointer after construction and allocates only 0x10 bytes.
- Before the constructor, `main` calls one import (`IsDebuggerPresent`) and
  local helpers that look like string/path handling; two of those helpers
  (`0x7E4130`, `0x7BBAC0`) were not inspected. No SDL or thread-creating import
  is called directly on that path, so the extender should run before SDL
  initialisation, before data mods are read, and before game threads exist.
  That is an inference from the call list, **not verified at runtime**.

Consequences:

1. **Under a debugger the BugSplat trigger never fires**: `IsDebuggerPresent()`
   makes `main` skip the crash sender entirely, so the proxy is never called.
   The proxy therefore also starts from a thread created in `DllMain` when a
   debugger is already attached (see the README's debugger fallback).
2. The extender's loader runs on its own worker thread, concurrently with the
   game's `main`. It is racing the game from the very first instruction after
   the constructor returns; plugins must not assume the game is paused.
3. The two-step ordering the plugin API promises (load, then POSTLOAD) says
   nothing about game state. There is currently no "data loaded" message.

## Anchors for the current build (`d10cec5f`)

Found through string cross-references; roles are inferred from the strings and
from the game's own `errors.log` and are **not** runtime-verified.

| RVA | What | Evidence |
| --- | --- | --- |
| `0x6E0D70` | `main` | called from the argv wrapper; first call is `IsDebuggerPresent` |
| `0x4D9760` | start of mod loading | only user of `"Loading mods:"`; log shows `~~ Initializing mods loading. ~~` then `Loading mods:` |
| `0x4DC9C0` | reads one mod's `mod.json` | uses `data/mods/{}/mod.json` and the "Missing necessary mod info" error |
| `0x4DD4B0`, `0x4DB0C0` | mod info validation | same error string |
| `0x4DE420`, `0x4DE800`, `0x4DEC20` | `assets.json` loading | `{}/assets.json`, "Missing assets.json file" |
| `0x4DBEC0` | entity file loading | `data/mods/{}/entities/{}.json` patterns |
| `0x659B80` | main menu setup | only user of `"main_menu"` |
| `0x7F1870` | Steam workshop publishing | `Steam workshop: ...` messages |

The executable contains MSVC RTTI (for example `.?AVMainMenuScreen@@`).
Resolving classes and vtables by RTTI name is a better long-term anchor than
fixed RVAs, because names survive rebuilds that shift addresses.

## Crashes already recorded on this machine

`logs/proxy-crash.log` holds output in this project's diagnostics format from
earlier runs of a proxy build in the real game. It contains 24 access
violations, **all null-pointer writes**, none with a plugin or file-issue
breadcrumb (no plugin was loaded then):

| Build | Count | Fault RVA | Stack (exe RVAs, innermost first) |
| --- | --- | --- | --- |
| `7cec802f` | 8 | `0x6E7E52` | `0x8134EA` (CRT/thread start) |
| `7cec802f` | 7 | `0x6E7E52` | `0xE3681`, `0x42687D`, `0x4E55BE` |
| `7cec802f` | 7 | `0x6E7E52` | `0xE3681`, `0x50D247`, `0x4E20F5` |
| `7cec802f` | 1 | `0x6E7E52` | `0xE3681`, `0x426897`, `0x4E55BE` |
| `09565e8d` | 1 | `0x6DF222` | `0x8096FA` (CRT/thread start) |

Reading: one defect (a write through a null pointer in a single function)
accounts for 23 of 24 records on the older build, reached from at least three
call paths that sit in the same address neighbourhood as the data-mod loading
code. That is a lead, not a diagnosis: the function cannot be inspected
because that executable is no longer installed, and the current build has
different addresses. Next time it crashes with the current build the log will
show whether the signature persists.

## Not yet done

- No runtime verification of any of the above (nothing was launched).
- No game function has a stable, documented hook point yet. The mod-loading
  entry (`0x4D9760`) is the best candidate for a "data about to load" message.
- An exe-hash-keyed anchor table that plugins can query.
