#include "manifest.hpp"
#include "module_runtime.hpp"
#include "plugin_api.h"

#include <windows.h>

#include <string>

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
    constexpr wchar_t ManifestEnvironmentVariable[] = L"SOULASH2_NATIVE_MODULE_MANIFEST";

    volatile LONG g_startState = 0;
    wchar_t g_manifestPath[MAX_PATH]{};
    HMODULE g_loadedModule = nullptr;

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

    bool IsLocalManifestPath(const wchar_t* path)
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

    DWORD WINAPI LoadNativeModule(void*)
    {
        std::string text;
        if (!ReadManifest(g_manifestPath, text))
        {
            InterlockedExchange(&g_startState, 2);
            return 1;
        }

        soulash::native_modules::Manifest manifest;
        std::string parseError;
        if (!soulash::native_modules::ParseManifest(text, manifest, parseError))
        {
            ReportFailure("invalid native module manifest");
            InterlockedExchange(&g_startState, 2);
            return 1;
        }

        wchar_t modulePath[MAX_PATH]{};
        if (!ResolveModulePath(g_manifestPath, manifest.module, modulePath))
        {
            InterlockedExchange(&g_startState, 2);
            return 1;
        }

        SoulashDiagnosticsLogModuleEvent(
            modulePath, manifest.id.c_str(), "loading", ERROR_SUCCESS);
        g_loadedModule = LoadLibraryExW(
            modulePath,
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (g_loadedModule == nullptr)
        {
            const DWORD error = GetLastError();
            SoulashDiagnosticsLogModuleEvent(
                modulePath, manifest.id.c_str(), "load_failed", error);
            ReportFailure("cannot load manifest-listed native module", error);
            InterlockedExchange(&g_startState, 2);
            return 1;
        }
        SoulashDiagnosticsLogModuleEvent(
            modulePath, manifest.id.c_str(), "loaded", ERROR_SUCCESS);
        if (!SoulashDiagnosticsRegisterModule(g_loadedModule, manifest.id.c_str()))
        {
            ReportFailure("cannot register native module for crash attribution", GetLastError());
        }

        const auto initialize = reinterpret_cast<SoulashExtenderPluginInitFn>(
            GetProcAddress(g_loadedModule, SOULASH_EXTENDER_PLUGIN_INIT_EXPORT));
        if (initialize != nullptr)
        {
            SoulashDiagnosticsLogModuleEvent(
                modulePath, manifest.id.c_str(), "initializing", ERROR_SUCCESS);
            if (!initialize(&g_pluginApi))
            {
                SoulashDiagnosticsLogModuleEvent(
                    modulePath, manifest.id.c_str(),
                    "initialization_failed", ERROR_DLL_INIT_FAILED);
                ReportFailure("native module plugin initialization returned failure");
                InterlockedExchange(&g_startState, 2);
                return 1;
            }
            SoulashDiagnosticsLogModuleEvent(
                modulePath, manifest.id.c_str(), "initialized", ERROR_SUCCESS);
        }
        else
        {
            SoulashDiagnosticsLogModuleEvent(
                modulePath, manifest.id.c_str(),
                "loaded_without_initializer", ERROR_SUCCESS);
            DebugLog("loaded module has no optional SoulashExtenderPluginInit; DllMain only");
        }

        DebugLog("manifest-listed native module loaded");
        InterlockedExchange(&g_startState, 2);
        return 0;
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
        if (pathLength == 0)
        {
            InterlockedExchange(&g_startState, 2);
            return;
        }
        if (pathLength >= MAX_PATH || !IsLocalManifestPath(g_manifestPath))
        {
            ReportFailure("native module manifest must be a local fixed-drive path");
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

        HANDLE thread = CreateThread(nullptr, 0, LoadNativeModule, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
            ReportFailure("cannot start native module loader thread", GetLastError());
            InterlockedExchange(&g_startState, 2);
            return;
        }
        CloseHandle(thread);
    }
}
