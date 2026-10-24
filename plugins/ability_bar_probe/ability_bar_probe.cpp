// Ability-bar probe: a LOG-ONLY interface-v3 plugin that confirms, inside the
// real game, what static analysis found about the ability hotbar
// (docs/ability-bar.md). It changes no game behaviour: the only write it
// makes is replacing one vtable entry of the DrawActionBar UI system with a
// pass-through hook that calls the original and then reads memory.
//
// Everything below is keyed to one executable build. On any other build the
// plugin logs "inactive" and does nothing. Offsets come from the disassembly
// of build 924e1ef3 (version 0.10.4.0.1); see docs/ability-bar.md for the
// evidence behind each one.
#include "plugin_api.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    // ---- Build 924e1ef3 (0.10.4.0.1) facts ----------------------------------
    constexpr char SupportedExeSha256[] =
        "924e1ef316ce5ab4aacf75c2eb83a16932151a0642a2e57af14e7025dac0d430";

    // Global dword: the player's entity id (written by five functions, read by
    // every UI system through `mov edx,[0xAFE668]`).
    constexpr std::uintptr_t PlayerEntityIdRva = 0xAFE668;

    // DrawActionBar (RTTI ".?AVDrawActionBar@@", 0x38 bytes, vtable of 5):
    //   [0] scalar deleting destructor   [1] bool update(float dt)
    //   [2] (shared no-op)               [3] void draw(Renderer*)
    //   [4] void handleInput()           <- toggles the bar set on action 65
    constexpr unsigned int DrawActionBarUpdateSlot = 1;
    constexpr std::uintptr_t DrawActionBarContextOffset = 0x08; // game context pointer
    constexpr std::uintptr_t DrawActionBarRectOffset = 0x10;    // 10 floats: layout rectangle(s)
    constexpr unsigned int DrawActionBarRectFloats = 10;

    // context+0x100 -> component registry; registry+0x08 -> ECStorage<AbilityUser>.
    constexpr std::uintptr_t ContextRegistryOffset = 0x100;
    constexpr std::uintptr_t RegistryAbilityUserStorageOffset = 0x08;

    // ECStorage<T> (from the getter at 0x91DA0): a sparse page table and a
    // dense array of components.
    //   +0x08/+0x10 : vector<{int* begin; int* end; int* cap}> pages, 0x2000 ints each
    //   +0x38/+0x40 : dense component array begin/end, stride 0x260 for AbilityUser
    constexpr std::uintptr_t StoragePagesBeginOffset = 0x08;
    constexpr std::uintptr_t StoragePagesEndOffset = 0x10;
    constexpr std::uintptr_t StorageDenseBeginOffset = 0x38;
    constexpr std::uintptr_t StorageDenseEndOffset = 0x40;
    constexpr std::size_t StoragePageEntrySize = 24;
    constexpr unsigned int StoragePageShift = 13;
    constexpr unsigned int StoragePageMask = 0x1FFF;
    constexpr std::size_t AbilityUserSize = 0x260;

    // struct AbilityUser (RTTI ".?AUAbilityUser@@"):
    //   +0x00 vtable            +0x08 int slots[24] (2 sets x 12; -1 = empty;
    //                                 otherwise an index into the abilities vector)
    //   +0x68 int currentSet    (0 or 1; "Ability Bar Switch" does cur = (cur == 0))
    //   +0x70/+0x78 vector<Ability*> abilities
    //   +0xB0 float timer       +0xB4 int selectedSlot (0..23 or -1)
    // The save file writes exactly 24 slots and never writes currentSet.
    constexpr std::uintptr_t AbilityUserSlotsOffset = 0x08;
    constexpr unsigned int AbilityUserSlotCount = 24;
    constexpr std::uintptr_t AbilityUserCurrentSetOffset = 0x68;
    constexpr std::uintptr_t AbilityUserAbilitiesBeginOffset = 0x70;
    constexpr std::uintptr_t AbilityUserAbilitiesEndOffset = 0x78;
    constexpr std::uintptr_t AbilityUserSelectedSlotOffset = 0xB4;

    // ---- Plugin state ---------------------------------------------------------
    using UpdateFn = int (*)(void* self, float dt);

    const SoulashExtenderInterface* g_extender = nullptr;
    UpdateFn g_originalUpdate = nullptr;
    const void* g_abilityUserVtable = nullptr;
    const std::uint32_t* g_playerEntityId = nullptr;

    // Last observation, so only changes are logged.
    bool g_haveSnapshot = false;
    std::int32_t g_lastSlots[AbilityUserSlotCount]{};
    std::int32_t g_lastSet = -2;
    std::int32_t g_lastSelected = -2;
    std::uint32_t g_lastPlayerId = 0;
    unsigned long long g_frames = 0;
    unsigned long long g_lastHeartbeatFrame = 0;
    bool g_loggedFirstFrame = false;
    bool g_loggedFault = false;

    void Log(const char* format, ...)
    {
        char line[480];
        va_list args;
        va_start(args, format);
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
        va_end(args);
        char prefixed[512];
        _snprintf_s(prefixed, sizeof(prefixed), _TRUNCATE, "AbilityBarProbe: %s", line);
        g_extender->log(prefixed);
    }

    template <typename T>
    T Read(const void* base, std::uintptr_t offset)
    {
        T value;
        std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
        return value;
    }

    // Replicates the game's component getter (0x91DA0) without calling game
    // code. Returns nullptr when the id is not present in the storage.
    const void* FindComponent(const void* storage, std::uint32_t entityId)
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
        const std::size_t denseCount = static_cast<std::size_t>(denseEnd - denseBegin) / AbilityUserSize;
        if (denseBegin == nullptr || static_cast<std::size_t>(denseIndex) >= denseCount)
        {
            return nullptr;
        }
        return denseBegin + static_cast<std::size_t>(denseIndex) * AbilityUserSize;
    }

    void FormatSlots(const std::int32_t* slots, char* out, std::size_t size)
    {
        std::size_t used = 0;
        for (unsigned int i = 0; i < AbilityUserSlotCount && used < size; ++i)
        {
            const int written = _snprintf_s(out + used, size - used, _TRUNCATE,
                i == 0 ? "%d" : (i == 12 ? " / %d" : ",%d"), slots[i]);
            if (written < 0)
            {
                break;
            }
            used += static_cast<std::size_t>(written);
        }
    }

    // Plain-C body so it can sit inside __try (no objects with destructors).
    void ProbeFrame(const void* self)
    {
        ++g_frames;
        const auto context = Read<const unsigned char*>(self, DrawActionBarContextOffset);
        if (context == nullptr)
        {
            return;
        }
        const std::uint32_t playerId = *g_playerEntityId;
        const auto registry = Read<const unsigned char*>(context, ContextRegistryOffset);
        const auto storage = registry == nullptr
            ? nullptr : Read<const unsigned char*>(registry, RegistryAbilityUserStorageOffset);

        if (!g_loggedFirstFrame)
        {
            g_loggedFirstFrame = true;
            float rect[DrawActionBarRectFloats];
            std::memcpy(rect, static_cast<const unsigned char*>(self) + DrawActionBarRectOffset, sizeof(rect));
            Log("first update: self=%p context=%p registry=%p abilityUserStorage=%p player_id=%u",
                self, context, registry, storage, playerId);
            Log("bar floats +0x10..+0x34: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
                rect[0], rect[1], rect[2], rect[3], rect[4], rect[5], rect[6], rect[7], rect[8], rect[9]);
        }
        if (storage == nullptr)
        {
            return;
        }

        const void* abilityUser = FindComponent(storage, playerId);
        if (abilityUser == nullptr)
        {
            if (g_haveSnapshot || playerId != g_lastPlayerId)
            {
                Log("player_id=%u has no AbilityUser component yet", playerId);
                g_lastPlayerId = playerId;
                g_haveSnapshot = false;
            }
            return;
        }
        const auto vtable = Read<const void*>(abilityUser, 0);
        if (vtable != g_abilityUserVtable)
        {
            if (!g_haveSnapshot)
            {
                Log("component at %p has vtable %p, expected AbilityUser vtable %p: layout assumption wrong, probe idle",
                    abilityUser, vtable, g_abilityUserVtable);
                g_haveSnapshot = true; // suppress repeats
            }
            return;
        }

        std::int32_t slots[AbilityUserSlotCount];
        std::memcpy(slots, static_cast<const unsigned char*>(abilityUser) + AbilityUserSlotsOffset, sizeof(slots));
        const auto currentSet = Read<std::int32_t>(abilityUser, AbilityUserCurrentSetOffset);
        const auto selected = Read<std::int32_t>(abilityUser, AbilityUserSelectedSlotOffset);
        const auto abilitiesBegin = Read<const unsigned char*>(abilityUser, AbilityUserAbilitiesBeginOffset);
        const auto abilitiesEnd = Read<const unsigned char*>(abilityUser, AbilityUserAbilitiesEndOffset);
        const long long abilityCount = abilitiesEnd >= abilitiesBegin ? (abilitiesEnd - abilitiesBegin) / 8 : -1;

        const bool slotsChanged = std::memcmp(slots, g_lastSlots, sizeof(slots)) != 0;
        const bool changed = !g_haveSnapshot || playerId != g_lastPlayerId
            || slotsChanged || currentSet != g_lastSet || selected != g_lastSelected;
        const bool heartbeat = g_frames - g_lastHeartbeatFrame >= 3600; // roughly once a minute at 60 fps
        if (!changed && !heartbeat)
        {
            return;
        }
        char slotText[256];
        FormatSlots(slots, slotText, sizeof(slotText));
        Log("%s frame=%llu player_id=%u abilityUser=%p set=%d selected=%d abilities=%lld slots[set0 / set1]=%s",
            changed ? (g_haveSnapshot ? "CHANGE" : "snapshot") : "heartbeat",
            g_frames, playerId, abilityUser, currentSet, selected, abilityCount, slotText);
        if (g_haveSnapshot && currentSet != g_lastSet)
        {
            Log("ability bar set switched %d -> %d (Ability Bar Switch, action 65, toggles +0x68)", g_lastSet, currentSet);
        }
        std::memcpy(g_lastSlots, slots, sizeof(slots));
        g_lastSet = currentSet;
        g_lastSelected = selected;
        g_lastPlayerId = playerId;
        g_haveSnapshot = true;
        g_lastHeartbeatFrame = g_frames;
    }

    void GuardedProbe(const void* self)
    {
        __try
        {
            ProbeFrame(self);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_loggedFault)
            {
                g_loggedFault = true;
                Log("memory read faulted (code 0x%08lX); probe disabled, game unaffected",
                    GetExceptionCode());
            }
        }
    }

    // Replaces DrawActionBar vtable slot 1. Same signature as the original:
    // `this` in rcx, dt in xmm1, result in eax.
    int UpdateHook(void* self, float dt)
    {
        const int result = g_originalUpdate(self, dt);
        if (!g_loggedFault)
        {
            GuardedProbe(self);
        }
        return result;
    }

    bool Install()
    {
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
        g_playerEntityId = reinterpret_cast<const std::uint32_t*>(
            static_cast<unsigned char*>(g_extender->gameBase) + PlayerEntityIdRva);

        void** slot = static_cast<void**>(vtable) + DrawActionBarUpdateSlot;
        g_originalUpdate = reinterpret_cast<UpdateFn>(*slot);
        void* hook = reinterpret_cast<void*>(&UpdateHook);
        if (!g_extender->patchMemory(g_extender->pluginHandle, slot, &hook, sizeof(hook)))
        {
            Log("patching DrawActionBar vtable slot %u failed; inactive", DrawActionBarUpdateSlot);
            g_originalUpdate = nullptr;
            return false;
        }
        Log("active: DrawActionBar vtable=%p update=%p hooked, AbilityUser vtable=%p, player id at %p",
            vtable, reinterpret_cast<void*>(g_originalUpdate), abilityUserVtable,
            static_cast<const void*>(g_playerEntityId));
        return true;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SoulashExtenderPluginLoad(
    const SoulashExtenderInterface* extender)
{
    if (extender == nullptr
        || extender->structureSize < sizeof(SoulashExtenderInterface)
        || extender->interfaceVersion != SOULASH_EXTENDER_INTERFACE_VERSION
        || extender->gameExecutableSha256 == nullptr
        || extender->gameBase == nullptr)
    {
        return FALSE;
    }
    g_extender = extender;

    if (std::strcmp(extender->gameExecutableSha256, SupportedExeSha256) != 0)
    {
        Log("inactive: game build %.8s is not the analysed build %.8s (no hooks installed)",
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
