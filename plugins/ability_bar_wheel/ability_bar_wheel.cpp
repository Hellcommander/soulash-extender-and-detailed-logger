// Ability-bar wheel switcher (step (b) of docs/ability-bar.md): scrolling the
// mouse wheel while the cursor is over the ability bar switches the bar set by
// writing the same dword the game's own "Ability Bar Switch" action (keymap id
// 65) writes: AbilityUser+0x68 = (AbilityUser+0x68 == 0). Nothing else in the
// game is changed; the game's drawing and casting follow that dword.
//
// OFF BY DEFAULT. It does nothing until <plugin directory>\<plugin id>.ini has
//   [AbilityBarWheel]
//   Enabled=1
// Trigger=modifier (default): the wheel switches once a modifier key
// (Modifier=ctrl|shift|alt) has been held for HoldMs milliseconds (owner's
// request: a short tap keeps the wheel as camera zoom, nobody wants the bar
// flipping on every scroll); Trigger=overbar: the wheel switches while the
// cursor is inside the bar rectangle. The mouse wheel is the game's camera
// zoom, so accepted wheel events are hidden from the game.
//
// Hooks (both logged by the extender as `patch address=`):
//   * DrawActionBar vtable slot 1 (update): pass-through that remembers the
//     live bar object (for its rectangle, and as proof the game world is up)
//     and applies pending wheel steps on the game's own thread.
//   * The game's IAT entry for SDL2.dll!SDL_PollEvent: pass-through that sees
//     each SDL_MOUSEWHEEL event, and, when the trigger condition holds, turns
//     it into a pending step (and, by default, hides that event from the game
//     so the camera does not zoom).
//
// Everything is keyed to one executable build and verified by the probe plugin
// (docs/ability-bar.md section 2.3). On any other build it logs and stays idle.
#include "plugin_api.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    // ---- Build 924e1ef3 (0.10.4.0.1) facts (runtime-verified by AbilityBarProbe) ----
    constexpr char SupportedExeSha256[] =
        "924e1ef316ce5ab4aacf75c2eb83a16932151a0642a2e57af14e7025dac0d430";
    constexpr std::uintptr_t PlayerEntityIdRva = 0xAFE668;

    constexpr unsigned int DrawActionBarUpdateSlot = 1;
    constexpr std::uintptr_t DrawActionBarContextOffset = 0x08;
    // Floats at +0x10..: the handler at 0x3FC7D7 accepts the cursor when
    // x in [+0x20, +0x28] and (windowHeight - y) in [+0x24, +0x14].
    constexpr std::uintptr_t BarLeftOffset = 0x20;
    constexpr std::uintptr_t BarRightOffset = 0x28;
    constexpr std::uintptr_t BarBottomOffset = 0x24;   // flipped y, lower bound
    constexpr std::uintptr_t BarTopOffset = 0x14;      // flipped y, upper bound

    constexpr std::uintptr_t ContextRegistryOffset = 0x100;
    constexpr std::uintptr_t RegistryAbilityUserStorageOffset = 0x08;
    constexpr std::uintptr_t RegistryUiStateStorageOffset = 0x1A0;

    // ECStorage<T>: sparse page table + dense array (getters 0x91DA0 / 0x27D30).
    constexpr std::uintptr_t StoragePagesBeginOffset = 0x08;
    constexpr std::uintptr_t StoragePagesEndOffset = 0x10;
    constexpr std::uintptr_t StorageDenseBeginOffset = 0x38;
    constexpr std::uintptr_t StorageDenseEndOffset = 0x40;
    constexpr std::size_t StoragePageEntrySize = 24;
    constexpr unsigned int StoragePageShift = 13;
    constexpr unsigned int StoragePageMask = 0x1FFF;
    constexpr std::size_t AbilityUserSize = 0x260;
    constexpr std::size_t UiStateSize = 0x130;

    constexpr std::uintptr_t AbilityUserCurrentSetOffset = 0x68;
    // UI state component: +0x60 is the screen/mode id. The game's handler
    // (0x3FC82D) does not process bar input at all in these modes.
    constexpr std::uintptr_t UiStateModeOffset = 0x60;
    constexpr unsigned long long BarInputBlockedModes = 0x13200060400ull; // bit n = mode n, n <= 0x28

    // SDL2 (stable ABI, resolved by name from the game's own SDL2.dll).
    constexpr std::uint32_t SdlKeyDownEvent = 0x300;
    constexpr std::uint32_t SdlKeyUpEvent = 0x301;
    constexpr std::uintptr_t SdlKeyRepeatOffset = 0x0D;   // Uint8 repeat
    constexpr std::uintptr_t SdlKeySymOffset = 0x14;      // SDL_Keycode keysym.sym
    constexpr std::uint32_t SdlKeyLCtrl = 0x400000E0, SdlKeyRCtrl = 0x400000E4;
    constexpr std::uint32_t SdlKeyLShift = 0x400000E1, SdlKeyRShift = 0x400000E5;
    constexpr std::uint32_t SdlKeyLAlt = 0x400000E2, SdlKeyRAlt = 0x400000E6;
    constexpr std::uint32_t SdlMouseWheelEvent = 0x403;
    constexpr std::uintptr_t SdlWheelYOffset = 0x14;        // Sint32 y (notches)
    constexpr std::uintptr_t SdlWheelDirectionOffset = 0x18;
    constexpr std::uintptr_t SdlWheelPreciseYOffset = 0x20; // float, SDL >= 2.0.18
    using SdlPollEventFn = int (*)(void* event);
    using SdlGetMouseStateFn = std::uint32_t (*)(int* x, int* y);
    using SdlGetMouseFocusFn = void* (*)();
    using SdlGetWindowSizeFn = void (*)(void* window, int* w, int* h);
    using SdlGetModStateFn = int (*)();
    constexpr int SdlModShift = 0x0003;
    constexpr int SdlModCtrl = 0x00C0;
    constexpr int SdlModAlt = 0x0300;

    // ---- State ----------------------------------------------------------------
    using UpdateFn = int (*)(void* self, float dt);

    const SoulashExtenderInterface* g_extender = nullptr;
    bool g_swallowWheel = true;
    bool g_triggerOverBar = false;      // Trigger=overbar; otherwise Trigger=modifier
    int g_modifierMask = SdlModCtrl;    // Modifier=ctrl|shift|alt
    const char* g_modifierName = "ctrl";
    unsigned long long g_holdMs = 400;  // HoldMs: modifier must be held this long first
    bool g_logEvents = true;            // LogEvents: one line per wheel event (bounded)
    // SuppressModifierKey: hide the modifier's own key events from the game.
    // Needed for ctrl, which is the game's "Ability Bar Switch" key: holding it
    // auto-repeats and the game flips the bar every few frames by itself.
    bool g_suppressModifierKey = true;
    std::uint32_t g_modifierKeys[2] = {SdlKeyLCtrl, SdlKeyRCtrl};
    unsigned int g_suppressedKeyLogLines = 0;
    SdlGetModStateFn g_getModState = nullptr;
    volatile unsigned long long g_modifierHeldSince = 0; // tick of the first frame the modifier was seen held
    volatile LONG g_armed = 0;                            // modifier held for >= HoldMs (sampled per frame)
    unsigned int g_eventLogLines = 0;
    UpdateFn g_originalUpdate = nullptr;
    SdlPollEventFn g_originalPollEvent = nullptr;
    SdlGetMouseStateFn g_getMouseState = nullptr;
    SdlGetMouseFocusFn g_getMouseFocus = nullptr;
    SdlGetWindowSizeFn g_getWindowSize = nullptr;
    const void* g_abilityUserVtable = nullptr;
    const std::uint32_t* g_playerEntityId = nullptr;

    void* volatile g_bar = nullptr;              // DrawActionBar seen by the last update
    volatile unsigned long long g_barSeenTick = 0; // GetTickCount64 at that update
    volatile LONG g_pendingSteps = 0;              // +1 wheel up, -1 wheel down, summed
    bool g_faulted = false;
    unsigned int g_toggles = 0;

    void Log(const char* format, ...)
    {
        char line[480];
        va_list args;
        va_start(args, format);
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
        va_end(args);
        char prefixed[512];
        _snprintf_s(prefixed, sizeof(prefixed), _TRUNCATE, "AbilityBarWheel: %s", line);
        g_extender->log(prefixed);
    }

    template <typename T>
    T Read(const void* base, std::uintptr_t offset)
    {
        T value;
        std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
        return value;
    }

    // Same walk as the game's ECStorage getters, without calling game code.
    const void* FindComponent(const void* storage, std::uint32_t entityId, std::size_t stride)
    {
        const auto pagesBegin = Read<const unsigned char*>(storage, StoragePagesBeginOffset);
        const auto pagesEnd = Read<const unsigned char*>(storage, StoragePagesEndOffset);
        if (pagesBegin == nullptr || pagesEnd < pagesBegin)
        {
            return nullptr;
        }
        const std::size_t pageCount = static_cast<std::size_t>(pagesEnd - pagesBegin) / StoragePageEntrySize;
        const std::size_t page = entityId >> StoragePageShift;
        if (page >= pageCount)
        {
            return nullptr;
        }
        const auto pageBegin = Read<const std::int32_t*>(pagesBegin, page * StoragePageEntrySize);
        const auto pageEnd = Read<const std::int32_t*>(pagesBegin, page * StoragePageEntrySize + 8);
        if (pageBegin == nullptr || pageBegin == pageEnd)
        {
            return nullptr;
        }
        const std::int32_t denseIndex = pageBegin[entityId & StoragePageMask];
        if (denseIndex < 0)
        {
            return nullptr;
        }
        const auto denseBegin = Read<const unsigned char*>(storage, StorageDenseBeginOffset);
        const auto denseEnd = Read<const unsigned char*>(storage, StorageDenseEndOffset);
        if (denseBegin == nullptr || denseEnd < denseBegin
            || static_cast<std::size_t>(denseIndex) >= static_cast<std::size_t>(denseEnd - denseBegin) / stride)
        {
            return nullptr;
        }
        return denseBegin + static_cast<std::size_t>(denseIndex) * stride;
    }

    bool MouseOverBar(const void* bar)
    {
        int x = 0, y = 0;
        g_getMouseState(&x, &y);
        void* window = g_getMouseFocus();
        if (window == nullptr)
        {
            return false;
        }
        int width = 0, height = 0;
        g_getWindowSize(window, &width, &height);
        const float fx = static_cast<float>(x);
        const float fy = static_cast<float>(height - y);
        return fx >= Read<float>(bar, BarLeftOffset) && fx <= Read<float>(bar, BarRightOffset)
            && fy >= Read<float>(bar, BarBottomOffset) && fy <= Read<float>(bar, BarTopOffset);
    }

    // True when a wheel event is ours (modifier held, or cursor over the bar).
    // Plain C body for __try.
    bool WheelAccepted(const void* event)
    {
        const void* bar = g_bar;
        // The bar object is only trusted while the game keeps calling its
        // update (it is destroyed when leaving the game world); this also
        // keeps the plugin inert in menus.
        if (bar == nullptr || GetTickCount64() - g_barSeenTick > 500)
        {
            return false;
        }
        const auto wheelY = Read<std::int32_t>(event, SdlWheelYOffset);
        const bool accepted = wheelY != 0
            && (g_triggerOverBar ? MouseOverBar(bar) : g_armed != 0);
        if (g_logEvents && g_eventLogLines < 300)
        {
            ++g_eventLogLines;
            Log("wheel event y=%d preciseY=%.2f direction=%u modifier=%s armed=%d -> %s",
                wheelY, Read<float>(event, SdlWheelPreciseYOffset), Read<std::uint32_t>(event, SdlWheelDirectionOffset),
                (g_getModState() & g_modifierMask) != 0 ? "held" : "up", g_armed != 0 ? 1 : 0,
                accepted ? (g_swallowWheel ? "SWALLOWED" : "accepted, passed on") : "passed to game");
        }
        if (!accepted)
        {
            return false;
        }
        InterlockedAdd(&g_pendingSteps, wheelY > 0 ? 1 : -1);
        return true;
    }

    // Sampled once per frame from the update hook: arms the wheel after the
    // modifier has been held continuously for HoldMs.
    void SampleModifier()
    {
        if (g_triggerOverBar)
        {
            g_armed = 1;
            return;
        }
        const unsigned long long now = GetTickCount64();
        if ((g_getModState() & g_modifierMask) == 0)
        {
            if (g_armed != 0 && g_logEvents)
            {
                Log("%s released: wheel back to the game", g_modifierName);
            }
            g_modifierHeldSince = 0;
            g_armed = 0;
            return;
        }
        if (g_modifierHeldSince == 0)
        {
            g_modifierHeldSince = now;
        }
        if (g_armed == 0 && now - g_modifierHeldSince >= g_holdMs)
        {
            g_armed = 1;
            if (g_logEvents)
            {
                Log("%s held %llu ms: wheel now switches bar sets", g_modifierName, g_holdMs);
            }
        }
    }

    // Key down/up of the modifier itself: hidden from the game when configured,
    // so the game's own binding on that key (Left Ctrl = Ability Bar Switch,
    // with keyboard auto-repeat) stays quiet while we drive the bar. SDL's own
    // modifier state (SDL_GetModState) is updated when the event is generated,
    // not when it is polled, so arming still works. Plain C body for __try.
    bool ModifierKeyEvent(const void* event)
    {
        if (!g_suppressModifierKey || g_triggerOverBar)
        {
            return false;
        }
        const auto sym = Read<std::uint32_t>(event, SdlKeySymOffset);
        if (sym != g_modifierKeys[0] && sym != g_modifierKeys[1])
        {
            return false;
        }
        // Only while the game world is up; menus keep their normal keys.
        if (g_bar == nullptr || GetTickCount64() - g_barSeenTick > 500)
        {
            return false;
        }
        if (g_logEvents && g_suppressedKeyLogLines < 40 && Read<std::uint8_t>(event, SdlKeyRepeatOffset) == 0)
        {
            ++g_suppressedKeyLogLines;
            Log("%s key %s hidden from the game", g_modifierName,
                Read<std::uint32_t>(event, 0) == SdlKeyDownEvent ? "down" : "up");
        }
        return true;
    }

    // True when the game must not see this event. Plain C body for __try.
    bool ShouldSwallow(const void* event)
    {
        const auto type = Read<std::uint32_t>(event, 0);
        if (type == SdlMouseWheelEvent)
        {
            return WheelAccepted(event) && g_swallowWheel;
        }
        if (type == SdlKeyDownEvent || type == SdlKeyUpEvent)
        {
            return ModifierKeyEvent(event);
        }
        return false;
    }

    bool GuardedShouldSwallow(const void* event)
    {
        __try
        {
            return ShouldSwallow(event);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_faulted)
            {
                g_faulted = true;
                Log("memory read faulted in the event hook (0x%08lX); disabled", GetExceptionCode());
            }
            return false;
        }
    }

    int PollEventHook(void* event)
    {
        int result = g_originalPollEvent(event);
        if (g_faulted || event == nullptr)
        {
            return result;
        }
        while (result == 1 && GuardedShouldSwallow(event))
        {
            // Hide the consumed event: hand the game the next one instead.
            result = g_originalPollEvent(event);
        }
        return result;
    }

    // Plain C body for __try. Applies the pending steps exactly like action 65.
    void ApplySteps(void* self, LONG steps)
    {
        const auto context = Read<const unsigned char*>(self, DrawActionBarContextOffset);
        const auto registry = context == nullptr ? nullptr : Read<const unsigned char*>(context, ContextRegistryOffset);
        if (registry == nullptr)
        {
            return;
        }
        const std::uint32_t playerId = *g_playerEntityId;
        const auto uiStorage = Read<const unsigned char*>(registry, RegistryUiStateStorageOffset);
        const void* uiState = uiStorage == nullptr ? nullptr : FindComponent(uiStorage, playerId, UiStateSize);
        if (uiState != nullptr)
        {
            const auto mode = Read<std::uint32_t>(uiState, UiStateModeOffset);
            if (mode <= 0x28 && ((BarInputBlockedModes >> mode) & 1ull) != 0)
            {
                Log("wheel ignored: UI mode %u blocks bar input (as the game's own handler does)", mode);
                return;
            }
        }
        const auto storage = Read<const unsigned char*>(registry, RegistryAbilityUserStorageOffset);
        const void* abilityUser = storage == nullptr ? nullptr : FindComponent(storage, playerId, AbilityUserSize);
        if (abilityUser == nullptr || Read<const void*>(abilityUser, 0) != g_abilityUserVtable)
        {
            Log("wheel ignored: no AbilityUser for player %u (or unexpected vtable)", playerId);
            return;
        }
        // Two sets exist today, so only the parity of the steps matters.
        if ((steps & 1) == 0)
        {
            Log("wheel: %ld steps, even, set unchanged", steps);
            return;
        }
        auto* currentSet = reinterpret_cast<std::int32_t*>(
            const_cast<unsigned char*>(static_cast<const unsigned char*>(abilityUser)) + AbilityUserCurrentSetOffset);
        const std::int32_t before = *currentSet;
        const std::int32_t after = before == 0 ? 1 : 0;
        *currentSet = after;
        ++g_toggles;
        Log("wheel %s (%s): set %d -> %d (toggle %u, player %u)",
            steps > 0 ? "up" : "down", g_triggerOverBar ? "over bar" : g_modifierName,
            before, after, g_toggles, playerId);
    }

    void GuardedApplySteps(void* self, LONG steps)
    {
        __try
        {
            ApplySteps(self, steps);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_faulted)
            {
                g_faulted = true;
                Log("memory access faulted while switching (0x%08lX); disabled", GetExceptionCode());
            }
        }
    }

    int UpdateHook(void* self, float dt)
    {
        const int result = g_originalUpdate(self, dt);
        g_bar = self;
        g_barSeenTick = GetTickCount64();
        if (!g_faulted)
        {
            SampleModifier();
            const LONG steps = InterlockedExchange(&g_pendingSteps, 0);
            if (steps != 0)
            {
                GuardedApplySteps(self, steps);
            }
        }
        return result;
    }

    // Finds the game's IAT slot for SDL2.dll!SDL_PollEvent by walking the
    // import directory (build-independent). Plain C body for __try.
    void** FindPollEventIatSlot(unsigned char* base)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return nullptr;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return nullptr;
        }
        const IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imports.VirtualAddress == 0)
        {
            return nullptr;
        }
        for (const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
            descriptor->Name != 0; ++descriptor)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), "SDL2.dll") != 0
                || descriptor->OriginalFirstThunk == 0)
            {
                continue;
            }
            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
            auto* functions = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
            for (std::size_t i = 0; names[i].u1.AddressOfData != 0; ++i)
            {
                if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal))
                {
                    continue;
                }
                const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                if (std::strcmp(byName->Name, "SDL_PollEvent") == 0)
                {
                    return reinterpret_cast<void**>(&functions[i].u1.Function);
                }
            }
        }
        return nullptr;
    }

    void** GuardedFindPollEventIatSlot(unsigned char* base)
    {
        __try
        {
            return FindPollEventIatSlot(base);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ReadConfiguration()
    {
        wchar_t iniPath[MAX_PATH * 2];
        swprintf_s(iniPath, L"%s%S.ini", g_extender->pluginDirectory, g_extender->pluginId);
        const UINT enabled = GetPrivateProfileIntW(L"AbilityBarWheel", L"Enabled", 0, iniPath);
        g_swallowWheel = GetPrivateProfileIntW(L"AbilityBarWheel", L"SwallowWheel", 1, iniPath) != 0;
        g_holdMs = GetPrivateProfileIntW(L"AbilityBarWheel", L"HoldMs", 400, iniPath);
        g_logEvents = GetPrivateProfileIntW(L"AbilityBarWheel", L"LogEvents", 1, iniPath) != 0;
        wchar_t text[32]{};
        GetPrivateProfileStringW(L"AbilityBarWheel", L"Trigger", L"modifier", text, 32, iniPath);
        g_triggerOverBar = _wcsicmp(text, L"overbar") == 0;
        GetPrivateProfileStringW(L"AbilityBarWheel", L"Modifier", L"ctrl", text, 32, iniPath);
        bool suppressDefault = false;
        if (_wcsicmp(text, L"shift") == 0)
        {
            g_modifierMask = SdlModShift;
            g_modifierName = "shift";
            g_modifierKeys[0] = SdlKeyLShift;
            g_modifierKeys[1] = SdlKeyRShift;
        }
        else if (_wcsicmp(text, L"alt") == 0)
        {
            g_modifierMask = SdlModAlt;
            g_modifierName = "alt";
            g_modifierKeys[0] = SdlKeyLAlt;
            g_modifierKeys[1] = SdlKeyRAlt;
        }
        else
        {
            g_modifierMask = SdlModCtrl;
            g_modifierName = "ctrl";
            g_modifierKeys[0] = SdlKeyLCtrl;
            g_modifierKeys[1] = SdlKeyRCtrl;
            suppressDefault = true; // the game binds Left Ctrl itself
        }
        g_suppressModifierKey = GetPrivateProfileIntW(
            L"AbilityBarWheel", L"SuppressModifierKey", suppressDefault ? 1 : 0, iniPath) != 0;
        if (enabled != 1)
        {
            Log("disabled (default). Set [AbilityBarWheel] Enabled=1 in %S%s.ini to turn it on; nothing hooked",
                g_extender->pluginDirectory, g_extender->pluginId);
            return false;
        }
        return true;
    }

    bool Install()
    {
        const HMODULE sdl = GetModuleHandleW(L"SDL2.dll");
        if (sdl == nullptr)
        {
            Log("SDL2.dll is not loaded in this process; inactive");
            return false;
        }
        g_getMouseState = reinterpret_cast<SdlGetMouseStateFn>(GetProcAddress(sdl, "SDL_GetMouseState"));
        g_getMouseFocus = reinterpret_cast<SdlGetMouseFocusFn>(GetProcAddress(sdl, "SDL_GetMouseFocus"));
        g_getWindowSize = reinterpret_cast<SdlGetWindowSizeFn>(GetProcAddress(sdl, "SDL_GetWindowSize"));
        g_getModState = reinterpret_cast<SdlGetModStateFn>(GetProcAddress(sdl, "SDL_GetModState"));
        if (g_getMouseState == nullptr || g_getMouseFocus == nullptr || g_getWindowSize == nullptr
            || g_getModState == nullptr)
        {
            Log("SDL2 mouse/window functions not found; inactive");
            return false;
        }

        void* vtable = nullptr;
        if (!g_extender->findRttiVtable(g_extender->pluginHandle, "DrawActionBar", &vtable) || vtable == nullptr)
        {
            Log("DrawActionBar vtable not found by RTTI; inactive");
            return false;
        }
        void* abilityUserVtable = nullptr;
        if (!g_extender->findRttiVtable(g_extender->pluginHandle, ".?AUAbilityUser@@", &abilityUserVtable)
            || abilityUserVtable == nullptr)
        {
            Log("AbilityUser vtable not found by RTTI; inactive");
            return false;
        }
        g_abilityUserVtable = abilityUserVtable;
        auto* base = static_cast<unsigned char*>(g_extender->gameBase);
        g_playerEntityId = reinterpret_cast<const std::uint32_t*>(base + PlayerEntityIdRva);

        void** iatSlot = GuardedFindPollEventIatSlot(base);
        if (iatSlot == nullptr || *iatSlot == nullptr)
        {
            Log("IAT slot for SDL2.dll!SDL_PollEvent not found; inactive");
            return false;
        }

        // Install the update hook first (it only records and applies), then the
        // event hook; each chains to whatever was in the slot (other plugins
        // such as AbilityBarProbe may already hook update).
        void** updateSlot = static_cast<void**>(vtable) + DrawActionBarUpdateSlot;
        g_originalUpdate = reinterpret_cast<UpdateFn>(*updateSlot);
        void* updateHook = reinterpret_cast<void*>(&UpdateHook);
        if (!g_extender->patchMemory(g_extender->pluginHandle, updateSlot, &updateHook, sizeof(updateHook)))
        {
            Log("patching DrawActionBar vtable slot %u failed; inactive", DrawActionBarUpdateSlot);
            g_originalUpdate = nullptr;
            return false;
        }
        g_originalPollEvent = reinterpret_cast<SdlPollEventFn>(*iatSlot);
        void* pollHook = reinterpret_cast<void*>(&PollEventHook);
        if (!g_extender->patchMemory(g_extender->pluginHandle, iatSlot, &pollHook, sizeof(pollHook)))
        {
            // Undo the update hook so the plugin is either fully on or fully off.
            void* restore = reinterpret_cast<void*>(g_originalUpdate);
            g_extender->patchMemory(g_extender->pluginHandle, updateSlot, &restore, sizeof(restore));
            g_originalUpdate = nullptr;
            g_originalPollEvent = nullptr;
            Log("patching the SDL_PollEvent IAT slot failed; inactive");
            return false;
        }
        Log("active: trigger=%s, hold=%llu ms, suppress modifier key=%d, swallow wheel=%d, log events=%d, update hook on vtable %p (previous %p), SDL_PollEvent IAT slot %p (previous %p)",
            g_triggerOverBar ? "over bar" : g_modifierName, g_holdMs, g_suppressModifierKey ? 1 : 0,
            g_swallowWheel ? 1 : 0, g_logEvents ? 1 : 0,
            vtable, reinterpret_cast<void*>(g_originalUpdate), static_cast<void*>(iatSlot),
            reinterpret_cast<void*>(g_originalPollEvent));
        return true;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SoulashExtenderPluginLoad(
    const SoulashExtenderInterface* extender)
{
    if (extender == nullptr
        || extender->structureSize < sizeof(SoulashExtenderInterface)
        || extender->interfaceVersion != SOULASH_EXTENDER_INTERFACE_VERSION
        || extender->pluginId == nullptr
        || extender->pluginDirectory == nullptr
        || extender->gameExecutableSha256 == nullptr
        || extender->gameBase == nullptr)
    {
        return FALSE;
    }
    g_extender = extender;

    if (!ReadConfiguration())
    {
        return TRUE;
    }
    if (std::strcmp(extender->gameExecutableSha256, SupportedExeSha256) != 0)
    {
        Log("inactive: game build %.8s is not the verified build %.8s (no hooks installed)",
            extender->gameExecutableSha256[0] == '\0' ? "unknown " : extender->gameExecutableSha256,
            SupportedExeSha256);
        return TRUE;
    }
    Install();
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
