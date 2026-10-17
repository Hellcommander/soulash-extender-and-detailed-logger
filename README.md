# Soulash 2 Native Extender and Detailed Logger

An open-source project aiming to provide a native-extender alternative to
S2NF, together with richer local game diagnostics and practical work toward
preventing known crashes. The current x64 Windows prototype is only an early
foundation: it is not a drop-in S2NF replacement, offers only an opt-in
single-module bootstrap, and does not prevent or recover from arbitrary
crashes.

Crash prevention must come from understanding and correcting specific causes,
not swallowing access violations or resuming execution after memory corruption.
Until the game's extender lifecycle and failure causes are verified, the
prototype gathers bounded local evidence only and makes no compatibility or
crash-prevention guarantee.

## Safety and compatibility

See [CREDITS.md](CREDITS.md) for project attribution and referenced software.

- The `BugSplat64.dll` proxy preserves the four decorated x64 export names and
  ABI signatures confirmed for Soulash 2. Their signatures were checked by demangling the
  import names with Microsoft's x64 `undname`:
  - `sendAdditionalFile(wchar_t const*)`
  - `MiniDmpSender::~MiniDmpSender()`
  - `MiniDmpSender(wchar_t const*, wchar_t const*, wchar_t const*, wchar_t const*, unsigned long)`
  - `setGuardByteBufferSize(int) -> int`
- This repository does not build or replace BugSplat itself. Nothing found so
  far shows that the source or redistribution rights for `BugSplat64.dll` are
  available (BugSplat publishes open-source SDKs for other engines, such as its
  Unity package, and documentation, but those are not this DLL), so the proxy
  always forwards to the user's own renamed original.
- The proxy uses small call-through thunks for those four exports so it can
  activate opt-in diagnostics outside loader lock. On the first exported API
  call, it resolves all four exact symbols from the sibling
  `BugSplat64_original.dll`, then holds that module for process lifetime. This
  must be a user-provided, byte-for-byte renamed copy of that user's signed
  original. This repository does not contain the vendor DLL. Do not use a DLL
  from another source or version. With the original missing or ABI incomplete,
  the first API call fails fast; it does not return a success-shaped fallback.
- Diagnostics are disabled unless enabled with the adjacent
  `SoulashExtender.ini` or the `SOULASH2_DIAGNOSTICS_LOG` environment variable.
  The included INI opts in and writes `logs\proxy-crash.log` relative to the
  proxy/game directory (the `logs` directory must already exist). Place this
  INI beside `BugSplat64.dll`. `Enabled` must equal `1`; the environment variable, if
  present, takes precedence and must contain an absolute path on a local fixed
  drive. On the first proxy API call, logger startup is attempted once; a
  missing, invalid, or unwritable log path (including one whose parent
  directory does not exist) does not block or replace the original BugSplat
  call. To override the INI path, set the variable in the process that launches
  the game, for example
  `$env:SOULASH2_DIAGNOSTICS_LOG = 'C:\Temp\Soulash2-crash.log'`. This is an
  experimental wrapper rather than a transparent linker forwarder; behavior
  for resolving missing exports differs from Windows forwarders. An earlier
  build is known from its log to have run in the game; the current build has
  not been validated there. See [docs/game-notes.md](docs/game-notes.md) for
  the statically verified load point and its limits (notably: the proxy is
  never called when a debugger is attached).
- The proxy can also start one manifest-listed native module when
  `SOULASH2_NATIVE_MODULE_MANIFEST` is set to a local fixed-drive manifest path.
  After the underlying BugSplat method returns, startup is queued on a worker;
  the worker may run before the proxy wrapper returns. The loader is not run in
  this proxy's `DllMain`, but this trigger point may still be too early for game
  initialization and is not a verified extender lifecycle. The manifest is the
  strict three-line `SOULASH2_NATIVE_MODULES_V1` format shown by the validation CLI;
  its module path is relative to the manifest directory. With the variable
  unset, no module is loaded. A module may export
  `SoulashExtenderPluginInit(const SoulashExtenderApi*)` from
  `include/plugin_api.h` to receive ABI version 2, a debug-log callback, and a
  structured file-issue reporting callback.
  The callback writes bounded, newline-sanitized messages to the opted-in
  local diagnostics log with a UTC timestamp, PID, and thread ID, and also
  sends text to the Windows debugger. A module without that export is loaded
  for its `DllMain` side effects only.
  This prototype accepts one module per manifest; multiple plugins come from
  the plugin directory. There is no script integration, no built-in game
  hooks, and no S2NF-compatible lifecycle. The manifest shape was observed in the workshop
  target but S2NF's actual loading behavior is unknown; this bootstrap is not a
  compatibility claim.
- Loading native code executes its `DllMain` and optional plugin initializer
  inside the game process with full process privileges. A faulty or malicious
  module can hang or crash the game; the bootstrap cannot contain it. The
  manifest path is explicit opt-in, not a sandbox or trust boundary. The only
  module executed by tests is a purpose-built synthetic DLL.

Example opt-in for an explicitly reviewed module manifest:

```powershell
$env:SOULASH2_NATIVE_MODULE_MANIFEST = 'C:\Mods\MyMod\native_modules.txt'
```

Set it in the process environment before launching the game. The manifest's
`module=` value is resolved relative to that manifest file.

## Debugger fallback

The game builds its crash sender only when no debugger is attached, so the
BugSplat call that normally triggers start-up never happens under a debugger.
When the proxy is mapped into a process that already has a debugger attached,
`DllMain` therefore only creates a thread (no other work under loader lock);
that thread runs the same opt-in diagnostics and plugin start-up. Opt out with
`SOULASH2_DEBUGGER_FALLBACK=0` or `[Extender] DebuggerFallback=0` in the INI.
The fallback runs concurrently with the game's own start-up and the proxy pins
itself in memory when it triggers. It does not call the original BugSplat DLL.
Verified by a test that attaches as a debugger to a child process; a debugger
attached after the proxy loaded does not trigger it (the normal BugSplat call
path applies in that case).

## Plugin directory (SKSE-style loading)

With `[Plugins] Enabled=1` in the adjacent `SoulashExtender.ini` (or
`SOULASH2_PLUGIN_DIRECTORY` set to an absolute local fixed-drive path), the
proxy maps every `*.dll` in the plugin directory (default
`SoulashExtender\Plugins`, relative to `BugSplat64.dll`) in case-insensitive
alphabetical order, at most 64. A plugin's id is its file name without `.dll`
and must match `[A-Za-z0-9][A-Za-z0-9._-]*`; other names and duplicate ids are
skipped. A manifest-listed module (below) loads first. One plugin failing to
load or initialize does not stop the others.

Lifecycle: each DLL is mapped (`DllMain` runs) and its entry point is called;
once all have loaded, `SOULASH_EXTENDER_MSG_POSTLOAD` and then
`SOULASH_EXTENDER_MSG_POSTPOSTLOAD` are sent to listeners. This all happens on
a worker thread at the first BugSplat API call, an unverified point in game
startup; the game's main thread may already be running.

## Sandboxed Lua scripts (untrusted mods)

Native plugins run with full process privileges and cannot be isolated from the
game, so they are trusted only by pinning (above). For mods you do not trust,
the extender embeds a Lua 5.4 interpreter whose environment is a whitelist.
Scripts are discovered the same way as plugins: `[Scripts] Enabled=1`,
`Directory=Scripts` in the INI (or `SOULASH2_SCRIPT_DIRECTORY`), one
`*.lua` file per script, each in its own isolated state. After all plugins
load, each script's top level runs, then its optional global `on_load()`.

| Tier | What it is | Trust model |
| --- | --- | --- |
| 0 | JSON data mods | no code |
| 1 | `*.lua` scripts | sandboxed: whitelisted API, capped resources |
| 2 | native plugin DLLs | fully trusted: SHA-256 pinned, your decision |

What a script can do: arithmetic, string, table, math and utf8 functions, its
own tables and functions, and `ext.log(text)`. What it cannot: files, processes,
the OS, the module system, the debug library, binary chunks, dynamic code
loading, patterns (`string.match`, `gmatch`, `gsub`; `string.find` is plain
substring search), coroutines, finalizers (`__gc`), randomness or time. The
interpreter is built without the io, os, package and debug libraries at all.

Limits per call: 16 MiB of memory, 5,000,000 instructions, 200 ms wall clock,
strings of at most 1 MiB, 256 KiB of source. Native work inside library
functions is charged against the same budget in proportion to the data it
touches, and an exhausted budget cannot be caught by `pcall`. A script that
fails or exceeds a limit is logged (`script <id> failed: ...`) and skipped;
the game and other scripts continue.

This lowers risk; it is not a proof. Sandbox escapes in language runtimes have
happened, so keep the Lua sources current (see Build and tests), keep the
limits, and treat scripts from unknown authors as suspect. Scripts cannot yet
affect the game: the API is logging only. Querying game state and the
skill-conditions evaluator are the next steps.

## Skill-conditions files

A mod may ship `extender\conditions.json` (format: [docs/skill-conditions.md](docs/skill-conditions.md)).
With `[Conditions] Enabled=1` in the INI (optional `Directory=`, default
`<game folder>\data\mods`; or `SOULASH2_MODS_DIRECTORY`), the extender looks in
every mod folder at start-up, loads each file with the strict loader, and logs
`conditions <mod>: N condition(s) loaded`, or `conditions <mod> rejected: <reason>`,
plus a total. A rejected file never affects other mods. This gives mod authors
feedback from the real environment. **Nothing evaluates the conditions in the
game yet**: that needs a source of game state (game hooks or the Lua layer),
so for now the files are validated and counted only. Offline, the same checks
and a per-condition native/evaluator report are available from
`tools/validate_conditions.py`.

## Plugin allowlist (SHA-256 pinning)

A loaded plugin runs with full process privileges, so the INI can pin exactly
which DLLs may load:

```ini
[Plugins]
Enabled=1
RequireHash=1

[PluginHashes]
MyPlugin=<64 hex characters, SHA-256 of MyPlugin.dll>
```

With `RequireHash=1`, a plugin (directory or manifest) loads only when its id
has an entry whose hash matches the file. The DLL is opened with write sharing
denied from the hash check until it is mapped, so it cannot change in between.
Refusals are logged as `load_failed` events and happen before any of the
plugin's code runs. This is integrity pinning for files you already trust, not
malware detection, and the INI itself must be protected from edits.

## Plugin authoring (interface v3, experimental)

Export `SoulashExtenderPluginLoad(const SoulashExtenderInterface*)` as declared
in `include/plugin_api.h`. The interface provides:

- `pluginId`, `pluginHandle`, `gameDirectory`, `pluginDirectory`, `gameBase`
  and `gameExecutableSha256` (check the hash before touching game code; no
  game version is assumed anywhere).
- `log` and `reportFileIssue` (same behavior as v2).
- `registerListener` / `dispatchMessage`: synchronous inter-plugin messaging.
  Plugin message types start at `SOULASH_EXTENDER_MSG_USER_BASE`; the
  extender's own lifecycle messages come from `SOULASH_EXTENDER_CORE_SENDER`.
- `patchMemory` and `installDetour`: bounded memory patch and 14-byte
  absolute-jump detour with a trampoline. The caller must supply a stolen-byte
  count that ends on an instruction boundary and covers only
  position-independent instructions. Patching is not atomic against other
  threads running the target. Both calls are logged with the plugin id.

- `installDetourAuto`: like `installDetour` but without a hand-counted byte
  count. An x64 length decoder (`src/x64_length.cpp`) walks whole instructions
  from the target until 14 bytes are covered, then rebuilds them in a
  trampoline placed within 2 GB of the target: relative `call`/`jmp`/`jcc`
  become absolute forms and RIP-relative operands are re-based. It refuses
  (and leaves the target untouched, logging the reason) when it meets an
  undecodable instruction, `loop`/`jrcxz`/`xbegin`, a function shorter than
  14 bytes, a stolen branch that lands inside the replaced bytes, or an
  operand the trampoline cannot reach. It cannot know whether other code jumps
  into the middle of the replaced bytes, and replacement is not atomic against
  threads already running the target.
- `findFunctionsReferencingString` and `findRttiVtable`: build-independent
  anchors. They scan the mapped game image for an exact string (and the
  functions that load its address) or for an MSVC class by RTTI name, so a
  plugin does not need fixed RVAs that change with every game update. Leaf
  functions without unwind data are not reported, and a class in an anonymous
  namespace carries a hashed name component and must be passed decorated.

The extender does not know any game function addresses or structures; it
provides the mechanism, not game hooks. Plugins must find their own targets.

```cpp
#include "plugin_api.h"

extern "C" __declspec(dllexport)
BOOL WINAPI SoulashExtenderPluginLoad(const SoulashExtenderInterface* extender)
{
    if (extender == nullptr
        || extender->interfaceVersion != SOULASH_EXTENDER_INTERFACE_VERSION
        || extender->structureSize < sizeof(SoulashExtenderInterface))
    {
        return FALSE;
    }

    extender->log("plugin loaded");
    return TRUE;
}
```

Modules that export only the older `SoulashExtenderPluginInit` still load and
receive the v2 structure described below.

## Plugin authoring (legacy ABI v2)

Include `include/plugin_api.h` and export the exact C entry point declared
there. Return `TRUE` only after successful initialization; `FALSE` is recorded
as an initialization failure, but the DLL remains loaded and its process
side-effects cannot be rolled back. Keep initialization bounded and avoid
blocking waits: it runs on a worker thread at an unverified point in game
startup. Do not assume game objects, UI, or other plugins are ready.

```cpp
#include "plugin_api.h"

extern "C" __declspec(dllexport)
BOOL WINAPI SoulashExtenderPluginInit(const SoulashExtenderApi* api)
{
    if (api == nullptr ||
        api->abiVersion != SOULASH_EXTENDER_PLUGIN_ABI_VERSION ||
        api->structureSize < sizeof(SoulashExtenderApi))
    {
        return FALSE;
    }

    api->log("plugin initialized");
    return TRUE;
}
```

ABI v2 exposes the structure size, ABI version, a best-effort local/debug log
callback, and `reportFileIssue(path, operation, reason)`. The latter records a
plugin-reported breadcrumb only when local diagnostics are enabled; it does
not observe, intercept, validate, or block game file operations, and is not
evidence that a reported file caused a crash. Operations are read, write,
parse, or validate. Paths and reasons are bounded, control characters are
replaced, invalid operation values are rejected, and a busy/unavailable logger
returns `FALSE`. Messages are limited to 512 bytes and logging must not be
relied on for correctness. The API struct and callbacks remain valid for the
lifetime of the process, so a plugin may retain the pointer for later calls.
ABI changes require a new version and compatibility tests; the current
bootstrap loads only the single DLL named by the manifest.

- The known Faster Sleep Simulation workshop module declares a V1 native
  module manifest and statically exposes the same four BugSplat exports as
  forwarders to `BugSplat64_original.dll`. The proxy preserves those ABI names
  and calls through to that same sibling original, so the two DLL designs do
  not require competing implementations of the BugSplat functions. The build
  and harness verify this arrangement using a purpose-built synthetic
  forwarder module and fake original only; the workshop DLL is never loaded.
  Whether the module is loaded by S2NF, and whether its forwarded target is
  found under the real loader's search path, remain unverified.
- No install, copy, or rename operation is performed by the build or tests.
  **Do not install this prototype in the game before isolated harness tests,
  review, and explicit user approval.** Test only with the purpose-built fake
  original included here.
- The `native_modules.txt` parser validates the observed
  `SOULASH2_NATIVE_MODULES_V1` text shape. S2NF's actual loader contract has not
  been verified; parsing this manifest is not S2NF protocol support.
- The optional module host's `--load` mode executes arbitrary DLL code in the
  host process, including its `DllMain`; it is not a sandbox. It is never run
  by the test suite. Do not load an untrusted or unreviewed module.
- On opt-in, the proxy's embedded diagnostics component installs a vectored
  exception observer from the first BugSplat API call (never from `DllMain`).
  At startup it records a UTC
  timestamp, process ID, executable path, a best-effort SHA-256 of the
  executable (skipped if larger than 512 MiB or unreadable), and at most 128
  loaded module paths in a fixed-size metadata buffer. After this prototype's
  opt-in manifest module loads, its full path and image range are added to the
  attribution cache and a `module_loaded` record is written. The logger records
  the manifest module ID and path for `loading`, `loaded`, `initializing`, and
  completion/failure lifecycle events, and includes the ID alongside module
  paths in stack attribution.
  If the fault occurs on the loader worker while Windows is mapping the DLL or
  while the plugin initializer runs, the crash record includes the active
  `module_operation_in_progress` path even before module registration. The
  exception handler ignores
  non-access-violation exceptions, reuses that
  cached metadata, and writes the access-violation code, address, operation,
  target address, exception flags/parameter count, process/thread IDs, captured
  x64 instruction/stack/frame pointers and general-purpose registers, plus
  best-effort module/RVA attribution for the exception and instruction addresses
  from cached module ranges. It also attempts a bounded 12-frame x64 unwind
  from the captured exception context using Windows unwind metadata; frames are
  labeled with module path and RVA, not function names. This is best-effort:
  optimized code, missing unwind metadata, stack corruption, or modules not
  present in the startup snapshot/registered through this prototype can leave
  frames missing or unmapped. No symbols are bundled. The handler uses bounded
  stack buffers and `WriteFile`;
  an address outside those cached ranges is marked `<unmapped>`. If a plugin
  successfully reported a file issue earlier on that same thread, the most
  recent bounded breadcrumb is copied into the crash record as
  `preceding_file_issue_same_thread`; reports from other threads are not
  attributed to the crash. This is temporal/plugin-provided context, not proof
  that the file caused the fault, and vanilla game file access is not observed.
  The handler does not
  allocate or enumerate modules in the exception handler. The handler always returns
  `EXCEPTION_CONTINUE_SEARCH`; it never marks an access violation handled or
  resumes the game. A one-shot recursion guard logs at most one access
  violation per process.
  Startup returns a Win32 error if the local log cannot be opened or the
  startup metadata cannot be written; callers can continue without diagnostics.
  Game file I/O events, the game-log tail, and a dump are not collected.
  Plugins may explicitly report file breadcrumbs through the ABI, but those
  records describe only what the plugin reports and do not establish that the
  file caused a crash. Without game I/O instrumentation or a causal stack trace,
  the logger cannot identify an exact game data file from an access violation.
  Windows/game behavior after an access violation is not safely recoverable
  in general. Logs may be incomplete or corrupt after memory corruption.
- Diagnostics do not upload data. Logging is best-effort and must not be
  treated as a substitute for a debugger or a complete crash dump.

## Length decoder validation

The decoder computes instruction lengths and control-flow facts only (it is
not a disassembler). It is checked two ways in the test suite: a table of
hand-verified encodings (including VEX/EVEX and rejection of invalid or
truncated input), and a comparison against `dumpbin /disasm` of the test
executable itself. A one-off run of the same comparison over the whole of
`Soulash 2.exe` (build `d10cec5f`) matched on about 2.2 million instructions
with no length mismatches; the only inputs it rejected were 11 `D5` bytes that
are jump-table data inside code, not instructions.

## Build and tests

Requirements: x64 Windows, Visual Studio C++ Build Tools with the Windows SDK,
PowerShell, and the **Lua 5.4.9 sources**. Lua is not stored in this repository;
download it from the official site:

- Download page: <https://www.lua.org/ftp/>
- Tarball: <https://www.lua.org/ftp/lua-5.4.9.tar.gz>
- Expected SHA-256:
  `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`

Either run `.\scripts\fetch-lua.ps1` (downloads, verifies the hash, and unpacks
only `src` to `third_party\lua-5.4.9\`, which is git-ignored), or unpack the
tarball yourself so that `third_party\lua-5.4.9\src\lua.h` exists. The build
stops with these links if the sources are missing. Only the core and the base,
table, string, math and utf8 libraries are compiled. Optional: Python 3, which
runs the skill-conditions validator in the test suite when present.

Open an x64 Native Tools/Developer PowerShell prompt, then run:

```powershell
.\scripts\fetch-lua.ps1
.\scripts\build.ps1
.\scripts\test.ps1
```

Artifacts are written to the ignored `.build\` directory. The build inspects
the proxy's PE exports and imports. Tests exercise parser validation, calls
through all four proxy wrappers using a synthetic fake DLL, fail-fast behavior
when the required original is absent, default-disabled and environment-opt-in
diagnostics, logger-startup failure isolation, emitted diagnostic
fields/bounds, mapped-module/RVA and unmapped fault attribution, coexistence
with a synthetic extender-style forwarder DLL, opt-in manifest-driven loading
of a synthetic plugin (including init API and failure isolation), plugin
directory discovery with v2 and v3 synthetic plugins (ordering, lifecycle
messages, inter-plugin messaging, detour/patch), and static manifest
validation. They do not load the workshop DLL or contact any network
service.

## Native module host

Validation is static and does not load modules:

```powershell
.\.build\native-module-host.exe --validate .\native_modules.txt .\mod-folder
```

Loading is opt-in and runs code in-process:

```powershell
.\.build\native-module-host.exe --load .\native_modules.txt .\mod-folder
```

The host validates a strict three-line V1 manifest, rejects absolute and
traversing module paths, and limits DLL dependency search to the module's
directory and Windows System32. The proxy's opt-in bootstrap also accepts that
manifest shape from `SOULASH2_NATIVE_MODULE_MANIFEST`, but runs only one listed
module and is not a full S2NF lifecycle implementation. These checks do not
make the loaded module safe, and path validation is not a security boundary
against filesystem reparse points or concurrent changes.

## After a game update (re-install warning)

**Every Steam update or "Verify integrity of game files" restores the vendor
`BugSplat64.dll` and removes the proxy.** Nothing in this project can survive
that, because the extender is loaded by being that file. After any game update:

1. Close the game.
2. Run `scripts\check-install.ps1 -GameDir "<Soulash 2 folder>"`. It reports
   `EXTENDER NOT INSTALLED` when the vendor file is back, a changed vendor DLL,
   a build that `docs/game-notes.md` does not know, and plugin or chain files
   that no longer match their SHA-256 pins. It changes nothing.
3. Re-install as described below (rename the vendor DLL to
   `BugSplat64_original.dll`, then copy this build's `BugSplat64.dll`), or
   simply paste the packaged proxy back (see "Keeping it installed").
4. Treat anchors and any pinned hashes as stale until re-verified for the new
   exe: string and RTTI anchors usually survive an update, fixed RVAs do not.

The game's own start-up never reports a missing extender; plugins simply do not
load. Check the diagnostics log: a start with the extender active writes a
`timestamp_utc=` / `exe_sha256=` block and module events.

### Keeping it installed

The game loads whatever file is named `BugSplat64.dll`, so the only
maintenance is putting the proxy back after each update (copy the packaged
`BugSplat64.dll` over the vendor file again, after renaming that one to
`BugSplat64_original.dll` if the update restored it). `check-install.ps1` tells
you when that is needed.

Optionally set the proxy file read-only (`attrib +R BugSplat64.dll`) so an
update is less likely to replace it. This is **untested**: Steam may overwrite
it anyway, "Verify integrity" will restore the vendor file regardless, and a
read-only file can make an update fail with a disk write error. If an update
fails, clear the attribute (`attrib -R BugSplat64.dll`) and retry.

An earlier version of this project also had a launcher (`loader.exe`) that
injected the extender into a suspended game so that nothing in the game folder
was replaced. It was removed in favour of supporting a single install route.

## Chain loading other extenders

By default the proxy forwards to `BugSplat64_original.dll`. To stack it in
front of another extender that is itself a BugSplat64-compatible DLL (one that
exports the same four symbols and forwards to the real one), name that DLL in
the INI or the environment:

```ini
[Chain]
Next=OtherExtender.dll
NextSha256=<64 hex characters>
```

`SOULASH2_CHAIN_NEXT` and `SOULASH2_CHAIN_NEXT_SHA256` override the INI. The
target must be a plain file name in the proxy's folder ending in `.dll`; paths,
other extensions and the proxy's own name are refused. With
`[Plugins] RequireHash=1` a chained target needs `NextSha256`. A configured but
missing, unpinned-under-RequireHash or mismatching target fails fast at the
first BugSplat call, like a missing original; nothing is silently skipped. Two
instances of this proxy in one process are refused so diagnostics and plugins
start once. The other extender's DLL runs with full privileges: pin it.
Whether any particular third-party extender works behind this proxy is
unverified; the tests use a purpose-built forwarding DLL.

## Installation and rollback

This is an unsupported, unverified experiment, not a recommended general
installation. The harness verifies forwarding only against the purpose-built
fake original. The real signed BugSplat DLL's four imports/exports were checked
statically, but neither it nor the game has been run with this proxy. Close the
game before changing files. For an explicitly approved local test, preserve
the signed original by renaming it byte-for-byte to
`BugSplat64_original.dll`, then place this build's `BugSplat64.dll` beside it.
Do not download, substitute, or distribute the vendor DLL. Do not overwrite an
existing `BugSplat64_original.dll`; stop and resolve that state manually.

Rollback with the game closed: remove only the experimental proxy
`BugSplat64.dll`, rename `BugSplat64_original.dll` back to `BugSplat64.dll`,
then verify the restored DLL's signature and compare its hash with the
pre-install hash. No proxy sidecar is required because diagnostics and module
runtime are linked into the proxy. The optional module bootstrap remains
experimental and is disabled unless explicitly enabled through its documented
environment variable.
