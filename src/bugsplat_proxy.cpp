#include <windows.h>
#include <intrin.h>

#include <cstddef>
#include <cstdio>
#include <cwchar>

#include "module_runtime.hpp"
#include "plugin_services.hpp"

extern "C" BOOL WINAPI SoulashDiagnosticsStart(const wchar_t* logPath);
extern "C" BOOL WINAPI SoulashDiagnosticsWritePluginMessage(const char* message);

namespace
{
    using Constructor = void* (__cdecl*)(
        void*,
        const wchar_t*,
        const wchar_t*,
        const wchar_t*,
        const wchar_t*,
        unsigned long);
    using Destructor = void (__cdecl*)(void*);
    using SendAdditionalFile = void (__cdecl*)(void*, const wchar_t*);
    using SetGuardByteBufferSize = int (__cdecl*)(void*, int);

    struct OriginalApi
    {
        HMODULE module = nullptr;
        Constructor constructor = nullptr;
        Destructor destructor = nullptr;
        SendAdditionalFile sendAdditionalFile = nullptr;
        SetGuardByteBufferSize setGuardByteBufferSize = nullptr;
        DWORD error = ERROR_SUCCESS;
    };

    INIT_ONCE g_initialization = INIT_ONCE_STATIC_INIT;
    OriginalApi g_original;
    volatile LONG g_diagnosticsChecked = 0;
    char g_chainNote[160]{};

    bool GetProxyPath(wchar_t (&path)[MAX_PATH])
    {
        HMODULE proxyModule = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetProxyPath),
                &proxyModule))
        {
            return false;
        }

        const DWORD length = GetModuleFileNameW(proxyModule, path, MAX_PATH);
        return length != 0 && length < MAX_PATH;
    }

    // Fills `iniPath` with the SoulashExtender.ini path beside the proxy DLL.
    bool BuildIniPath(wchar_t (&iniPath)[MAX_PATH])
    {
        if (!GetProxyPath(iniPath))
        {
            return false;
        }

        wchar_t* fileName = std::wcsrchr(iniPath, L'\\');
        if (fileName == nullptr)
        {
            return false;
        }
        ++fileName;
        constexpr wchar_t IniName[] = L"SoulashExtender.ini";
        constexpr std::size_t IniNameLength = sizeof(IniName) / sizeof(IniName[0]);
        const std::size_t directoryLength = static_cast<std::size_t>(fileName - iniPath);
        if (directoryLength + IniNameLength > MAX_PATH)
        {
            return false;
        }
        std::wmemcpy(fileName, IniName, IniNameLength);
        return true;
    }

    bool ReadDiagnosticsIniPath(wchar_t (&logPath)[MAX_PATH])
    {
        wchar_t proxyPath[MAX_PATH]{};
        if (!BuildIniPath(proxyPath))
        {
            return false;
        }

        wchar_t* fileName = nullptr;
        if (GetPrivateProfileIntW(L"Diagnostics", L"Enabled", 0, proxyPath) != 1)
        {
            return false;
        }

        wchar_t configuredPath[MAX_PATH]{};
        const DWORD pathLength = GetPrivateProfileStringW(
            L"Diagnostics", L"LogPath", L"", configuredPath,
            static_cast<DWORD>(sizeof(configuredPath) / sizeof(configuredPath[0])),
            proxyPath);
        if (pathLength == 0 || pathLength >= MAX_PATH)
        {
            return false;
        }

        const bool absolutePath =
            ((configuredPath[0] >= L'a' && configuredPath[0] <= L'z')
                || (configuredPath[0] >= L'A' && configuredPath[0] <= L'Z'))
            && configuredPath[1] == L':'
            && configuredPath[2] == L'\\';
        if (absolutePath)
        {
            std::wmemcpy(logPath, configuredPath, pathLength + 1);
            return true;
        }

        fileName = std::wcsrchr(proxyPath, L'\\');
        if (fileName == nullptr)
        {
            return false;
        }
        ++fileName;
        const std::size_t rootLength = static_cast<std::size_t>(fileName - proxyPath);
        if (rootLength + pathLength >= MAX_PATH)
        {
            return false;
        }
        std::wmemcpy(logPath, proxyPath, rootLength);
        std::wmemcpy(logPath + rootLength, configuredPath, pathLength + 1);
        return true;
    }

    [[noreturn]] void FailProxy(DWORD error)
    {
        OutputDebugStringA(
            "Soulash BugSplat proxy: BugSplat64_original.dll is missing or does not export the expected x64 ABI.\n");
        SetLastError(error);
        RaiseFailFastException(nullptr, nullptr, 0);
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }

    void StartOptInDiagnostics()
    {
        if (InterlockedCompareExchange(&g_diagnosticsChecked, 1, 0) != 0)
        {
            return;
        }

        const DWORD previousError = GetLastError();
        wchar_t logPath[MAX_PATH]{};
        const DWORD pathLength = GetEnvironmentVariableW(
            L"SOULASH2_DIAGNOSTICS_LOG",
            logPath,
            static_cast<DWORD>(sizeof(logPath) / sizeof(logPath[0])));
        if (pathLength != 0 && pathLength < sizeof(logPath) / sizeof(logPath[0]))
        {
            SoulashDiagnosticsStart(logPath);
        }
        else if (pathLength == 0 && ReadDiagnosticsIniPath(logPath))
        {
            SoulashDiagnosticsStart(logPath);
        }
        if (g_chainNote[0] != '\0')
        {
            SoulashDiagnosticsWritePluginMessage(g_chainNote);
        }
        SetLastError(previousError);
    }

    bool IsValidChainFileName(const wchar_t* name, const wchar_t* selfName)
    {
        const std::size_t length = std::wcslen(name);
        if (length < 5 || length > 100 || _wcsicmp(name + length - 4, L".dll") != 0
            || _wcsicmp(name, selfName) == 0)
        {
            return false;
        }
        for (std::size_t index = 0; index < length; ++index)
        {
            const wchar_t value = name[index];
            const bool alphaNumeric = (value >= L'a' && value <= L'z')
                || (value >= L'A' && value <= L'Z') || (value >= L'0' && value <= L'9');
            if (!alphaNumeric && value != L'.' && value != L'_' && value != L'-')
            {
                return false;
            }
        }
        return name[0] != L'.';
    }

    // Chain loading: instead of the sibling BugSplat64_original.dll, the proxy
    // can forward to another extender's BugSplat64-compatible DLL, which in
    // turn forwards to the real one. The target is a plain file name in the
    // proxy's own directory: SOULASH2_CHAIN_NEXT, else [Chain] Next in the INI.
    DWORD ResolveChainTarget(
        const wchar_t* selfName,
        wchar_t (&targetName)[MAX_PATH],
        bool& chained,
        wchar_t (&expectedHash)[80],
        bool& hashRequired)
    {
        wchar_t iniPath[MAX_PATH]{};
        const bool haveIni = BuildIniPath(iniPath);

        wchar_t configured[MAX_PATH]{};
        DWORD length = GetEnvironmentVariableW(L"SOULASH2_CHAIN_NEXT", configured, MAX_PATH);
        if (length == 0 && haveIni)
        {
            length = GetPrivateProfileStringW(L"Chain", L"Next", L"", configured, MAX_PATH, iniPath);
        }
        if (length == 0)
        {
            return ERROR_SUCCESS;
        }
        if (length >= MAX_PATH || !IsValidChainFileName(configured, selfName))
        {
            return ERROR_INVALID_NAME;
        }

        wcscpy_s(targetName, MAX_PATH, configured);
        chained = true;
        if (GetEnvironmentVariableW(L"SOULASH2_CHAIN_NEXT_SHA256", expectedHash, 80) == 0 && haveIni)
        {
            GetPrivateProfileStringW(L"Chain", L"NextSha256", L"", expectedHash, 80, iniPath);
        }
        hashRequired = haveIni
            && GetPrivateProfileIntW(L"Plugins", L"RequireHash", 0, iniPath) == 1;
        return ERROR_SUCCESS;
    }

    BOOL CALLBACK InitializeOriginal(PINIT_ONCE, PVOID, PVOID*)
    {
        HMODULE proxyModule = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&InitializeOriginal),
                &proxyModule))
        {
            g_original.error = GetLastError();
            return TRUE;
        }

        wchar_t originalPath[MAX_PATH]{};
        const DWORD pathLength = GetModuleFileNameW(proxyModule, originalPath, MAX_PATH);
        if (pathLength == 0 || pathLength >= MAX_PATH)
        {
            g_original.error = pathLength == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE;
            return TRUE;
        }

        wchar_t* fileName = std::wcsrchr(originalPath, L'\\');
        if (fileName == nullptr)
        {
            fileName = std::wcsrchr(originalPath, L'/');
        }
        if (fileName == nullptr)
        {
            g_original.error = ERROR_BAD_PATHNAME;
            return TRUE;
        }

        ++fileName;
        wchar_t targetName[MAX_PATH] = L"BugSplat64_original.dll";
        wchar_t expectedHash[80]{};
        bool chained = false;
        bool hashRequired = false;
        const DWORD chainError = ResolveChainTarget(fileName, targetName, chained, expectedHash, hashRequired);
        if (chainError != ERROR_SUCCESS)
        {
            g_original.error = chainError;
            return TRUE;
        }

        const std::size_t targetNameLength = std::wcslen(targetName) + 1;
        const std::size_t directoryLength = static_cast<std::size_t>(fileName - originalPath);
        if (directoryLength + targetNameLength > MAX_PATH)
        {
            g_original.error = ERROR_FILENAME_EXCED_RANGE;
            return TRUE;
        }
        std::wmemcpy(fileName, targetName, targetNameLength);

        // Two instances of this proxy in one process would each start their own
        // diagnostics and plugins, so only the first may initialize.
        wchar_t guardName[64];
        swprintf_s(guardName, L"Local\\SoulashExtenderProxyActive.%lu", GetCurrentProcessId());
        HANDLE guard = CreateMutexW(nullptr, FALSE, guardName);
        if (guard != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(guard);
            g_original.error = ERROR_ALREADY_EXISTS;
            return TRUE;
        }
        // The guard handle is intentionally kept for the life of the process.

        // A chained extender runs with full privileges: pin it by hash when a
        // hash is configured, and require one under [Plugins] RequireHash=1.
        HANDLE pin = INVALID_HANDLE_VALUE;
        if (chained && (hashRequired || expectedHash[0] != L'\0'))
        {
            bool verified = false;
            pin = CreateFileW(
                originalPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            char actual[65]{};
            if (pin != INVALID_HANDLE_VALUE && std::wcslen(expectedHash) == 64
                && soulash::plugin_services::Sha256OfHandle(pin, actual))
            {
                verified = true;
                for (unsigned index = 0; index < 64; ++index)
                {
                    wchar_t want = expectedHash[index];
                    if (want >= L'A' && want <= L'F')
                    {
                        want = static_cast<wchar_t>(want - L'A' + L'a');
                    }
                    verified = verified && want == static_cast<wchar_t>(actual[index]);
                }
            }
            if (!verified)
            {
                if (pin != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(pin);
                }
                g_original.error = ERROR_ACCESS_DENIED;
                return TRUE;
            }
        }

        HMODULE original = LoadLibraryExW(
            originalPath,
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const DWORD loadError = GetLastError();
        if (pin != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pin);
        }
        if (original == nullptr)
        {
            g_original.error = loadError;
            return TRUE;
        }
        if (original == proxyModule)
        {
            g_original.error = ERROR_CIRCULAR_DEPENDENCY;
            return TRUE;
        }
        if (chained)
        {
            _snprintf_s(g_chainNote, sizeof(g_chainNote), _TRUNCATE,
                "chain_next loaded name=%ls", targetName);
        }

        g_original.constructor = reinterpret_cast<Constructor>(
            GetProcAddress(original, "?" "?0MiniDmpSender@@QEAA@PEB_W000K@Z"));
        g_original.destructor = reinterpret_cast<Destructor>(
            GetProcAddress(original, "?" "?1MiniDmpSender@@UEAA@XZ"));
        g_original.sendAdditionalFile = reinterpret_cast<SendAdditionalFile>(
            GetProcAddress(original, "?sendAdditionalFile@MiniDmpSender@@QEAAXPEB_W@Z"));
        g_original.setGuardByteBufferSize = reinterpret_cast<SetGuardByteBufferSize>(
            GetProcAddress(original, "?setGuardByteBufferSize@MiniDmpSender@@QEAAHH@Z"));

        if (g_original.constructor == nullptr
            || g_original.destructor == nullptr
            || g_original.sendAdditionalFile == nullptr
            || g_original.setGuardByteBufferSize == nullptr)
        {
            g_original.error = ERROR_PROC_NOT_FOUND;
            FreeLibrary(original);
            g_original.constructor = nullptr;
            g_original.destructor = nullptr;
            g_original.sendAdditionalFile = nullptr;
            g_original.setGuardByteBufferSize = nullptr;
            return TRUE;
        }

        g_original.module = original;
        return TRUE;
    }

    // The game skips constructing its crash sender when a debugger is
    // attached (it calls IsDebuggerPresent first in main), so no proxy export
    // would ever be called and nothing would load. In that case the same
    // opt-in start-up is triggered from a thread created in DllMain; the
    // thread cannot run before the loader lock is released.
    // Disable with SOULASH2_DEBUGGER_FALLBACK=0 or [Extender] DebuggerFallback=0.
    DWORD WINAPI DebuggerFallbackThread(void*)
    {
        bool enabled = true;
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(
            L"SOULASH2_DEBUGGER_FALLBACK", value, sizeof(value) / sizeof(value[0]));
        if (length != 0)
        {
            enabled = !(length == 1 && value[0] == L'0');
        }
        else
        {
            wchar_t iniPath[MAX_PATH]{};
            if (BuildIniPath(iniPath))
            {
                enabled = GetPrivateProfileIntW(L"Extender", L"DebuggerFallback", 1, iniPath) != 0;
            }
        }

        if (enabled)
        {
            OutputDebugStringA(
                "Soulash extender: debugger detected; starting without the BugSplat trigger.\r\n");
            StartOptInDiagnostics();
            soulash::module_runtime::StartOptInModule();
        }
        return 0;
    }

    const OriginalApi& GetOriginal()
    {
        if (!InitOnceExecuteOnce(&g_initialization, InitializeOriginal, nullptr, nullptr))
        {
            FailProxy(GetLastError());
        }
        if (g_original.error != ERROR_SUCCESS || g_original.module == nullptr)
        {
            FailProxy(g_original.error == ERROR_SUCCESS ? ERROR_DLL_NOT_FOUND : g_original.error);
        }
        return g_original;
    }
}

extern "C" void* __cdecl ProxyConstructor(
    void* self,
    const wchar_t* applicationName,
    const wchar_t* email,
    const wchar_t* key,
    const wchar_t* url,
    unsigned long flags)
{
    const OriginalApi& original = GetOriginal();
    StartOptInDiagnostics();
    void* result = original.constructor(self, applicationName, email, key, url, flags);
    const DWORD lastError = GetLastError();
    soulash::module_runtime::StartOptInModule();
    SetLastError(lastError);
    return result;
}

extern "C" void __cdecl ProxyDestructor(void* self)
{
    const OriginalApi& original = GetOriginal();
    StartOptInDiagnostics();
    original.destructor(self);
    const DWORD lastError = GetLastError();
    soulash::module_runtime::StartOptInModule();
    SetLastError(lastError);
}

extern "C" void __cdecl ProxySendAdditionalFile(void* self, const wchar_t* path)
{
    const OriginalApi& original = GetOriginal();
    StartOptInDiagnostics();
    original.sendAdditionalFile(self, path);
    const DWORD lastError = GetLastError();
    soulash::module_runtime::StartOptInModule();
    SetLastError(lastError);
}

extern "C" int __cdecl ProxySetGuardByteBufferSize(void* self, int size)
{
    const OriginalApi& original = GetOriginal();
    StartOptInDiagnostics();
    const int result = original.setGuardByteBufferSize(self, size);
    const DWORD lastError = GetLastError();
    soulash::module_runtime::StartOptInModule();
    SetLastError(lastError);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH && IsDebuggerPresent())
    {
        // Only create a thread here: it starts after the loader lock is
        // released. Pinning keeps the proxy mapped while that thread runs.
        HMODULE self = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&DllMain),
                &self))
        {
            HANDLE thread = CreateThread(nullptr, 0, DebuggerFallbackThread, nullptr, 0, nullptr);
            if (thread != nullptr)
            {
                CloseHandle(thread);
            }
        }
    }
    return TRUE;
}
