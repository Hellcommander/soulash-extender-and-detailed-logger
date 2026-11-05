#pragma once

#include <windows.h>

// Update guard: helps the extender survive game updates.
//   * detects a new game build (exe SHA-256 + version.txt) and a replaced vendor
//     BugSplat DLL, and logs it;
//   * crash-loop safe mode: after N consecutive sessions with native plugins
//     loaded that ended without a clean exit, the next start loads no native
//     plugins (Lua scripts and data files still load);
//   * writes <proxy dir>\SoulashExtender\compat-report.txt at every start: build,
//     update, safe mode, and every plugin's load outcome + reported status.
// Configuration: [UpdateGuard] in SoulashExtender.ini (Enabled=1,
// SafeModeAfterUncleanExits=2, ForceSafeMode=0). docs/update-guard.md.
namespace soulash::update_guard
{
    // Starts the session (plugin loader thread, before any plugin loads).
    // Returns false when native plugins must NOT be loaded (safe mode).
    bool BeginSession(const wchar_t* proxyDirectory, const wchar_t* iniPath,
        const char* exeSha256, const wchar_t* gameDirectory);

    // Plugin outcomes for the report ("loaded", "refused: ...", "skipped: safe mode", ...).
    void RecordPluginOutcome(const char* pluginId, const char* outcome);

    // From the plugin interface's reportStatus.
    void RecordPluginStatus(const char* pluginId, unsigned int status, const char* detail);

    // After all plugins loaded: persists plugins_loaded, writes the report.
    void FinishStartup(unsigned nativePluginsLoaded);

    // The game shut down normally (BugSplat destructor or process exit).
    // fromLoaderLock: called from DllMain; never blocks.
    void MarkCleanExit(bool fromLoaderLock);
}
