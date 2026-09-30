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
  for resolving missing exports differs from Windows forwarders, and the proxy
  has not been validated against the vendor DLL or in the game.
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
  This prototype accepts one module per manifest. The v2 API does not provide
  game hooks, script integration, multiple-plugin discovery, or an
  S2NF-compatible lifecycle. The manifest shape was observed in the workshop
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

## Plugin authoring (experimental ABI v2)

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

## Build and tests

Requirements: x64 Windows, Visual Studio C++ Build Tools with the Windows SDK,
and PowerShell. Open an x64 Native Tools/Developer PowerShell prompt, then run:

```powershell
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
of a synthetic plugin (including init API and failure isolation), and static
manifest validation. They do not load the workshop DLL or contact any network
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
