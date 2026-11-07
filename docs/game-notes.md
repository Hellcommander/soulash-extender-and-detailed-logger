# Soulash 2 game notes (static analysis)

Findings about `Soulash 2.exe` that the extender depends on. Everything here
came from reading files only: the executable was never run, patched, or
loaded for this analysis. Addresses are RVAs (add the runtime image base; the
image uses ASLR). **Every RVA is valid only for the exact executable hash it
is listed under.** Reproduce them with `tools/pe_xrefs.py`.

| Build | `version.txt` | SHA-256 (first 8) | Seen in |
| --- | --- | --- | --- |
| newest | 0.10.6.1 | `526b6ab4` | Steam update 2026-11-06; plugins verified the ability-bar layout at load (see below) |
| previous | 0.10.5.1 | `60b72c10` | Steam update 2026-10-31; ability-bar layout re-verified (see below) |
| older | 0.10.4.0.1 | `924e1ef3` | game moved to `D:\games\Steam\...`; re-verified below; ability-bar probe runs |
| older | 0.10.3.4 | `d10cec5f` | analysed in the sections below |
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

## Re-verification on build `924e1ef3` (0.10.4.0.1)

Same method, static only. The structure is unchanged; addresses moved:

| What | `d10cec5f` | `924e1ef3` |
| --- | --- | --- |
| `main` (calls `IsDebuggerPresent` first) | `0x6E0D70` | `0x6E5080` |
| `MiniDmpSender` constructor call (first proxy call) | `0x6E0FB5` | `0x6E52C5` |
| `sendAdditionalFile` / `setGuardByteBufferSize` calls | `0x6E10F0` / `0x6E110C` | `0x6E5400` / `0x6E541C` |
| destructor wrapper | `0x6E1250` | `0x6E5560` |
| start of mod loading (`"Loading mods:"`) | `0x4D9760` | `0x4DDB70` |
| `mod.json` readers (`data/mods/{}/mod.json`) | `0x4DC9C0` ... | `0x4E0DD0`, `0x4E1250`, `0x4E1340`, `0x5985B0` |
| passive loader (`restrict_shield`) | `0x509430` | `0x50D840` (same 16 keys) |
| ability loader (`one_of`) | `0x420E50` | `0x424CF0` (same 24 keys) |

The BugSplat call is still the first thing `main` does after `IsDebuggerPresent`,
so the load-point conclusions above hold. Anchors found by string or RTTI name
survived; every fixed RVA shifted by roughly 0x4000-0x4300, which is exactly
why plugins should use the anchor API rather than addresses.

## Build `526b6ab4` (0.10.6.1, 2026-11-06): extender survived the update

Unlike the other sections, this is from runtime logs, not static analysis.
The Steam update again kept the proxy (`BugSplat64.dll` still the proxy,
`BugSplat64_original.dll` the vendor file), and `check-install.ps1` reported
no plugin pin mismatch. In both game sessions after the update
(`logs\proxy-crash.log`, pids 114296 and 30384) AbilityBarProbe and
AbilityBarWheel accepted the build through their load-time layout check and
hooked as before; one session loaded the mods twice, ten minutes apart, with
no crash entry. RVAs derived from the logged runtime addresses (image base
`0x7FF7CD770000` in pid 30384):

| What | `60b72c10` | `526b6ab4` |
| --- | --- | --- |
| `DrawActionBar` vtable / `update` | `0x8D70C0` / `0x401120` | `0x8DF328` / `0x405060` |
| player entity id global | `0xB07668` | `0xB11D6C` |
| AbilityUser vtable | `0x8C14C8` | `0x8C9528` |
| `SDL2.dll!SDL_PollEvent` IAT slot | not recorded | `0x8A43A8` |

The struct offsets were not re-read by hand; the plugins' pattern check
(`plugins/common/ability_bar_layout.hpp`) passing is the evidence. TestBot was
disabled in both sessions, so its Steam-stats blocking and screen detection
have not run on this build yet. Game changelog 0.10.6 lists no engine change
that touches the extender; the only modding change is `required_items` on
producers.

## Build `60b72c10` (0.10.5.1, 2026-10-31): ability bar unchanged

The Steam update kept the installed proxy (the vendor DLL was not replaced:
`BugSplat64.dll` is still the proxy, `BugSplat64_original.dll` the vendor
file). Static re-check of everything the ability-bar plugins use:

| What | `924e1ef3` | `60b72c10` |
| --- | --- | --- |
| `DrawActionBar` vtable / `update` / `handleInput` | `0x8CEFE0` / `0x3FC3F0` / `0x3FC720` | `0x8D70C0` / `0x401120` / `0x401450` |
| player entity id global | `0xAFE668` | `0xB07668` |
| AbilityUser / UI-state getters | `0x91DA0` / `0x27D30` | `0x91FA0` / `0x27F20` (same page table, strides 0x260 / 0x130) |
| AbilityUser vtable (dtor, serialise, deserialise) | `0x8B9430` | `0x8C14C8` (`0x6C650`, `0x6C910`, `0x6D090`) |
| ability save formatter (`id*int*count`) | `0x3627E0` | `0x3672E0` |
| player save writer (`"Saving player"`) | `0x6D09C0` area | `0x6D79D0` |

Every struct offset (`context+0x100`, registry `+0x08`/`+0x1A0`, UI mode
`+0x60`, blocked-mode mask `0x13200060400`, `AbilityUser+0x68` toggle, `+0xB0`
timer, bar rectangle `+0x14/+0x20/+0x24/+0x28`) is byte-identical. Only the
player-id global moved, so both plugins now derive it from `DrawActionBar::update`
at load and verify every pattern instead of trusting an exe hash
(`plugins/common/ability_bar_layout.hpp`; `native-tests.exe --ability-bar-layout
<exe> 401120 401450 6d79d0` runs the same check on a file and prints
`verified=1, player id rva 0xb07668`).

The installed copy at this location has the vendor `BugSplat64.dll` and no
`BugSplat64_original.dll`, i.e. the extender is not installed there. It also
now ships `data.7z` (about 420 MB) next to an extracted `data\` folder.

### Ability bar (located statically; probe plugin written, not yet run)

Full write-up: [ability-bar.md](ability-bar.md). Summary for build `924e1ef3`:

- The bar is the UI system `DrawActionBar` (RTTI, vtable `0x8CEFE0`, 5 virtuals;
  `update` `0x3FC3F0`, `draw` `0x3FC4A0`, `handleInput` `0x3FC720`). Keymap
  actions are not dispatched through a switch table: each UI system asks
  `0x5FA7D0` for the key bound to a gameplay-key id and polls the InputManager
  (`0x7D4170`), which is why no id table was found earlier.
- "Ability Bar Switch" (65) is handled at `0x3FC933`-`0x3FC9B9`: it toggles the
  `int` at **`AbilityUser+0x68`** (0 <-> 1). Slots are `int slots[24]` at
  `AbilityUser+0x08` (2 sets x 12, `-1` empty, else an index into the ability
  vector at `+0x70`). `AbilityUser` is a 0x260-byte component fetched with
  `0x91DA0(storage, g_playerEntityId @ 0xAFE668)`.
- Saves are text (`player.sav`, `Ability user;24|...`): the serialiser writes
  exactly 24 slots and never the current set; the deserialiser has no bound
  check, so extra sets must be stored by the extender, not in the save.
- The mouse wheel already becomes synthetic key codes `0x400000A4` (up) and
  `0x40000119` (down) in `InputManager::handleEvent` (`0x7EFC80`).
- `plugins/ability_bar_probe` is a log-only plugin (vtable pass-through on
  `update`, hash-gated) that verifies the above at runtime; the owner's steps
  are in ability-bar.md section 2.1. Tools: `tools/pe_rtti.py`, `tools/asm_fn.py`.

## Anchors for the previous build (`d10cec5f`)

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
