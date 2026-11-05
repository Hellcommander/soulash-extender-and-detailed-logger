// TestBot: a built-in test bot for Soulash 2. While it is enabled, Steam
// achievements AND stats are blocked for the whole game session, so test runs
// can never unlock, clear or upload anything.
//
// OFF BY DEFAULT. Nothing is hooked until <plugin directory>\TestBot.ini has
//   [TestBot]
//   Enabled=1
// The bot then runs <plugin directory>\<Script> (default TestBot.script; format
// in plugins/common/test_bot_script.hpp and docs/test-bot.md): keys, modifier
// state, mouse wheel, clicks, waits, "wait until in the game world" and checks
// of the ability-bar set. Input goes in through SDL_PushEvent, so the game and
// the other plugins see it exactly like real input.
//
// Achievement/stat blocking (fail closed: no blocking -> the bot does not run):
//   * The game fetches ISteamUserStats through its IAT entry for
//     steam_api64.dll!SteamInternal_FindOrCreateUserInterface (0x809970 in
//     60b72c10). TestBot wraps that entry; when a STEAMUSERSTATS_INTERFACE_*
//     object comes back, it gives the object a private copy of its vtable in
//     which SetStat (int/float), UpdateAvgRateStat, SetAchievement,
//     ClearAchievement, StoreStats, IndicateAchievementProgress and
//     ResetAllStats return success without doing anything. Read calls pass.
//   * The slot of each method is read at load from steam_api64.dll's own flat
//     exports (SteamAPI_ISteamUserStats_*: `mov rax,[rcx]; jmp [rax+slot]`), so
//     it follows whatever steam_api64.dll the game ships.
//   * Once the player is in the world, TestBot also requests the interface
//     itself (same version string the game uses) and patches it, in case the
//     game fetched it before the hook existed.
//
// Hooks (all logged by the extender as `patch address=`): the IAT entry above,
// the IAT entry for SDL2.dll!SDL_PollEvent (pass-through, used as the bot's
// per-frame clock on the game thread), and DrawActionBar vtable slot 1 (pass-
// through; world detection and bar-set reads, only on builds that pass
// VerifyAbilityBarLayout).
#include "plugin_api.h"
#include "../common/ability_bar_layout.hpp"
#include "../common/test_bot_script.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace testbot = soulash_plugins::testbot;

    // ---- DrawActionBar / AbilityUser layout (verified at load) ----------------
    constexpr unsigned int DrawActionBarUpdateSlot = 1;
    constexpr unsigned int DrawActionBarHandleInputSlot = 4;
    constexpr std::uintptr_t DrawActionBarContextOffset = 0x08;
    constexpr std::uintptr_t ContextRegistryOffset = 0x100;
    constexpr std::uintptr_t RegistryAbilityUserStorageOffset = 0x08;
    constexpr std::uintptr_t StoragePagesBeginOffset = 0x08;
    constexpr std::uintptr_t StoragePagesEndOffset = 0x10;
    constexpr std::uintptr_t StorageDenseBeginOffset = 0x38;
    constexpr std::uintptr_t StorageDenseEndOffset = 0x40;
    constexpr std::size_t StoragePageEntrySize = 24;
    constexpr unsigned int StoragePageShift = 13;
    constexpr unsigned int StoragePageMask = 0x1FFF;
    constexpr std::size_t AbilityUserSize = 0x260;
    constexpr std::uintptr_t AbilityUserCurrentSetOffset = 0x68;
    constexpr unsigned long long WorldFreshMs = 1000;   // update hook seen this recently = in the world

    // ---- SDL2 (stable ABI, resolved by name from the game's SDL2.dll) ---------
    constexpr std::size_t SdlEventSize = 56;
    constexpr std::uint32_t SdlKeyDown = 0x300, SdlKeyUp = 0x301;
    constexpr std::uint32_t SdlMouseButtonDown = 0x401, SdlMouseButtonUp = 0x402, SdlMouseWheel = 0x403;
    using SdlPollEventFn = int (*)(void* event);
    using SdlPushEventFn = int (*)(void* event);
    using SdlGetTicksFn = std::uint32_t (*)();
    using SdlGetModStateFn = int (*)();
    using SdlSetModStateFn = void (*)(int modstate);
    using SdlGetScancodeFromKeyFn = int (*)(std::int32_t key);
    using SdlGetFocusFn = void* (*)();
    using SdlGetWindowIdFn = std::uint32_t (*)(void* window);
    using SdlWarpMouseInWindowFn = void (*)(void* window, int x, int y);
    using SdlGetMouseStateFn = std::uint32_t (*)(int* x, int* y);

    // ---- Steam ----------------------------------------------------------------
    using FindOrCreateUserInterfaceFn = void* (*)(int steamUser, const char* version);
    using GetHSteamUserFn = int (*)();
    constexpr char StatsInterfacePrefix[] = "STEAMUSERSTATS_INTERFACE_VERSION";
    constexpr unsigned int MaxPatchedObjects = 4;
    constexpr unsigned int MaxVtableSlots = 96;
    constexpr unsigned int MinVtableSlots = 24;          // ResetAllStats is slot 20 in v013
    constexpr long MaxBlockedLogLines = 200;

    enum BlockedIndex
    {
        BlockSetStatFloat,
        BlockSetStatInt32,
        BlockUpdateAvgRateStat,
        BlockSetAchievement,
        BlockClearAchievement,
        BlockStoreStats,
        BlockIndicateAchievementProgress,
        BlockResetAllStats,
        BlockCount
    };

    struct BlockedMethod
    {
        const char* exportName;
        const char* label;
        void* stub;
        int offset;                 // vtable byte offset, from steam_api64.dll
        volatile long calls;
    };

    // ---- State ----------------------------------------------------------------
    using UpdateFn = int (*)(void* self, float dt);

    const SoulashExtenderInterface* g_extender = nullptr;
    bool g_stopOnFailure = false;
    bool g_logBlockedCalls = true;

    // Steam blocking
    bool g_blockingReady = false;
    FindOrCreateUserInterfaceFn g_originalFindInterface = nullptr;
    GetHSteamUserFn g_getHSteamUser = nullptr;
    char g_statsVersion[64] = "";
    CRITICAL_SECTION g_patchLock;
    void* g_patchedObjects[MaxPatchedObjects]{};
    void* g_vtableCopies[MaxPatchedObjects][1 + MaxVtableSlots]{};
    unsigned int g_patchedCount = 0;
    volatile long g_blockedLogLines = 0;
    bool g_loggedSelfRequest = false;

    // World detection
    UpdateFn g_originalUpdate = nullptr;
    const void* g_abilityUserVtable = nullptr;
    const std::uint32_t* g_playerEntityId = nullptr;
    volatile unsigned long long g_worldSeenMs = 0;
    volatile long g_barSet = -1;
    bool g_loggedUpdateFault = false;

    // SDL
    SdlPollEventFn g_originalPollEvent = nullptr;
    SdlPushEventFn g_pushEvent = nullptr;
    SdlGetTicksFn g_getTicks = nullptr;
    SdlGetModStateFn g_getModState = nullptr;
    SdlSetModStateFn g_setModState = nullptr;
    SdlGetScancodeFromKeyFn g_getScancodeFromKey = nullptr;
    SdlGetFocusFn g_getKeyboardFocus = nullptr;
    SdlGetFocusFn g_getMouseFocus = nullptr;
    SdlGetWindowIdFn g_getWindowId = nullptr;
    SdlWarpMouseInWindowFn g_warpMouse = nullptr;
    SdlGetMouseStateFn g_getMouseState = nullptr;

    // Script runner (game thread only: driven from the SDL_PollEvent hook)
    std::vector<testbot::Step> g_steps;
    bool g_running = false;
    std::size_t g_stepIndex = 0;
    int g_phase = 0;
    unsigned long long g_deadline = 0;
    std::int64_t g_counter = 0;
    unsigned int g_failures = 0;
    unsigned int g_checks = 0;
    unsigned long long g_startedMs = 0;

    void Log(const char* format, ...)
    {
        char line[480];
        va_list args;
        va_start(args, format);
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
        va_end(args);
        char prefixed[512];
        _snprintf_s(prefixed, sizeof(prefixed), _TRUNCATE, "TestBot: %s", line);
        g_extender->log(prefixed);
    }

    unsigned long long NowMs()
    {
        return GetTickCount64();
    }

    // ===========================================================================
    // Steam achievements / stats blocking
    // ===========================================================================

    // Copies a C string from the game (inside __try: the pointer is theirs).
    void GuardedCopyName(const char* name, char* out, std::size_t size)
    {
        out[0] = '\0';
        if (name == nullptr)
        {
            return;
        }
        __try
        {
            std::size_t i = 0;
            for (; i + 1 < size && name[i] != '\0'; ++i)
            {
                const char c = name[i];
                out[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
            }
            out[i] = '\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            strcpy_s(out, size, "<unreadable>");
        }
    }

    BlockedMethod g_blocked[BlockCount];

    void NoteBlocked(BlockedIndex index, const char* name, const char* detail)
    {
        const long calls = InterlockedIncrement(&g_blocked[index].calls);
        if (!g_logBlockedCalls)
        {
            return;
        }
        const long line = InterlockedIncrement(&g_blockedLogLines);
        if (line > MaxBlockedLogLines)
        {
            if (line == MaxBlockedLogLines + 1)
            {
                Log("blocked-call log limit (%ld lines) reached; counts are still kept", MaxBlockedLogLines);
            }
            return;
        }
        char copy[96];
        GuardedCopyName(name, copy, sizeof(copy));
        Log("BLOCKED %s(%s%s) #%ld - nothing sent to Steam", g_blocked[index].label, copy, detail, calls);
    }

    bool SetStatFloatStub(void*, const char* name, float value)
    {
        char detail[48];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, ", %g", static_cast<double>(value));
        NoteBlocked(BlockSetStatFloat, name, detail);
        return true;
    }

    bool SetStatInt32Stub(void*, const char* name, std::int32_t value)
    {
        char detail[48];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, ", %d", value);
        NoteBlocked(BlockSetStatInt32, name, detail);
        return true;
    }

    bool UpdateAvgRateStatStub(void*, const char* name, float count, double length)
    {
        char detail[64];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, ", %g, %g", static_cast<double>(count), length);
        NoteBlocked(BlockUpdateAvgRateStat, name, detail);
        return true;
    }

    bool SetAchievementStub(void*, const char* name)
    {
        NoteBlocked(BlockSetAchievement, name, "");
        return true;
    }

    bool ClearAchievementStub(void*, const char* name)
    {
        NoteBlocked(BlockClearAchievement, name, "");
        return true;
    }

    bool StoreStatsStub(void*)
    {
        NoteBlocked(BlockStoreStats, nullptr, "");
        return true;
    }

    bool IndicateAchievementProgressStub(void*, const char* name, std::uint32_t current, std::uint32_t maximum)
    {
        char detail[48];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, ", %u/%u", current, maximum);
        NoteBlocked(BlockIndicateAchievementProgress, name, detail);
        return true;
    }

    bool ResetAllStatsStub(void*, bool achievementsToo)
    {
        NoteBlocked(BlockResetAllStats, nullptr, achievementsToo ? "achievements too" : "stats only");
        return true;
    }

    void InitBlockedTable()
    {
        g_blocked[BlockSetStatFloat] = {"SteamAPI_ISteamUserStats_SetStatFloat", "SetStat(float)",
            reinterpret_cast<void*>(&SetStatFloatStub), -1, 0};
        g_blocked[BlockSetStatInt32] = {"SteamAPI_ISteamUserStats_SetStatInt32", "SetStat(int32)",
            reinterpret_cast<void*>(&SetStatInt32Stub), -1, 0};
        g_blocked[BlockUpdateAvgRateStat] = {"SteamAPI_ISteamUserStats_UpdateAvgRateStat", "UpdateAvgRateStat",
            reinterpret_cast<void*>(&UpdateAvgRateStatStub), -1, 0};
        g_blocked[BlockSetAchievement] = {"SteamAPI_ISteamUserStats_SetAchievement", "SetAchievement",
            reinterpret_cast<void*>(&SetAchievementStub), -1, 0};
        g_blocked[BlockClearAchievement] = {"SteamAPI_ISteamUserStats_ClearAchievement", "ClearAchievement",
            reinterpret_cast<void*>(&ClearAchievementStub), -1, 0};
        g_blocked[BlockStoreStats] = {"SteamAPI_ISteamUserStats_StoreStats", "StoreStats",
            reinterpret_cast<void*>(&StoreStatsStub), -1, 0};
        g_blocked[BlockIndicateAchievementProgress] = {"SteamAPI_ISteamUserStats_IndicateAchievementProgress",
            "IndicateAchievementProgress", reinterpret_cast<void*>(&IndicateAchievementProgressStub), -1, 0};
        g_blocked[BlockResetAllStats] = {"SteamAPI_ISteamUserStats_ResetAllStats", "ResetAllStats",
            reinterpret_cast<void*>(&ResetAllStatsStub), -1, 0};
    }

    int GuardedDecodeExport(const void* code)
    {
        __try
        {
            return testbot::DecodeVtableJump(static_cast<const unsigned char*>(code));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Readable slots starting at `vtable` (inside one committed, readable region).
    unsigned int ReadableSlots(const void* vtable)
    {
        MEMORY_BASIC_INFORMATION info{};
        const auto* start = static_cast<const unsigned char*>(vtable) - sizeof(void*);
        if (VirtualQuery(start, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0
            || (info.Protect & PAGE_GUARD) != 0)
        {
            return 0;
        }
        const auto* regionEnd = static_cast<const unsigned char*>(info.BaseAddress) + info.RegionSize;
        const auto available = static_cast<std::size_t>(regionEnd - static_cast<const unsigned char*>(vtable)) / sizeof(void*);
        return available > MaxVtableSlots ? MaxVtableSlots : static_cast<unsigned int>(available);
    }

    bool GuardedCopyVtable(void* const* source, unsigned int slots, void** copy)
    {
        __try
        {
            copy[0] = source[-1];   // RTTI complete object locator stays valid
            for (unsigned int i = 0; i < slots; ++i)
            {
                copy[1 + i] = source[i];
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Gives one ISteamUserStats object a private vtable with the write methods stubbed.
    void PatchStatsObject(void* object, const char* version)
    {
        char versionCopy[64];
        GuardedCopyName(version, versionCopy, sizeof(versionCopy));
        EnterCriticalSection(&g_patchLock);
        for (unsigned int i = 0; i < g_patchedCount; ++i)
        {
            if (g_patchedObjects[i] == object)
            {
                LeaveCriticalSection(&g_patchLock);
                return;
            }
        }
        if (g_patchedCount >= MaxPatchedObjects)
        {
            LeaveCriticalSection(&g_patchLock);
            Log("WARNING: more than %u user-stats objects; %s object %p NOT blocked", MaxPatchedObjects, versionCopy, object);
            return;
        }
        void* const* original = nullptr;
        std::memcpy(&original, object, sizeof(original));
        const unsigned int slots = original == nullptr ? 0 : ReadableSlots(original);
        void** copy = g_vtableCopies[g_patchedCount];
        if (slots < MinVtableSlots || !GuardedCopyVtable(original, slots, copy))
        {
            LeaveCriticalSection(&g_patchLock);
            Log("WARNING: %s object %p: vtable %p unreadable (%u slots); NOT blocked", versionCopy, object,
                static_cast<const void*>(original), slots);
            return;
        }
        for (const BlockedMethod& method : g_blocked)
        {
            copy[1 + method.offset / 8] = method.stub;
        }
        void* replacement = &copy[1];
        if (!g_extender->patchMemory(g_extender->pluginHandle, object, &replacement, sizeof(replacement)))
        {
            LeaveCriticalSection(&g_patchLock);
            Log("WARNING: could not replace the vtable of %s object %p; NOT blocked", versionCopy, object);
            return;
        }
        g_patchedObjects[g_patchedCount++] = object;
        LeaveCriticalSection(&g_patchLock);
        Log("achievements+stats BLOCKED on %s object %p (vtable %p -> private copy of %u slots)",
            versionCopy, object, static_cast<const void*>(original), slots);
    }

    bool IsStatsVersion(const char* version)
    {
        __try
        {
            return version != nullptr && std::strncmp(version, StatsInterfacePrefix, sizeof(StatsInterfacePrefix) - 1) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* FindOrCreateUserInterfaceHook(int steamUser, const char* version)
    {
        void* result = g_originalFindInterface(steamUser, version);
        if (result != nullptr && IsStatsVersion(version))
        {
            PatchStatsObject(result, version);
        }
        return result;
    }

    // Fallback once in the world: request the interface ourselves (Steam returns
    // the same cached object the game holds) and patch it.
    void EnsureStatsObjectPatched()
    {
        if (g_patchedCount > 0 || g_loggedSelfRequest || g_getHSteamUser == nullptr || g_statsVersion[0] == '\0')
        {
            return;
        }
        g_loggedSelfRequest = true;
        const int user = g_getHSteamUser();
        if (user == 0)
        {
            Log("Steam is not running for this process (no user); no achievement can be granted this session");
            return;
        }
        void* object = FindOrCreateUserInterfaceHook(user, g_statsVersion);
        if (object == nullptr)
        {
            Log("Steam returned no %s object; no achievement can be granted this session", g_statsVersion);
        }
    }

    // The game's own user-stats version string ("STEAMUSERSTATS_INTERFACE_VERSION013").
    void FindStatsVersionString(const unsigned char* image, std::size_t size)
    {
        const std::size_t prefix = sizeof(StatsInterfacePrefix) - 1;
        for (std::size_t i = 0; i + prefix + 4 < size; ++i)
        {
            if (image[i] != 'S' || std::memcmp(image + i, StatsInterfacePrefix, prefix) != 0)
            {
                continue;
            }
            std::size_t length = prefix;
            while (i + length < size && length < sizeof(g_statsVersion) - 1 && image[i + length] >= '0'
                && image[i + length] <= '9')
            {
                ++length;
            }
            if (length > prefix && image[i + length] == '\0')
            {
                std::memcpy(g_statsVersion, image + i, length);
                g_statsVersion[length] = '\0';
                return;
            }
        }
    }

    void GuardedFindStatsVersionString(const unsigned char* image, std::size_t size)
    {
        __try
        {
            FindStatsVersionString(image, size);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_statsVersion[0] = '\0';
        }
    }

    // ===========================================================================
    // Import table
    // ===========================================================================
    void** FindImportSlot(unsigned char* base, const char* dll, const char* function)
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
            if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll) != 0
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
                if (std::strcmp(byName->Name, function) == 0)
                {
                    return reinterpret_cast<void**>(&functions[i].u1.Function);
                }
            }
        }
        return nullptr;
    }

    void** GuardedFindImportSlot(unsigned char* base, const char* dll, const char* function)
    {
        __try
        {
            return FindImportSlot(base, dll, function);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // ===========================================================================
    // World detection (DrawActionBar::update pass-through)
    // ===========================================================================
    template <typename T>
    T Read(const void* base, std::uintptr_t offset)
    {
        T value;
        std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
        return value;
    }

    const unsigned char* FindAbilityUser(const void* storage, std::uint32_t entityId)
    {
        const auto pagesBegin = Read<const unsigned char*>(storage, StoragePagesBeginOffset);
        const auto pagesEnd = Read<const unsigned char*>(storage, StoragePagesEndOffset);
        if (pagesBegin == nullptr || pagesEnd < pagesBegin)
        {
            return nullptr;
        }
        const std::size_t page = entityId >> StoragePageShift;
        if (page >= static_cast<std::size_t>(pagesEnd - pagesBegin) / StoragePageEntrySize)
        {
            return nullptr;
        }
        const auto pageBegin = Read<const std::int32_t*>(pagesBegin, page * StoragePageEntrySize);
        const auto pageEnd = Read<const std::int32_t*>(pagesBegin, page * StoragePageEntrySize + 8);
        if (pageBegin == nullptr || pageBegin == pageEnd)
        {
            return nullptr;
        }
        const std::int32_t dense = pageBegin[entityId & StoragePageMask];
        const auto denseBegin = Read<const unsigned char*>(storage, StorageDenseBeginOffset);
        const auto denseEnd = Read<const unsigned char*>(storage, StorageDenseEndOffset);
        if (dense < 0 || denseBegin == nullptr
            || static_cast<std::size_t>(dense) >= static_cast<std::size_t>(denseEnd - denseBegin) / AbilityUserSize)
        {
            return nullptr;
        }
        return denseBegin + static_cast<std::size_t>(dense) * AbilityUserSize;
    }

    void ObserveWorld(const void* self)
    {
        const auto context = Read<const unsigned char*>(self, DrawActionBarContextOffset);
        const auto registry = context == nullptr ? nullptr : Read<const unsigned char*>(context, ContextRegistryOffset);
        const auto storage = registry == nullptr ? nullptr : Read<const unsigned char*>(registry, RegistryAbilityUserStorageOffset);
        if (storage == nullptr)
        {
            return;
        }
        const unsigned char* abilityUser = FindAbilityUser(storage, *g_playerEntityId);
        if (abilityUser == nullptr || Read<const void*>(abilityUser, 0) != g_abilityUserVtable)
        {
            return;
        }
        g_barSet = Read<std::int32_t>(abilityUser, AbilityUserCurrentSetOffset);
        g_worldSeenMs = NowMs();
    }

    int UpdateHook(void* self, float dt)
    {
        const int result = g_originalUpdate(self, dt);
        if (!g_loggedUpdateFault)
        {
            __try
            {
                ObserveWorld(self);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_loggedUpdateFault = true;
                Log("world read faulted (code 0x%08lX); world detection off, game unaffected", GetExceptionCode());
            }
        }
        return result;
    }

    bool InWorld()
    {
        const unsigned long long seen = g_worldSeenMs;
        return seen != 0 && NowMs() - seen <= WorldFreshMs;
    }

    soulash_plugins::AbilityBarLayout GuardedVerifyLayout(void* vtable)
    {
        soulash_plugins::AbilityBarLayout layout;
        __try
        {
            const auto* image = static_cast<const unsigned char*>(g_extender->gameBase);
            void** slots = static_cast<void**>(vtable);
            layout = soulash_plugins::VerifyAbilityBarLayout(image, soulash_plugins::MappedImageSize(image),
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

    void InstallWorldDetection()
    {
        void* vtable = nullptr;
        void* abilityUserVtable = nullptr;
        if (!g_extender->findRttiVtable(g_extender->pluginHandle, "DrawActionBar", &vtable) || vtable == nullptr
            || !g_extender->findRttiVtable(g_extender->pluginHandle, ".?AUAbilityUser@@", &abilityUserVtable)
            || abilityUserVtable == nullptr)
        {
            Log("world detection off: DrawActionBar/AbilityUser not found by RTTI (wait_world and expect_bar_set will fail)");
            return;
        }
        const soulash_plugins::AbilityBarLayout layout = GuardedVerifyLayout(vtable);
        if (!layout.verified)
        {
            Log("world detection off: ability-bar layout not verified (%s); wait_world and expect_bar_set will fail",
                layout.failure);
            return;
        }
        g_abilityUserVtable = abilityUserVtable;
        g_playerEntityId = layout.playerEntityId;
        void** slot = static_cast<void**>(vtable) + DrawActionBarUpdateSlot;
        g_originalUpdate = reinterpret_cast<UpdateFn>(*slot);
        void* hook = reinterpret_cast<void*>(&UpdateHook);
        if (!g_extender->patchMemory(g_extender->pluginHandle, slot, &hook, sizeof(hook)))
        {
            g_originalUpdate = nullptr;
            Log("world detection off: patching DrawActionBar update failed");
            return;
        }
        Log("world detection on (DrawActionBar update hooked, previous %p; player id rva 0x%llX)",
            reinterpret_cast<void*>(g_originalUpdate), static_cast<unsigned long long>(layout.playerEntityIdRva));
    }

    // ===========================================================================
    // Input injection
    // ===========================================================================
    void* FocusWindow()
    {
        void* window = g_getKeyboardFocus();
        return window != nullptr ? window : g_getMouseFocus();
    }

    std::uint32_t FocusWindowId()
    {
        void* window = FocusWindow();
        return window == nullptr ? 0 : g_getWindowId(window);
    }

    template <typename T>
    void Put(unsigned char* event, std::size_t offset, T value)
    {
        std::memcpy(event + offset, &value, sizeof(T));
    }

    bool Push(unsigned char* event, const char* what)
    {
        const int result = g_pushEvent(event);
        if (result != 1)
        {
            Log("SDL_PushEvent(%s) returned %d", what, result);
            return false;
        }
        return true;
    }

    bool PushKey(std::uint32_t keycode, bool down)
    {
        unsigned char event[SdlEventSize]{};
        Put<std::uint32_t>(event, 0, down ? SdlKeyDown : SdlKeyUp);
        Put<std::uint32_t>(event, 4, g_getTicks());
        Put<std::uint32_t>(event, 8, FocusWindowId());
        event[12] = down ? 1 : 0;                                   // state
        event[13] = 0;                                              // repeat
        Put<std::int32_t>(event, 16, g_getScancodeFromKey(static_cast<std::int32_t>(keycode)));
        Put<std::uint32_t>(event, 20, keycode);                     // keysym.sym
        Put<std::uint16_t>(event, 24, static_cast<std::uint16_t>(g_getModState()));
        return Push(event, down ? "key down" : "key up");
    }

    bool PushWheel(int notch)
    {
        unsigned char event[SdlEventSize]{};
        int x = 0;
        int y = 0;
        g_getMouseState(&x, &y);
        Put<std::uint32_t>(event, 0, SdlMouseWheel);
        Put<std::uint32_t>(event, 4, g_getTicks());
        Put<std::uint32_t>(event, 8, FocusWindowId());
        Put<std::int32_t>(event, 20, notch);                        // y
        Put<float>(event, 32, static_cast<float>(notch));           // preciseY (SDL >= 2.0.18)
        Put<std::int32_t>(event, 36, x);                            // mouseX (SDL >= 2.26)
        Put<std::int32_t>(event, 40, y);
        return Push(event, "mouse wheel");
    }

    bool PushButton(int button, int x, int y, bool down)
    {
        unsigned char event[SdlEventSize]{};
        Put<std::uint32_t>(event, 0, down ? SdlMouseButtonDown : SdlMouseButtonUp);
        Put<std::uint32_t>(event, 4, g_getTicks());
        Put<std::uint32_t>(event, 8, FocusWindowId());
        event[16] = static_cast<unsigned char>(button);
        event[17] = down ? 1 : 0;
        event[18] = 1;                                              // clicks
        Put<std::int32_t>(event, 20, x);
        Put<std::int32_t>(event, 24, y);
        return Push(event, down ? "mouse down" : "mouse up");
    }

    // ===========================================================================
    // Script runner
    // ===========================================================================
    const char* OpName(testbot::Op op)
    {
        switch (op)
        {
        case testbot::Op::Wait: return "wait";
        case testbot::Op::WaitWorld: return "wait_world";
        case testbot::Op::Key: return "key";
        case testbot::Op::KeyDown: return "key_down";
        case testbot::Op::KeyUp: return "key_up";
        case testbot::Op::Mod: return "mod";
        case testbot::Op::Wheel: return "wheel";
        case testbot::Op::Click: return "click";
        case testbot::Op::ExpectBarSet: return "expect_bar_set";
        case testbot::Op::Log: return "log";
        }
        return "?";
    }

    void Finish(const char* why)
    {
        g_running = false;
        unsigned long blocked = 0;
        for (const BlockedMethod& method : g_blocked)
        {
            blocked += static_cast<unsigned long>(method.calls);
        }
        Log("script %s after %zu/%zu steps in %llu ms: %u check(s), %u failure(s) -> %s. Achievements/stats stay blocked until the game exits (%lu call(s) blocked so far).",
            why, g_stepIndex, g_steps.size(), NowMs() - g_startedMs, g_checks, g_failures,
            g_failures == 0 && std::strcmp(why, "finished") == 0 ? "PASS" : "FAIL", blocked);
    }

    void NextStep()
    {
        ++g_stepIndex;
        g_phase = 0;
        g_counter = 0;
    }

    void Fail(const testbot::Step& step, const char* message, bool abort)
    {
        ++g_failures;
        Log("FAIL line %d (%s): %s", step.line, OpName(step.op), message);
        if (abort || g_stopOnFailure)
        {
            Finish("aborted");
        }
        else
        {
            NextStep();
        }
    }

    // Returns true when the runner should keep processing steps this tick.
    bool RunStep(const testbot::Step& step, unsigned long long now)
    {
        switch (step.op)
        {
        case testbot::Op::Wait:
            if (g_phase == 0)
            {
                g_deadline = now + static_cast<unsigned long long>(step.a);
                g_phase = 1;
            }
            if (now < g_deadline)
            {
                return false;
            }
            NextStep();
            return true;

        case testbot::Op::WaitWorld:
            if (g_phase == 0)
            {
                g_deadline = now + static_cast<unsigned long long>(step.a);
                g_phase = 1;
            }
            if (InWorld())
            {
                EnsureStatsObjectPatched();
                Log("line %d: in the game world (bar set %ld)", step.line, g_barSet);
                NextStep();
                return true;
            }
            if (now >= g_deadline)
            {
                Fail(step, g_originalUpdate == nullptr ? "world detection is off on this build" : "timed out", true);
            }
            return false;

        case testbot::Op::Key:
            if (g_phase == 0)
            {
                PushKey(step.key, true);
                g_deadline = now + static_cast<unsigned long long>(step.b);
                g_phase = 1;
                return false;
            }
            if (now < g_deadline)
            {
                return false;
            }
            PushKey(step.key, false);
            NextStep();
            return false;

        case testbot::Op::KeyDown:
        case testbot::Op::KeyUp:
            PushKey(step.key, step.op == testbot::Op::KeyDown);
            NextStep();
            return false;

        case testbot::Op::Mod:
            g_setModState(static_cast<int>(step.a));
            NextStep();
            return true;

        case testbot::Op::Wheel:
        {
            const int direction = step.a > 0 ? 1 : -1;
            PushWheel(direction);
            if (++g_counter >= (step.a > 0 ? step.a : -step.a))
            {
                NextStep();
            }
            return false;   // one notch per frame
        }

        case testbot::Op::Click:
            if (g_phase == 0)
            {
                void* window = FocusWindow();
                if (window == nullptr)
                {
                    Fail(step, "no focused game window", false);
                    return true;
                }
                g_warpMouse(window, static_cast<int>(step.a), static_cast<int>(step.b));
                g_phase = 1;
                return false;
            }
            if (g_phase == 1)
            {
                PushButton(static_cast<int>(step.c), static_cast<int>(step.a), static_cast<int>(step.b), true);
                g_phase = 2;
                return false;
            }
            PushButton(static_cast<int>(step.c), static_cast<int>(step.a), static_cast<int>(step.b), false);
            NextStep();
            return false;

        case testbot::Op::ExpectBarSet:
        {
            ++g_checks;
            if (!InWorld())
            {
                Fail(step, "not in the game world", false);
                return true;
            }
            const long actual = g_barSet;
            if (actual != step.a)
            {
                char message[96];
                _snprintf_s(message, sizeof(message), _TRUNCATE, "bar set is %ld, expected %lld", actual, step.a);
                Fail(step, message, false);
                return true;
            }
            Log("PASS line %d: bar set is %ld", step.line, actual);
            NextStep();
            return true;
        }

        case testbot::Op::Log:
            Log("mark line %d: %s", step.line, step.text.c_str());
            NextStep();
            return true;
        }
        NextStep();
        return true;
    }

    // Called on the game thread once per frame (after the event queue drains).
    void Tick()
    {
        if (!g_running)
        {
            return;
        }
        const unsigned long long now = NowMs();
        for (int budget = 0; budget < 32 && g_running; ++budget)
        {
            if (g_stepIndex >= g_steps.size())
            {
                Finish("finished");
                return;
            }
            if (!RunStep(g_steps[g_stepIndex], now))
            {
                return;
            }
        }
    }

    int PollEventHook(void* event)
    {
        const int result = g_originalPollEvent(event);
        if (result == 0)
        {
            Tick();
        }
        return result;
    }

    // ===========================================================================
    // Setup
    // ===========================================================================
    bool ValidScriptName(const wchar_t* name)
    {
        if (name[0] == L'\0' || wcsstr(name, L"..") != nullptr)
        {
            return false;
        }
        for (const wchar_t* c = name; *c != L'\0'; ++c)
        {
            if (!((*c >= L'a' && *c <= L'z') || (*c >= L'A' && *c <= L'Z') || (*c >= L'0' && *c <= L'9')
                    || *c == L'_' || *c == L'-' || *c == L'.'))
            {
                return false;
            }
        }
        return true;
    }

    bool LoadScript(const wchar_t* iniPath)
    {
        wchar_t name[64]{};
        GetPrivateProfileStringW(L"TestBot", L"Script", L"TestBot.script", name, 64, iniPath);
        if (!ValidScriptName(name))
        {
            Log("Script=%S is not a plain file name in the plugin folder; bot will not run", name);
            return false;
        }
        wchar_t path[MAX_PATH * 2];
        swprintf_s(path, L"%s%s", g_extender->pluginDirectory, name);
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            Log("cannot open script %S (error %lu); bot will not run", path, GetLastError());
            return false;
        }
        LARGE_INTEGER size{};
        std::string text;
        bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0
            && static_cast<unsigned long long>(size.QuadPart) <= testbot::MaxScriptBytes;
        if (ok)
        {
            text.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            ok = text.empty() || (ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) != FALSE
                && read == text.size());
        }
        CloseHandle(file);
        if (!ok)
        {
            Log("cannot read script %S (missing, unreadable or over 64 KiB); bot will not run", path);
            return false;
        }
        testbot::ParseResult parsed = testbot::ParseScript(text);
        if (!parsed.ok)
        {
            Log("script %S line %d: %s; bot will not run", path, parsed.errorLine, parsed.error.c_str());
            return false;
        }
        if (parsed.steps.empty())
        {
            Log("script %S has no steps; bot will not run", path);
            return false;
        }
        g_steps = std::move(parsed.steps);
        Log("script %S loaded: %zu step(s)", path, g_steps.size());
        return true;
    }

    bool InstallStatsBlocking(unsigned char* base)
    {
        const HMODULE steamApi = GetModuleHandleW(L"steam_api64.dll");
        if (steamApi == nullptr)
        {
            Log("steam_api64.dll is not loaded; cannot guarantee achievements are blocked");
            return false;
        }
        for (BlockedMethod& method : g_blocked)
        {
            const void* code = reinterpret_cast<const void*>(GetProcAddress(steamApi, method.exportName));
            method.offset = code == nullptr ? -1 : GuardedDecodeExport(code);
            if (method.offset < 0 || method.offset / 8 + 1 > static_cast<int>(MinVtableSlots))
            {
                Log("steam_api64.dll!%s %s; cannot guarantee achievements are blocked",
                    method.exportName, code == nullptr ? "is missing" : "has an unexpected shape");
                return false;
            }
        }
        g_getHSteamUser = reinterpret_cast<GetHSteamUserFn>(GetProcAddress(steamApi, "SteamAPI_GetHSteamUser"));
        GuardedFindStatsVersionString(base, soulash_plugins::MappedImageSize(base));
        void** slot = GuardedFindImportSlot(base, "steam_api64.dll", "SteamInternal_FindOrCreateUserInterface");
        if (slot == nullptr || *slot == nullptr)
        {
            Log("the game does not import steam_api64.dll!SteamInternal_FindOrCreateUserInterface; cannot guarantee achievements are blocked");
            return false;
        }
        g_originalFindInterface = reinterpret_cast<FindOrCreateUserInterfaceFn>(*slot);
        void* hook = reinterpret_cast<void*>(&FindOrCreateUserInterfaceHook);
        if (!g_extender->patchMemory(g_extender->pluginHandle, slot, &hook, sizeof(hook)))
        {
            g_originalFindInterface = nullptr;
            Log("patching the SteamInternal_FindOrCreateUserInterface import failed; cannot guarantee achievements are blocked");
            return false;
        }
        char offsets[256];
        std::size_t used = 0;
        for (const BlockedMethod& method : g_blocked)
        {
            const int written = _snprintf_s(offsets + used, sizeof(offsets) - used, _TRUNCATE, "%s%s@0x%X",
                used == 0 ? "" : " ", method.label, method.offset);
            used += written < 0 ? 0 : static_cast<std::size_t>(written);
        }
        Log("achievement/stat blocking armed (import hooked; game interface '%s'); stubbed: %s",
            g_statsVersion[0] == '\0' ? "?" : g_statsVersion, offsets);
        return true;
    }

    bool ResolveSdl()
    {
        const HMODULE sdl = GetModuleHandleW(L"SDL2.dll");
        if (sdl == nullptr)
        {
            return false;
        }
        g_pushEvent = reinterpret_cast<SdlPushEventFn>(GetProcAddress(sdl, "SDL_PushEvent"));
        g_getTicks = reinterpret_cast<SdlGetTicksFn>(GetProcAddress(sdl, "SDL_GetTicks"));
        g_getModState = reinterpret_cast<SdlGetModStateFn>(GetProcAddress(sdl, "SDL_GetModState"));
        g_setModState = reinterpret_cast<SdlSetModStateFn>(GetProcAddress(sdl, "SDL_SetModState"));
        g_getScancodeFromKey = reinterpret_cast<SdlGetScancodeFromKeyFn>(GetProcAddress(sdl, "SDL_GetScancodeFromKey"));
        g_getKeyboardFocus = reinterpret_cast<SdlGetFocusFn>(GetProcAddress(sdl, "SDL_GetKeyboardFocus"));
        g_getMouseFocus = reinterpret_cast<SdlGetFocusFn>(GetProcAddress(sdl, "SDL_GetMouseFocus"));
        g_getWindowId = reinterpret_cast<SdlGetWindowIdFn>(GetProcAddress(sdl, "SDL_GetWindowID"));
        g_warpMouse = reinterpret_cast<SdlWarpMouseInWindowFn>(GetProcAddress(sdl, "SDL_WarpMouseInWindow"));
        g_getMouseState = reinterpret_cast<SdlGetMouseStateFn>(GetProcAddress(sdl, "SDL_GetMouseState"));
        return g_pushEvent != nullptr && g_getTicks != nullptr && g_getModState != nullptr && g_setModState != nullptr
            && g_getScancodeFromKey != nullptr && g_getKeyboardFocus != nullptr && g_getMouseFocus != nullptr
            && g_getWindowId != nullptr && g_warpMouse != nullptr && g_getMouseState != nullptr;
    }

    void Start()
    {
        wchar_t iniPath[MAX_PATH * 2];
        swprintf_s(iniPath, L"%s%S.ini", g_extender->pluginDirectory, g_extender->pluginId);
        if (GetPrivateProfileIntW(L"TestBot", L"Enabled", 0, iniPath) != 1)
        {
            Log("disabled (default). Set [TestBot] Enabled=1 in %S to run the bot; achievements untouched, nothing hooked",
                iniPath);
            return;
        }
        g_stopOnFailure = GetPrivateProfileIntW(L"TestBot", L"StopOnFailure", 0, iniPath) != 0;
        g_logBlockedCalls = GetPrivateProfileIntW(L"TestBot", L"LogBlockedCalls", 1, iniPath) != 0;
        InitializeCriticalSection(&g_patchLock);
        InitBlockedTable();

        // 1. Blocking first, and unconditionally while the bot is enabled.
        auto* base = static_cast<unsigned char*>(g_extender->gameBase);
        g_blockingReady = InstallStatsBlocking(base);
        if (!g_blockingReady)
        {
            Log("REFUSING to run: achievement/stat blocking could not be installed (fail closed)");
            return;
        }

        // 2. Script and input.
        const bool scriptOk = LoadScript(iniPath);
        if (!ResolveSdl())
        {
            Log("SDL2.dll functions not found; bot will not run (blocking stays on)");
            return;
        }
        InstallWorldDetection();
        void** pollSlot = GuardedFindImportSlot(base, "SDL2.dll", "SDL_PollEvent");
        if (pollSlot == nullptr || *pollSlot == nullptr)
        {
            Log("IAT slot for SDL2.dll!SDL_PollEvent not found; bot will not run (blocking stays on)");
            return;
        }
        g_originalPollEvent = reinterpret_cast<SdlPollEventFn>(*pollSlot);
        void* pollHook = reinterpret_cast<void*>(&PollEventHook);
        if (!g_extender->patchMemory(g_extender->pluginHandle, pollSlot, &pollHook, sizeof(pollHook)))
        {
            g_originalPollEvent = nullptr;
            Log("patching the SDL_PollEvent import failed; bot will not run (blocking stays on)");
            return;
        }
        if (!scriptOk)
        {
            return;
        }
        g_startedMs = NowMs();
        g_running = true;
        Log("bot ACTIVE: %zu step(s), stop on failure=%d; achievements and stats are blocked for this session",
            g_steps.size(), g_stopOnFailure ? 1 : 0);
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
        || extender->gameBase == nullptr)
    {
        return FALSE;
    }
    g_extender = extender;
    Start();
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
