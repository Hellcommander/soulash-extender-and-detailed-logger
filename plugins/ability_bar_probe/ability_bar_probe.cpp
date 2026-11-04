// Ability-bar probe: a LOG-ONLY interface-v3 plugin that confirms, inside the
// real game, what static analysis found about the ability hotbar
// (docs/ability-bar.md). It changes no game behaviour: the only write it
// makes is replacing one vtable entry of the DrawActionBar UI system with a
// pass-through hook that calls the original and then reads memory.
//
// Offsets come from the disassembly of build 924e1ef3 (version 0.10.4.0.1)
// and were re-checked on 60b72c10 (0.10.5.1); see docs/ability-bar.md for the
// evidence behind each one. The plugin does not trust a build hash: at load it
// verifies DrawActionBar's own code against every pattern it relies on
// (plugins/common/ability_bar_layout.hpp) and takes the player-id global from
// it. If anything does not match it logs "inactive" and does nothing.
#include "plugin_api.h"
#include "../common/ability_bar_layout.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    // ---- Layout facts (verified at load by VerifyAbilityBarLayout) ----------
    // The player's entity id is a global dword read by every UI system through
    // `mov edx,[rip+X]` (0xAFE668 in 924e1ef3, 0xB07668 in 60b72c10); its
    // address is taken from DrawActionBar::update at load.

    // DrawActionBar (RTTI ".?AVDrawActionBar@@", 0x38 bytes, vtable of 5):
    //   [0] scalar deleting destructor   [1] bool update(float dt)
    //   [2] (shared no-op)               [3] void draw(Renderer*)
    //   [4] void handleInput()           <- toggles the bar set on action 65
    constexpr unsigned int DrawActionBarUpdateSlot = 1;
    constexpr unsigned int DrawActionBarHandleInputSlot = 4;
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

    // Step (c) groundwork (log-only): ability ids and the current save's name.
    // Ability objects start with their id as an MSVC std::string (the save
    // formatter 0x3672E0 in 60b72c10 copies Ability+0x00 and appends
    // "*<int +0x158>*<count of vector<string> +0x140>", e.g. "acid_magic_acid_dart*0*0").
    constexpr std::uintptr_t AbilityIdOffset = 0x00;
    constexpr unsigned int MaxLoggedAbilities = 64;
    const void* g_saveNameString = nullptr;   // std::string inside a static singleton
    long long g_lastLoggedAbilityCount = -2;
    std::uint32_t g_lastLoggedAbilityPlayer = 0;
    const void* g_lastLoggedAbilitiesBegin = nullptr;
    char g_lastSaveName[128] = "?";

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

    // One line per ~380 characters: "abilities[0..]: 0=id 1=id ...".
    void LogAbilityIds(const unsigned char* begin, long long count)
    {
        char line[400];
        std::size_t used = 0;
        unsigned int first = 0;
        const unsigned int shown = count > MaxLoggedAbilities ? MaxLoggedAbilities : static_cast<unsigned int>(count);
        for (unsigned int i = 0; i < shown; ++i)
        {
            const auto ability = Read<const unsigned char*>(begin, i * sizeof(void*));
            char id[96];
            if (ability == nullptr
                || !soulash_plugins::ReadMsvcString(ability + AbilityIdOffset, id, sizeof(id)))
            {
                std::memcpy(id, "<unreadable>", sizeof("<unreadable>"));
            }
            char entry[112];
            const int length = _snprintf_s(entry, sizeof(entry), _TRUNCATE, "%s%u=%s", used == 0 ? "" : " ", i, id);
            if (length < 0)
            {
                continue;
            }
            if (used + static_cast<std::size_t>(length) >= sizeof(line) - 1 && used > 0)
            {
                Log("abilities[%u..]: %s", first, line);
                used = 0;
                first = i;
                _snprintf_s(entry, sizeof(entry), _TRUNCATE, "%u=%s", i, id);
            }
            const int appended = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, "%s", entry);
            used += appended < 0 ? 0 : static_cast<std::size_t>(appended);
        }
        if (used > 0)
        {
            Log("abilities[%u..]: %s%s", first, line, count > MaxLoggedAbilities ? " (more not shown)" : "");
        }
    }

    void LogSaveNameIfChanged()
    {
        if (g_saveNameString == nullptr)
        {
            return;
        }
        char name[sizeof(g_lastSaveName)];
        if (!soulash_plugins::ReadMsvcString(g_saveNameString, name, sizeof(name)))
        {
            // Zeroed until the game's singleton is constructed.
            std::memcpy(name, "<not set>", sizeof("<not set>"));
        }
        if (std::strcmp(name, g_lastSaveName) != 0)
        {
            Log("save folder name='%s' (the save is %%APPDATA%%\\WizardsOfTheCode\\Soulash2\\saves\\%s)", name, name);
            std::memcpy(g_lastSaveName, name, sizeof(name));
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
        if (abilityCount >= 0
            && (abilityCount != g_lastLoggedAbilityCount || playerId != g_lastLoggedAbilityPlayer
                || abilitiesBegin != g_lastLoggedAbilitiesBegin))
        {
            Log("player_id=%u knows %lld abilit%s (slot values index this list):", playerId, abilityCount,
                abilityCount == 1 ? "y" : "ies");
            LogAbilityIds(abilitiesBegin, abilityCount);
            g_lastLoggedAbilityCount = abilityCount;
            g_lastLoggedAbilityPlayer = playerId;
            g_lastLoggedAbilitiesBegin = abilitiesBegin;
        }
        LogSaveNameIfChanged();
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

    soulash_plugins::AbilityBarLayout GuardedVerifyLayout(void* vtable)
    {
        soulash_plugins::AbilityBarLayout layout;
        __try
        {
            const auto* image = static_cast<const unsigned char*>(g_extender->gameBase);
            void** slots = static_cast<void**>(vtable);
            layout = soulash_plugins::VerifyAbilityBarLayout(image,
                soulash_plugins::MappedImageSize(image),
                static_cast<const unsigned char*>(slots[DrawActionBarUpdateSlot]),
                static_cast<const unsigned char*>(slots[DrawActionBarHandleInputSlot]));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            layout = soulash_plugins::AbilityBarLayout{};
            layout.failure = "access fault while reading game code";
        }
        return layout;
    }

    soulash_plugins::SaveNameLocation GuardedLocateSaveName(void* saver)
    {
        soulash_plugins::SaveNameLocation location;
        __try
        {
            const auto* image = static_cast<const unsigned char*>(g_extender->gameBase);
            location = soulash_plugins::LocateSaveName(image, soulash_plugins::MappedImageSize(image),
                static_cast<const unsigned char*>(saver));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            location = soulash_plugins::SaveNameLocation{};
            location.failure = "access fault while reading game code";
        }
        return location;
    }

    // Log-only: finds the std::string holding the current save's folder name.
    void LocateSaveNameString()
    {
        void* savers[4]{};
        unsigned int count = 0;
        if (!g_extender->findFunctionsReferencingString(g_extender->pluginHandle, "Saving player", savers, 4, &count)
            || count == 0)
        {
            Log("save name: no function references \"Saving player\"; save folder will not be logged");
            return;
        }
        const char* failure = "";
        for (unsigned int i = 0; i < count && i < 4; ++i)
        {
            const soulash_plugins::SaveNameLocation location = GuardedLocateSaveName(savers[i]);
            if (location.found)
            {
                g_saveNameString = location.nameString;
                Log("save name: std::string at singleton rva 0x%llX + 0x%X (writer %p)",
                    static_cast<unsigned long long>(location.singletonRva), location.nameOffset, savers[i]);
                return;
            }
            failure = location.failure;
        }
        Log("save name: not located (%s); save folder will not be logged", failure);
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
        const soulash_plugins::AbilityBarLayout layout = GuardedVerifyLayout(vtable);
        if (!layout.verified)
        {
            Log("inactive: game build %.8s does not match the verified ability-bar layout (%s); no hooks installed",
                g_extender->gameExecutableSha256[0] == '\0' ? "unknown " : g_extender->gameExecutableSha256,
                layout.failure);
            return false;
        }
        g_playerEntityId = layout.playerEntityId;
        LocateSaveNameString();

        void** slot = static_cast<void**>(vtable) + DrawActionBarUpdateSlot;
        g_originalUpdate = reinterpret_cast<UpdateFn>(*slot);
        void* hook = reinterpret_cast<void*>(&UpdateHook);
        if (!g_extender->patchMemory(g_extender->pluginHandle, slot, &hook, sizeof(hook)))
        {
            Log("patching DrawActionBar vtable slot %u failed; inactive", DrawActionBarUpdateSlot);
            g_originalUpdate = nullptr;
            return false;
        }
        Log("active: game build %.8s, layout verified, DrawActionBar vtable=%p update=%p hooked, AbilityUser vtable=%p, player id at %p (rva 0x%llX)",
            g_extender->gameExecutableSha256[0] == '\0' ? "unknown " : g_extender->gameExecutableSha256,
            vtable, reinterpret_cast<void*>(g_originalUpdate), abilityUserVtable,
            static_cast<const void*>(g_playerEntityId),
            static_cast<unsigned long long>(layout.playerEntityIdRva));
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

    Install();
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
