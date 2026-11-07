#include "update_guard.hpp"

#include "plugin_api.h"
#include "sha256.hpp"
#include "update_guard_logic.hpp"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

extern "C" BOOL WINAPI SoulashDiagnosticsWritePluginMessage(const char* message);

namespace
{
    using namespace soulash::update_guard;

    struct PluginLine
    {
        std::string id;
        std::string outcome;
        unsigned status = 0;      // 0 = not reported
        std::string detail;
    };

    SRWLOCK g_lock = SRWLOCK_INIT;
    bool g_begun = false;
    bool g_cleanExitPending = false;
    bool g_finished = false;
    unsigned g_nativeLoaded = 0;
    State g_state;
    Decision g_decision;
    std::vector<PluginLine> g_plugins;
    wchar_t g_statePath[MAX_PATH]{};
    wchar_t g_reportPath[MAX_PATH]{};

    void Log(const std::string& message)
    {
        SoulashDiagnosticsWritePluginMessage(("update guard: " + message).c_str());
    }

    std::string NowUtc()
    {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char text[32];
        std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond);
        return text;
    }

    std::string Short(const std::string& sha)
    {
        return sha.empty() ? std::string("unknown") : sha.substr(0, 8);
    }

    bool ReadSmallFile(const wchar_t* path, std::string& text, std::size_t maximum)
    {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0
            && static_cast<unsigned long long>(size.QuadPart) <= maximum;
        if (ok)
        {
            text.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            ok = text.empty() || (ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) != FALSE
                && read == text.size());
        }
        CloseHandle(file);
        return ok;
    }

    // Write to <path>.tmp, then replace: a crash mid-write never leaves a torn file.
    bool WriteFileAtomically(const wchar_t* path, const std::string& text)
    {
        wchar_t temporary[MAX_PATH];
        if (swprintf_s(temporary, L"%s.tmp", path) < 0)
        {
            return false;
        }
        HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD written = 0;
        const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE
            && written == text.size() && FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
        return ok && MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    std::string ReadGameVersion(const wchar_t* gameDirectory)
    {
        if (gameDirectory == nullptr || gameDirectory[0] == L'\0')
        {
            return {};
        }
        wchar_t path[MAX_PATH];
        if (swprintf_s(path, L"%sversion.txt", gameDirectory) < 0)
        {
            return {};
        }
        std::string text;
        if (!ReadSmallFile(path, text, 256))
        {
            return {};
        }
        const std::size_t end = text.find_first_of("\r\n");
        std::string version = text.substr(0, end);
        while (!version.empty() && (version.back() == ' ' || version.back() == '\t'))
        {
            version.pop_back();
        }
        return IsSafeValue(version, 32) ? version : std::string();
    }

    std::string HashFile(const wchar_t* path)
    {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        char hex[65]{};
        const bool ok = soulash::Sha256OfHandle(file, hex);
        CloseHandle(file);
        return ok ? std::string(hex) : std::string();
    }

    // Caller holds g_lock (shared or exclusive).
    bool PersistState()
    {
        return g_statePath[0] != L'\0' && WriteFileAtomically(g_statePath, SerializeState(g_state));
    }

    const char* StatusName(unsigned status)
    {
        switch (status)
        {
        case SOULASH_PLUGIN_STATUS_ACTIVE: return "active";
        case SOULASH_PLUGIN_STATUS_INACTIVE: return "INACTIVE on this build";
        case SOULASH_PLUGIN_STATUS_DISABLED: return "disabled by its config";
        case SOULASH_PLUGIN_STATUS_DEGRADED: return "DEGRADED on this build";
        default: return "no status reported";
        }
    }

    PluginLine& LineFor(const char* id)
    {
        for (PluginLine& line : g_plugins)
        {
            if (_stricmp(line.id.c_str(), id) == 0)
            {
                return line;
            }
        }
        g_plugins.push_back(PluginLine{id, "", 0, ""});
        return g_plugins.back();
    }

    std::string BuildReport(unsigned nativePluginsLoaded)
    {
        const Decision& d = g_decision;
        std::string r;
        r += "Soulash Extender compatibility report (rewritten at every start; docs/update-guard.md)\r\n";
        r += "written_utc=" + NowUtc() + "\r\n";
        r += "game_version=" + (g_state.gameVersion.empty() ? std::string("unknown") : g_state.gameVersion) + "\r\n";
        r += "game_sha256=" + (g_state.exeSha256.empty() ? std::string("unknown") : g_state.exeSha256) + "\r\n";
        if (d.firstStart)
        {
            r += "game_build=first start with the update guard (no previous record)\r\n";
        }
        else if (d.gameUpdated)
        {
            r += "game_build=UPDATED since the last start (was " + (d.previousVersion.empty() ? std::string("?") : d.previousVersion)
                + " " + Short(d.previousSha256) + "); check every plugin below\r\n";
        }
        else
        {
            r += "game_build=unchanged since the last start\r\n";
        }
        r += std::string("first_run_on_this_build=") + (d.firstRunOnBuild ? "yes" : "no") + "\r\n";
        r += "vendor_bugsplat_sha256=" + (g_state.originalSha256.empty() ? std::string("unknown (BugSplat64_original.dll not found)") : g_state.originalSha256)
            + (d.vendorDllChanged ? " (CHANGED since the last start)" : "") + "\r\n";
        if (!d.firstStart)
        {
            r += std::string("previous_session=") + (d.previousClean ? "clean exit" : "ended WITHOUT a clean exit (crash, hang or kill)")
                + (d.previousHadPlugins ? ", native plugins were loaded" : ", no native plugins") + "\r\n";
        }
        r += "unclean_streak=" + std::to_string(d.uncleanStreak) + "\r\n";
        r += d.safeMode ? "safe_mode=YES: " + d.safeModeReason + ". Native plugins were NOT loaded. Exit the game normally once to leave safe mode.\r\n"
                        : std::string("safe_mode=no\r\n");
        r += "known_builds=";
        for (std::size_t i = 0; i < g_state.knownBuilds.size(); ++i)
        {
            const KnownBuild& build = g_state.knownBuilds[i];
            r += (i == 0 ? "" : ", ") + Short(build.sha256) + " " + (build.version.empty() ? "?" : build.version)
                + " (first seen " + build.firstSeenUtc + ")";
        }
        r += "\r\n";
        unsigned active = 0, attention = 0;
        r += "plugins=" + std::to_string(g_plugins.size()) + " found, " + std::to_string(nativePluginsLoaded) + " loaded\r\n";
        for (const PluginLine& line : g_plugins)
        {
            r += "  " + line.id + ": " + (line.outcome.empty() ? std::string("?") : line.outcome) + "; " + StatusName(line.status);
            if (!line.detail.empty())
            {
                r += ": " + line.detail;
            }
            r += "\r\n";
            if (line.status == SOULASH_PLUGIN_STATUS_ACTIVE)
            {
                ++active;
            }
            if (line.status == SOULASH_PLUGIN_STATUS_INACTIVE || line.status == SOULASH_PLUGIN_STATUS_DEGRADED
                || line.outcome.rfind("loaded", 0) != 0)
            {
                ++attention;
            }
        }
        r += "summary=" + std::to_string(active) + " active, " + std::to_string(attention) + " need attention\r\n";
        r += "end of report\r\n";
        return r;
    }
}

namespace soulash::update_guard
{
    bool BeginSession(const wchar_t* proxyDirectory, const wchar_t* iniPath,
        const char* exeSha256, const wchar_t* gameDirectory)
    {
        if (iniPath != nullptr && GetPrivateProfileIntW(L"UpdateGuard", L"Enabled", 1, iniPath) == 0)
        {
            Log("disabled ([UpdateGuard] Enabled=0); no update detection, no safe mode, no report");
            return true;
        }
        Config config;
        if (iniPath != nullptr)
        {
            config.safeModeAfterUncleanExits = GetPrivateProfileIntW(L"UpdateGuard", L"SafeModeAfterUncleanExits", 2, iniPath);
            config.forceSafeMode = GetPrivateProfileIntW(L"UpdateGuard", L"ForceSafeMode", 0, iniPath) == 1;
        }

        wchar_t folder[MAX_PATH];
        wchar_t vendorPath[MAX_PATH];
        if (proxyDirectory == nullptr
            || swprintf_s(folder, L"%sSoulashExtender", proxyDirectory) < 0
            || swprintf_s(vendorPath, L"%sBugSplat64_original.dll", proxyDirectory) < 0)
        {
            return true;
        }
        CreateDirectoryW(folder, nullptr);
        const std::string version = ReadGameVersion(gameDirectory);
        const std::string vendorSha = HashFile(vendorPath);
        const std::string sha = exeSha256 != nullptr && IsHex64(exeSha256) ? std::string(exeSha256) : std::string();

        AcquireSRWLockExclusive(&g_lock);
        swprintf_s(g_statePath, L"%s\\UpdateGuard.state", folder);
        swprintf_s(g_reportPath, L"%s\\compat-report.txt", folder);
        std::string text;
        State previous;
        const bool havePrevious = ReadSmallFile(g_statePath, text, MaxStateBytes) && ParseState(text, previous);
        g_decision = Decide(havePrevious ? &previous : nullptr, config, sha, version, vendorSha, NowUtc());
        g_state = g_decision.next;
        if (g_cleanExitPending)
        {
            g_state.cleanExit = true;
        }
        g_begun = true;
        const bool persisted = PersistState();
        const Decision decision = g_decision;
        ReleaseSRWLockExclusive(&g_lock);

        const std::string build = (version.empty() ? std::string("?") : version) + " (" + Short(sha) + ")";
        if (decision.gameUpdated)
        {
            Log("GAME UPDATED: " + (decision.previousVersion.empty() ? std::string("?") : decision.previousVersion)
                + " (" + Short(decision.previousSha256) + ") -> " + build
                + ". Plugins verify themselves; see SoulashExtender\\compat-report.txt");
        }
        else
        {
            Log(std::string("game build ") + build + (decision.firstStart ? " (first start with the update guard)" : " (unchanged)"));
        }
        if (decision.vendorDllChanged)
        {
            Log("BugSplat64_original.dll changed since the last start (vendor DLL replaced; the proxy was re-applied)");
        }
        if (!decision.firstStart && !decision.previousClean)
        {
            Log("the previous session ended without a clean exit (crash, hang or kill); unclean streak "
                + std::to_string(decision.uncleanStreak));
        }
        if (decision.safeMode)
        {
            Log("SAFE MODE: " + decision.safeModeReason
                + ". Native plugins will NOT be loaded this start (Lua scripts and data files still load). "
                  "Exit the game normally once to leave safe mode, or set [UpdateGuard] SafeModeAfterUncleanExits=0.");
        }
        if (!persisted)
        {
            Log("cannot write SoulashExtender\\UpdateGuard.state (error " + std::to_string(GetLastError())
                + "); safe mode cannot protect the next start");
        }
        return !decision.safeMode;
    }

    void RecordPluginOutcome(const char* pluginId, const char* outcome)
    {
        if (pluginId == nullptr || outcome == nullptr)
        {
            return;
        }
        AcquireSRWLockExclusive(&g_lock);
        LineFor(pluginId).outcome = outcome;
        ReleaseSRWLockExclusive(&g_lock);
    }

    void RecordPluginStatus(const char* pluginId, unsigned int status, const char* detail)
    {
        if (pluginId == nullptr)
        {
            return;
        }
        AcquireSRWLockExclusive(&g_lock);
        PluginLine& line = LineFor(pluginId);
        line.status = status;
        line.detail = detail == nullptr ? std::string() : std::string(detail);
        // A status change after startup (a plugin degrading itself mid-game)
        // refreshes the report; before that FinishStartup writes it once.
        if (g_finished && g_reportPath[0] != L'\0')
        {
            WriteFileAtomically(g_reportPath, BuildReport(g_nativeLoaded));
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    void FinishStartup(unsigned nativePluginsLoaded)
    {
        AcquireSRWLockExclusive(&g_lock);
        if (!g_begun)
        {
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        g_state.pluginsLoaded = nativePluginsLoaded > 0;
        g_nativeLoaded = nativePluginsLoaded;
        PersistState();
        const std::string report = BuildReport(nativePluginsLoaded);
        g_finished = true;
        unsigned inactive = 0;
        std::string names;
        for (const PluginLine& line : g_plugins)
        {
            if (line.status == SOULASH_PLUGIN_STATUS_INACTIVE || line.status == SOULASH_PLUGIN_STATUS_DEGRADED)
            {
                ++inactive;
                names += (names.empty() ? "" : ", ") + line.id;
            }
        }
        const bool written = g_reportPath[0] != L'\0' && WriteFileAtomically(g_reportPath, report);
        ReleaseSRWLockExclusive(&g_lock);
        if (inactive > 0)
        {
            Log(std::to_string(inactive) + " plugin(s) inactive or degraded on this build: " + names);
        }
        Log(written ? std::string("compatibility report written to SoulashExtender\\compat-report.txt")
                    : "cannot write SoulashExtender\\compat-report.txt (error " + std::to_string(GetLastError()) + ")");
    }

    void MarkCleanExit(bool fromLoaderLock)
    {
        // From DllMain other threads are already gone and may have died holding
        // the lock: never wait there.
        if (fromLoaderLock)
        {
            if (!TryAcquireSRWLockExclusive(&g_lock))
            {
                return;
            }
        }
        else
        {
            AcquireSRWLockExclusive(&g_lock);
        }
        if (!g_begun)
        {
            g_cleanExitPending = true;
        }
        else if (!g_state.cleanExit)
        {
            g_state.cleanExit = true;
            PersistState();
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
}
