#include "manifest.hpp"
#include "plugin_api.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr DWORD FailFastExceptionStatus = 0xC0000602u;
    constexpr wchar_t DiagnosticsEnvironmentVariable[] = L"SOULASH2_DIAGNOSTICS_LOG";
    int failures = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    bool Parses(std::string_view text, soulash::native_modules::Manifest* result = nullptr)
    {
        soulash::native_modules::Manifest manifest;
        std::string error;
        const bool parsed = soulash::native_modules::ParseManifest(text, manifest, error);
        if (parsed && result != nullptr)
        {
            *result = std::move(manifest);
        }
        return parsed;
    }

    void TestManifestParser()
    {
        constexpr std::string_view validManifest =
            "SOULASH2_NATIVE_MODULES_V1\r\n"
            "id=Snow.Faster_Sleep_Simulation\r\n"
            "module=native/Faster_Sleep_Simulation.dll\r\n";
        soulash::native_modules::Manifest manifest;
        Check(Parses(validManifest, &manifest), "known V1 manifest parses");
        Check(manifest.id == "Snow.Faster_Sleep_Simulation", "module id is preserved");
        Check(manifest.module == "native/Faster_Sleep_Simulation.dll", "relative module path is preserved");
        Check(Parses(
            "SOULASH2_NATIVE_MODULES_V1\n"
            "module=native/example.DLL\n"
            "id=Example.Module\n"), "field order is not significant");

        const std::array<std::string_view, 15> invalidManifests = {
            "WRONG_HEADER\nid=Example\nmodule=native/example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nid=Again\nmodule=native/example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=native/example.dll\nmodule=native/other.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=native/example.dll\nextra=value\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=../Example\nmodule=native/example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example/Bad\nmodule=native/example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=../example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=native/../../example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=C:/example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=/native/example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=native\\example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=native//example.dll\n",
            "SOULASH2_NATIVE_MODULES_V1\nid=Example\nmodule=native/not-a-dll.txt\n",
            "SOULASH2_NATIVE_MODULES_V1\n\nid=Example\nmodule=native/example.dll\n"
        };

        for (const std::string_view invalidManifest : invalidManifests)
        {
            Check(!Parses(invalidManifest), "malformed or unsafe manifest is rejected");
        }

        std::string oversized(4097, 'A');
        Check(!Parses(oversized), "oversized manifest is rejected");
    }

    bool RaiseAndCatch(DWORD code, ULONG_PTR* parameters, ULONG parameterCount)
    {
        __try
        {
            RaiseException(code, 0, parameterCount, parameters);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
        return false;
    }

    bool RaiseSyntheticFirstChanceException()
    {
        return RaiseAndCatch(0xE0424242, nullptr, 0);
    }

    volatile LONG rewriteExceptionAddress = 0;

    LONG CALLBACK MakeFaultAddressUnknown(PEXCEPTION_POINTERS exceptionInfo)
    {
        if (exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr
            && exceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        {
            exceptionInfo->ExceptionRecord->ExceptionAddress = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(0xDEADBEEF));
            InterlockedExchange(&rewriteExceptionAddress, 1);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool TriggerSyntheticAccessViolation()
    {
        __try
        {
            *reinterpret_cast<volatile int*>(static_cast<std::uintptr_t>(1)) = 7;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
        return false;
    }

    bool ReadLog(const wchar_t* logPath, std::string& contents);

    int RunProxyTest(
        const wchar_t* proxyPath,
        const wchar_t* originalPath,
        const wchar_t* mode,
        const wchar_t* diagnosticsLogPath)
    {
        SetEnvironmentVariableW(L"SOULASH2_NATIVE_MODULE_MANIFEST", nullptr);
        SetEnvironmentVariableW(L"SOULASH2_TEST_MODULE_MARKER", nullptr);
        const bool diagnosticsEnabled = std::wstring_view(mode) == L"enabled";
        const bool diagnosticsFailure = std::wstring_view(mode) == L"failure";
        const bool diagnosticsIni = std::wstring_view(mode) == L"ini";
        Check(diagnosticsEnabled || diagnosticsFailure || diagnosticsIni
                || std::wstring_view(mode) == L"disabled",
            "proxy diagnostics test mode is valid");
        Check(SetEnvironmentVariableW(
            DiagnosticsEnvironmentVariable,
            diagnosticsEnabled || diagnosticsFailure ? diagnosticsLogPath : nullptr) != FALSE,
            "proxy diagnostics opt-in environment is configured");
        Check(SetEnvironmentVariableW(
            L"SOULASH2_TEST_MODULE_MARKER", diagnosticsLogPath) != FALSE,
            "proxy module-disabled marker path is configured");
        if (failures != 0)
        {
            return failures;
        }

        HMODULE original = LoadLibraryW(originalPath);
        Check(original != nullptr, "fake original DLL loads");
        if (original == nullptr)
        {
            return failures == 0 ? 1 : failures;
        }

        HMODULE proxy = LoadLibraryExW(proxyPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Check(proxy != nullptr, "proxy loads with fake original beside it");
        if (proxy == nullptr)
        {
            FreeLibrary(original);
            SetEnvironmentVariableW(DiagnosticsEnvironmentVariable, nullptr);
            return failures == 0 ? 1 : failures;
        }

        using Constructor = void* (__cdecl*)(void*, const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, unsigned long);
        using Destructor = void (__cdecl*)(void*);
        using SendAdditionalFile = void (__cdecl*)(void*, const wchar_t*);
        using SetGuardSize = int (__cdecl*)(void*, int);
        using GetCallCounts = void (WINAPI*)(int*, int*, int*);

        constexpr char constructorName[] = "?" "?0MiniDmpSender@@QEAA@PEB_W000K@Z";
        constexpr char destructorName[] = "?" "?1MiniDmpSender@@UEAA@XZ";
        constexpr char sendName[] = "?sendAdditionalFile@MiniDmpSender@@QEAAXPEB_W@Z";
        constexpr char guardName[] = "?setGuardByteBufferSize@MiniDmpSender@@QEAAHH@Z";

        auto constructor = reinterpret_cast<Constructor>(GetProcAddress(proxy, constructorName));
        auto destructor = reinterpret_cast<Destructor>(GetProcAddress(proxy, destructorName));
        auto sendAdditionalFile = reinterpret_cast<SendAdditionalFile>(GetProcAddress(proxy, sendName));
        auto setGuardSize = reinterpret_cast<SetGuardSize>(GetProcAddress(proxy, guardName));
        auto getCallCounts = reinterpret_cast<GetCallCounts>(GetProcAddress(original, "FakeGetCallCounts"));
        Check(constructor != nullptr, "constructor wrapper is exported");
        Check(destructor != nullptr, "destructor wrapper is exported");
        Check(sendAdditionalFile != nullptr, "sendAdditionalFile wrapper is exported");
        Check(setGuardSize != nullptr, "setGuardByteBufferSize wrapper is exported");
        Check(getCallCounts != nullptr, "fake-original call counter is available");

        if (constructor != nullptr && destructor != nullptr && sendAdditionalFile != nullptr
            && setGuardSize != nullptr && getCallCounts != nullptr)
        {
            alignas(std::max_align_t) std::array<std::byte, 128> storage{};
            void* object = constructor(storage.data(), L"app", L"email", L"key", L"url", 32);
            Check(object == storage.data(), "constructor forwards the supplied object address");
            Check(GetLastError() == 0x5101,
                "constructor preserves the fake original's last-error result");
            Check(setGuardSize(object, 73) == 80, "guard-size return value comes from fake original");
            Check(GetLastError() == 0x5104,
                "guard-size thunk preserves the fake original's last-error result");
            sendAdditionalFile(object, L"synthetic.dmp");
            Check(GetLastError() == 0x5103,
                "additional-file thunk preserves the fake original's last-error result");

            int constructors = 0;
            int sends = 0;
            int destructors = 0;
            getCallCounts(&constructors, &sends, &destructors);
            Check(constructors == 1 && sends == 1 && destructors == 0,
                "constructor and additional-file calls reached fake original");
            destructor(object);
            Check(GetLastError() == 0x5102,
                "destructor thunk preserves the fake original's last-error result");
            getCallCounts(&constructors, &sends, &destructors);
            Check(destructors == 1, "destructor reached fake original");
        }

        FreeLibrary(proxy);
        FreeLibrary(original);

        if (diagnosticsEnabled)
        {
            std::string diagnosticsLog;
            Check(ReadLog(diagnosticsLogPath, diagnosticsLog),
                "opt-in diagnostics writes a bounded local startup log");
            Check(diagnosticsLog.find("timestamp_utc=") != std::string::npos,
                "opt-in proxy startup records diagnostics metadata");
        }
        else if (diagnosticsIni)
        {
            std::string diagnosticsLog;
            Check(ReadLog(diagnosticsLogPath, diagnosticsLog),
                "INI opt-in writes the configured local diagnostics log");
            Check(diagnosticsLog.find("timestamp_utc=") != std::string::npos,
                "INI opt-in starts the diagnostics logger");
        }
        else if (!diagnosticsFailure)
        {
            Check(GetFileAttributesW(diagnosticsLogPath) == INVALID_FILE_ATTRIBUTES,
                "diagnostics remain disabled by default");
            Check(GetFileAttributesW(diagnosticsLogPath) == INVALID_FILE_ATTRIBUTES,
                "module loading remains disabled without explicit manifest opt-in");
        }
        else
        {
            Check(GetFileAttributesW(diagnosticsLogPath) == INVALID_FILE_ATTRIBUTES,
                "invalid opt-in log path fails open without blocking forwarding");
        }

        SetEnvironmentVariableW(DiagnosticsEnvironmentVariable, nullptr);
        if (failures == 0)
        {
            std::cout << "PASS: all four proxy exports call fake original; diagnostics "
                << (diagnosticsEnabled ? "environment opt-in"
                    : diagnosticsIni ? "INI opt-in"
                    : diagnosticsFailure ? "failure-open" : "disabled")
                << " path works\n";
        }
        return failures;
    }

    int RunCoexistenceTest(
        const wchar_t* proxyPath,
        const wchar_t* originalPath,
        const wchar_t* extenderPath)
    {
        Check(SetEnvironmentVariableW(DiagnosticsEnvironmentVariable, nullptr) != FALSE,
            "coexistence test disables diagnostics");
        SetEnvironmentVariableW(L"SOULASH2_NATIVE_MODULE_MANIFEST", nullptr);
        HMODULE original = LoadLibraryW(originalPath);
        Check(original != nullptr, "fake original loads for coexistence test");
        if (original == nullptr)
        {
            return failures;
        }

        HMODULE proxy = LoadLibraryExW(proxyPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Check(proxy != nullptr, "project proxy loads beside fake original");
        HMODULE extender = LoadLibraryExW(extenderPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Check(extender != nullptr, "synthetic extender forwarder module loads beside fake original");
        if (proxy == nullptr || extender == nullptr)
        {
            if (extender != nullptr) FreeLibrary(extender);
            if (proxy != nullptr) FreeLibrary(proxy);
            FreeLibrary(original);
            return failures == 0 ? 1 : failures;
        }

        using Constructor = void* (__cdecl*)(void*, const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, unsigned long);
        using Destructor = void (__cdecl*)(void*);
        using SendAdditionalFile = void (__cdecl*)(void*, const wchar_t*);
        using SetGuardSize = int (__cdecl*)(void*, int);
        using GetCallCounts = void (WINAPI*)(int*, int*, int*);

        constexpr char constructorName[] = "?" "?0MiniDmpSender@@QEAA@PEB_W000K@Z";
        constexpr char destructorName[] = "?" "?1MiniDmpSender@@UEAA@XZ";
        constexpr char sendName[] = "?sendAdditionalFile@MiniDmpSender@@QEAAXPEB_W@Z";
        constexpr char guardName[] = "?setGuardByteBufferSize@MiniDmpSender@@QEAAHH@Z";

        auto proxyConstructor = reinterpret_cast<Constructor>(GetProcAddress(proxy, constructorName));
        auto extenderConstructor = reinterpret_cast<Constructor>(GetProcAddress(extender, constructorName));
        auto proxyDestructor = reinterpret_cast<Destructor>(GetProcAddress(proxy, destructorName));
        auto extenderDestructor = reinterpret_cast<Destructor>(GetProcAddress(extender, destructorName));
        auto proxySend = reinterpret_cast<SendAdditionalFile>(GetProcAddress(proxy, sendName));
        auto extenderSend = reinterpret_cast<SendAdditionalFile>(GetProcAddress(extender, sendName));
        auto proxyGuard = reinterpret_cast<SetGuardSize>(GetProcAddress(proxy, guardName));
        auto extenderGuard = reinterpret_cast<SetGuardSize>(GetProcAddress(extender, guardName));
        auto getCallCounts = reinterpret_cast<GetCallCounts>(GetProcAddress(original, "FakeGetCallCounts"));

        Check(proxyConstructor != nullptr && extenderConstructor != nullptr,
            "proxy and extender expose constructor ABI");
        Check(proxyDestructor != nullptr && extenderDestructor != nullptr,
            "proxy and extender expose destructor ABI");
        Check(proxySend != nullptr && extenderSend != nullptr,
            "proxy and extender expose sendAdditionalFile ABI");
        Check(proxyGuard != nullptr && extenderGuard != nullptr,
            "proxy and extender expose setGuardByteBufferSize ABI");
        Check(getCallCounts != nullptr, "fake original call counter resolves");

        if (proxyConstructor != nullptr && extenderConstructor != nullptr
            && proxyDestructor != nullptr && extenderDestructor != nullptr
            && proxySend != nullptr && extenderSend != nullptr
            && proxyGuard != nullptr && extenderGuard != nullptr && getCallCounts != nullptr)
        {
            alignas(std::max_align_t) std::array<std::byte, 128> proxyStorage{};
            alignas(std::max_align_t) std::array<std::byte, 128> extenderStorage{};
            void* proxyObject = proxyConstructor(
                proxyStorage.data(), L"proxy", L"email", L"key", L"url", 32);
            void* extenderObject = extenderConstructor(
                extenderStorage.data(), L"extender", L"email", L"key", L"url", 32);
            Check(proxyObject == proxyStorage.data() && extenderObject == extenderStorage.data(),
                "both ABI paths preserve their caller object address");
            Check(proxyGuard(proxyObject, 10) == 17, "proxy guard call reaches fake original");
            Check(extenderGuard(extenderObject, 20) == 27, "extender guard call reaches fake original");
            proxySend(proxyObject, L"proxy.dmp");
            extenderSend(extenderObject, L"extender.dmp");
            proxyDestructor(proxyObject);
            extenderDestructor(extenderObject);

            int constructors = 0;
            int sends = 0;
            int destructors = 0;
            getCallCounts(&constructors, &sends, &destructors);
            Check(constructors == 2 && sends == 2 && destructors == 2,
                "all four calls through both modules reach the shared fake original");
        }

        FreeLibrary(extender);
        FreeLibrary(proxy);
        FreeLibrary(original);
        if (failures == 0)
        {
            std::cout << "PASS: proxy and synthetic extender forwarders coexist against fake original\n";
        }
        return failures;
    }

    int RunNativeModuleTest(
        const wchar_t* proxyPath,
        const wchar_t* originalPath,
        const wchar_t* manifestPath,
        const wchar_t* markerPath,
        bool expectModule)
    {
        const std::wstring diagnosticsLogPath = std::wstring(markerPath) + L".diagnostics.log";
        SetEnvironmentVariableW(
            DiagnosticsEnvironmentVariable,
            expectModule ? diagnosticsLogPath.c_str() : nullptr);
        Check(SetEnvironmentVariableW(
            L"SOULASH2_NATIVE_MODULE_MANIFEST", manifestPath) != FALSE,
            "native module manifest opt-in is set");
        Check(SetEnvironmentVariableW(L"SOULASH2_TEST_MODULE_MARKER", markerPath) != FALSE,
            "synthetic module marker path is set");

        HMODULE original = LoadLibraryW(originalPath);
        Check(original != nullptr, "fake original loads for module runtime test");
        HMODULE proxy = original == nullptr
            ? nullptr : LoadLibraryExW(proxyPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Check(proxy != nullptr, "proxy loads for module runtime test");
        if (original == nullptr || proxy == nullptr)
        {
            if (proxy != nullptr) FreeLibrary(proxy);
            if (original != nullptr) FreeLibrary(original);
            SetEnvironmentVariableW(L"SOULASH2_NATIVE_MODULE_MANIFEST", nullptr);
            SetEnvironmentVariableW(L"SOULASH2_TEST_MODULE_MARKER", nullptr);
            SetEnvironmentVariableW(DiagnosticsEnvironmentVariable, nullptr);
            return failures == 0 ? 1 : failures;
        }

        using Constructor = void* (__cdecl*)(void*, const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, unsigned long);
        using Destructor = void (__cdecl*)(void*);
        using GetCallCounts = void (WINAPI*)(int*, int*, int*);
        auto constructor = reinterpret_cast<Constructor>(
            GetProcAddress(proxy, "?" "?0MiniDmpSender@@QEAA@PEB_W000K@Z"));
        auto destructor = reinterpret_cast<Destructor>(
            GetProcAddress(proxy, "?" "?1MiniDmpSender@@UEAA@XZ"));
        auto getCallCounts = reinterpret_cast<GetCallCounts>(
            GetProcAddress(original, "FakeGetCallCounts"));
        Check(constructor != nullptr && destructor != nullptr && getCallCounts != nullptr,
            "proxy calls and fake-original counter resolve");

        if (constructor != nullptr && destructor != nullptr && getCallCounts != nullptr)
        {
            alignas(std::max_align_t) std::array<std::byte, 128> storage{};
            void* object = constructor(storage.data(), L"module-test", L"email", L"key", L"url", 32);
            Check(object == storage.data(), "original constructor result is preserved with module startup");
            destructor(object);

            if (expectModule)
            {
                bool pluginInitialized = false;
                std::string marker;
                for (unsigned attempt = 0; attempt < 500; ++attempt)
                {
                    marker.clear();
                    if (ReadLog(markerPath, marker)
                        && marker.find("delayed-log-v2") != std::string::npos)
                    {
                        pluginInitialized = true;
                        break;
                    }
                    Sleep(10);
                }
                Check(pluginInitialized, "opt-in module reaches DllMain and plugin initialization");

                Check(marker.find("dllmain") != std::string::npos,
                    "manifest-listed module DllMain ran on startup worker");
                Check(marker.find("plugin-init-v1") != std::string::npos,
                    "versioned plugin initialization API was called");
                Check(marker.find("delayed-log-v2") != std::string::npos,
                    "plugin retained a stable API pointer for delayed callbacks");
                std::string diagnosticsLog;
                Check(ReadLog(diagnosticsLogPath.c_str(), diagnosticsLog),
                    "plugin diagnostics output is available in the local opted-in log");
                const std::size_t pluginMessagePosition =
                    diagnosticsLog.find(" plugin message=synthetic plugin initialized\r\n");
                Check(pluginMessagePosition != std::string::npos,
                    "plugin logger callback appends to the diagnostics file");
                if (pluginMessagePosition != std::string::npos)
                {
                    const std::size_t pluginRecordStart =
                        diagnosticsLog.rfind("timestamp_utc=", pluginMessagePosition);
                    Check(pluginRecordStart != std::string::npos,
                        "plugin callback record includes timestamp");
                    if (pluginRecordStart != std::string::npos)
                    {
                        const std::string pluginRecord = diagnosticsLog.substr(
                            pluginRecordStart, pluginMessagePosition - pluginRecordStart);
                        Check(pluginRecord.find("pid=") != std::string::npos
                                && pluginRecord.find(" thread=") != std::string::npos,
                            "plugin callback record includes process and thread ids");
                    }
                }
                Check(diagnosticsLog.find(" plugin message=synthetic multiline  message\r\n")
                        != std::string::npos,
                    "plugin messages are bounded and line breaks are sanitized");
                Check(diagnosticsLog.find(" plugin message=synthetic delayed plugin message\r\n")
                        != std::string::npos,
                    "delayed plugin logger callback reaches the local diagnostics log");
                Check(diagnosticsLog.find(
                        " file_issue operation=parse path=C:\\Synthetic\\bad  -config.json "
                        "reason=malformed JSON  expected object\r\n")
                        != std::string::npos,
                    "plugin file-issue callback writes a structured sanitized breadcrumb");
                Check(diagnosticsLog.find("invalid operation must fail") == std::string::npos,
                    "invalid file-issue operation is rejected");
                const std::size_t longReasonPosition =
                    diagnosticsLog.find(" file_issue operation=validate path=");
                Check(longReasonPosition != std::string::npos,
                    "plugin file-issue callback records validation breadcrumbs");
                if (longReasonPosition != std::string::npos)
                {
                    const std::size_t reasonPosition =
                        diagnosticsLog.find(" reason=", longReasonPosition);
                    const std::size_t lineEnd =
                        diagnosticsLog.find("\r\n", reasonPosition);
                    Check(reasonPosition != std::string::npos
                            && lineEnd != std::string::npos
                            && lineEnd - reasonPosition == 8 + 512,
                        "file-issue reason is truncated to its 512-byte bound");
                }
            }
            else
            {
                Sleep(100);
                Check(GetFileAttributesW(markerPath) == INVALID_FILE_ATTRIBUTES,
                    "missing manifest fails open without loading a module");
            }

            int constructors = 0;
            int sends = 0;
            int destructors = 0;
            getCallCounts(&constructors, &sends, &destructors);
            Check(constructors == 1 && destructors == 1,
                "original BugSplat calls succeed regardless of module startup outcome");
        }

        FreeLibrary(proxy);
        FreeLibrary(original);
        SetEnvironmentVariableW(L"SOULASH2_NATIVE_MODULE_MANIFEST", nullptr);
        SetEnvironmentVariableW(L"SOULASH2_TEST_MODULE_MARKER", nullptr);
        SetEnvironmentVariableW(DiagnosticsEnvironmentVariable, nullptr);
        DeleteFileW(diagnosticsLogPath.c_str());
        if (failures == 0)
        {
            std::cout << "PASS: opt-in native module " << (expectModule ? "load" : "failure-open")
                << " preserves fake-original calls\n";
        }
        return failures;
    }

    int RunMissingOriginalChild(const wchar_t* proxyPath)
    {
        SetEnvironmentVariableW(DiagnosticsEnvironmentVariable, nullptr);
        HMODULE proxy = LoadLibraryW(proxyPath);
        Check(proxy != nullptr, "wrapper proxy loads without its original");
        if (proxy == nullptr)
        {
            return failures;
        }

        using Constructor = void* (__cdecl*)(void*, const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, unsigned long);
        auto constructor = reinterpret_cast<Constructor>(
            GetProcAddress(proxy, "?" "?0MiniDmpSender@@QEAA@PEB_W000K@Z"));
        Check(constructor != nullptr, "constructor wrapper resolves without original");
        if (constructor == nullptr)
        {
            return failures;
        }
        alignas(std::max_align_t) std::array<std::byte, 128> storage{};
        constructor(storage.data(), L"app", L"email", L"key", L"url", 32);
        std::cerr << "FAIL: call unexpectedly returned without original DLL\n";
        return 1;
    }

    int RunMissingOriginalTest(const wchar_t* proxyPath)
    {
        wchar_t executablePath[MAX_PATH]{};
        const DWORD executableLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
        Check(executableLength != 0 && executableLength < MAX_PATH,
            "test executable path is available");
        if (executableLength == 0 || executableLength >= MAX_PATH)
        {
            return failures;
        }

        std::wstring commandLine = L"\"" + std::wstring(executablePath)
            + L"\" --missing-child \"" + proxyPath + L"\"";
        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executablePath, mutableCommandLine.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        {
            std::cerr << "FAIL: cannot launch missing-original harness, error " << GetLastError() << '\n';
            return 1;
        }

        const DWORD waitResult = WaitForSingleObject(process.hProcess, 10000);
        DWORD exitCode = ERROR_GEN_FAILURE;
        const bool failFastObserved = waitResult == WAIT_OBJECT_0
            && GetExitCodeProcess(process.hProcess, &exitCode)
            && exitCode == FailFastExceptionStatus;
        if (waitResult != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, INFINITE);
        }
        Check(failFastObserved, "missing original fails fast on first proxy call");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (failures == 0)
        {
            std::cout << "PASS: absent original fails fast on proxy call\n";
        }
        return failures;
    }

    int RunDiagnosticsChild(
        const wchar_t* diagnosticsPath,
        const wchar_t* logPath,
        const wchar_t* syntheticPluginPath,
        bool rewriteAddress)
    {
        HMODULE diagnostics = LoadLibraryW(diagnosticsPath);
        Check(diagnostics != nullptr, "diagnostics DLL loads");
        if (diagnostics == nullptr)
        {
            return failures;
        }

        using StartDiagnostics = BOOL (WINAPI*)(const wchar_t*);
        auto start = reinterpret_cast<StartDiagnostics>(
            GetProcAddress(diagnostics, "SoulashDiagnosticsStart"));
        Check(start != nullptr, "diagnostics start export exists");
        if (start != nullptr)
        {
            Check(start(L"") == FALSE, "empty log path is rejected");
            Check(GetLastError() == ERROR_INVALID_PARAMETER,
                "empty log path reports invalid parameter");
            Check(start(L"C") == FALSE, "one-character log path is rejected");
            Check(GetLastError() == ERROR_INVALID_PARAMETER,
                "one-character log path reports invalid parameter");
            Check(start(L"\\\\server\\share\\crash.log") == FALSE,
                "network log path is rejected");
            Check(GetLastError() == ERROR_INVALID_PARAMETER,
                "rejected network path reports invalid parameter");
            std::wstring directoryPath(logPath);
            const std::size_t separator = directoryPath.find_last_of(L"\\/");
            if (separator != std::wstring::npos)
            {
                directoryPath.resize(separator);
                Check(start(directoryPath.c_str()) == FALSE,
                    "directory cannot be used as a crash log file");
            }
            Check(start(logPath) != FALSE, "local diagnostics logger starts");
                using ReportFileIssue = BOOL (WINAPI*)(
                    const wchar_t*, unsigned int, const char*);
                using RegisterModule = BOOL (WINAPI*)(HMODULE, const char*);
                using LogModuleEvent = BOOL (WINAPI*)(
                    const wchar_t*, const char*, const char*, DWORD);
                auto reportFileIssue = reinterpret_cast<ReportFileIssue>(
                    GetProcAddress(diagnostics, "SoulashDiagnosticsReportFileIssue"));
                auto registerModule = reinterpret_cast<RegisterModule>(
                    GetProcAddress(diagnostics, "SoulashDiagnosticsRegisterModule"));
                auto logModuleEvent = reinterpret_cast<LogModuleEvent>(
                    GetProcAddress(diagnostics, "SoulashDiagnosticsLogModuleEvent"));
                Check(reportFileIssue != nullptr,
                    "diagnostics file-issue reporting export exists");
                Check(registerModule != nullptr,
                    "diagnostics module-registration export exists");
                Check(logModuleEvent != nullptr,
                    "diagnostics module-lifecycle export exists");
                HMODULE syntheticPlugin = nullptr;
                using SyntheticFault = BOOL (WINAPI*)();
                SyntheticFault syntheticFault = nullptr;
                if (!rewriteAddress)
                {
                    syntheticPlugin = LoadLibraryW(syntheticPluginPath);
                    Check(syntheticPlugin != nullptr,
                        "purpose-built synthetic plugin loads for crash-attribution test");
                    if (syntheticPlugin != nullptr)
                    {
                        Check(registerModule != nullptr
                                && !registerModule(syntheticPlugin, "invalid plugin id"),
                            "invalid plugin IDs are rejected");
                        Check(registerModule != nullptr
                                && registerModule(syntheticPlugin, "Test.SyntheticNativeModule"),
                            "synthetic plugin is registered for crash attribution");
                        syntheticFault = reinterpret_cast<SyntheticFault>(
                            GetProcAddress(syntheticPlugin, "SyntheticTriggerAccessViolation"));
                    }
                    Check(syntheticFault != nullptr,
                        "synthetic plugin fault entry point resolves");
                    if (logModuleEvent != nullptr)
                    {
                        Check(logModuleEvent(
                                L"C:\\Synthetic\\SyntheticNativeModule.dll",
                                "Test.SyntheticNativeModule",
                                "loading",
                                ERROR_SUCCESS),
                            "module loading stage is accepted");
                        Check(logModuleEvent(
                                L"C:\\Synthetic\\SyntheticNativeModule.dll",
                                "Test.SyntheticNativeModule",
                                "loaded",
                                ERROR_SUCCESS),
                            "module loaded stage is accepted");
                        Check(logModuleEvent(
                                L"C:\\Synthetic\\SyntheticNativeModule.dll",
                                "Test.SyntheticNativeModule",
                                "initializing",
                                ERROR_SUCCESS),
                            "module initialization stage is accepted");
                    }
                }
                if (!rewriteAddress && reportFileIssue != nullptr)
                {
                    Check(reportFileIssue(
                            L"C:\\Synthetic\\plugin-input.json",
                            SOULASH_EXTENDER_FILE_PARSE,
                            "synthetic plugin parse"),
                        "same-thread synthetic file issue is recorded before fault");
                }
                Check(RaiseSyntheticFirstChanceException(),
                    "diagnostics observer ignores unrelated first-chance exceptions");
                PVOID rewriteHandler = nullptr;
                if (rewriteAddress)
                {
                    rewriteExceptionAddress = 0;
                    rewriteHandler = AddVectoredExceptionHandler(1, MakeFaultAddressUnknown);
                    Check(rewriteHandler != nullptr, "unknown-address test handler registers");
                }

                const BOOL faultContinued = rewriteAddress
                    ? TriggerSyntheticAccessViolation()
                    : syntheticFault != nullptr && syntheticFault();
                Check(faultContinued != FALSE,
                    "diagnostics observer continues a synthetic write access violation");
                if (rewriteHandler != nullptr)
                {
                    Check(InterlockedCompareExchange(&rewriteExceptionAddress, 0, 0) == 1,
                        "test observer rewrites the synthetic fault address");
                    RemoveVectoredExceptionHandler(rewriteHandler);
                }

                Check(GetFileAttributesW(logPath) != INVALID_FILE_ATTRIBUTES,
                    "local crash log file is created");
                if (syntheticPlugin != nullptr)
                {
                    FreeLibrary(syntheticPlugin);
                }
        }

        return failures;
    }

    bool RunDiagnosticsChildProcess(
        const wchar_t* executablePath,
        const wchar_t* diagnosticsPath,
        const wchar_t* logPath,
        const wchar_t* syntheticPluginPath,
        const wchar_t* mode)
    {
        std::wstring commandLine = L"\"" + std::wstring(executablePath)
                + L"\" --diagnostics-child \"" + diagnosticsPath
                + L"\" \"" + logPath + L"\" \"" + syntheticPluginPath
                + L"\" " + mode;
        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                    executablePath,
                    mutableCommandLine.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &startup,
                    &process))
        {
                std::cerr << "FAIL: cannot launch diagnostics test process, error " << GetLastError() << '\n';
                return false;
        }

        const DWORD waitResult = WaitForSingleObject(process.hProcess, 15000);
        DWORD exitCode = ERROR_GEN_FAILURE;
        const bool succeeded = waitResult == WAIT_OBJECT_0
                && GetExitCodeProcess(process.hProcess, &exitCode)
                && exitCode == 0;
        if (waitResult != WAIT_OBJECT_0)
        {
                TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                WaitForSingleObject(process.hProcess, INFINITE);
        }
        else if (!succeeded)
        {
                std::cerr << "FAIL: diagnostics child exited with code " << exitCode << '\n';
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return succeeded;
    }

    bool ReadLog(const wchar_t* logPath, std::string& contents)
    {
        HANDLE log = CreateFileW(logPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE)
        {
                return false;
        }

        char bytes[32768 + 24576]{};
        DWORD bytesRead = 0;
        const BOOL readSucceeded = ReadFile(log, bytes, sizeof(bytes) - 1, &bytesRead, nullptr);
        LARGE_INTEGER logSize{};
        const BOOL sizeSucceeded = GetFileSizeEx(log, &logSize);
        CloseHandle(log);
        if (!readSucceeded || !sizeSucceeded || logSize.QuadPart > 32768 + 24576)
        {
                return false;
        }
        contents.assign(bytes, bytesRead);
        return true;
    }

    int RunDiagnosticsChildCommand(
        const wchar_t* diagnosticsPath,
        const wchar_t* logPath,
        const wchar_t* syntheticPluginPath,
        const wchar_t* mode)
    {
        if (std::wstring_view(mode) != L"mapped" && std::wstring_view(mode) != L"unknown")
        {
            std::cerr << "FAIL: invalid diagnostics child mode\n";
            return 2;
        }
        return RunDiagnosticsChild(
            diagnosticsPath,
            logPath,
            syntheticPluginPath,
            std::wstring_view(mode) == L"unknown");
    }

    int RunDiagnosticsTest(
        const wchar_t* diagnosticsPath,
        const wchar_t* logPath,
        const wchar_t* syntheticPluginPath)
    {
        wchar_t executablePath[MAX_PATH]{};
        const DWORD executableLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
        Check(executableLength != 0 && executableLength < MAX_PATH,
                "test executable path is available");
        if (executableLength == 0 || executableLength >= MAX_PATH)
        {
                return failures;
        }

        std::wstring mappedLogPath = std::wstring(logPath) + L".mapped";
        std::wstring unknownLogPath = std::wstring(logPath) + L".unknown";
        Check(RunDiagnosticsChildProcess(
                executablePath, diagnosticsPath, mappedLogPath.c_str(),
                syntheticPluginPath, L"mapped"),
                "mapped-address diagnostics child passes");
        Check(RunDiagnosticsChildProcess(
                executablePath, diagnosticsPath, unknownLogPath.c_str(),
                syntheticPluginPath, L"unknown"),
                "unknown-address diagnostics child passes");

        std::string mappedLog;
        Check(ReadLog(mappedLogPath.c_str(), mappedLog), "mapped-address log is readable and bounded");
        if (!mappedLog.empty())
        {
                Check(mappedLog.find("timestamp_utc=20") != std::string::npos,
                    "startup log contains a UTC timestamp");
                Check(mappedLog.find("pid=") != std::string::npos,
                    "startup log contains the process id");
                Check(mappedLog.find("exe=") != std::string::npos,
                    "startup log contains the executable path field");
                const std::string hashPrefix = "exe_sha256=";
                const std::size_t hashPosition = mappedLog.find(hashPrefix);
                Check(hashPosition != std::string::npos,
                    "startup log contains executable SHA-256");
                if (hashPosition != std::string::npos)
                {
                    const std::size_t hashStart = hashPosition + hashPrefix.size();
                    const std::size_t hashEnd = mappedLog.find("\r\n", hashStart);
                    Check(hashEnd != std::string::npos && hashEnd - hashStart == 64,
                        "executable SHA-256 is exactly 64 hex characters");
                    if (hashEnd != std::string::npos && hashEnd - hashStart == 64)
                    {
                        for (std::size_t index = hashStart; index < hashEnd; ++index)
                        {
                            const char value = mappedLog[index];
                            Check((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'),
                                "executable SHA-256 contains only lowercase hex");
                        }
                    }
                }
                Check(mappedLog.find("module[0]=") != std::string::npos,
                    "startup log contains a bounded module snapshot");
                Check(mappedLog.find("module[128]=") == std::string::npos,
                    "module snapshot never exceeds 128 entries");
                Check(mappedLog.find("exception code=0xC0000005") != std::string::npos,
                    "exception log contains the access-violation code");
                Check(mappedLog.find("access=write target=0x0000000000000001") != std::string::npos,
                    "exception log contains bounded access type and target address");
                Check(mappedLog.find("fault_module=") != std::string::npos
                        && mappedLog.find("native-tests.exe", mappedLog.find("fault_module="))
                            != std::string::npos,
                    "mapped fault is attributed to the test executable");
                Check(mappedLog.find("fault_rva=0x") != std::string::npos,
                    "mapped fault contains an RVA");
                Check(mappedLog.find("context_rip=0x") != std::string::npos
                        && mappedLog.find("context_rsp=0x") != std::string::npos
                        && mappedLog.find("context_rbp=0x") != std::string::npos,
                    "exception record includes the captured instruction and stack pointers");
                Check(mappedLog.find(" rax=0x") != std::string::npos
                        && mappedLog.find(" rbx=0x") != std::string::npos
                        && mappedLog.find(" rcx=0x") != std::string::npos
                        && mappedLog.find(" rdx=0x") != std::string::npos
                        && mappedLog.find(" r8=0x") != std::string::npos
                        && mappedLog.find(" r9=0x") != std::string::npos,
                    "exception record includes bounded general-purpose register context");
                Check(mappedLog.find("context_module=") != std::string::npos
                        && mappedLog.find("context_rva=0x") != std::string::npos,
                    "captured instruction pointer is attributed to its cached module");
                Check(mappedLog.find("module_loaded path=") != std::string::npos
                        && mappedLog.find("SyntheticNativeModule.dll") != std::string::npos
                        && mappedLog.find("plugin_id=Test.SyntheticNativeModule")
                            != std::string::npos,
                    "loaded plugin path and manifest ID are recorded");
                Check(mappedLog.find("module_event stage=loading "
                            "plugin_id=Test.SyntheticNativeModule path=")
                            != std::string::npos
                        && mappedLog.find("module_event stage=loaded "
                            "plugin_id=Test.SyntheticNativeModule path=")
                            != std::string::npos
                        && mappedLog.find("module_event stage=initializing "
                            "plugin_id=Test.SyntheticNativeModule path=")
                            != std::string::npos
                        && mappedLog.find("module_event stage=loading "
                            "plugin_id=Test.SyntheticNativeModule") != std::string::npos,
                    "plugin ID is present in load and initialization lifecycle events");
                const std::size_t exceptionPosition = mappedLog.find("exception code=");
                const std::size_t faultModulePosition =
                    mappedLog.find("fault_module=", exceptionPosition);
                Check(faultModulePosition != std::string::npos
                        && mappedLog.find(
                            "plugin_id=Test.SyntheticNativeModule", faultModulePosition)
                            != std::string::npos
                        && mappedLog.find("SyntheticNativeModule.dll", faultModulePosition)
                            != std::string::npos,
                    "faulting module is attributed by manifest ID and full path");
                const std::size_t firstFramePosition =
                    mappedLog.find("frame[0]=", exceptionPosition);
                Check(firstFramePosition != std::string::npos
                        && mappedLog.find("SyntheticNativeModule.dll", firstFramePosition)
                            != std::string::npos
                        && mappedLog.find(
                            "plugin_id=Test.SyntheticNativeModule", firstFramePosition)
                            != std::string::npos,
                    "fault stack begins in the identified synthetic plugin");
                Check(mappedLog.find("stack_trace_end", exceptionPosition) != std::string::npos
                        && mappedLog.find("frame[1]=", firstFramePosition) != std::string::npos,
                    "bounded unwind captures callers beyond the faulting instruction");
                const std::size_t stackTraceEnd =
                    mappedLog.find("stack_trace_end", exceptionPosition);
                const std::size_t frameCount = [&mappedLog, exceptionPosition, stackTraceEnd]()
                {
                    std::size_t count = 0;
                    std::size_t position = exceptionPosition;
                    while (position < stackTraceEnd
                        && (position = mappedLog.find(" frame[", position)) != std::string::npos
                        && position < stackTraceEnd)
                    {
                        ++count;
                        ++position;
                    }
                    return count;
                }();
                Check(frameCount <= 12,
                    "crash stack trace never exceeds its fixed 12-frame limit");
                Check(mappedLog.find("exception code=0xC0000005 flags=0x")
                        != std::string::npos
                        && mappedLog.find(" parameters=2 pid=") != std::string::npos,
                    "exception record includes flags, parameter count, and process id");
                Check(mappedLog.find(
                        "preceding_file_issue_same_thread={file_issue operation=parse "
                        "path=C:\\Synthetic\\plugin-input.json reason=synthetic plugin parse}")
                        != std::string::npos,
                    "crash record includes the latest same-thread plugin file breadcrumb");
                Check(mappedLog.find(
                        "module_operation_in_progress={stage=initializing "
                        "path=C:\\Synthetic\\SyntheticNativeModule.dll "
                        "plugin_id=Test.SyntheticNativeModule}")
                        != std::string::npos,
                    "crash record identifies the plugin being initialized on the faulting thread");
                Check(mappedLog.find("thread=") != std::string::npos,
                    "exception log contains the thread id");
                Check(mappedLog.size() <= 32768 + 24576,
                    "startup metadata and exception record fit the fixed log bound");
        }

        std::string unknownLog;
        Check(ReadLog(unknownLogPath.c_str(), unknownLog), "unknown-address log is readable and bounded");
        if (!unknownLog.empty())
        {
                Check(unknownLog.find("fault_module=<unmapped>") != std::string::npos,
                    "unknown fault address is reported as unmapped");
                Check(unknownLog.find("preceding_file_issue_same_thread=") == std::string::npos,
                    "crash record omits a file breadcrumb that was not reported on its thread");
        }

        DeleteFileW(mappedLogPath.c_str());
        DeleteFileW(unknownLogPath.c_str());
        if (failures == 0)
        {
                std::cout << "PASS: diagnostics metadata, mapped/unmapped fault attribution, bounds, and exception continuation\n";
        }
        return failures;
    }

    void PrintUsage()
    {
        std::cerr << "usage: native-tests.exe --unit\n"
            << "       native-tests.exe --proxy <proxy-dll> <fake-original-dll> <disabled|enabled|failure> <log-path>\n"
            << "       native-tests.exe --coexist <proxy-dll> <fake-original-dll> <synthetic-extender-dll>\n"
            << "       native-tests.exe --module <proxy-dll> <fake-original-dll> <manifest> <marker>\n"
            << "       native-tests.exe --module-failure <proxy-dll> <fake-original-dll> <manifest>\n"
            << "       native-tests.exe --missing <proxy-dll>\n"
            << "       native-tests.exe --missing-child <proxy-dll>\n"
            << "       native-tests.exe --diagnostics <diagnostics-dll> <local-log-path> <synthetic-plugin-dll>\n";
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--unit")
    {
        TestManifestParser();
        if (failures == 0)
        {
            std::cout << "PASS: native module manifest tests\n";
        }
        return failures;
    }

    if (argc == 6 && std::wstring_view(argv[1]) == L"--proxy")
    {
        return RunProxyTest(argv[2], argv[3], argv[4], argv[5]);
    }

    if (argc == 5 && std::wstring_view(argv[1]) == L"--coexist")
    {
        return RunCoexistenceTest(argv[2], argv[3], argv[4]);
    }

    if (argc == 6 && std::wstring_view(argv[1]) == L"--module")
    {
        return RunNativeModuleTest(argv[2], argv[3], argv[4], argv[5], true);
    }

    if (argc == 5 && std::wstring_view(argv[1]) == L"--module-failure")
    {
        const std::wstring markerPath = std::wstring(argv[4]) + L".marker";
        return RunNativeModuleTest(argv[2], argv[3], argv[4], markerPath.c_str(), false);
    }

    if (argc == 3 && std::wstring_view(argv[1]) == L"--missing")
    {
        return RunMissingOriginalTest(argv[2]);
    }

    if (argc == 3 && std::wstring_view(argv[1]) == L"--missing-child")
    {
        return RunMissingOriginalChild(argv[2]);
    }

    if (argc == 5 && std::wstring_view(argv[1]) == L"--diagnostics")
    {
        return RunDiagnosticsTest(argv[2], argv[3], argv[4]);
    }

    if (argc == 6 && std::wstring_view(argv[1]) == L"--diagnostics-child")
    {
        return RunDiagnosticsChildCommand(argv[2], argv[3], argv[4], argv[5]);
    }

    PrintUsage();
    return 2;
}
