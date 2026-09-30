#include "plugin_api.h"

#include <windows.h>

#include <cstdint>

namespace
{
    const SoulashExtenderApi* retainedApi = nullptr;

    void WriteMarker(const char* entry)
    {
        wchar_t markerPath[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(
            L"SOULASH2_TEST_MODULE_MARKER", markerPath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            return;
        }

        HANDLE file = CreateFileW(
            markerPath,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        DWORD written = 0;
        WriteFile(file, entry, static_cast<DWORD>(lstrlenA(entry)), &written, nullptr);
        WriteFile(file, "\r\n", 2, &written, nullptr);
        CloseHandle(file);
    }

    DWORD WINAPI LogAfterInitialization(void*)
    {
        Sleep(100);
        if (retainedApi != nullptr)
        {
            retainedApi->log("synthetic delayed plugin message");
            WriteMarker("delayed-log-v2");
        }
        return 0;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SoulashExtenderPluginInit(const SoulashExtenderApi* api)
{
    if (api == nullptr
        || api->structureSize != sizeof(SoulashExtenderApi)
        || api->abiVersion != SOULASH_EXTENDER_PLUGIN_ABI_VERSION
        || api->log == nullptr
        || api->reportFileIssue == nullptr)
    {
        return FALSE;
    }

    retainedApi = api;
    api->log("synthetic plugin initialized");
    api->log("synthetic multiline\r\nmessage");
    api->log(nullptr);
    if (!api->reportFileIssue(
            L"C:\\Synthetic\\bad\r\n-config.json",
            SOULASH_EXTENDER_FILE_PARSE,
            "malformed JSON\r\nexpected object")
        || api->reportFileIssue(
            L"C:\\Synthetic\\bad-config.json",
            999,
            "invalid operation must fail"))
    {
        return FALSE;
    }
    char longReason[700];
    for (unsigned int index = 0; index < sizeof(longReason) - 1; ++index)
    {
        longReason[index] = 'R';
    }
    longReason[sizeof(longReason) - 1] = '\0';
    if (!api->reportFileIssue(
            L"C:\\Synthetic\\long-reason.json",
            SOULASH_EXTENDER_FILE_VALIDATE,
            longReason))
    {
        return FALSE;
    }
    WriteMarker("plugin-init-v1");
    HANDLE thread = CreateThread(nullptr, 0, LogAfterInitialization, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI SyntheticTriggerAccessViolation()
{
    __try
    {
        *reinterpret_cast<volatile int*>(static_cast<std::uintptr_t>(1)) = 7;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return TRUE;
    }
    return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        WriteMarker("dllmain");
    }
    return TRUE;
}
