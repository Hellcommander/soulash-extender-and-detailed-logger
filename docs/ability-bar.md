# Ability bar: multi-set hotbars (static analysis, probe design, roadmap)

Goal (owner's request): more than two ability-bar sets, cycled with the mouse
wheel. The game ships two sets toggled by the keymap action **"Ability Bar
Switch" (id 65, default Left Ctrl)**.

Everything in this file came from reading `Soulash 2.exe` build **`924e1ef3`**
(version 0.10.4.0.1, SHA-256
`924e1ef316ce5ab4aacf75c2eb83a16932151a0642a2e57af14e7025dac0d430`), the
installed `core_2` data and the owner's own save and keymap files. The probe
plugin in the second half of this file verifies it at runtime; section 2.3
lists what the first live run confirmed (anchors, the component chain, the
slot array) and what is still open (the `+0x68` toggle). RVAs are
image-relative; reproduce them with the tools named in each section.

## 1. What the game does today

### 1.1 The bar is a UI "system" called `DrawActionBar`

RTTI name `.?AVDrawActionBar@@`, object size 0x38, constructed at `0x433670`
(allocated with `operator new(0x38)`, vtable `0x8CEFE0` stored at +0, the game
context pointer passed as the constructor's second argument stored at **+0x08**;
+0x10..+0x34 are layout floats, zeroed in the constructor and computed from the
window size). Found with `tools/pe_rtti.py <exe> DrawActionBar`.

| vtable slot | RVA | What it is (from the body) |
| --- | --- | --- |
| 0 | `0x3FC3C0` | scalar deleting destructor (`operator delete(this, 0x38)`) |
| 1 | `0x3FC3F0` | `bool update(float dt)`: if the player exists, and the UI mode is 0x29 (bar focus) forwards to `0x27F940`; always adds `dt` to `AbilityUser+0xB0` |
| 2 | `0x1BE80` | shared no-op |
| 3 | `0x3FC4A0` | `draw(Renderer*)`: draws background and, when the UI mode is 2 or 0x29, the focus frame; reads the keymap entry for action 81 to print its key |
| 4 | `0x3FC720` | `handleInput()`: see 1.2 |

All five are tiny wrappers; the heavy lifting (slot icons, tooltips) is in
`DrawActionIcons` (`0x3F19D0`, vtable `0x8CEEC0`), which reads the same data.

### 1.2 The handler for action 65 (`0x3FC720`, chunk `0x3FC84E`-`0x3FC9F0`)

MSVC split this function into several `.pdata` chunks; `tools/asm_fn.py <exe>
<asm> range 3fc720 3fcb40` prints it whole. Pattern used for every keymap action
in the game:

```
call 0xAD500                 ; game singleton (static object at RVA 0xAFC920)
mov  edx, <gameplay_key>     ; 0x51 = 81 "Ability Bar Focus", 0x41 = 65 "Ability Bar Switch"
call 0x5FA7D0                ; keymap entry for that id -> rax (+0x40 ase_key, +0x44 shift flag)
mov  rcx,[this+8]->+0x170    ; InputManager
call 0x7D4170(input, key, shift) ; "is key pressed this frame" -> al
...
call 0x7D4460(input, key)    ; consume the key
```

For action 65 the effect (`0x3FC99B`-`0x3FC9B9`) is exactly:

```
rcx = [[context+0x100] + 8]          ; ECStorage<AbilityUser>
rax = GetComponent(rcx, g_playerId)  ; 0x91DA0, g_playerId = dword at RVA 0xAFE668
ecx = ([rax+0x68] == 0) ? 1 : 0
[rax+0x68] = ecx                     ; <- the current bar set, toggled
```

So the "current set" is a single `int` at **`AbilityUser+0x68`**, toggled
0 <-> 1. Nothing else happens on a switch: drawing and casting read that int.

### 1.3 Slot storage: `struct AbilityUser`

RTTI `.?AUAbilityUser@@` (a struct), vtable `0x8B9430` with three virtuals
(destructor `0x6C6D0`, serialise `0x6C990`, deserialise `0x6D110`). Instances
are 0x260 bytes in an `ECStorage<AbilityUser>` (dense array stride 0x260,
`imul rax,rax,260h` in the getter `0x91DA0`).

| Offset | Type | Meaning | Evidence |
| --- | --- | --- | --- |
| +0x00 | vtable | | |
| +0x08 | `int slots[24]` | 2 sets x 12 slots; `-1` empty, else index into `abilities` | slot-key helper `0x3FCAB0`: slot n of set 0 is dword index `2+n`, of set 1 is `14+n` (`cmove eax, 0Eh`), compared with `-1` |
| +0x68 | `int currentSet` | 0 or 1 | toggle above; `0x3FD4F6` adds 12 to the slot index when it is 1 |
| +0x70/+0x78/+0x80 | `vector<Ability*>` | known abilities (0x198-byte objects) | serialiser writes `(end-begin)/8` then each via `0x3627E0` as `id*x*y` |
| +0x90 | `set<string>` | (count at +0x98) last block of the save line | |
| +0xB0 | `float` | timer, `update()` adds `dt` | |
| +0xB4 | `int selectedSlot` | 0..23 or -1 (number key pressed twice clears it) | `0x3FD538` |
| +0x1B0..+0x1C8 | `deque<int>` | 5 ints in the owner's save | |
| +0x1D0/+0x1D8/+0x1E0 | map/list | `id*float` pairs (cooldowns) | |

The slot-key helper (`0x3FCAB0`) maps gameplay keys 0x29..0x34 (41..52, the
twelve "Ability n" actions) to slot `n` of the current set, so a bar set has
**12 slots** and there are exactly **24** in the struct.

### 1.4 Save format: text, fixed at 24 slots, current set not saved

`player.sav` (in `%APPDATA%\WizardsOfTheCode\Soulash2\saves\<name>\`) is a text
file: `ENTITY:` / id / `SET:` / one `Name;field|field|...` line per component.
The bar is the `Ability user;` line, e.g.

```
Ability user;24|0|1|2|3|4|5|-1|-1|...|-1|6|acid_magic_acid_dart*0*0|...|5|0|0|0|0|0|1|<id>*152.00002|5|<five ids>
```

- The serialiser (`0x6C990`) writes the literal count **24** (`cmp rbx,18h`),
  then the 24 slot ints, then the abilities, deque, cooldowns and string set.
  **`currentSet` is not written**: every load starts on set 0.
- The deserialiser (`0x6D110`) splits on `|`, reads the count with `strtoul`
  and then writes `count` ints to `this+4+4*i` **without checking against 24**.
  A line with more than 24 slots would overwrite `currentSet` and the
  `abilities` vector. So the save format cannot carry extra sets safely:
  **extra sets must live in extender-owned storage** (todo item 1, step c).
- Slot values are *indices* into the abilities vector, so any extender-side
  copy of a set should store ability ids (strings) and re-resolve them, or it
  goes stale when abilities are learned or forgotten.

### 1.5 Keymap and the mouse wheel

- User keymap: `%APPDATA%\WizardsOfTheCode\Soulash2\data\keymap.json`
  (loaded by `0x4BB990`, the only user of `"data/keymap.json"`; the path is
  relative to that AppData folder, not the game folder). Entries are
  `{ase_key, description, gameplay_key, shift, string_key}`; `ase_key` is the
  SDL keycode (Left Ctrl = `1073742048` = `SDLK_LCTRL`).
- `InputManager::handleEvent` (`0x7EFC80`, called from the `SDL_PollEvent`
  loop at `0x5508D0`) turns `SDL_MOUSEWHEEL` (0x403) into **synthetic key
  codes**: `wheel.y > 0` presses key `0x400000A4` (`1073742004`),
  `wheel.y < 0` presses key `0x40000119` (`1073742105`) (`0x7F001D`-`0x7F004B`,
  via `0x7D3F00` = "press key").
- Therefore the wheel is already a "key" to the keymap system. Whether a
  keymap entry with `ase_key = 1073742004` fires action 65 on scroll-up is a
  **data-only experiment** (no extender needed); see 3.1. Expect the wheel's
  other uses (camera zoom, list scrolling) to keep firing too.

### 1.6 Other addresses worth keeping (build `924e1ef3`)

| RVA | Role |
| --- | --- |
| `0xAD500` | game singleton getter (`&0xAFC920`); `+0x1F8` keymap override, `+0x60` keymap mode, `+0x240`/`+0x200` keymap maps, `+0x68` set of bound keys |
| `0x5FA7D0` | keymap entry by gameplay key |
| `0x7D4170` / `0x7D4460` / `0x7D3F00` | InputManager: is-pressed(key, shift) / consume(key) / press(key) |
| `0x27D30` | component getter for the UI-state storage (`[registry+0x1A0]`); `+0x60` is the UI mode (0x29 = bar focus, 0x1F, 0x15, 0x17 ... other screens) |
| `0x91DA0` | `ECStorage<AbilityUser>::get(entityId)` |
| `0x5BE090` | the component-name table used by save/load (`"Ability user"`, 76 others) |
| `0x6D4CF0` | save-file reader (`ENTITY:` marker) |
| `0xAFE668` | `g_playerEntityId` (written at `0x474154`, `0x679988`, `0x67A46E`, `0x6D3465`, `0x6D7607`) |

## 2. The probe plugin (`plugins/ability_bar_probe`)

Built by `scripts/build.ps1` to `.build\plugins\AbilityBarProbe.dll`. It is
**log-only**: the single write it performs is replacing vtable slot 1 of
`DrawActionBar` (its `update`) with a pass-through hook, through the extender's
`patchMemory` (8 bytes in `.rdata`, logged as `patch address=...`). The hook
calls the original, then reads memory inside a `__try` block; a fault disables
the probe and is logged instead of crashing the game.

Anchors are build-independent where possible (`findRttiVtable("DrawActionBar")`,
`findRttiVtable(".?AUAbilityUser@@")`); the struct offsets and the player-id
global are not, so the plugin checks `gameExecutableSha256` against
`924e1ef3...` and logs `inactive` on any other build without touching anything.

What it logs (all lines start with `AbilityBarProbe:`; they go to the
diagnostics log, `logs\proxy-crash.log` by default, and to the debugger):

| Line | Confirms |
| --- | --- |
| `active: DrawActionBar vtable=... update=... hooked, AbilityUser vtable=..., player id at ...` | RTTI anchors resolve at runtime; patch succeeded |
| `first update: self=... context=... registry=... abilityUserStorage=... player_id=N` | `this+8`, `+0x100`, `+0x08` chain and the player id global |
| `bar floats +0x10..+0x34: ...` | the layout rectangle used for mouse-over (needed for "wheel over the bar") |
| `snapshot ... set=0 selected=-1 abilities=K slots[set0 / set1]=...` | slot array offset and layout; compare the 24 ints and K with the `Ability user;24|...|K|` line of the save |
| `CHANGE ...` followed by `ability bar set switched 0 -> 1` | `+0x68` is the current set and action 65 toggles it |
| `CHANGE ...` with a different slot value | the slot that the drag-and-drop or number key changed |
| `heartbeat ...` about once a minute | the hook keeps running |
| `component at ... has vtable ..., expected ...` / `memory read faulted` | a layout assumption is wrong; stop and re-analyse |

### 2.1 Exact steps for the owner

Preconditions: the extender is installed in the game folder (proxy
`BugSplat64.dll`, vendor DLL renamed to `BugSplat64_original.dll`,
`SoulashExtender.ini` with `[Diagnostics] Enabled=1` and `[Plugins] Enabled=1`).
**Installing is the owner's explicit decision; nothing in this repo does it.**
Keep a copy of the save you will use (copy the whole
`%APPDATA%\WizardsOfTheCode\Soulash2\saves\<name>` folder) even though the
probe writes nothing.

1. Build: `pwsh -NoProfile -File scripts\test.ps1` (or `scripts\build.ps1`).
   The build copies `.build\plugins\AbilityBarProbe.dll` into
   `<game>\SoulashExtender\Plugins\` itself and prints its SHA-256 (owner's
   instruction; `-GameDir ''` skips the copy). Optional pinning: put that hash
   into `[PluginHashes] AbilityBarProbe=<hash>` with `RequireHash=1`.
2. Start the game from Steam as usual, load the test save.
3. Wait two seconds in the game world, then:
   a. press **Left Ctrl** (Ability Bar Switch) twice, pausing a second between;
   b. drag one ability from the book of abilities (`v`) into an empty slot of
      the visible set, then press the number key of that slot twice;
   c. hover the mouse over the bar and **scroll the wheel** up and down a few
      times (this is expected to do nothing yet; it tells us whether the
      wheel is swallowed by something else);
   d. press Left Ctrl once more.
4. Quit to the desktop (do not save over the test save if asked).
5. Send `logs\proxy-crash.log` (or the lines containing `AbilityBarProbe`),
   plus the `Ability user;` line of the save's `player.sav`, and say which
   set was visible before step 3a.

What a good run looks like: `active`, `first update`, one `snapshot`, two
`set switched` lines for 3a (0 -> 1 -> 0), one `CHANGE` with a slot going
from `-1` to an index for 3b and `selected` flipping, no `faulted` line. If the
`snapshot` slots differ from the save line, the slot-array offset is wrong and
step (c) of the roadmap must not start.

### 2.2 Rollback

Delete `SoulashExtender\Plugins\AbilityBarProbe.dll`. The vtable patch lives
only in memory; nothing on disk is modified by the probe.

### 2.3 Runtime evidence so far (2026-10-24, pid 9356, build `924e1ef3`)

The owner ran the game with the proxy and the probe installed (from the
effects session). Start-up portion of the log, verbatim values:

```
patch address=00007FF6D564EFE8 size=8
active: DrawActionBar vtable=00007FF6D564EFE0 update=00007FF6D517C3F0 hooked, AbilityUser vtable=00007FF6D5639430, player id at 00007FF6D587E668
first update: self=0000028DF59CE7F0 context=00007FF6D587EC50 registry=0000028E1DAF6A30 abilityUserStorage=0000028E2748B7C0 player_id=9014
bar floats +0x10..+0x34: 1120.0 188.0 2720.0 188.0 1120.0 0.0 2720.0 0.0 1600.0 188.0
snapshot frame=1 player_id=9014 abilityUser=0000028E2B8C7640 set=0 selected=-1 abilities=5 slots[set0 / set1]=0,1,2,3,4,-1,... / -1,...
```

Confirmed by this: both RTTI anchors resolve (image base `0x7FF6D4D80000`, so
the vtables are at RVAs `0x8CEFE0` and `0x8B9430` as predicted); the
`this+8 -> +0x100 -> +0x08` chain and the storage page/dense layout yield a
component whose first qword is the `AbilityUser` vtable; the slot array at
`+0x08` holds plausible data (five known abilities in slots 0-4 of set 0, the
rest `-1`, `set=0`, `selected=-1`); no fault.

**Second run (pid 78332, player 1678, the `test` save):** the snapshot was
`abilities=6 slots[set0]=0,1,2,3,4,5,-1x6 / set1 all -1`, which is exactly the
save file's `Ability user;24|0|1|2|3|4|5|-1|...|-1|6|...` line, so the slot
array offset and order are confirmed against the owner's own data. The owner
then pressed Left Ctrl repeatedly: 91 `CHANGE` lines, each followed by
`ability bar set switched 0 -> 1` / `1 -> 0`, with the slot array unchanged
throughout, so **`AbilityUser+0x68` is the current set and action 65 toggles
it**, as predicted. Wheel scrolling produced nothing from the probe. No
`faulted`, `vtable`, rejected or access-violation lines in either run.

**Third run (pid 98376, same save, 41 toggles, 0 faults):** the owner dragged
abilities onto both bars and the probe logged one `CHANGE` per drop, ten in
all: on set 0 slots 6..9 became `3,5,5,2`; after switching, on set 1 slots
1,2,6,7,10,11 became `0,1,3,1,4,1`. The final array
`0,1,2,3,4,5,3,5,5,2,-1,-1 / -1,0,1,-1,-1,-1,3,1,-1,-1,4,1` is byte-for-byte
the line the game then saved
(`Ability user;24|0|1|2|3|4|5|3|5|5|2|-1|-1|-1|0|1|-1|-1|-1|3|1|-1|-1|4|1|6|...`).
So drops write straight into `slots[set*12+n]` of the shown set, values are
ability-list indices, and the serialiser writes exactly those 24 ints.

Everything section 1 claims about storage, toggling and saving is now
runtime-verified. Still unobserved: the UI-mode values (`0x29` bar focus) and
the number-key `selected` path; neither matters for steps (b) and (c).

New fact: the game **context is a static object** at RVA **`0xAFEC50`**
(`0x7FF6D587EC50 - 0x7FF6D4D80000`), not a heap allocation, so later plugins
can reach the registry without hooking `DrawActionBar` at all. The bar
rectangle values are in a 2720-pixel-wide layout space (the owner's window).

## 3. Roadmap after the probe

### 3.1 Step (b) first attempt: pure data, no code

Back up `%APPDATA%\WizardsOfTheCode\Soulash2\data\keymap.json`, then change the
`"Ability Bar Switch"` entry's `ase_key` from `1073742048` to `1073742004`
(wheel up) or `1073742105` (wheel down) and start the game. If scrolling
toggles the bar, the game's own input path already accepts the wheel as a key
and step (b) in the extender reduces to "when the wheel moves over the bar
rectangle, press the configured Ability Bar Switch key" (or nothing at all).
If it does not, the probe can be extended to log `0x7D4170`'s view of key
`0x400000A4`. Either way, the game may rewrite `keymap.json` from the options
screen, so keep the backup.

### 3.2 Step (b) in the extender: wheel -> switch (IMPLEMENTED, not yet run)

`plugins/ability_bar_wheel` -> `.build\plugins\AbilityBarWheel.dll` plus
`AbilityBarWheel.ini` (deployed next to it; **`Enabled=0` by default**, the
plugin logs `disabled` and hooks nothing until the owner sets `Enabled=1`).

How it works, on build `924e1ef3` only:

1. Wraps the game's **IAT entry for `SDL2.dll!SDL_PollEvent`** (found by import
   name, build-independent; the exe has exactly one such slot, and
   `InputManager::handleEvent` `0x7EFC80` is reached only from the ten
   `SDL_PollEvent` loops). For each `SDL_MOUSEWHEEL` event it checks the
   trigger: `Trigger=modifier` (default) accepts the event once the `Modifier`
   key (`ctrl` default, or `shift`/`alt`, via `SDL_GetModState`) has been held
   for `HoldMs` (default 400 ms, sampled every frame in the update hook, so a
   short tap or a plain scroll keeps zooming the camera; owner's request, since
   nobody wants the bar flipping on every scroll); `Trigger=overbar` accepts
   it while the cursor is inside the bar rectangle, tested exactly as the
   game's handler does (`x` in `[+0x20, +0x28]`, `windowHeight - y` in
   `[+0x24, +0x14]` of the live `DrawActionBar`). Either way the game world
   must be up (the bar's `update` ran within the last 500 ms). An accepted
   event becomes a pending step (+1 up, -1 down) and, with `SwallowWheel=1`,
   is replaced by the next event so the game never sees it. With `LogEvents=1`
   every wheel event the hook sees is logged (`y`, `preciseY`, armed state,
   swallowed or passed), capped at 300 lines per session.

   Camera zoom, for reference: `0x7D33C0` (called each frame from the pump at
   `0x550A39`) zooms when `is-pressed(0x400000A4 / 0x40000119)` (`0x7D4170`)
   is true; those synthetic keys enter the pressed-key set only from
   `handleEvent` (`0x7F001D`-`0x7F004B`). Second live test (2026-10-24):
   switching worked (54 toggles) but the owner reported it "not working";
   the third test's event log showed all 275 wheel events `SWALLOWED` and none
   passed, while the probe logged the **game itself flipping the bar every few
   frames**: Left Ctrl is the game's own "Ability Bar Switch" key and keyboard
   auto-repeat re-fires it for as long as it is held. Hence
   `SuppressModifierKey` (default on for `ctrl`): the modifier's own key
   down/up events are hidden from the game while the game world is up, so its
   binding stays silent; `SDL_GetModState` still reports the key as held, so
   arming works. With it on, Ctrl alone no longer toggles the bar.
2. Wraps `DrawActionBar::update` (vtable slot 1, chaining behind the probe's
   hook if that plugin is present) to remember the live bar object and, on the
   game's own thread, apply pending steps: skip when the UI mode is one the
   game's handler itself ignores (`0x13200060400` bit mask at `0x3FC82D`),
   otherwise `AbilityUser+0x68 = (+0x68 == 0)` once per odd step count. That is
   the same dword write action 65 performs; no instruction is detoured.
3. Every read sits inside `__try`; a fault logs once and disables the plugin.

Owner steps: build (the DLL and the INI template are deployed by the build;
**with `RequireHash=1` paste the `[PluginHashes]` lines the build prints**, or
the game refuses the DLLs with `load_failed ... error=5`, which is what the
first attempt on 2026-10-24 ran into), set `Enabled=1` in
`<game>\SoulashExtender\Plugins\AbilityBarWheel.ini`, start the game, load
the save, hold Ctrl for half a second, then scroll. Expected log lines
(`AbilityBarWheel:`): `active: trigger=ctrl, hold=400 ms, ...`, `ctrl held 400
ms: wheel now switches bar sets`, then per notch a `wheel event ... armed=1 ->
SWALLOWED` line and `wheel up/down (ctrl): set 0 -> 1 (toggle N, player P)`
with the bar visibly switching, `ctrl released: wheel back to the game`, and
`wheel ignored: UI mode M ...` when a blocking screen is open. Scrolling
without the modifier (or within the first 400 ms) must zoom exactly as before
and log `... armed=0 -> passed to game`. Note Left Ctrl is also the game's own
switch key, so pressing it toggles once by itself; `Modifier=shift` or `alt`
avoids that. Rollback: `Enabled=0` (or delete the DLL); nothing on disk is
changed by the plugin.

Scrolling "past the normal limits" (more than two sets) is step (c) below.

### 3.3 Step (c): N sets

- Keep the game's 24 slots as the *live* window. Extender storage holds
  `N x 12` entries as **ability ids** (strings) plus the index of the live set.
- On a wheel step: copy the live set's indices out (resolve to ids through the
  abilities vector at `+0x70`, element -> id string via the same object layout
  `0x3627E0` formats), copy the target set in (resolve ids -> indices; unknown
  id -> `-1`), keep `+0x68 = 0` or switch physical sets as the owner prefers.
- Persist extender sets in a sidecar next to the save, never in `player.sav`
  (section 1.4). The save folder is known from the `player.sav` writer
  (`0x6D09C0` and friends) or simply from the newest folder under
  `saves\` at save time; both need runtime confirmation.
- Draw a small "set N/M" indicator; the game's own drawing needs no change.

Open questions for the owner before (c): should the game's two physical sets
remain reachable with Left Ctrl, or become sets 1 and 2 of the N? Should sets
be per character (save) or global?
