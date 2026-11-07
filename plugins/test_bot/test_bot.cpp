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
// VerifyAbilityBarLayout), and, only when the script has snapshot steps, the
// IAT entry for SDL2.dll!SDL_GL_SwapWindow (glReadPixels of the finished back
// buffer, GL state saved/restored; PNGs written on a worker thread), and, only
// when the script names screens, slot 7 (draw) of every verified *Screen
// vtable (pass-through thunk recording `this`; see InstallScreenDetection).
//
// Output: <game>\SoulashExtender\TestBot\<UTC run id>\results.txt + snapshot
// PNGs; <game>\SoulashExtender\TestBot\latest.txt names the newest run. Live
// feedback while the game runs: <game>\SoulashExtender\TestBot\status.json
// (state, current step, what it waits for, screen, counters, last log lines;
// rewritten atomically on every change and at least once a second) and
// live.png (the current frame, refreshed every LiveFrameMs; 0 turns it off).
// Input (Inbox=1): *.script files dropped into <game>\SoulashExtender\TestBot\
// inbox run, in name order, whenever no script is running (each is moved into
// the run folder as it is taken); a file named `stop` there aborts the running
// script. Each script is a "job" with its own PASS/FAIL line.
#include "plugin_api.h"
#include "../common/ability_bar_layout.hpp"
#include "../common/pe_original.hpp"
#include "../common/plugin_status.hpp"
#include "../common/png_writer.hpp"
#include "../common/test_bot_script.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
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
    using SdlGetWindowSizeFn = void (*)(void* window, int* width, int* height);
    constexpr std::uint32_t SdlMouseMotion = 0x400, SdlTextInput = 0x303;
    constexpr int DragSteps = 10;

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
    soulash_plugins::StartupReason g_startupReason;
    bool g_enabled = false;
    bool g_stopOnFailure = false;
    bool g_logBlockedCalls = true;
    constexpr int DefaultLiveFrameMs = 2000;
    constexpr int MinLiveFrameMs = 250;
    unsigned long long g_liveFrameMs = 0;     // LiveFrameMs: refresh <TestBot>\live.png this often (0 = off)
    bool g_inbox = true;                      // Inbox: run *.script files dropped into <TestBot>\inbox
    bool g_ready = false;                     // blocking, SDL and the poll hook are in place: scripts can run
    unsigned char* g_gameBase = nullptr;

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
    std::size_t g_announcedStep = static_cast<std::size_t>(-1);

    // Run folder <game>\SoulashExtender\TestBot\<UTC run id>\ with results.txt
    // (every TestBot log line, step starts, errors.log extracts) and snapshots.
    wchar_t g_runDirectory[MAX_PATH * 2] = L"";
    wchar_t g_testBotDirectory[MAX_PATH * 2] = L"";    // <game>\SoulashExtender\TestBot
    HANDLE g_results = INVALID_HANDLE_VALUE;
    SRWLOCK g_resultsLock = SRWLOCK_INIT;
    unsigned long long g_resultsStartMs = 0;

    // The last log lines, for status.json (guarded by g_resultsLock).
    constexpr std::size_t LogTailLines = 16;
    std::string g_logTail[LogTailLines];
    std::size_t g_logTailNext = 0;
    std::size_t g_logTailCount = 0;

    // Live status (<TestBot>\status.json), written on the game thread only.
    enum class RunState
    {
        NotRunning,     // the bot cannot run scripts this session (blocking refused, SDL hooks missing)
        Idle,           // ready, no script yet: waiting for the inbox
        Running,
        Finished,
        Aborted,
        Stopped         // by a `stop` file in the inbox
    };
    RunState g_runState = RunState::NotRunning;
    const char* g_result = nullptr;                   // "PASS" / "FAIL" / "STOPPED" once the script has ended

    // Jobs: the INI script and every inbox script, in order.
    unsigned int g_jobIndex = 0;                      // 0 = no job yet
    std::string g_jobName;
    const char* g_jobSource = "";                     // "ini" / "inbox"
    unsigned int g_jobsDone = 0;
    unsigned int g_inboxPending = 0;
    unsigned long long g_inboxCheckedMs = 0;
    std::string g_lastJobError;                       // an inbox script that could not be run
    wchar_t g_inboxStuck[MAX_PATH] = L"";             // a taken file that could neither be moved nor deleted
    bool g_captureInstallTried = false;
    bool g_screenInstallTried = false;
    std::string g_notRunningReason;
    std::string g_lastFailure;
    unsigned long long g_stepStartedMs = 0;
    std::atomic<bool> g_statusDirty{false};           // set by Log() from any thread
    bool g_statusForce = false;
    unsigned long long g_statusWrittenMs = 0;
    unsigned long long g_loopTicks = 0;
    unsigned long long g_loopRateMs = 0;
    unsigned int g_loopHz = 0;                        // event-loop iterations per second
    std::atomic<unsigned long> g_snapshotsWritten{0};

    void ResultRaw(const char* data, std::size_t size)
    {
        if (g_results == INVALID_HANDLE_VALUE || size == 0)
        {
            return;
        }
        AcquireSRWLockExclusive(&g_resultsLock);
        DWORD written = 0;
        WriteFile(g_results, data, static_cast<DWORD>(size), &written, nullptr);
        ReleaseSRWLockExclusive(&g_resultsLock);
    }

    // One stamped line: into results.txt and the log tail that status.json shows.
    void ResultLine(const char* line)
    {
        if (g_resultsStartMs == 0)
        {
            return;   // before the run folder exists only proxy-crash.log gets the line
        }
        char text[640];
        const int length = _snprintf_s(text, sizeof(text) - 2, _TRUNCATE, "[%9llu ms] %s",
            GetTickCount64() - g_resultsStartMs, line);
        const std::size_t size = length < 0 ? std::strlen(text) : static_cast<std::size_t>(length);
        AcquireSRWLockExclusive(&g_resultsLock);
        try
        {
            g_logTail[g_logTailNext].assign(text, size);
            g_logTailNext = (g_logTailNext + 1) % LogTailLines;
            if (g_logTailCount < LogTailLines)
            {
                ++g_logTailCount;
            }
        }
        catch (...)
        {
        }
        if (g_results != INVALID_HANDLE_VALUE)
        {
            text[size] = '\r';
            text[size + 1] = '\n';
            DWORD written = 0;
            WriteFile(g_results, text, static_cast<DWORD>(size + 2), &written, nullptr);
        }
        ReleaseSRWLockExclusive(&g_resultsLock);
        g_statusDirty.store(true);
    }

    void Log(const char* format, ...)
    {
        char line[480];
        va_list args;
        va_start(args, format);
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
        va_end(args);
        g_startupReason.Capture(line);
        ResultLine(line);
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
            const std::size_t imageSize = soulash_plugins::MappedImageSize(image);
            // AbilityBarProbe/AbilityBarWheel load first and hook slot 1: verify the game's own code.
            layout = soulash_plugins::VerifyAbilityBarLayout(image, imageSize,
                soulash_plugins::OriginalCodePointer(image, imageSize, slots + DrawActionBarUpdateSlot),
                soulash_plugins::OriginalCodePointer(image, imageSize, slots + DrawActionBarHandleInputSlot));
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

    bool PushButton(int button, int x, int y, bool down, int clicks = 1)
    {
        unsigned char event[SdlEventSize]{};
        Put<std::uint32_t>(event, 0, down ? SdlMouseButtonDown : SdlMouseButtonUp);
        Put<std::uint32_t>(event, 4, g_getTicks());
        Put<std::uint32_t>(event, 8, FocusWindowId());
        event[16] = static_cast<unsigned char>(button);
        event[17] = down ? 1 : 0;
        event[18] = static_cast<unsigned char>(clicks);             // 2 = double click
        Put<std::int32_t>(event, 20, x);
        Put<std::int32_t>(event, 24, y);
        return Push(event, down ? "mouse down" : "mouse up");
    }

    // SDL_MOUSEMOTION with an explicit button mask (SDL's own motion events from
    // SDL_WarpMouseInWindow do not know about injected button presses).
    bool PushMotion(int x, int y, int xrel, int yrel, std::uint32_t buttonMask)
    {
        unsigned char event[SdlEventSize]{};
        Put<std::uint32_t>(event, 0, SdlMouseMotion);
        Put<std::uint32_t>(event, 4, g_getTicks());
        Put<std::uint32_t>(event, 8, FocusWindowId());
        Put<std::uint32_t>(event, 16, buttonMask);                  // state
        Put<std::int32_t>(event, 20, x);
        Put<std::int32_t>(event, 24, y);
        Put<std::int32_t>(event, 28, xrel);
        Put<std::int32_t>(event, 32, yrel);
        return Push(event, "mouse motion");
    }

    // SDL_TEXTINPUT: what a text field receives for typed characters.
    bool PushText(char character)
    {
        unsigned char event[SdlEventSize]{};
        Put<std::uint32_t>(event, 0, SdlTextInput);
        Put<std::uint32_t>(event, 4, g_getTicks());
        Put<std::uint32_t>(event, 8, FocusWindowId());
        event[12] = static_cast<unsigned char>(character);          // text[32], NUL-terminated
        return Push(event, "text input");
    }

    bool WarpTo(int x, int y)
    {
        void* window = FocusWindow();
        if (window == nullptr)
        {
            return false;
        }
        g_warpMouse(window, x, y);
        return true;
    }

    // ===========================================================================
    // Screen detection (wait_screen / expect_screen / log_screen)
    //
    // Every full screen of the game (MainMenuScreen, GameScreen, the editor's
    // EntityMakerScreen / MapMakerScreen / ProceduralMapMakerScreen /
    // DataTableScreen / AnimatorScreen, ...) is a class with an 8-slot vtable
    // whose slot 7 draws it and starts by clearing the frame (glClearColor,
    // glClearDepth, glClear). A screen class is accepted only when its slot-7
    // function calls the game's glClear import within its first 0x120 bytes.
    // Its slot 7 then gets a 25-byte pass-through thunk:
    //     mov rax, &g_lastDrawnScreen ; mov [rax], rcx ; mov rax, previous ; jmp rax
    // which only records `this` and jumps on with every register and the stack
    // untouched, so it is exact whatever the real signature. The script runner
    // reads (and clears) the last drawn screen once per frame.
    // ===========================================================================
    constexpr unsigned int ScreenDrawSlot = 7;
    constexpr unsigned int ScreenVtableSlots = 8;
    constexpr std::size_t MaxScreens = 40;
    constexpr std::size_t ThunkSize = 32;
    constexpr std::size_t ThunkPageSize = 4096;
    constexpr std::size_t GlClearScanBytes = 0x120;
    constexpr unsigned long long ScreenFreshMs = 1000;

    const char* const ScreenClassNames[] = {
        "AnimatorScreen", "CemeteryScreen", "CharacterBasicInfoScreen", "CreditsScreen", "DataTableScreen",
        "DifficultySelectionScreen", "EntityMakerScreen", "GameScreen", "LoadingScreen", "MainMenuScreen",
        "MapMakerScreen", "ModsScreen", "OptionsScreen", "PlayerCreationScreen", "PreGameScreen",
        "ProceduralMapMakerScreen", "SettingsScreen", "StartingLocationScreen", "WorldGenerationScreen",
    };

    struct ScreenClass
    {
        const char* name;
        const void* vtable;
    };

    ScreenClass g_screens[MaxScreens]{};
    std::size_t g_screenCount = 0;
    void* volatile g_lastDrawnScreen = nullptr;     // written by the thunks
    int g_currentScreen = -1;
    unsigned long long g_currentScreenMs = 0;
    bool g_screenDetection = false;
    SdlGetWindowSizeFn g_getWindowSize = nullptr;
    SdlGetWindowSizeFn g_getDrawableSizeForLog = nullptr;

    // Does the function at `code` call through the import slot `importSlot`
    // (FF 15 rel32) within its first GlClearScanBytes? SEH-guarded.
    bool GuardedCallsImport(const unsigned char* code, const unsigned char* imageEnd, const void* importSlot)
    {
        __try
        {
            for (std::size_t i = 0; i + 6 <= GlClearScanBytes && code + i + 6 <= imageEnd; ++i)
            {
                if (code[i] == 0xFF && code[i + 1] == 0x15)
                {
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, code + i + 2, sizeof(displacement));
                    if (code + i + 6 + displacement == static_cast<const unsigned char*>(importSlot))
                    {
                        return true;
                    }
                }
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const void* GuardedReadVtable(const void* object)
    {
        __try
        {
            return *static_cast<const void* const*>(object);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ScriptUsesScreens()
    {
        for (const testbot::Step& step : g_steps)
        {
            if (step.op == testbot::Op::WaitScreen || step.op == testbot::Op::ExpectScreen
                || step.op == testbot::Op::LogScreen)
            {
                return true;
            }
        }
        return false;
    }

    void InstallScreenDetection(unsigned char* base)
    {
        const std::size_t imageSize = soulash_plugins::MappedImageSize(base);
        void** glClearSlot = GuardedFindImportSlot(base, "OPENGL32.dll", "glClear");
        if (imageSize == 0 || glClearSlot == nullptr)
        {
            Log("screen detection off: %s; wait_screen/expect_screen will FAIL",
                imageSize == 0 ? "game image size unknown" : "no OPENGL32.dll!glClear import");
            return;
        }
        struct Pending
        {
            const char* name;
            void** slot;
            void* previous;
        };
        Pending pending[MaxScreens]{};
        std::size_t pendingCount = 0;
        std::string skipped;
        const unsigned char* imageEnd = base + imageSize;
        for (const char* name : ScreenClassNames)
        {
            void* vtable = nullptr;
            const char* why = nullptr;
            if (!g_extender->findRttiVtable(g_extender->pluginHandle, name, &vtable) || vtable == nullptr)
            {
                why = "not found";
            }
            else
            {
                void** slots = static_cast<void**>(vtable);
                bool inImage = reinterpret_cast<unsigned char*>(slots + ScreenVtableSlots) <= imageEnd;
                for (unsigned int i = 0; inImage && i < ScreenVtableSlots; ++i)
                {
                    const unsigned char* code = soulash_plugins::OriginalCodePointer(base, imageSize, slots + i);
                    inImage = code >= base && code < imageEnd;
                }
                const unsigned char* draw = inImage
                    ? soulash_plugins::OriginalCodePointer(base, imageSize, slots + ScreenDrawSlot) : nullptr;
                if (!inImage)
                {
                    why = "vtable shorter than 8 slots";
                }
                else if (!GuardedCallsImport(draw, imageEnd, glClearSlot))
                {
                    why = "slot 7 does not draw (no glClear)";
                }
                else if (pendingCount < MaxScreens)
                {
                    pending[pendingCount++] = {name, slots + ScreenDrawSlot, slots[ScreenDrawSlot]};
                    g_screens[g_screenCount++] = {name, vtable};
                }
            }
            if (why != nullptr)
            {
                skipped += std::string(skipped.empty() ? "" : ", ") + name + " (" + why + ")";
            }
        }
        if (pendingCount == 0)
        {
            Log("screen detection off: no screen class verified (%s); wait_screen/expect_screen will FAIL", skipped.c_str());
            g_screenCount = 0;
            return;
        }

        // Write every thunk, make the page execute-only, then patch the slots.
        auto* page = static_cast<unsigned char*>(VirtualAlloc(nullptr, ThunkPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (page == nullptr || pendingCount * ThunkSize > ThunkPageSize)
        {
            Log("screen detection off: cannot allocate thunks (error %lu)", GetLastError());
            g_screenCount = 0;
            return;
        }
        const auto seen = reinterpret_cast<std::uint64_t>(&g_lastDrawnScreen);
        for (std::size_t i = 0; i < pendingCount; ++i)
        {
            unsigned char* thunk = page + i * ThunkSize;
            const auto previous = reinterpret_cast<std::uint64_t>(pending[i].previous);
            std::memset(thunk, 0xCC, ThunkSize);
            thunk[0] = 0x48; thunk[1] = 0xB8;                       // mov rax, imm64
            std::memcpy(thunk + 2, &seen, 8);
            thunk[10] = 0x48; thunk[11] = 0x89; thunk[12] = 0x08;   // mov [rax], rcx
            thunk[13] = 0x48; thunk[14] = 0xB8;                     // mov rax, imm64
            std::memcpy(thunk + 15, &previous, 8);
            thunk[23] = 0xFF; thunk[24] = 0xE0;                     // jmp rax
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(page, ThunkPageSize, PAGE_EXECUTE_READ, &oldProtect))
        {
            Log("screen detection off: cannot make thunks executable (error %lu)", GetLastError());
            g_screenCount = 0;
            return;
        }
        FlushInstructionCache(GetCurrentProcess(), page, ThunkPageSize);
        std::string hooked;
        for (std::size_t i = 0; i < pendingCount; ++i)
        {
            void* thunk = page + i * ThunkSize;
            if (g_extender->patchMemory(g_extender->pluginHandle, pending[i].slot, &thunk, sizeof(thunk)))
            {
                hooked += std::string(hooked.empty() ? "" : ", ") + pending[i].name;
            }
            else
            {
                skipped += std::string(skipped.empty() ? "" : ", ") + pending[i].name + " (patch failed)";
                for (std::size_t k = 0; k < g_screenCount; ++k)
                {
                    if (g_screens[k].name == pending[i].name)
                    {
                        g_screens[k].vtable = nullptr;   // never matches
                    }
                }
            }
        }
        g_screenDetection = !hooked.empty();
        Log("screen detection %s: draw slot hooked on %s", g_screenDetection ? "on" : "off", hooked.empty() ? "nothing" : hooked.c_str());
        if (!skipped.empty())
        {
            Log("screen classes not hooked: %s", skipped.c_str());
        }
    }

    int FindScreen(const std::string& name)
    {
        for (std::size_t i = 0; i < g_screenCount; ++i)
        {
            if (g_screens[i].vtable != nullptr && name == g_screens[i].name)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Once per frame on the game thread.
    void UpdateCurrentScreen(unsigned long long now)
    {
        if (!g_screenDetection)
        {
            return;
        }
        void* drawn = InterlockedExchangePointer(const_cast<void**>(&g_lastDrawnScreen), nullptr);
        if (drawn == nullptr)
        {
            return;
        }
        const void* vtable = GuardedReadVtable(drawn);
        for (std::size_t i = 0; i < g_screenCount; ++i)
        {
            if (g_screens[i].vtable != nullptr && g_screens[i].vtable == vtable)
            {
                if (static_cast<int>(i) != g_currentScreen)
                {
                    Log("screen: %s -> %s", g_currentScreen < 0 ? "(none)" : g_screens[g_currentScreen].name,
                        g_screens[i].name);
                }
                g_currentScreen = static_cast<int>(i);
                g_currentScreenMs = now;
                return;
            }
        }
    }

    // The screen drawn within the last second, or nullptr.
    const char* CurrentScreen(unsigned long long now)
    {
        return g_currentScreen >= 0 && now - g_currentScreenMs <= ScreenFreshMs ? g_screens[g_currentScreen].name : nullptr;
    }

    // ===========================================================================
    // Run folder, screen capture (snapshot) and errors.log extraction (mods_errors)
    // ===========================================================================
    bool OpenRunFolder()
    {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        wchar_t base[MAX_PATH * 2];
        swprintf_s(base, L"%sSoulashExtender", g_extender->gameDirectory);
        CreateDirectoryW(base, nullptr);
        wcscat_s(base, L"\\TestBot");
        CreateDirectoryW(base, nullptr);
        wcscpy_s(g_testBotDirectory, base);
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            swprintf_s(g_runDirectory, attempt == 0 ? L"%s\\%04u%02u%02u-%02u%02u%02uZ" : L"%s\\%04u%02u%02u-%02u%02u%02uZ-%d",
                base, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, attempt + 1);
            if (CreateDirectoryW(g_runDirectory, nullptr))
            {
                wchar_t path[MAX_PATH * 2];
                swprintf_s(path, L"%s\\results.txt", g_runDirectory);
                g_resultsStartMs = GetTickCount64();
                g_results = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                // latest.txt: where the newest run is, for whoever reads the results.
                swprintf_s(path, L"%s\\latest.txt", base);
                HANDLE latest = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (latest != INVALID_HANDLE_VALUE)
                {
                    char text[MAX_PATH * 4];
                    const int length = _snprintf_s(text, sizeof(text), _TRUNCATE, "%S\r\n", g_runDirectory);
                    DWORD written = 0;
                    if (length > 0)
                    {
                        WriteFile(latest, text, static_cast<DWORD>(length), &written, nullptr);
                    }
                    CloseHandle(latest);
                }
                Log("run folder %S (results.txt%s)", g_runDirectory,
                    g_results == INVALID_HANDLE_VALUE ? " could NOT be created" : "");
                Log("game exe sha256 %s", g_extender->gameExecutableSha256 != nullptr ? g_extender->gameExecutableSha256 : "unknown");
                return true;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
            {
                break;
            }
        }
        Log("cannot create a run folder under %S (error %lu); no results file, snapshots disabled", base, GetLastError());
        g_runDirectory[0] = L'\0';
        return false;
    }

    // ---- snapshot: SDL_GL_SwapWindow import hook + glReadPixels of the back buffer
    using SdlGlSwapWindowFn = void (*)(void* window);
    using SdlGlGetDrawableSizeFn = void (*)(void* window, int* width, int* height);
    using GlGetIntegervFn = void (WINAPI*)(unsigned int name, int* data);
    using GlPixelStoreiFn = void (WINAPI*)(unsigned int name, int value);
    using GlReadBufferFn = void (WINAPI*)(unsigned int mode);
    using GlReadPixelsFn = void (WINAPI*)(int x, int y, int width, int height, unsigned int format, unsigned int type, void* data);
    using GlGetErrorFn = unsigned int (WINAPI*)();
    using GlBindFn = void (WINAPI*)(unsigned int target, unsigned int name);
    using WglGetProcAddressFn = PROC (WINAPI*)(LPCSTR name);
    using WglGetCurrentContextFn = HGLRC (WINAPI*)();

    constexpr unsigned int GlPackRowLength = 0x0D02, GlPackSkipRows = 0x0D03, GlPackSkipPixels = 0x0D04,
        GlPackAlignment = 0x0D05, GlReadBufferName = 0x0C02, GlBack = 0x0405, GlRgb = 0x1907, GlUnsignedByte = 0x1401,
        GlReadFramebuffer = 0x8CA8, GlReadFramebufferBinding = 0x8CAA, GlPixelPackBuffer = 0x88EB,
        GlPixelPackBufferBinding = 0x88ED;
    constexpr long MaxWritersInFlight = 3;
    constexpr unsigned long long CaptureStallMs = 5000;

    SdlGlSwapWindowFn g_originalSwapWindow = nullptr;
    SdlGlGetDrawableSizeFn g_getDrawableSize = nullptr;
    GlGetIntegervFn g_glGetIntegerv = nullptr;
    GlPixelStoreiFn g_glPixelStorei = nullptr;
    GlReadBufferFn g_glReadBuffer = nullptr;
    GlReadPixelsFn g_glReadPixels = nullptr;
    GlGetErrorFn g_glGetError = nullptr;
    WglGetProcAddressFn g_wglGetProcAddress = nullptr;
    WglGetCurrentContextFn g_wglGetCurrentContext = nullptr;
    GlBindFn g_glBindFramebuffer = nullptr;
    GlBindFn g_glBindBuffer = nullptr;
    bool g_glExtensionsResolved = false;
    soulash_plugins::png::Compress2Fn g_compress2 = nullptr;
    bool g_captureReady = false;

    // Shared between the script runner and the swap hook (normally the same thread).
    std::atomic<bool> g_captureActive{false};
    std::atomic<int> g_captureDone{0};
    std::atomic<bool> g_captureFailed{false};
    std::atomic<unsigned long long> g_captureNextDue{0};
    std::atomic<unsigned long long> g_captureLastProgress{0};
    std::atomic<long> g_writersInFlight{0};
    int g_captureTotal = 0;
    unsigned long long g_captureInterval = 0;
    int g_captureLine = 0;
    char g_captureName[testbot::MaxSnapshotNameLength + 1] = "";
    char g_captureError[160] = "";

    struct SnapshotJob
    {
        std::vector<unsigned char> pixels;
        int width = 0;
        int height = 0;
        int line = 0;
        bool live = false;                  // live.png: written to `path`, then renamed over `finalPath`
        wchar_t path[MAX_PATH * 2]{};
        wchar_t finalPath[MAX_PATH * 2]{};
    };

    // Live frame (LiveFrameMs): the current frame as <TestBot>\live.png, one in flight at a time.
    std::atomic<bool> g_liveInFlight{false};
    std::atomic<unsigned long long> g_liveNextDue{0};
    std::atomic<unsigned long long> g_liveWrittenMs{0};
    std::atomic<unsigned long> g_liveFrames{0};
    std::atomic<unsigned long> g_liveErrors{0};
    std::atomic<int> g_liveWidth{0};
    std::atomic<int> g_liveHeight{0};

    bool WriteWholeFile(const wchar_t* path, const void* data, std::size_t size)
    {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD written = 0;
        const bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) != FALSE && written == size;
        CloseHandle(file);
        return ok;
    }

    // Writes `temporary`, then renames it over `path`, so a reader never sees a
    // partial file. If the rename is refused (a reader holds `path` open without
    // FILE_SHARE_DELETE), `path` is written in place instead.
    bool PublishFile(const wchar_t* temporary, const wchar_t* path, const void* data, std::size_t size)
    {
        if (!WriteWholeFile(temporary, data, size))
        {
            return false;
        }
        if (MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING))
        {
            return true;
        }
        return WriteWholeFile(path, data, size);
    }

    DWORD WINAPI SnapshotWriter(void* parameter)
    {
        SnapshotJob* job = static_cast<SnapshotJob*>(parameter);
        std::vector<unsigned char> png;
        try
        {
            png = soulash_plugins::png::EncodeRgb(job->pixels.data(), job->width, job->height, true, g_compress2, 1);
        }
        catch (...)
        {
            png.clear();
        }
        const bool ok = !png.empty()
            && (job->live ? PublishFile(job->path, job->finalPath, png.data(), png.size())
                          : WriteWholeFile(job->path, png.data(), png.size()));
        const DWORD error = ok ? 0 : GetLastError();
        const wchar_t* target = job->live ? job->finalPath : job->path;
        const wchar_t* name = wcsrchr(target, L'\\');
        name = name != nullptr ? name + 1 : target;
        if (job->live)
        {
            if (ok)
            {
                g_liveWidth.store(job->width);
                g_liveHeight.store(job->height);
                g_liveWrittenMs.store(NowMs());
                if (g_liveFrames.fetch_add(1) == 0)
                {
                    Log("live frame: %S refreshed every %llu ms (%dx%d, %zu bytes)", name, g_liveFrameMs, job->width,
                        job->height, png.size());
                }
            }
            else if (g_liveErrors.fetch_add(1) == 0)
            {
                Log("live frame: FAILED to write %S (error %lu); will keep trying", target, error);
            }
            g_liveInFlight.store(false);
        }
        else if (ok)
        {
            g_snapshotsWritten.fetch_add(1);
            Log("snapshot line %d: wrote %S (%dx%d, %zu bytes)", job->line, name, job->width, job->height, png.size());
        }
        else
        {
            Log("snapshot line %d: FAILED to write %S (error %lu)", job->line, job->path, error);
        }
        delete job;
        g_writersInFlight.fetch_sub(1);
        return 0;
    }

    void CaptureFailed(const char* message)
    {
        strcpy_s(g_captureError, message);
        g_captureFailed.store(true);
    }

    // All GL calls in one SEH-guarded function (no C++ objects in here). Saves
    // and restores every bit of state it touches; the game never notices.
    bool GuardedReadBackBuffer(int width, int height, unsigned char* pixels, unsigned int* glError, bool* bindingsOk)
    {
        __try
        {
            for (int i = 0; i < 16 && g_glGetError() != 0; ++i)
            {
            }
            int alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0, readBuffer = 0, readFramebuffer = 0, packBuffer = 0;
            g_glGetIntegerv(GlPackAlignment, &alignment);
            g_glGetIntegerv(GlPackRowLength, &rowLength);
            g_glGetIntegerv(GlPackSkipRows, &skipRows);
            g_glGetIntegerv(GlPackSkipPixels, &skipPixels);
            g_glGetIntegerv(GlReadBufferName, &readBuffer);
            g_glGetIntegerv(GlReadFramebufferBinding, &readFramebuffer);   // INVALID_ENUM before GL 3: stays 0
            g_glGetIntegerv(GlPixelPackBufferBinding, &packBuffer);        // INVALID_ENUM before GL 2.1: stays 0
            for (int i = 0; i < 16 && g_glGetError() != 0; ++i)
            {
            }
            *bindingsOk = (readFramebuffer == 0 || g_glBindFramebuffer != nullptr)
                && (packBuffer == 0 || g_glBindBuffer != nullptr);
            if (!*bindingsOk)
            {
                return true;
            }
            if (readFramebuffer != 0)
            {
                g_glBindFramebuffer(GlReadFramebuffer, 0);
            }
            if (packBuffer != 0)
            {
                g_glBindBuffer(GlPixelPackBuffer, 0);
            }
            g_glPixelStorei(GlPackAlignment, 1);
            g_glPixelStorei(GlPackRowLength, 0);
            g_glPixelStorei(GlPackSkipRows, 0);
            g_glPixelStorei(GlPackSkipPixels, 0);
            g_glReadBuffer(GlBack);
            g_glReadPixels(0, 0, width, height, GlRgb, GlUnsignedByte, pixels);
            *glError = g_glGetError();
            g_glReadBuffer(static_cast<unsigned int>(readBuffer));
            g_glPixelStorei(GlPackAlignment, alignment);
            g_glPixelStorei(GlPackRowLength, rowLength);
            g_glPixelStorei(GlPackSkipRows, skipRows);
            g_glPixelStorei(GlPackSkipPixels, skipPixels);
            if (packBuffer != 0)
            {
                g_glBindBuffer(GlPixelPackBuffer, static_cast<unsigned int>(packBuffer));
            }
            if (readFramebuffer != 0)
            {
                g_glBindFramebuffer(GlReadFramebuffer, static_cast<unsigned int>(readFramebuffer));
            }
            for (int i = 0; i < 16 && g_glGetError() != 0; ++i)
            {
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    GlBindFn ResolveGlExtension(const char* name)
    {
        const PROC proc = g_wglGetProcAddress(name);
        const auto value = reinterpret_cast<std::intptr_t>(proc);
        return value >= -1 && value <= 3 ? nullptr : reinterpret_cast<GlBindFn>(proc);
    }

    // Reads the finished back buffer into a new job; nullptr with a message when it cannot.
    SnapshotJob* ReadFrame(void* window, char (&error)[160])
    {
        if (g_wglGetCurrentContext() == nullptr)
        {
            strcpy_s(error, "no current OpenGL context on the thread that presents frames");
            return nullptr;
        }
        if (!g_glExtensionsResolved)
        {
            g_glExtensionsResolved = true;
            g_glBindFramebuffer = ResolveGlExtension("glBindFramebuffer");
            g_glBindBuffer = ResolveGlExtension("glBindBuffer");
        }
        int width = 0, height = 0;
        g_getDrawableSize(window, &width, &height);
        if (width <= 0 || height <= 0 || width > soulash_plugins::png::MaxDimension
            || height > soulash_plugins::png::MaxDimension)
        {
            _snprintf_s(error, sizeof(error), _TRUNCATE, "unusable drawable size %dx%d", width, height);
            return nullptr;
        }
        SnapshotJob* job = new (std::nothrow) SnapshotJob;
        if (job == nullptr)
        {
            strcpy_s(error, "out of memory");
            return nullptr;
        }
        try
        {
            job->pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
        }
        catch (...)
        {
            delete job;
            strcpy_s(error, "out of memory for the frame buffer copy");
            return nullptr;
        }
        unsigned int glError = 0;
        bool bindingsOk = true;
        if (!GuardedReadBackBuffer(width, height, job->pixels.data(), &glError, &bindingsOk))
        {
            delete job;
            g_captureReady = false;
            strcpy_s(error, "reading the frame faulted; screen capture switched off for this session");
            return nullptr;
        }
        if (!bindingsOk || glError != 0)
        {
            delete job;
            _snprintf_s(error, sizeof(error), _TRUNCATE, bindingsOk
                ? "glReadPixels reported GL error 0x%X" : "a framebuffer/pack buffer is bound and cannot be unbound (no GL 3 entry points)", glError);
            return nullptr;
        }
        job->width = width;
        job->height = height;
        return job;
    }

    void StartWriter(SnapshotJob* job)
    {
        g_writersInFlight.fetch_add(1);
        HANDLE thread = CreateThread(nullptr, 0, SnapshotWriter, job, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
        else
        {
            SnapshotWriter(job);   // no thread: write it here (one slow frame)
        }
    }

    // snapshot step: one frame of the current series.
    void CaptureFrame(void* window)
    {
        const unsigned long long now = NowMs();
        char error[160];
        SnapshotJob* job = ReadFrame(window, error);
        if (job == nullptr)
        {
            CaptureFailed(error);
            return;
        }
        const int index = g_captureDone.load() + 1;
        job->line = g_captureLine;
        swprintf_s(job->path, L"%s\\%S_%02d.png", g_runDirectory, g_captureName, index);
        StartWriter(job);
        g_captureDone.store(index);
        g_captureNextDue.store(now + g_captureInterval);
        g_captureLastProgress.store(now);
    }

    // LiveFrameMs: the current frame for whoever watches the run. A failed read
    // (minimised window, GL error) is retried later; a fault turns capture off.
    void CaptureLiveFrame(void* window)
    {
        const unsigned long long now = NowMs();
        char error[160];
        SnapshotJob* job = ReadFrame(window, error);
        if (job == nullptr)
        {
            g_liveNextDue.store(now + (g_liveFrameMs > 1000 ? g_liveFrameMs : 1000));
            if (g_liveErrors.fetch_add(1) == 0)
            {
                Log("live frame: %s%s", error, g_captureReady ? "; will keep trying" : "");
            }
            return;
        }
        job->live = true;
        swprintf_s(job->path, L"%s\\live.tmp.png", g_testBotDirectory);
        swprintf_s(job->finalPath, L"%s\\live.png", g_testBotDirectory);
        g_liveInFlight.store(true);
        g_liveNextDue.store(now + g_liveFrameMs);
        StartWriter(job);
    }

    void SwapWindowHook(void* window)
    {
        bool captured = false;
        if (g_captureActive.load() && g_captureReady && !g_captureFailed.load() && g_captureDone.load() < g_captureTotal
            && NowMs() >= g_captureNextDue.load() && g_writersInFlight.load() < MaxWritersInFlight)
        {
            CaptureFrame(window);
            captured = true;
        }
        if (!captured && g_liveFrameMs != 0 && g_captureReady && !g_liveInFlight.load()
            && NowMs() >= g_liveNextDue.load() && g_writersInFlight.load() < MaxWritersInFlight)
        {
            CaptureLiveFrame(window);
        }
        g_originalSwapWindow(window);
    }

    bool ScriptHasSnapshots()
    {
        for (const testbot::Step& step : g_steps)
        {
            if (step.op == testbot::Op::Snapshot)
            {
                return true;
            }
        }
        return false;
    }

    void InstallScreenCapture(unsigned char* base)
    {
        const HMODULE sdl = GetModuleHandleW(L"SDL2.dll");
        const HMODULE gl = GetModuleHandleW(L"opengl32.dll");
        if (g_runDirectory[0] == L'\0' || sdl == nullptr || gl == nullptr)
        {
            Log("screen capture unavailable (%s); snapshot steps will FAIL",
                g_runDirectory[0] == L'\0' ? "no run folder" : (sdl == nullptr ? "SDL2.dll not loaded" : "opengl32.dll not loaded"));
            return;
        }
        g_getDrawableSize = reinterpret_cast<SdlGlGetDrawableSizeFn>(GetProcAddress(sdl, "SDL_GL_GetDrawableSize"));
        g_glGetIntegerv = reinterpret_cast<GlGetIntegervFn>(GetProcAddress(gl, "glGetIntegerv"));
        g_glPixelStorei = reinterpret_cast<GlPixelStoreiFn>(GetProcAddress(gl, "glPixelStorei"));
        g_glReadBuffer = reinterpret_cast<GlReadBufferFn>(GetProcAddress(gl, "glReadBuffer"));
        g_glReadPixels = reinterpret_cast<GlReadPixelsFn>(GetProcAddress(gl, "glReadPixels"));
        g_glGetError = reinterpret_cast<GlGetErrorFn>(GetProcAddress(gl, "glGetError"));
        g_wglGetProcAddress = reinterpret_cast<WglGetProcAddressFn>(GetProcAddress(gl, "wglGetProcAddress"));
        g_wglGetCurrentContext = reinterpret_cast<WglGetCurrentContextFn>(GetProcAddress(gl, "wglGetCurrentContext"));
        if (g_getDrawableSize == nullptr || g_glGetIntegerv == nullptr || g_glPixelStorei == nullptr
            || g_glReadBuffer == nullptr || g_glReadPixels == nullptr || g_glGetError == nullptr
            || g_wglGetProcAddress == nullptr || g_wglGetCurrentContext == nullptr)
        {
            Log("screen capture unavailable (SDL/OpenGL functions missing); snapshot steps will FAIL");
            return;
        }
        const HMODULE zlib = GetModuleHandleW(L"zlib1.dll");
        g_compress2 = zlib == nullptr ? nullptr
                                      : reinterpret_cast<soulash_plugins::png::Compress2Fn>(GetProcAddress(zlib, "compress2"));
        void** slot = GuardedFindImportSlot(base, "SDL2.dll", "SDL_GL_SwapWindow");
        if (slot == nullptr || *slot == nullptr)
        {
            Log("screen capture unavailable (no SDL2.dll!SDL_GL_SwapWindow import); snapshot steps will FAIL");
            return;
        }
        g_originalSwapWindow = reinterpret_cast<SdlGlSwapWindowFn>(*slot);
        void* hook = reinterpret_cast<void*>(&SwapWindowHook);
        if (!g_extender->patchMemory(g_extender->pluginHandle, slot, &hook, sizeof(hook)))
        {
            g_originalSwapWindow = nullptr;
            Log("screen capture unavailable (patching the SDL_GL_SwapWindow import failed); snapshot steps will FAIL");
            return;
        }
        g_captureReady = true;
        Log("screen capture ready: SDL_GL_SwapWindow import hooked (slot %p, previous %p), PNG %s",
            static_cast<void*>(slot), reinterpret_cast<void*>(g_originalSwapWindow),
            g_compress2 != nullptr ? "compressed with the game's zlib1.dll" : "uncompressed (zlib1.dll not loaded)");
    }

    // ---- mods_errors: <game>\logs\errors.log from its last "Loading mods:" line
    constexpr std::size_t MaxErrorsLogBytes = 4 * 1024 * 1024;
    constexpr unsigned MaxCopiedLogLines = 3000;

    // Returns false with a message when the log cannot be read.
    bool ExtractModsErrors(const testbot::Step& step, unsigned& errors, unsigned& warnings, char (&message)[160])
    {
        wchar_t path[MAX_PATH * 2];
        swprintf_s(path, L"%slogs\\errors.log", g_extender->gameDirectory);
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            _snprintf_s(message, sizeof(message), _TRUNCATE, "cannot open logs\\errors.log (error %lu)", GetLastError());
            return false;
        }
        LARGE_INTEGER size{};
        std::string text;
        bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
        if (ok)
        {
            const long long start = size.QuadPart > static_cast<long long>(MaxErrorsLogBytes)
                ? size.QuadPart - static_cast<long long>(MaxErrorsLogBytes) : 0;
            LARGE_INTEGER offset{};
            offset.QuadPart = start;
            text.resize(static_cast<std::size_t>(size.QuadPart - start));
            DWORD read = 0;
            ok = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) != FALSE
                && (text.empty() || ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) != FALSE);
            text.resize(read);
        }
        CloseHandle(file);
        if (!ok)
        {
            _snprintf_s(message, sizeof(message), _TRUNCATE, "cannot read logs\\errors.log");
            return false;
        }
        const std::size_t marker = text.rfind("Loading mods:");
        std::size_t begin = 0;
        if (marker != std::string::npos)
        {
            const std::size_t lineStart = text.rfind('\n', marker);
            begin = lineStart == std::string::npos ? 0 : lineStart + 1;
        }
        std::string block;
        char header[200];
        _snprintf_s(header, sizeof(header), _TRUNCATE, "---- line %d: logs\\errors.log, %s ----\r\n", step.line,
            marker != std::string::npos ? "from its last \"Loading mods:\" line" : "no \"Loading mods:\" line, whole file");
        block += header;
        errors = 0;
        warnings = 0;
        unsigned copied = 0, total = 0;
        std::size_t position = begin;
        while (position < text.size())
        {
            std::size_t end = text.find('\n', position);
            if (end == std::string::npos)
            {
                end = text.size();
            }
            std::string_view line(text.data() + position, end - position);
            position = end + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            ++total;
            if (line.find("[error]") != std::string_view::npos)
            {
                ++errors;
            }
            else if (line.find("[warning]") != std::string_view::npos)
            {
                ++warnings;
            }
            if (copied < MaxCopiedLogLines)
            {
                ++copied;
                block += "  | ";
                block.append(line.data(), line.size());
                block += "\r\n";
            }
        }
        if (copied < total)
        {
            block += "  | ... (" + std::to_string(total - copied) + " more line(s) not copied)\r\n";
        }
        block += "---- end of errors.log extract ----\r\n";
        ResultRaw(block.data(), block.size());
        _snprintf_s(message, sizeof(message), _TRUNCATE, "%u error line(s), %u warning line(s), %u line(s) in the pass",
            errors, warnings, total);
        return true;
    }

    // ===========================================================================
    // Live status for whoever watches the run (a person or an agent):
    // <game>\SoulashExtender\TestBot\status.json. Rewritten through a temporary
    // file and a rename (readers never see a partial file) on every change and at
    // least once a second while the game pumps events, so a stale `updated_utc`
    // means the game is not running its event loop (loading, hung, or gone).
    // ===========================================================================
    constexpr unsigned long long StatusHeartbeatMs = 1000;
    constexpr unsigned long long StatusMinIntervalMs = 100;
    constexpr unsigned long long LoopRateWindowMs = 500;
    constexpr int StatusSchema = 1;

    std::string Utf8(const wchar_t* text)
    {
        std::string out;
        const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 1)
        {
            out.resize(static_cast<std::size_t>(needed));
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
            out.pop_back();   // the terminator
        }
        return out;
    }

    void UtcNow(char (&text)[40])
    {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth,
            now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    }

    const char* RunStateName(RunState state)
    {
        switch (state)
        {
        case RunState::NotRunning: return "not_running";
        case RunState::Idle: return "idle";
        case RunState::Running: return "running";
        case RunState::Finished: return "finished";
        case RunState::Aborted: return "aborted";
        case RunState::Stopped: return "stopped";
        }
        return "?";
    }

    // What the current step is waiting for, or "" when it is not waiting.
    std::string WaitingFor(unsigned long long now)
    {
        if (!g_running || g_stepIndex >= g_steps.size())
        {
            return "";
        }
        const testbot::Step& step = g_steps[g_stepIndex];
        const unsigned long long left = g_phase == 0 ? static_cast<unsigned long long>(step.a)
                                                     : (g_deadline > now ? g_deadline - now : 0);
        char text[240];
        switch (step.op)
        {
        case testbot::Op::WaitWorld:
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                "the game world (load a save or start a new game); %llu s before the run aborts", (left + 999) / 1000);
            return text;
        case testbot::Op::WaitScreen:
        {
            const char* current = CurrentScreen(now);
            _snprintf_s(text, sizeof(text), _TRUNCATE, "screen %s (now %s); %llu s before the run aborts",
                step.text.c_str(), current != nullptr ? current : "none drawn in the last second", (left + 999) / 1000);
            return text;
        }
        case testbot::Op::Wait:
            _snprintf_s(text, sizeof(text), _TRUNCATE, "pause, %llu ms left", left);
            return text;
        case testbot::Op::Snapshot:
            if (g_phase != 0)
            {
                _snprintf_s(text, sizeof(text), _TRUNCATE, "snapshot %s: %d/%d frame(s) captured", g_captureName,
                    g_captureDone.load(), g_captureTotal);
                return text;
            }
            return "";
        default:
            return "";
        }
    }

    // Minimal JSON object builder; fields in insertion order.
    struct JsonObject
    {
        std::string text = "{";
        bool first = true;

        void Key(const char* name)
        {
            text += first ? "\"" : ",\"";
            first = false;
            text += name;
            text += "\":";
        }

        void Raw(const char* name, const std::string& value)
        {
            Key(name);
            text += value;
        }

        void Str(const char* name, std::string_view value)
        {
            Key(name);
            text += testbot::JsonQuote(value);
        }

        void StrOrNull(const char* name, const char* value)
        {
            Key(name);
            text += value != nullptr && value[0] != '\0' ? testbot::JsonQuote(value) : std::string("null");
        }

        void Num(const char* name, long long value)
        {
            Key(name);
            text += std::to_string(value);
        }

        void Bool(const char* name, bool value)
        {
            Key(name);
            text += value ? "true" : "false";
        }

        void Null(const char* name)
        {
            Key(name);
            text += "null";
        }

        std::string Close()
        {
            text += '}';
            return text;
        }
    };

    std::string JsonSize(int width, int height)
    {
        return width > 0 && height > 0 ? "[" + std::to_string(width) + "," + std::to_string(height) + "]"
                                       : std::string("null");
    }

    std::string BuildStatus(unsigned long long now)
    {
        char stamp[40];
        UtcNow(stamp);
        const std::string waiting = WaitingFor(now);
        JsonObject json;
        json.Num("schema", StatusSchema);
        json.Str("updated_utc", stamp);
        json.Num("run_ms", static_cast<long long>(now - g_resultsStartMs));
        json.Num("pid", static_cast<long long>(GetCurrentProcessId()));
        json.Str("state", g_running && !waiting.empty() ? "waiting" : RunStateName(g_runState));
        if (g_jobIndex == 0)
        {
            json.Str("reason", g_notRunningReason);
        }
        json.StrOrNull("result", g_result);
        json.Str("run_dir", Utf8(g_runDirectory));
        if (g_jobIndex != 0)
        {
            JsonObject job;
            job.Num("index", g_jobIndex);
            job.Str("name", g_jobName);
            job.Str("source", g_jobSource);
            json.Raw("job", job.Close());
        }
        else
        {
            json.Null("job");
        }
        json.Num("steps_total", static_cast<long long>(g_steps.size()));
        json.Num("steps_done", static_cast<long long>(g_stepIndex));
        if (g_running && g_stepIndex < g_steps.size())
        {
            const testbot::Step& step = g_steps[g_stepIndex];
            JsonObject current;
            current.Num("index", static_cast<long long>(g_stepIndex + 1));
            current.Num("line", step.line);
            current.Str("command", testbot::DescribeStep(step));
            current.Num("elapsed_ms", static_cast<long long>(now - g_stepStartedMs));
            json.Raw("step", current.Close());
        }
        else
        {
            json.Null("step");
        }
        json.StrOrNull("waiting_for", waiting.c_str());
        json.Num("checks", g_checks);
        json.Num("failures", g_failures);
        json.StrOrNull("last_failure", g_lastFailure.c_str());
        json.Bool("screen_detection", g_screenDetection);
        json.StrOrNull("screen", g_screenDetection ? CurrentScreen(now) : nullptr);
        json.Bool("world_detection", g_originalUpdate != nullptr);
        const bool inWorld = InWorld();
        json.Bool("in_world", inWorld);
        json.Raw("bar_set", inWorld ? std::to_string(g_barSet) : std::string("null"));
        int width = 0, height = 0, drawableWidth = 0, drawableHeight = 0;
        void* window = FocusWindow();
        if (window != nullptr && g_getWindowSize != nullptr)
        {
            g_getWindowSize(window, &width, &height);
        }
        if (window != nullptr && g_getDrawableSizeForLog != nullptr)
        {
            g_getDrawableSizeForLog(window, &drawableWidth, &drawableHeight);
        }
        json.Raw("window", JsonSize(width, height));
        json.Raw("drawable", JsonSize(drawableWidth, drawableHeight));
        unsigned long blocked = 0;
        for (const BlockedMethod& method : g_blocked)
        {
            blocked += static_cast<unsigned long>(method.calls);
        }
        json.Num("blocked_steam_calls", blocked);
        json.Bool("screen_capture", g_captureReady);
        json.Num("snapshots_written", g_snapshotsWritten.load());
        if (g_liveFrameMs != 0 && g_captureReady)
        {
            JsonObject live;
            live.Str("file", Utf8(g_testBotDirectory) + "\\live.png");
            live.Num("every_ms", static_cast<long long>(g_liveFrameMs));
            live.Num("frames", g_liveFrames.load());
            const unsigned long long written = g_liveWrittenMs.load();
            live.Raw("age_ms", written != 0 ? std::to_string(now - written) : std::string("null"));
            live.Raw("size", JsonSize(g_liveWidth.load(), g_liveHeight.load()));
            live.Num("errors", g_liveErrors.load());
            json.Raw("live_frame", live.Close());
        }
        else
        {
            json.Null("live_frame");
        }
        if (g_inbox)
        {
            JsonObject inbox;
            inbox.Str("dir", Utf8(g_testBotDirectory) + "\\inbox");
            inbox.Bool("accepting", g_ready && !g_running);
            inbox.Num("pending", g_inboxPending);
            inbox.Num("jobs_done", g_jobsDone);
            inbox.StrOrNull("last_error", g_lastJobError.c_str());
            json.Raw("inbox", inbox.Close());
        }
        else
        {
            json.Null("inbox");
        }
        json.Num("loop_hz", g_loopHz);
        std::string tail = "[";
        AcquireSRWLockExclusive(&g_resultsLock);
        try
        {
            for (std::size_t i = 0; i < g_logTailCount; ++i)
            {
                const std::size_t index = (g_logTailNext + LogTailLines - g_logTailCount + i) % LogTailLines;
                tail += i == 0 ? "" : ",";
                tail += testbot::JsonQuote(g_logTail[index]);
            }
        }
        catch (...)
        {
            ReleaseSRWLockExclusive(&g_resultsLock);
            throw;
        }
        ReleaseSRWLockExclusive(&g_resultsLock);
        tail += "]";
        json.Raw("log_tail", tail);
        return json.Close() + "\n";
    }

    void WriteStatus(unsigned long long now)
    {
        if (g_testBotDirectory[0] == L'\0')
        {
            return;
        }
        try
        {
            const std::string text = BuildStatus(now);
            wchar_t temporary[MAX_PATH * 2], path[MAX_PATH * 2];
            swprintf_s(temporary, L"%s\\status.tmp.json", g_testBotDirectory);
            swprintf_s(path, L"%s\\status.json", g_testBotDirectory);
            PublishFile(temporary, path, text.data(), text.size());
        }
        catch (...)
        {
        }
        g_statusDirty.store(false);
        g_statusForce = false;
        g_statusWrittenMs = now;
    }

    // Once per frame: the loop rate, then a write on change (at most every
    // 100 ms), on demand (Finish) or as the heartbeat (every second).
    void UpdateStatus(unsigned long long now)
    {
        ++g_loopTicks;
        if (now - g_loopRateMs >= LoopRateWindowMs)
        {
            g_loopHz = static_cast<unsigned int>(g_loopTicks * 1000 / (now - g_loopRateMs));
            g_loopTicks = 0;
            g_loopRateMs = now;
        }
        const unsigned long long since = now - g_statusWrittenMs;
        if (g_statusForce || since >= StatusHeartbeatMs || (g_statusDirty.load() && since >= StatusMinIntervalMs))
        {
            WriteStatus(now);
        }
    }

    // ===========================================================================
    // Script runner
    // ===========================================================================
    void Finish(const char* why)
    {
        g_running = false;
        g_captureActive.store(false);   // a snapshot series does not outlive its job
        const bool finished = std::strcmp(why, "finished") == 0;
        const bool stopped = std::strcmp(why, "stopped") == 0;
        g_runState = finished ? RunState::Finished : (stopped ? RunState::Stopped : RunState::Aborted);
        g_result = stopped ? "STOPPED" : (g_failures == 0 && finished ? "PASS" : "FAIL");
        ++g_jobsDone;
        g_statusForce = true;
        unsigned long blocked = 0;
        for (const BlockedMethod& method : g_blocked)
        {
            blocked += static_cast<unsigned long>(method.calls);
        }
        Log("script %s %s after %zu/%zu steps in %llu ms: %u check(s), %u failure(s) -> %s. Achievements/stats stay blocked until the game exits (%lu call(s) blocked so far).",
            g_jobName.c_str(), why, g_stepIndex, g_steps.size(), NowMs() - g_startedMs, g_checks, g_failures, g_result, blocked);
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
        Log("FAIL line %d (%s): %s", step.line, testbot::OpName(step.op), message);
        try
        {
            g_lastFailure = "line " + std::to_string(step.line) + " (" + testbot::OpName(step.op) + "): " + message;
        }
        catch (...)
        {
        }
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
            if (g_phase == 0 && g_originalUpdate == nullptr)
            {
                Fail(step, "world detection is off on this build (see the 'world detection' log line)", true);
                return false;
            }
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

        case testbot::Op::Snapshot:
            if (g_phase == 0)
            {
                if (!g_captureReady)
                {
                    Fail(step, "screen capture unavailable (see the 'screen capture' log line)", false);
                    return true;
                }
                strcpy_s(g_captureName, step.text.c_str());
                g_captureLine = step.line;
                g_captureTotal = static_cast<int>(step.a);
                g_captureInterval = static_cast<unsigned long long>(step.b);
                g_captureDone.store(0);
                g_captureFailed.store(false);
                g_captureNextDue.store(now);
                g_captureLastProgress.store(now);
                g_captureActive.store(true);
                g_phase = 1;
                return false;
            }
            if (g_captureFailed.load())
            {
                g_captureActive.store(false);
                Fail(step, g_captureError, false);
                return true;
            }
            if (g_captureDone.load() >= g_captureTotal)
            {
                g_captureActive.store(false);
                Log("line %d: snapshot %s: %d frame(s) captured", step.line, g_captureName, g_captureTotal);
                NextStep();
                return true;
            }
            if (now > g_captureLastProgress.load() + g_captureInterval + CaptureStallMs)
            {
                g_captureActive.store(false);
                Fail(step, "no frame was presented for 5 s (SDL_GL_SwapWindow hook not reached?)", false);
                return true;
            }
            return false;

        case testbot::Op::ModsErrors:
        {
            unsigned errors = 0, warnings = 0;
            char message[160];
            if (!ExtractModsErrors(step, errors, warnings, message))
            {
                if (step.a >= 0)
                {
                    ++g_checks;
                }
                Fail(step, message, false);
                return true;
            }
            if (step.a < 0)
            {
                Log("line %d: errors.log last mod pass: %s (copied to results.txt)", step.line, message);
                NextStep();
                return true;
            }
            ++g_checks;
            if (errors > static_cast<unsigned long long>(step.a))
            {
                char failure[200];
                _snprintf_s(failure, sizeof(failure), _TRUNCATE, "errors.log last mod pass: %s; at most %lld allowed",
                    message, step.a);
                Fail(step, failure, false);
                return true;
            }
            Log("PASS line %d: errors.log last mod pass: %s (at most %lld errors allowed)", step.line, message, step.a);
            NextStep();
            return true;
        }

        case testbot::Op::WaitScreen:
        {
            if (g_phase == 0)
            {
                if (!g_screenDetection)
                {
                    Fail(step, "screen detection is off (see the 'screen detection' log line)", true);
                    return false;
                }
                g_counter = FindScreen(step.text);
                if (g_counter < 0)
                {
                    Fail(step, "unknown or unverified screen class (see 'screen detection on: ...' for the list)", true);
                    return false;
                }
                g_deadline = now + static_cast<unsigned long long>(step.a);
                g_phase = 1;
            }
            if (g_currentScreen == g_counter && CurrentScreen(now) != nullptr)
            {
                Log("line %d: on screen %s", step.line, step.text.c_str());
                NextStep();
                return true;
            }
            if (now >= g_deadline)
            {
                char message[160];
                const char* current = CurrentScreen(now);
                _snprintf_s(message, sizeof(message), _TRUNCATE, "timed out; current screen %s",
                    current != nullptr ? current : "(none drawn in the last second)");
                Fail(step, message, true);
            }
            return false;
        }

        case testbot::Op::ExpectScreen:
        {
            ++g_checks;
            const char* current = CurrentScreen(now);
            if (!g_screenDetection || FindScreen(step.text) < 0)
            {
                Fail(step, !g_screenDetection ? "screen detection is off" : "unknown or unverified screen class", false);
                return true;
            }
            if (current == nullptr || step.text != current)
            {
                char message[160];
                _snprintf_s(message, sizeof(message), _TRUNCATE, "screen is %s, expected %s",
                    current != nullptr ? current : "(none drawn in the last second)", step.text.c_str());
                Fail(step, message, false);
                return true;
            }
            Log("PASS line %d: screen is %s", step.line, current);
            NextStep();
            return true;
        }

        case testbot::Op::LogScreen:
        {
            const char* current = CurrentScreen(now);
            int width = 0, height = 0, drawableWidth = 0, drawableHeight = 0;
            void* window = FocusWindow();
            if (window != nullptr && g_getWindowSize != nullptr)
            {
                g_getWindowSize(window, &width, &height);
            }
            if (window != nullptr && g_getDrawableSizeForLog != nullptr)
            {
                g_getDrawableSizeForLog(window, &drawableWidth, &drawableHeight);
            }
            Log("line %d: screen %s; window %dx%d (click coordinates), drawable %dx%d (snapshot pixels)", step.line,
                !g_screenDetection ? "unknown (detection off)" : (current != nullptr ? current : "(none drawn in the last second)"),
                width, height, drawableWidth, drawableHeight);
            NextStep();
            return true;
        }

        case testbot::Op::Type:
            if (static_cast<std::size_t>(g_counter) >= step.text.size())
            {
                NextStep();
                return true;
            }
        {
            // Like a real keystroke: key down + SDL_TEXTINPUT, then key up on the next frame.
            // Text events alone lose the second of two equal characters in a row ("Freezing"
            // arrived as "Frezing"): the game's text field wants a key release in between.
            const char character = step.text[static_cast<std::size_t>(g_counter)];
            const std::uint32_t keycode = static_cast<unsigned char>(
                character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
            if (g_phase == 0)
            {
                PushKey(keycode, true);
                PushText(character);
                g_phase = 1;
                return false;
            }
            PushKey(keycode, false);
            g_phase = 0;
            ++g_counter;
            return false;   // one character per two frames
        }

        case testbot::Op::Move:
            if (!WarpTo(static_cast<int>(step.a), static_cast<int>(step.b)))
            {
                Fail(step, "no focused game window", false);
                return true;
            }
            NextStep();
            return false;

        case testbot::Op::DoubleClick:
        {
            const int x = static_cast<int>(step.a), y = static_cast<int>(step.b), button = static_cast<int>(step.c);
            switch (g_phase)
            {
            case 0:
                if (!WarpTo(x, y))
                {
                    Fail(step, "no focused game window", false);
                    return true;
                }
                break;
            case 1: PushButton(button, x, y, true, 1); break;
            case 2: PushButton(button, x, y, false, 1); break;
            case 3: PushButton(button, x, y, true, 2); break;
            default:
                PushButton(button, x, y, false, 2);
                NextStep();
                return false;
            }
            ++g_phase;
            return false;
        }

        case testbot::Op::Drag:
        {
            const int x1 = static_cast<int>(step.a), y1 = static_cast<int>(step.b);
            const int x2 = static_cast<int>(step.c), y2 = static_cast<int>(step.d);
            const int button = static_cast<int>(step.e);
            const std::uint32_t mask = 1u << (button - 1);
            if (g_phase == 0)
            {
                if (!WarpTo(x1, y1))
                {
                    Fail(step, "no focused game window", false);
                    return true;
                }
                g_phase = 1;
                return false;
            }
            if (g_phase == 1)
            {
                PushButton(button, x1, y1, true);
                g_phase = 2;
                g_counter = 0;
                return false;
            }
            if (g_counter < DragSteps)
            {
                const int previousX = x1 + (x2 - x1) * static_cast<int>(g_counter) / DragSteps;
                const int previousY = y1 + (y2 - y1) * static_cast<int>(g_counter) / DragSteps;
                ++g_counter;
                const int x = x1 + (x2 - x1) * static_cast<int>(g_counter) / DragSteps;
                const int y = y1 + (y2 - y1) * static_cast<int>(g_counter) / DragSteps;
                WarpTo(x, y);
                PushMotion(x, y, x - previousX, y - previousY, mask);
                return false;   // one step per frame
            }
            PushButton(button, x2, y2, false);
            NextStep();
            return false;
        }
        }
        NextStep();
        return true;
    }

    // ===========================================================================
    // Jobs and the inbox (the agent -> bot channel)
    //
    // A job is one script: the INI script at start-up, then every *.script file
    // dropped into <TestBot>\inbox, taken in name order whenever no job runs.
    // A taken file is moved into the run folder (jobNN_<name>), so it runs once
    // and the run keeps a copy. A file still open for writing is left alone
    // until its writer closes it (writers should write a temporary name and
    // rename). A file named `stop` in the inbox aborts the running job.
    // ===========================================================================
    constexpr unsigned long long InboxPollMs = 500;

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

    // Reads and parses a script file. False with `message` when it cannot run;
    // `busy` is set when another process still has the file open for writing.
    bool ReadScript(const wchar_t* path, std::vector<testbot::Step>& steps, char (&message)[320], bool* busy)
    {
        *busy = false;
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            *busy = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
            _snprintf_s(message, sizeof(message), _TRUNCATE, "cannot open %S (error %lu)", path, error);
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
            _snprintf_s(message, sizeof(message), _TRUNCATE, "cannot read %S (unreadable or over 64 KiB)", path);
            return false;
        }
        testbot::ParseResult parsed = testbot::ParseScript(text);
        if (!parsed.ok)
        {
            _snprintf_s(message, sizeof(message), _TRUNCATE, "line %d: %s", parsed.errorLine, parsed.error.c_str());
            return false;
        }
        if (parsed.steps.empty())
        {
            _snprintf_s(message, sizeof(message), _TRUNCATE, "no steps");
            return false;
        }
        steps = std::move(parsed.steps);
        return true;
    }

    // Hooks a job may need, installed once, when first needed (also mid-session).
    void EnsureScreenCapture()
    {
        if (!g_captureInstallTried)
        {
            g_captureInstallTried = true;
            InstallScreenCapture(g_gameBase);
        }
    }

    void EnsureScreenDetection()
    {
        if (!g_screenInstallTried)
        {
            g_screenInstallTried = true;
            InstallScreenDetection(g_gameBase);
        }
    }

    void BeginJob(std::vector<testbot::Step>&& steps, const char* name, const char* source)
    {
        g_steps = std::move(steps);
        g_stepIndex = 0;
        g_phase = 0;
        g_counter = 0;
        g_failures = 0;
        g_checks = 0;
        g_announcedStep = static_cast<std::size_t>(-1);
        g_lastFailure.clear();
        g_result = nullptr;
        ++g_jobIndex;
        g_jobName = name;
        g_jobSource = source;
        if (ScriptHasSnapshots())
        {
            EnsureScreenCapture();
        }
        if (ScriptUsesScreens())
        {
            EnsureScreenDetection();
        }
        g_startedMs = NowMs();
        g_stepStartedMs = g_startedMs;
        g_running = true;
        g_runState = RunState::Running;
        g_statusForce = true;
        Log("job %u (%s) %s ACTIVE: %zu step(s), stop on failure=%d; achievements and stats are blocked for this session",
            g_jobIndex, source, name, g_steps.size(), g_stopOnFailure ? 1 : 0);
    }

    void TakeInboxScript(const wchar_t* name)
    {
        wchar_t path[MAX_PATH * 2];
        swprintf_s(path, L"%s\\inbox\\%s", g_testBotDirectory, name);
        std::vector<testbot::Step> steps;
        char message[320];
        bool busy = false;
        const bool ok = ReadScript(path, steps, message, &busy);
        if (!ok && busy)
        {
            return;   // still being written; next poll
        }
        // Taken: into the run folder (a copy for the record), or gone.
        bool consumed = false;
        if (g_runDirectory[0] != L'\0')
        {
            wchar_t kept[MAX_PATH * 2];
            swprintf_s(kept, L"%s\\job%02u_%s", g_runDirectory, g_jobIndex + 1, name);
            consumed = MoveFileExW(path, kept, MOVEFILE_REPLACE_EXISTING) != FALSE;
        }
        if (!consumed)
        {
            consumed = DeleteFileW(path) != FALSE;
        }
        if (!consumed)
        {
            wcscpy_s(g_inboxStuck, name);   // never take it again this session
            Log("inbox: %S could neither be moved nor deleted (error %lu); ignored from now on", name, GetLastError());
            return;
        }
        if (!ok)
        {
            Log("inbox: %S skipped: %s", name, message);
            try
            {
                g_lastJobError = Utf8(name) + ": " + message;
            }
            catch (...)
            {
            }
            g_statusForce = true;
            return;
        }
        g_lastJobError.clear();
        BeginJob(std::move(steps), Utf8(name).c_str(), "inbox");
    }

    // Every 500 ms: the stop file, then (when idle) the next script in name order.
    void PollInbox(unsigned long long now)
    {
        if (!g_inbox || !g_ready || g_testBotDirectory[0] == L'\0' || now - g_inboxCheckedMs < InboxPollMs)
        {
            return;
        }
        g_inboxCheckedMs = now;
        wchar_t stopPath[MAX_PATH * 2];
        swprintf_s(stopPath, L"%s\\inbox\\stop", g_testBotDirectory);
        if (GetFileAttributesW(stopPath) != INVALID_FILE_ATTRIBUTES && DeleteFileW(stopPath))
        {
            if (g_running)
            {
                Log("inbox: stop received during step %zu/%zu", g_stepIndex + 1, g_steps.size());
                Finish("stopped");
            }
            else
            {
                Log("inbox: stop received, nothing was running");
            }
        }
        wchar_t pattern[MAX_PATH * 2];
        swprintf_s(pattern, L"%s\\inbox\\*.script", g_testBotDirectory);
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(pattern, &data);
        unsigned int pending = 0;
        wchar_t next[MAX_PATH] = L"";
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !ValidScriptName(data.cFileName)
                    || wcscmp(data.cFileName, g_inboxStuck) == 0)
                {
                    continue;
                }
                ++pending;
                if (next[0] == L'\0' || wcscmp(data.cFileName, next) < 0)
                {
                    wcscpy_s(next, data.cFileName);
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        if (pending != g_inboxPending)
        {
            g_inboxPending = pending;
            g_statusDirty.store(true);
        }
        if (!g_running && next[0] != L'\0')
        {
            TakeInboxScript(next);
        }
    }

    void RunSteps(unsigned long long now)
    {
        for (int budget = 0; budget < 32 && g_running; ++budget)
        {
            if (g_stepIndex >= g_steps.size())
            {
                Finish("finished");
                return;
            }
            if (g_announcedStep != g_stepIndex)
            {
                g_announcedStep = g_stepIndex;
                g_stepStartedMs = now;
                char line[400];
                _snprintf_s(line, sizeof(line), _TRUNCATE, "step %zu/%zu: line %d %s", g_stepIndex + 1, g_steps.size(),
                    g_steps[g_stepIndex].line, testbot::DescribeStep(g_steps[g_stepIndex]).c_str());
                ResultLine(line);
            }
            if (!RunStep(g_steps[g_stepIndex], now))
            {
                return;
            }
        }
    }

    // Called on the game thread once per frame (after the event queue drains).
    void Tick()
    {
        const unsigned long long now = NowMs();
        UpdateCurrentScreen(now);
        PollInbox(now);
        if (g_running)
        {
            RunSteps(now);
        }
        UpdateStatus(now);
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
    // The INI script (Script=, default TestBot.script, in the plugin folder).
    bool LoadScript(const wchar_t* iniPath, std::vector<testbot::Step>& steps, wchar_t (&name)[64])
    {
        GetPrivateProfileStringW(L"TestBot", L"Script", L"TestBot.script", name, 64, iniPath);
        if (!ValidScriptName(name))
        {
            Log("Script=%S is not a plain file name in the plugin folder; it will not run", name);
            return false;
        }
        wchar_t path[MAX_PATH * 2];
        swprintf_s(path, L"%s%s", g_extender->pluginDirectory, name);
        char message[320];
        bool busy = false;
        if (!ReadScript(path, steps, message, &busy))
        {
            Log("script %S: %s; it will not run", path, message);
            return false;
        }
        Log("script %S loaded: %zu step(s)", path, steps.size());
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
        // Optional (log_screen and status.json): window and drawable size.
        g_getWindowSize = reinterpret_cast<SdlGetWindowSizeFn>(GetProcAddress(sdl, "SDL_GetWindowSize"));
        g_getDrawableSizeForLog = reinterpret_cast<SdlGetWindowSizeFn>(GetProcAddress(sdl, "SDL_GL_GetDrawableSize"));
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
        g_enabled = true;
        OpenRunFolder();
        g_stopOnFailure = GetPrivateProfileIntW(L"TestBot", L"StopOnFailure", 0, iniPath) != 0;
        g_logBlockedCalls = GetPrivateProfileIntW(L"TestBot", L"LogBlockedCalls", 1, iniPath) != 0;
        const int liveFrameMs = static_cast<int>(GetPrivateProfileIntW(L"TestBot", L"LiveFrameMs", DefaultLiveFrameMs, iniPath));
        g_liveFrameMs = liveFrameMs <= 0 ? 0
            : static_cast<unsigned long long>(liveFrameMs < MinLiveFrameMs ? MinLiveFrameMs : liveFrameMs);
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
        g_inbox = GetPrivateProfileIntW(L"TestBot", L"Inbox", 1, iniPath) != 0;
        std::vector<testbot::Step> steps;
        wchar_t scriptName[64]{};
        const bool scriptOk = LoadScript(iniPath, steps, scriptName);
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
        g_ready = true;
        g_gameBase = base;
        if (g_inbox && g_testBotDirectory[0] != L'\0')
        {
            wchar_t inbox[MAX_PATH * 2];
            swprintf_s(inbox, L"%s\\inbox", g_testBotDirectory);
            CreateDirectoryW(inbox, nullptr);
            Log("inbox open: *.script files in %S run in name order when no script runs; a file named `stop` aborts the running one",
                inbox);
        }
        // Live frames do not need a script: whoever watches still sees the screen.
        if (g_liveFrameMs != 0)
        {
            EnsureScreenCapture();
            if (g_captureReady)
            {
                Log("live frame: %S\\live.png will be refreshed every %llu ms (LiveFrameMs; 0 turns it off)",
                    g_testBotDirectory, g_liveFrameMs);
            }
            else
            {
                Log("live frame off: screen capture unavailable (LiveFrameMs=%llu ignored)", g_liveFrameMs);
            }
        }
        if (scriptOk)
        {
            BeginJob(std::move(steps), Utf8(scriptName).c_str(), "ini");
        }
        else if (g_inbox)
        {
            g_runState = RunState::Idle;
            Log("idle: waiting for a script in the inbox");
        }
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SoulashExtenderPluginLoad(
    const SoulashExtenderInterface* extender)
{
    if (extender == nullptr
        || extender->structureSize < SOULASH_EXTENDER_INTERFACE_V3_BASE_SIZE
        || extender->interfaceVersion != SOULASH_EXTENDER_INTERFACE_VERSION
        || extender->pluginId == nullptr
        || extender->pluginDirectory == nullptr
        || extender->gameBase == nullptr)
    {
        return FALSE;
    }
    g_extender = extender;
    g_startupReason.Begin();
    Start();
    g_startupReason.End();
    if (!g_enabled)
    {
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_DISABLED, "Enabled=0 in TestBot.ini");
    }
    else if (!g_blockingReady)
    {
        // Fail closed: nothing runs, and the report says why.
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_INACTIVE, g_startupReason.Last());
    }
    else if (!g_running && g_ready && g_inbox)
    {
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_ACTIVE,
            "achievements and stats blocked; idle, inbox open");
    }
    else if (!g_running)
    {
        char detail[200];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, "achievements blocked, bot NOT running: %s",
            g_startupReason.Last());
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_DEGRADED, detail);
    }
    else
    {
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_ACTIVE,
            "achievements and stats blocked; script running");
    }
    // First status.json right away, so a watcher learns at once whether (and why not) the bot runs.
    if (g_enabled && g_testBotDirectory[0] != L'\0')
    {
        if (!g_running)
        {
            g_notRunningReason = g_startupReason.Last();
        }
        WriteStatus(NowMs());
    }
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    // Normal process exit: leave a last line in results.txt if the script was
    // still running. Loader lock is held and other threads are gone, so only
    // try the lock and only call WriteFile.
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr && g_running && g_results != INVALID_HANDLE_VALUE
        && TryAcquireSRWLockExclusive(&g_resultsLock))
    {
        char text[160];
        const int length = _snprintf_s(text, sizeof(text), _TRUNCATE,
            "[%9llu ms] game exited during step %zu/%zu: script NOT finished\r\n",
            GetTickCount64() - g_resultsStartMs, g_stepIndex + 1, g_steps.size());
        DWORD written = 0;
        if (length > 0)
        {
            WriteFile(g_results, text, static_cast<DWORD>(length), &written, nullptr);
        }
        ReleaseSRWLockExclusive(&g_resultsLock);
    }
    // ... and a last status.json saying the game exited normally (fixed buffer, kernel32 only).
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr && g_enabled && g_testBotDirectory[0] != L'\0')
    {
        char stamp[40];
        UtcNow(stamp);
        char text[512];
        const int length = _snprintf_s(text, sizeof(text), _TRUNCATE,
            "{\"schema\":%d,\"updated_utc\":\"%s\",\"run_ms\":%llu,\"pid\":%lu,\"state\":\"exited\",\"script\":\"%s\","
            "\"result\":%s%s%s,\"steps_done\":%zu,\"steps_total\":%zu,\"checks\":%u,\"failures\":%u}\n",
            StatusSchema, stamp, GetTickCount64() - g_resultsStartMs, GetCurrentProcessId(), RunStateName(g_runState),
            g_result != nullptr ? "\"" : "", g_result != nullptr ? g_result : "null", g_result != nullptr ? "\"" : "",
            g_stepIndex, g_steps.size(), g_checks, g_failures);
        if (length > 0)
        {
            wchar_t temporary[MAX_PATH * 2], path[MAX_PATH * 2];
            swprintf_s(temporary, L"%s\\status.tmp.json", g_testBotDirectory);
            swprintf_s(path, L"%s\\status.json", g_testBotDirectory);
            PublishFile(temporary, path, text, static_cast<std::size_t>(length));
        }
    }
    return TRUE;
}
