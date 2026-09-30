#include <windows.h>
#include <intrin.h>

#include <cstddef>
#include <cwchar>

#include "module_runtime.hpp"

extern "C" BOOL WINAPI SoulashDiagnosticsStart(const wchar_t* logPath);

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

    bool ReadDiagnosticsIniPath(wchar_t (&logPath)[MAX_PATH])
    {
        wchar_t proxyPath[MAX_PATH]{};
        if (!GetProxyPath(proxyPath))
        {
            return false;
        }

        wchar_t* fileName = std::wcsrchr(proxyPath, L'\\');
        if (fileName == nullptr)
        {
            return false;
        }
        ++fileName;
        constexpr wchar_t IniName[] = L"SoulashExtender.ini";
        constexpr std::size_t IniNameLength = sizeof(IniName) / sizeof(IniName[0]);
        const std::size_t directoryLength = static_cast<std::size_t>(fileName - proxyPath);
        if (directoryLength + IniNameLength > MAX_PATH)
        {
            return false;
        }
        std::wmemcpy(fileName, IniName, IniNameLength);

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
        SetLastError(previousError);
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
        constexpr wchar_t OriginalFileName[] = L"BugSplat64_original.dll";
        constexpr std::size_t OriginalFileNameLength =
            sizeof(OriginalFileName) / sizeof(OriginalFileName[0]);
        const std::size_t directoryLength = static_cast<std::size_t>(fileName - originalPath);
        if (directoryLength + OriginalFileNameLength > MAX_PATH)
        {
            g_original.error = ERROR_FILENAME_EXCED_RANGE;
            return TRUE;
        }
        std::wmemcpy(fileName, OriginalFileName, OriginalFileNameLength);

        HMODULE original = LoadLibraryExW(
            originalPath,
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (original == nullptr)
        {
            g_original.error = GetLastError();
            return TRUE;
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
