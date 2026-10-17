#include "manifest.hpp"
#include "module_runtime.hpp"
#include "plugin_api.h"
#include "plugin_services.hpp"
#include "conditions.hpp"
#include "script_sandbox.hpp"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

extern "C" BOOL WINAPI SoulashDiagnosticsWritePluginMessage(const char* message);
extern "C" BOOL WINAPI SoulashDiagnosticsReportFileIssue(
    const wchar_t* path,
    unsigned int operation,
    const char* reason);
extern "C" BOOL WINAPI SoulashDiagnosticsRegisterModule(HMODULE module, const char* moduleId);
extern "C" BOOL WINAPI SoulashDiagnosticsLogModuleEvent(
    const wchar_t* path,
    const char* moduleId,
    const char* stage,
    DWORD error);

namespace
{
    constexpr DWORD MaximumManifestBytes = 4096;
    constexpr std::size_t MaximumDirectoryPlugins = 64;
    constexpr wchar_t ManifestEnvironmentVariable[] = L"SOULASH2_NATIVE_MODULE_MANIFEST";
    constexpr wchar_t PluginDirectoryEnvironmentVariable[] = L"SOULASH2_PLUGIN_DIRECTORY";
    constexpr wchar_t DefaultPluginDirectory[] = L"SoulashExtender\\Plugins";

    volatile LONG g_startState = 0;
    wchar_t g_manifestPath[MAX_PATH]{};
    wchar_t g_pluginDirectory[MAX_PATH]{};
    wchar_t g_scriptDirectory[MAX_PATH]{};
    wchar_t g_iniPath[MAX_PATH]{};
    bool g_requireHash = false;

    struct PluginEntry
    {
        std::string id;
        std::wstring path;
    };

    void DebugLog(const char* message)
    {
        OutputDebugStringA("Soulash extender: ");
        OutputDebugStringA(message);
        OutputDebugStringA("\r\n");
    }

    void WINAPI PluginLog(const char* message)
    {
        if (message != nullptr)
        {
            SoulashDiagnosticsWritePluginMessage(message);
            DebugLog(message);
        }
    }

    // Passed to legacy ABI v2 plugins (SoulashExtenderPluginInit).
    const SoulashExtenderApi g_pluginApi{
        sizeof(SoulashExtenderApi),
        SOULASH_EXTENDER_PLUGIN_ABI_VERSION,
        PluginLog,
        SoulashDiagnosticsReportFileIssue
    };

    void ReportFailure(const char* message, DWORD error = ERROR_SUCCESS)
    {
        OutputDebugStringA("Soulash extender: ");
        OutputDebugStringA(message);
        if (error != ERROR_SUCCESS)
        {
            char digits[10];
            unsigned count = 0;
            do
            {
                digits[count++] = static_cast<char>('0' + error % 10);
                error /= 10;
            } while (error != 0 && count < sizeof(digits));
            OutputDebugStringA(" (Win32 error ");
            while (count > 0)
            {
                char value[] = {digits[--count], '\0'};
                OutputDebugStringA(value);
            }
            OutputDebugStringA(")");
        }
        OutputDebugStringA("\r\n");
    }

    bool IsLocalAbsolutePath(const wchar_t* path)
    {
        if (path == nullptr || path[0] == L'\0' || path[1] == L'\0' || path[2] == L'\0'
            || !((path[0] >= L'a' && path[0] <= L'z') || (path[0] >= L'A' && path[0] <= L'Z'))
            || path[1] != L':'
            || path[2] != L'\\')
        {
            return false;
        }
        wchar_t driveRoot[] = {path[0], L':', L'\\', L'\0'};
        return GetDriveTypeW(driveRoot) == DRIVE_FIXED;
    }

    bool ReadManifest(const wchar_t* manifestPath, std::string& text)
    {
        HANDLE file = CreateFileW(
            manifestPath,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            ReportFailure("cannot open native module manifest", GetLastError());
            return false;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0
            || size.QuadPart > MaximumManifestBytes)
        {
            const DWORD error = GetLastError() == ERROR_SUCCESS
                ? ERROR_FILE_TOO_LARGE : GetLastError();
            CloseHandle(file);
            ReportFailure("native module manifest has an invalid size", error);
            return false;
        }

        text.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD bytesRead = 0;
        const BOOL readSucceeded = text.empty()
            || ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &bytesRead, nullptr);
        CloseHandle(file);
        if (!readSucceeded || bytesRead != text.size())
        {
            ReportFailure("cannot read complete native module manifest", GetLastError());
            return false;
        }
        return true;
    }

    bool ResolveModulePath(const wchar_t* manifestPath, const std::string& module, wchar_t* result)
    {
        wchar_t fullManifestPath[MAX_PATH]{};
        const DWORD fullLength = GetFullPathNameW(
            manifestPath, MAX_PATH, fullManifestPath, nullptr);
        if (fullLength == 0 || fullLength >= MAX_PATH)
        {
            ReportFailure("cannot resolve native module manifest path", GetLastError());
            return false;
        }

        wchar_t* separator = nullptr;
        for (wchar_t* current = fullManifestPath; *current != L'\0'; ++current)
        {
            if (*current == L'\\' || *current == L'/')
            {
                separator = current;
            }
        }
        if (separator == nullptr)
        {
            ReportFailure("native module manifest path has no directory");
            return false;
        }
        separator[1] = L'\0';

        char utf8Path[241]{};
        if (module.size() >= sizeof(utf8Path))
        {
            ReportFailure("native module path exceeds the supported length");
            return false;
        }
        for (std::size_t index = 0; index < module.size(); ++index)
        {
            utf8Path[index] = module[index] == '/' ? '\\' : module[index];
        }
        const int wideLength = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, nullptr, 0);
        if (wideLength <= 0)
        {
            ReportFailure("native module path is not valid UTF-8", GetLastError());
            return false;
        }
        const std::size_t directoryLength = static_cast<std::size_t>(separator + 1 - fullManifestPath);
        if (directoryLength + static_cast<std::size_t>(wideLength) > MAX_PATH)
        {
            ReportFailure("resolved native module path exceeds MAX_PATH");
            return false;
        }
        for (std::size_t index = 0; index < directoryLength; ++index)
        {
            result[index] = fullManifestPath[index];
        }
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1,
                result + directoryLength, MAX_PATH - static_cast<int>(directoryLength)) <= 0)
        {
            ReportFailure("cannot convert native module path", GetLastError());
            return false;
        }
        return true;
    }

    // Plugin ids use the manifest id alphabet so they are safe in log records.
    bool IsValidPluginId(const std::string& id)
    {
        if (id.empty() || id.size() > 128)
        {
            return false;
        }
        for (std::size_t index = 0; index < id.size(); ++index)
        {
            const char value = id[index];
            const bool alphaNumeric = (value >= 'a' && value <= 'z')
                || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9');
            if (!alphaNumeric && (index == 0 || (value != '.' && value != '_' && value != '-')))
            {
                return false;
            }
        }
        return true;
    }

    bool PluginIdTaken(const std::vector<PluginEntry>& plugins, const std::string& id)
    {
        return std::any_of(plugins.begin(), plugins.end(), [&](const PluginEntry& entry) {
            return _stricmp(entry.id.c_str(), id.c_str()) == 0;
        });
    }

    bool GetProxyDirectory(wchar_t (&directory)[MAX_PATH])
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetProxyDirectory),
                &self))
        {
            return false;
        }
        const DWORD length = GetModuleFileNameW(self, directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            return false;
        }
        wchar_t* separator = std::wcsrchr(directory, L'\\');
        if (separator == nullptr)
        {
            return false;
        }
        separator[1] = L'\0';
        return true;
    }

    // Decides whether directory plugin discovery is enabled. The environment
    // variable (absolute, local fixed drive) wins; otherwise the INI beside
    // the proxy must contain [Plugins] Enabled=1.
    bool ConfigurePluginDirectory()
    {
        const DWORD environmentLength = GetEnvironmentVariableW(
            PluginDirectoryEnvironmentVariable, g_pluginDirectory, MAX_PATH);
        if (environmentLength != 0)
        {
            if (environmentLength >= MAX_PATH || !IsLocalAbsolutePath(g_pluginDirectory))
            {
                ReportFailure("plugin directory must be a local fixed-drive path");
                g_pluginDirectory[0] = L'\0';
                return false;
            }
            return true;
        }

        wchar_t proxyDirectory[MAX_PATH]{};
        if (!GetProxyDirectory(proxyDirectory))
        {
            return false;
        }
        wchar_t iniPath[MAX_PATH]{};
        constexpr wchar_t IniName[] = L"SoulashExtender.ini";
        if (std::wcslen(proxyDirectory) + sizeof(IniName) / sizeof(IniName[0]) > MAX_PATH)
        {
            return false;
        }
        wcscpy_s(iniPath, MAX_PATH, proxyDirectory);
        wcscat_s(iniPath, MAX_PATH, IniName);

        if (GetPrivateProfileIntW(L"Plugins", L"Enabled", 0, iniPath) != 1)
        {
            return false;
        }

        wchar_t configured[MAX_PATH]{};
        DWORD length = GetPrivateProfileStringW(
            L"Plugins", L"Directory", DefaultPluginDirectory, configured, MAX_PATH, iniPath);
        if (length == 0 || length >= MAX_PATH - 1)
        {
            return false;
        }
        if (IsLocalAbsolutePath(configured))
        {
            wcscpy_s(g_pluginDirectory, MAX_PATH, configured);
            return true;
        }
        if (std::wcslen(proxyDirectory) + length >= MAX_PATH)
        {
            ReportFailure("configured plugin directory exceeds MAX_PATH");
            return false;
        }
        wcscpy_s(g_pluginDirectory, MAX_PATH, proxyDirectory);
        wcscat_s(g_pluginDirectory, MAX_PATH, configured);
        return true;
    }

    // [Plugins] RequireHash=1 in the INI makes every plugin (directory or
    // manifest) load only if [PluginHashes] lists `<id>=<sha256 of the DLL>`.
    void LoadHashPolicy()
    {
        wchar_t directory[MAX_PATH]{};
        if (!GetProxyDirectory(directory))
        {
            return;
        }
        constexpr wchar_t IniName[] = L"SoulashExtender.ini";
        if (std::wcslen(directory) + sizeof(IniName) / sizeof(IniName[0]) > MAX_PATH)
        {
            return;
        }
        wcscpy_s(g_iniPath, MAX_PATH, directory);
        wcscat_s(g_iniPath, MAX_PATH, IniName);
        g_requireHash = GetPrivateProfileIntW(L"Plugins", L"RequireHash", 0, g_iniPath) == 1;
    }

    // Opens the DLL with write/delete sharing denied and verifies its hash. The
    // returned handle must be kept open until the library has been mapped so
    // the file cannot change between the check and the load.
    bool VerifyPluginHash(const PluginEntry& plugin, HANDLE& pin)
    {
        pin = CreateFileW(
            plugin.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pin == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        wchar_t key[130]{};
        for (std::size_t index = 0; index < plugin.id.size() && index < 128; ++index)
        {
            key[index] = static_cast<wchar_t>(plugin.id[index]);
        }
        wchar_t expected[80]{};
        GetPrivateProfileStringW(L"PluginHashes", key, L"", expected, 80, g_iniPath);
        char actual[65]{};
        if (std::wcslen(expected) != 64
            || !soulash::plugin_services::Sha256OfHandle(pin, actual))
        {
            return false;
        }
        for (unsigned index = 0; index < 64; ++index)
        {
            wchar_t want = expected[index];
            if (want >= L'A' && want <= L'F')
            {
                want = static_cast<wchar_t>(want - L'A' + L'a');
            }
            if (want != static_cast<wchar_t>(actual[index]))
            {
                return false;
            }
        }
        return true;
    }
    void CollectManifestPlugin(std::vector<PluginEntry>& plugins)
    {
        if (g_manifestPath[0] == L'\0')
        {
            return;
        }

        std::string text;
        if (!ReadManifest(g_manifestPath, text))
        {
            return;
        }

        soulash::native_modules::Manifest manifest;
        std::string parseError;
        if (!soulash::native_modules::ParseManifest(text, manifest, parseError))
        {
            ReportFailure("invalid native module manifest");
            return;
        }

        wchar_t modulePath[MAX_PATH]{};
        if (!ResolveModulePath(g_manifestPath, manifest.module, modulePath))
        {
            return;
        }
        plugins.push_back({manifest.id, modulePath});
    }

    void CollectDirectoryPlugins(std::vector<PluginEntry>& plugins)
    {
        if (g_pluginDirectory[0] == L'\0')
        {
            return;
        }

        std::wstring directory = g_pluginDirectory;
        if (directory.back() != L'\\')
        {
            directory.push_back(L'\\');
        }
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileExW(
            (directory + L"*.dll").c_str(), FindExInfoBasic, &found,
            FindExSearchNameMatch, nullptr, 0);
        if (search == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            {
                ReportFailure("cannot enumerate plugin directory", error);
            }
            return;
        }

        std::vector<std::wstring> names;
        do
        {
            if ((found.dwFileAttributes
                    & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0)
            {
                names.emplace_back(found.cFileName);
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);

        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        std::size_t accepted = 0;
        for (const std::wstring& name : names)
        {
            if (accepted == MaximumDirectoryPlugins)
            {
                ReportFailure("plugin directory has more plugins than the supported maximum");
                break;
            }

            std::string id;
            const std::size_t stemLength = name.size() - 4; // "*.dll" match guarantees the suffix
            for (std::size_t index = 0; index < stemLength; ++index)
            {
                if (name[index] > 0x7e)
                {
                    id.clear();
                    break;
                }
                id.push_back(static_cast<char>(name[index]));
            }
            if (!IsValidPluginId(id))
            {
                ReportFailure("skipping plugin whose file name is not a valid plugin id");
                continue;
            }
            if (PluginIdTaken(plugins, id))
            {
                ReportFailure("skipping plugin with a duplicate plugin id");
                continue;
            }
            plugins.push_back({id, directory + name});
            ++accepted;
        }
    }

    // Maps one plugin, registers it for crash attribution and runs its
    // initializer. Returns true when the plugin initialized successfully.
    bool LoadPlugin(const PluginEntry& plugin)
    {
        const wchar_t* path = plugin.path.c_str();
        const char* id = plugin.id.c_str();

        SoulashDiagnosticsLogModuleEvent(path, id, "loading", ERROR_SUCCESS);
        HANDLE pin = INVALID_HANDLE_VALUE;
        if (g_requireHash && !VerifyPluginHash(plugin, pin))
        {
            if (pin != INVALID_HANDLE_VALUE)
            {
                CloseHandle(pin);
            }
            SoulashDiagnosticsLogModuleEvent(path, id, "load_failed", ERROR_ACCESS_DENIED);
            ReportFailure("plugin refused: SHA-256 not listed in [PluginHashes]");
            return false;
        }
        HMODULE module = LoadLibraryExW(
            path,
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const DWORD loadError = GetLastError();
        if (pin != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pin);
        }
        if (module == nullptr)
        {
            const DWORD error = loadError;
            SoulashDiagnosticsLogModuleEvent(path, id, "load_failed", error);
            ReportFailure("cannot load native plugin", error);
            return false;
        }
        SoulashDiagnosticsLogModuleEvent(path, id, "loaded", ERROR_SUCCESS);
        if (!SoulashDiagnosticsRegisterModule(module, id))
        {
            ReportFailure("cannot register native plugin for crash attribution", GetLastError());
        }

        const auto load = reinterpret_cast<SoulashExtenderPluginLoadFn>(
            GetProcAddress(module, SOULASH_EXTENDER_PLUGIN_LOAD_EXPORT));
        const auto legacyInit = reinterpret_cast<SoulashExtenderPluginInitFn>(
            GetProcAddress(module, SOULASH_EXTENDER_PLUGIN_INIT_EXPORT));

        BOOL initialized = TRUE;
        if (load != nullptr)
        {
            const std::size_t separator = plugin.path.find_last_of(L'\\');
            const std::wstring directory = plugin.path.substr(0, separator + 1);
            const SoulashExtenderInterface* extender =
                soulash::plugin_services::CreateInterface(id, directory.c_str());
            if (extender == nullptr)
            {
                SoulashDiagnosticsLogModuleEvent(
                    path, id, "initialization_failed", ERROR_TOO_MANY_NAMES);
                ReportFailure("cannot allocate plugin interface (table full or duplicate id)");
                return false;
            }
            SoulashDiagnosticsLogModuleEvent(path, id, "initializing", ERROR_SUCCESS);
            initialized = load(extender);
        }
        else if (legacyInit != nullptr)
        {
            SoulashDiagnosticsLogModuleEvent(path, id, "initializing", ERROR_SUCCESS);
            initialized = legacyInit(&g_pluginApi);
        }
        else
        {
            SoulashDiagnosticsLogModuleEvent(path, id, "loaded_without_initializer", ERROR_SUCCESS);
            DebugLog("loaded module has no plugin entry point; DllMain only");
            return true;
        }

        if (!initialized)
        {
            SoulashDiagnosticsLogModuleEvent(
                path, id, "initialization_failed", ERROR_DLL_INIT_FAILED);
            ReportFailure("native plugin initialization returned failure");
            return false;
        }
        SoulashDiagnosticsLogModuleEvent(path, id, "initialized", ERROR_SUCCESS);
        return true;
    }

    // ---- Sandboxed Lua scripts --------------------------------------------
    // Each *.lua file in the script directory runs in its own sandbox
    // (src/script_sandbox.cpp). Scripts are not native code, so they are not
    // subject to plugin hash pinning; they can only log and are resource-capped.
    // [Scripts] Enabled=1 and Directory= in the INI, or SOULASH2_SCRIPT_DIRECTORY.
    constexpr wchar_t ScriptDirectoryEnvironmentVariable[] = L"SOULASH2_SCRIPT_DIRECTORY";
    constexpr wchar_t DefaultScriptDirectory[] = L"Scripts";
    constexpr std::size_t MaximumScripts = 64;

    struct ScriptHost
    {
        std::string id;
        std::unique_ptr<soulash::script::Sandbox> sandbox;
    };
    std::vector<std::unique_ptr<ScriptHost>> g_scripts;

    bool ConfigureScriptDirectory()
    {
        const DWORD environmentLength = GetEnvironmentVariableW(
            ScriptDirectoryEnvironmentVariable, g_scriptDirectory, MAX_PATH);
        if (environmentLength != 0)
        {
            if (environmentLength >= MAX_PATH || !IsLocalAbsolutePath(g_scriptDirectory))
            {
                ReportFailure("script directory must be a local fixed-drive path");
                g_scriptDirectory[0] = L'\0';
                return false;
            }
            return true;
        }

        if (g_iniPath[0] == L'\0'
            || GetPrivateProfileIntW(L"Scripts", L"Enabled", 0, g_iniPath) != 1)
        {
            return false;
        }
        wchar_t proxyDirectory[MAX_PATH]{};
        wchar_t configured[MAX_PATH]{};
        const DWORD length = GetPrivateProfileStringW(
            L"Scripts", L"Directory", DefaultScriptDirectory, configured, MAX_PATH, g_iniPath);
        if (!GetProxyDirectory(proxyDirectory) || length == 0 || length >= MAX_PATH - 1)
        {
            return false;
        }
        if (IsLocalAbsolutePath(configured))
        {
            wcscpy_s(g_scriptDirectory, MAX_PATH, configured);
            return true;
        }
        if (std::wcslen(proxyDirectory) + length >= MAX_PATH)
        {
            ReportFailure("configured script directory exceeds MAX_PATH");
            return false;
        }
        wcscpy_s(g_scriptDirectory, MAX_PATH, proxyDirectory);
        wcscat_s(g_scriptDirectory, MAX_PATH, configured);
        return true;
    }

    void ScriptLog(void* context, const char* message)
    {
        const auto* host = static_cast<const ScriptHost*>(context);
        const std::string line = "script " + host->id + ": " + message;
        SoulashDiagnosticsWritePluginMessage(line.c_str());
    }

    void LogScriptProblem(const std::string& id, const char* what, const std::string& detail)
    {
        const std::string line = "script " + id + " " + what + ": " + detail;
        SoulashDiagnosticsWritePluginMessage(line.c_str());
        DebugLog(line.c_str());
    }

    bool ReadScriptFile(const std::wstring& path, std::string& text)
    {
        HANDLE file = CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0
            && size.QuadPart <= static_cast<LONGLONG>(soulash::script::Limits{}.sourceBytes);
        if (ok)
        {
            text.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            ok = text.empty()
                || (ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)
                    && read == text.size());
        }
        CloseHandle(file);
        return ok;
    }

    // Runs every script, then calls its optional global on_load().
    void LoadScripts()
    {
        if (g_scriptDirectory[0] == L'\0')
        {
            return;
        }
        std::wstring directory = g_scriptDirectory;
        if (directory.back() != L'\\')
        {
            directory.push_back(L'\\');
        }
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileExW(
            (directory + L"*.lua").c_str(), FindExInfoBasic, &found,
            FindExSearchNameMatch, nullptr, 0);
        if (search == INVALID_HANDLE_VALUE)
        {
            return;
        }
        std::vector<std::wstring> names;
        do
        {
            if ((found.dwFileAttributes
                    & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0)
            {
                names.emplace_back(found.cFileName);
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        for (const std::wstring& name : names)
        {
            if (g_scripts.size() >= MaximumScripts)
            {
                ReportFailure("script directory has more scripts than the supported maximum");
                break;
            }
            std::string id;
            for (std::size_t index = 0; index + 4 < name.size(); ++index)
            {
                if (name[index] > 0x7e)
                {
                    id.clear();
                    break;
                }
                id.push_back(static_cast<char>(name[index]));
            }
            if (!IsValidPluginId(id))
            {
                ReportFailure("skipping script whose file name is not a valid id");
                continue;
            }

            std::string source;
            if (!ReadScriptFile(directory + name, source))
            {
                LogScriptProblem(id, "not loaded", "cannot read the file or it exceeds the size limit");
                continue;
            }
            auto host = std::make_unique<ScriptHost>();
            host->id = id;
            host->sandbox = std::make_unique<soulash::script::Sandbox>(
                soulash::script::Limits{}, ScriptLog, host.get());
            std::string error;
            if (!host->sandbox->Valid() || !host->sandbox->Run(id, source, error))
            {
                LogScriptProblem(id, "failed", error.empty() ? "sandbox unavailable" : error);
                continue;
            }
            std::string result;
            if (!host->sandbox->Call("on_load", {}, result, error)
                && error.find("no function named") == std::string::npos)
            {
                LogScriptProblem(id, "on_load failed", error);
            }
            SoulashDiagnosticsWritePluginMessage(("script " + id + " loaded").c_str());
            g_scripts.push_back(std::move(host));
        }
    }

    // ---- Skill-conditions files ---------------------------------------------
    // Every <mods>\<mod>\extender\conditions.json is loaded with the strict
    // loader (src/conditions.cpp) and the outcome is logged, so authors get
    // feedback from the real environment. Nothing evaluates them yet: there is
    // no game-state source. [Conditions] Enabled=1 and Directory= (default
    // <game folder>\data\mods) in the INI, or SOULASH2_MODS_DIRECTORY.
    constexpr wchar_t ModsDirectoryEnvironmentVariable[] = L"SOULASH2_MODS_DIRECTORY";
    constexpr std::size_t MaximumConditionMods = 512;

    struct ModConditions
    {
        std::string mod;
        soulash::conditions::ConditionSet set;
    };
    std::vector<std::unique_ptr<ModConditions>> g_conditionSets;
    wchar_t g_modsDirectory[MAX_PATH]{};

    bool ConfigureConditionsDirectory()
    {
        const DWORD environmentLength = GetEnvironmentVariableW(
            ModsDirectoryEnvironmentVariable, g_modsDirectory, MAX_PATH);
        if (environmentLength != 0)
        {
            if (environmentLength >= MAX_PATH || !IsLocalAbsolutePath(g_modsDirectory))
            {
                ReportFailure("mods directory must be a local fixed-drive path");
                g_modsDirectory[0] = L'\0';
                return false;
            }
            return true;
        }
        if (g_iniPath[0] == L'\0'
            || GetPrivateProfileIntW(L"Conditions", L"Enabled", 0, g_iniPath) != 1)
        {
            return false;
        }

        wchar_t configured[MAX_PATH]{};
        const DWORD length = GetPrivateProfileStringW(
            L"Conditions", L"Directory", L"", configured, MAX_PATH, g_iniPath);
        if (length != 0)
        {
            if (length >= MAX_PATH || !IsLocalAbsolutePath(configured))
            {
                ReportFailure("[Conditions] Directory must be a local fixed-drive path");
                return false;
            }
            wcscpy_s(g_modsDirectory, MAX_PATH, configured);
            return true;
        }

        // Default: data\mods beside the game executable.
        wchar_t executable[MAX_PATH]{};
        const DWORD executableLength = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        wchar_t* separator = executableLength != 0 && executableLength < MAX_PATH
            ? std::wcsrchr(executable, L'\\') : nullptr;
        constexpr wchar_t Relative[] = L"data\\mods";
        if (separator == nullptr
            || static_cast<std::size_t>(separator + 1 - executable) + sizeof(Relative) / sizeof(Relative[0]) > MAX_PATH)
        {
            return false;
        }
        separator[1] = L'\0';
        wcscpy_s(g_modsDirectory, MAX_PATH, executable);
        wcscat_s(g_modsDirectory, MAX_PATH, Relative);
        return true;
    }

    std::string NarrowName(const wchar_t* name)
    {
        std::string out;
        for (const wchar_t* c = name; *c != L'\0'; ++c)
        {
            const bool plain = *c >= 0x20 && *c <= 0x7e;
            out.push_back(plain ? static_cast<char>(*c) : '?');
        }
        return out;
    }

    void LoadConditionFiles()
    {
        if (g_modsDirectory[0] == L'\0')
        {
            return;
        }
        std::wstring root = g_modsDirectory;
        if (root.back() != L'\\')
        {
            root.push_back(L'\\');
        }
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileExW((root + L"*").c_str(), FindExInfoBasic, &found,
            FindExSearchNameMatch, nullptr, 0);
        if (search == INVALID_HANDLE_VALUE)
        {
            return;
        }
        std::vector<std::wstring> mods;
        do
        {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                && (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
                && found.cFileName[0] != L'.')
            {
                mods.emplace_back(found.cFileName);
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
        std::sort(mods.begin(), mods.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        std::size_t totalConditions = 0;
        std::size_t loadedMods = 0;
        for (const std::wstring& mod : mods)
        {
            if (g_conditionSets.size() >= MaximumConditionMods)
            {
                break;
            }
            const std::wstring path = root + mod + L"\\extender\\conditions.json";
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                continue;
            }
            const std::string name = NarrowName(mod.c_str());
            std::string text;
            if (!ReadScriptFile(path, text))
            {
                SoulashDiagnosticsWritePluginMessage(
                    ("conditions " + name + " rejected: cannot read the file or it exceeds 64 KiB").c_str());
                continue;
            }
            auto entry = std::make_unique<ModConditions>();
            entry->mod = name;
            std::string error;
            if (!entry->set.Load(text, error))
            {
                SoulashDiagnosticsWritePluginMessage(
                    ("conditions " + name + " rejected: " + error).c_str());
                continue;
            }
            SoulashDiagnosticsWritePluginMessage(
                ("conditions " + name + ": " + std::to_string(entry->set.Items().size())
                    + " condition(s) loaded").c_str());
            totalConditions += entry->set.Items().size();
            ++loadedMods;
            g_conditionSets.push_back(std::move(entry));
        }
        SoulashDiagnosticsWritePluginMessage(
            ("conditions: " + std::to_string(totalConditions) + " condition(s) from "
                + std::to_string(loadedMods) + " mod(s)").c_str());
    }

    DWORD WINAPI LoadNativeModules(void*)
    {
        soulash::plugin_services::Initialize();

        std::vector<PluginEntry> plugins;
        CollectManifestPlugin(plugins);
        std::vector<PluginEntry> directoryPlugins;
        CollectDirectoryPlugins(directoryPlugins);
        for (PluginEntry& entry : directoryPlugins)
        {
            if (PluginIdTaken(plugins, entry.id))
            {
                ReportFailure("skipping directory plugin that duplicates the manifest plugin id");
                continue;
            }
            plugins.push_back(std::move(entry));
        }

        unsigned failures = 0;
        for (const PluginEntry& plugin : plugins)
        {
            if (!LoadPlugin(plugin))
            {
                ++failures;
            }
        }

        LoadScripts();
        LoadConditionFiles();
        soulash::plugin_services::DispatchCoreMessage(SOULASH_EXTENDER_MSG_POSTLOAD);
        soulash::plugin_services::DispatchCoreMessage(SOULASH_EXTENDER_MSG_POSTPOSTLOAD);

        DebugLog(failures == 0 ? "native plugins loaded" : "native plugins loaded with failures");
        InterlockedExchange(&g_startState, 2);
        return failures == 0 ? 0 : 1;
    }
}

namespace soulash::module_runtime
{
    void StartOptInModule()
    {
        if (InterlockedCompareExchange(&g_startState, 1, 0) != 0)
        {
            return;
        }

        const DWORD pathLength = GetEnvironmentVariableW(
            ManifestEnvironmentVariable, g_manifestPath, MAX_PATH);
        if (pathLength != 0
            && (pathLength >= MAX_PATH || !IsLocalAbsolutePath(g_manifestPath)))
        {
            ReportFailure("native module manifest must be a local fixed-drive path");
            g_manifestPath[0] = L'\0';
        }
        else if (pathLength == 0)
        {
            g_manifestPath[0] = L'\0';
        }

        LoadHashPolicy();
        const bool directoryEnabled = ConfigurePluginDirectory();
        const bool scriptsEnabled = ConfigureScriptDirectory();
        const bool conditionsEnabled = ConfigureConditionsDirectory();
        if (g_manifestPath[0] == L'\0' && !directoryEnabled && !scriptsEnabled && !conditionsEnabled)
        {
            InterlockedExchange(&g_startState, 2);
            return;
        }

        HMODULE self = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&StartOptInModule),
                &self))
        {
            ReportFailure("cannot pin proxy for native module startup", GetLastError());
            InterlockedExchange(&g_startState, 2);
            return;
        }

        HANDLE thread = CreateThread(nullptr, 0, LoadNativeModules, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
            ReportFailure("cannot start native module loader thread", GetLastError());
            InterlockedExchange(&g_startState, 2);
            return;
        }
        CloseHandle(thread);
    }
}
