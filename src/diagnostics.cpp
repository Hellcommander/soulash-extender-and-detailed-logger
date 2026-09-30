#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include "plugin_api.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr std::size_t MetadataCapacity = 32768;
    constexpr unsigned MaximumModules = 128;
    constexpr unsigned MaximumStackFrames = 12;
    constexpr std::size_t ModulePathCapacity = 1024;
    constexpr std::size_t ModuleIdCapacity = 129;
    constexpr std::uint64_t MaximumExecutableHashBytes = 512ull * 1024 * 1024;
    constexpr DWORD ModuleSnapshotIntervalMilliseconds = 1000;

    struct CachedModule
    {
        std::uintptr_t base = 0;
        std::size_t size = 0;
        char path[ModulePathCapacity]{};
        char id[ModuleIdCapacity]{};
    };

    HANDLE g_logFile = INVALID_HANDLE_VALUE;
    volatile LONG g_started = 0;
    volatile LONG g_handlingException = 0;
    volatile LONG g_logWriteLock = 0;
    char g_metadata[MetadataCapacity]{};
    DWORD g_metadataLength = 0;
    CachedModule g_modules[MaximumModules]{};
    volatile LONG g_moduleCount = 0;
    volatile LONG g_moduleWriteLock = 0;
    CachedModule g_liveModules[MaximumModules]{};
    CachedModule g_liveModuleStaging[MaximumModules]{};
    volatile LONG g_liveModuleCount = 0;
    volatile LONG g_liveModuleLock = 0;
    volatile LONG g_liveSnapshotReady = 0;
    volatile LONG g_liveSnapshotTruncated = 0;
    volatile LONG g_liveSnapshotError = ERROR_SUCCESS;
    ULONGLONG g_liveSnapshotTick = 0;
    DWORD g_lastFileIssueThreadId = 0;
    char g_lastFileIssue[1536]{};
    std::size_t g_lastFileIssueLength = 0;
    volatile LONG g_activeModuleThreadId = 0;
    char g_activeModuleOperation[1536]{};
    std::size_t g_activeModuleOperationLength = 0;

    struct Buffer
    {
        char* bytes;
        std::size_t capacity;
        std::size_t length = 0;
    };

    void Append(Buffer& buffer, const char* text)
    {
        while (*text != '\0' && buffer.length < buffer.capacity)
        {
            buffer.bytes[buffer.length++] = *text++;
        }
    }

    void AppendHex(Buffer& buffer, std::uintptr_t value, unsigned digits)
    {
        static constexpr char HexDigits[] = "0123456789ABCDEF";
        Append(buffer, "0x");
        while (digits > 0 && buffer.length < buffer.capacity)
        {
            const unsigned shift = (digits - 1) * 4;
            buffer.bytes[buffer.length++] = HexDigits[(value >> shift) & 0x0f];
            --digits;
        }
    }

    void AppendDecimal(Buffer& buffer, unsigned long value)
    {
        char digits[10];
        unsigned count = 0;
        do
        {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && count < sizeof(digits));

        while (count > 0 && buffer.length < buffer.capacity)
        {
            buffer.bytes[buffer.length++] = digits[--count];
        }
    }

    void AppendDecimal64(Buffer& buffer, ULONGLONG value)
    {
        char digits[20];
        unsigned count = 0;
        do
        {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && count < sizeof(digits));

        while (count > 0 && buffer.length < buffer.capacity)
        {
            buffer.bytes[buffer.length++] = digits[--count];
        }
    }

    void AppendWide(Buffer& buffer, const wchar_t* text, std::size_t maximumCharacters)
    {
        char converted[1024];
        const int convertedLength = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text,
            -1,
            converted,
            static_cast<int>(sizeof(converted)),
            nullptr,
            nullptr);
        if (convertedLength <= 0)
        {
            Append(buffer, "<unavailable>");
            return;
        }

        const std::size_t textLength = static_cast<std::size_t>(convertedLength - 1);
        const std::size_t boundedLength = textLength < maximumCharacters ? textLength : maximumCharacters;
        const std::size_t available = buffer.capacity - buffer.length;
        const std::size_t copiedLength = boundedLength < available ? boundedLength : available;
        for (std::size_t index = 0; index < copiedLength; ++index)
        {
            buffer.bytes[buffer.length++] = converted[index];
        }
        if (copiedLength != textLength && buffer.length < buffer.capacity)
        {
            Append(buffer, "<truncated>");
        }
    }

    bool CacheModuleRecord(CachedModule& cached, const MODULEENTRY32W& module)
    {
        cached = {};
        cached.base = reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
        cached.size = module.modBaseSize;
        const int convertedLength = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            module.szExePath,
            -1,
            cached.path,
            static_cast<int>(sizeof(cached.path)),
            nullptr,
            nullptr);
        if (convertedLength <= 0)
        {
            return false;
        }
        return true;
    }

    void CacheModule(const MODULEENTRY32W& module)
    {
        const LONG moduleCount = InterlockedCompareExchange(&g_moduleCount, 0, 0);
        if (moduleCount >= static_cast<LONG>(MaximumModules))
        {
            return;
        }

        CachedModule& cached = g_modules[moduleCount];
        if (!CacheModuleRecord(cached, module))
        {
            constexpr char Unavailable[] = "<unavailable>";
            for (std::size_t index = 0; index < sizeof(Unavailable); ++index)
            {
                cached.path[index] = Unavailable[index];
            }
        }
        InterlockedExchange(&g_moduleCount, moduleCount + 1);
    }

    bool RefreshLiveModuleSnapshot()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        LONG moduleCount = 0;
        bool truncated = false;
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(snapshot, &module))
        {
            do
            {
                if (moduleCount == static_cast<LONG>(MaximumModules))
                {
                    truncated = true;
                    break;
                }
                if (CacheModuleRecord(g_liveModuleStaging[moduleCount], module))
                {
                    ++moduleCount;
                }
            } while (Module32NextW(snapshot, &module));
        }
        else
        {
            const DWORD error = GetLastError();
            CloseHandle(snapshot);
            SetLastError(error);
            return false;
        }
        CloseHandle(snapshot);

        if (InterlockedCompareExchange(&g_liveModuleLock, 1, 0) != 0)
        {
            SetLastError(ERROR_BUSY);
            return false;
        }
        for (LONG index = 0; index < moduleCount; ++index)
        {
            g_liveModules[index] = g_liveModuleStaging[index];
        }
        InterlockedExchange(&g_liveModuleCount, moduleCount);
        InterlockedExchange(&g_liveSnapshotTruncated, truncated ? 1 : 0);
        g_liveSnapshotTick = GetTickCount64();
        InterlockedExchange(&g_liveSnapshotReady, 1);
        InterlockedExchange(&g_liveSnapshotError, ERROR_SUCCESS);
        InterlockedExchange(&g_liveModuleLock, 0);
        return true;
    }

    DWORD WINAPI MonitorLoadedModules(void*)
    {
        for (;;)
        {
            if (!RefreshLiveModuleSnapshot())
            {
                InterlockedExchange(&g_liveSnapshotError, static_cast<LONG>(GetLastError()));
            }
            Sleep(ModuleSnapshotIntervalMilliseconds);
        }
    }

    const CachedModule* FindModule(
        std::uintptr_t address,
        const CachedModule* liveModules,
        LONG liveModuleCount)
    {
        for (LONG index = 0; index < liveModuleCount; ++index)
        {
            const CachedModule& module = liveModules[index];
            if (address >= module.base && address - module.base < module.size)
            {
                return &module;
            }
        }

        const LONG moduleCount = InterlockedCompareExchange(&g_moduleCount, 0, 0);
        for (LONG index = 0; index < moduleCount; ++index)
        {
            const CachedModule& module = g_modules[index];
            if (address >= module.base && address - module.base < module.size)
            {
                return &module;
            }
        }
        return nullptr;
    }

    void AppendModuleAttribution(
        Buffer& record,
        const char* label,
        const char* rvaLabel,
        std::uintptr_t address,
        const CachedModule* liveModules,
        LONG liveModuleCount)
    {
        Append(record, label);
        const CachedModule* module = FindModule(address, liveModules, liveModuleCount);
        if (module == nullptr)
        {
            Append(record, "<unmapped>");
            return;
        }

        for (std::size_t index = 0;
             index < sizeof(module->path) && module->path[index] != '\0'
                 && record.length < record.capacity;
             ++index)
        {
            record.bytes[record.length++] = module->path[index];
        }
        if (module->id[0] != '\0')
        {
            Append(record, " plugin_id=");
            Append(record, module->id);
        }
        Append(record, rvaLabel);
        AppendHex(record, address - module->base, static_cast<unsigned>(sizeof(void*) * 2));
    }

    void AppendExceptionStack(
        Buffer& record,
        const CONTEXT& exceptionContext,
        const CachedModule* liveModules,
        LONG liveModuleCount)
    {
        CONTEXT context = exceptionContext;
        ULONG_PTR stackLimit = 0;
        ULONG_PTR stackBase = 0;
        GetCurrentThreadStackLimits(&stackLimit, &stackBase);
        Append(record, " stack_trace_begin");
        if (stackLimit == 0 || stackBase <= stackLimit)
        {
            Append(record, " unavailable stack_trace_end");
            return;
        }

        for (unsigned frame = 0; frame < MaximumStackFrames; ++frame)
        {
            const std::uintptr_t instruction = static_cast<std::uintptr_t>(context.Rip);
            const ULONG_PTR stackPointer = static_cast<ULONG_PTR>(context.Rsp);
            if (instruction == 0 || stackPointer < stackLimit || stackPointer >= stackBase)
            {
                break;
            }

            Append(record, " frame[");
            AppendDecimal(record, frame);
            Append(record, "]=");
            AppendHex(record, instruction, static_cast<unsigned>(sizeof(void*) * 2));
            AppendModuleAttribution(
                record, " module=", " rva=", instruction, liveModules, liveModuleCount);

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION functionEntry = RtlLookupFunctionEntry(
                static_cast<DWORD64>(instruction), &imageBase, nullptr);
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            const ULONG_PTR previousStackPointer = stackPointer;
            RtlVirtualUnwind(
                UNW_FLAG_NHANDLER,
                imageBase,
                static_cast<DWORD64>(instruction),
                functionEntry,
                &context,
                &handlerData,
                &establisherFrame,
                nullptr);

            const ULONG_PTR nextStackPointer = static_cast<ULONG_PTR>(context.Rsp);
            if (nextStackPointer <= previousStackPointer
                || nextStackPointer < stackLimit
                || nextStackPointer >= stackBase
                || context.Rip == 0)
            {
                break;
            }
        }
        Append(record, " stack_trace_end");
    }

    void WriteLiveModuleInventory(
        const CachedModule* modules,
        LONG moduleCount,
        ULONGLONG snapshotAgeMilliseconds,
        bool truncated)
    {
        char line[ModulePathCapacity + 256];
        Buffer record{line, sizeof(line)};
        Append(record, "module_inventory_begin age_ms=");
        AppendDecimal64(record, snapshotAgeMilliseconds);
        Append(record, " count=");
        AppendDecimal(record, static_cast<unsigned long>(moduleCount));
        Append(record, truncated ? " truncated=1\r\n" : " truncated=0\r\n");
        DWORD written = 0;
        WriteFile(g_logFile, record.bytes, static_cast<DWORD>(record.length), &written, nullptr);

        for (LONG index = 0; index < moduleCount; ++index)
        {
            const CachedModule& module = modules[index];
            record.length = 0;
            Append(record, "live_module[");
            AppendDecimal(record, static_cast<unsigned long>(index));
            Append(record, "] base=");
            AppendHex(record, module.base, static_cast<unsigned>(sizeof(void*) * 2));
            Append(record, " size=");
            AppendHex(record, module.size, static_cast<unsigned>(sizeof(void*) * 2));
            Append(record, " path=");
            Append(record, module.path);
            if (module.id[0] != '\0')
            {
                Append(record, " plugin_id=");
                Append(record, module.id);
            }
            Append(record, "\r\n");
            written = 0;
            if (!WriteFile(g_logFile, record.bytes, static_cast<DWORD>(record.length),
                    &written, nullptr)
                || written != record.length)
            {
                return;
            }
        }

        constexpr char EndRecord[] = "module_inventory_end\r\n";
        written = 0;
        WriteFile(g_logFile, EndRecord, sizeof(EndRecord) - 1, &written, nullptr);
    }

    void AppendTimestamp(Buffer& buffer)
    {
        FILETIME fileTime{};
        SYSTEMTIME time{};
        GetSystemTimeAsFileTime(&fileTime);
        if (!FileTimeToSystemTime(&fileTime, &time))
        {
            Append(buffer, "timestamp_utc=unavailable\r\n");
            return;
        }

        Append(buffer, "timestamp_utc=");
        AppendDecimal(buffer, time.wYear);
        if (buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = '-';
        if (time.wMonth < 10 && buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = '0';
        AppendDecimal(buffer, time.wMonth);
        if (buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = '-';
        if (time.wDay < 10 && buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = '0';
        AppendDecimal(buffer, time.wDay);
        if (buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = 'T';
        if (time.wHour < 10 && buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = '0';
        AppendDecimal(buffer, time.wHour);
        if (buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = ':';
        if (time.wMinute < 10 && buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = '0';
        AppendDecimal(buffer, time.wMinute);
        if (buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = ':';
        if (time.wSecond < 10 && buffer.length < buffer.capacity) buffer.bytes[buffer.length++] = '0';
        AppendDecimal(buffer, time.wSecond);
        Append(buffer, "Z\r\n");
    }

    void AppendExecutableHash(Buffer& metadata, const wchar_t* executablePath)
    {
        HANDLE file = CreateFileW(
            executablePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            Append(metadata, "exe_sha256=unavailable\r\n");
            return;
        }

        LARGE_INTEGER fileSize{};
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        BYTE hashObject[4096]{};
        DWORD hashObjectLength = 0;
        DWORD resultLength = 0;
        BYTE digest[32]{};
        bool succeeded = GetFileSizeEx(file, &fileSize) != FALSE
            && fileSize.QuadPart >= 0
            && static_cast<std::uint64_t>(fileSize.QuadPart) <= MaximumExecutableHashBytes
            && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
            && BCRYPT_SUCCESS(BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&hashObjectLength),
                sizeof(hashObjectLength),
                &resultLength,
                0))
            && hashObjectLength <= sizeof(hashObject)
            && BCRYPT_SUCCESS(BCryptCreateHash(
                algorithm,
                &hash,
                hashObject,
                hashObjectLength,
                nullptr,
                0,
                0));

        BYTE chunk[16384];
        while (succeeded)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(file, chunk, sizeof(chunk), &bytesRead, nullptr))
            {
                succeeded = false;
                break;
            }
            if (bytesRead == 0)
            {
                break;
            }
            succeeded = BCRYPT_SUCCESS(BCryptHashData(hash, chunk, bytesRead, 0));
        }

        succeeded = succeeded
            && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
        if (hash != nullptr)
        {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        CloseHandle(file);

        if (!succeeded)
        {
            Append(metadata, "exe_sha256=unavailable\r\n");
            return;
        }

        static constexpr char HexDigits[] = "0123456789abcdef";
        Append(metadata, "exe_sha256=");
        for (const BYTE value : digest)
        {
            if (metadata.length + 2 > metadata.capacity)
            {
                break;
            }
            metadata.bytes[metadata.length++] = HexDigits[value >> 4];
            metadata.bytes[metadata.length++] = HexDigits[value & 0x0f];
        }
        Append(metadata, "\r\n");
    }

    void BuildStartupMetadata()
    {
        Buffer metadata{g_metadata, sizeof(g_metadata)};
        AppendTimestamp(metadata);
        Append(metadata, "pid=");
        AppendDecimal(metadata, GetCurrentProcessId());
        Append(metadata, "\r\nexe=");

        wchar_t executablePath[MAX_PATH]{};
        const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
        if (pathLength == 0 || pathLength >= MAX_PATH)
        {
            Append(metadata, "<unavailable>");
        }
        else
        {
            AppendWide(metadata, executablePath, MAX_PATH);
            Append(metadata, "\r\n");
            AppendExecutableHash(metadata, executablePath);
        }
        Append(metadata, "\r\n");

        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            Append(metadata, "module_snapshot=unavailable\r\n");
        }
        else
        {
            g_moduleCount = 0;
            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            if (!Module32FirstW(snapshot, &module))
            {
                Append(metadata, "module_snapshot=unavailable\r\n");
            }
            else
            {
                unsigned moduleCount = 0;
                do
                {
                    if (moduleCount == MaximumModules)
                    {
                        Append(metadata, "module_snapshot=truncated\r\n");
                        break;
                    }
                    Append(metadata, "module[");
                    AppendDecimal(metadata, moduleCount);
                    Append(metadata, "]=");
                    AppendWide(metadata, module.szExePath, MAX_PATH);
                    Append(metadata, "\r\n");
                    CacheModule(module);
                    ++moduleCount;
                } while (Module32NextW(snapshot, &module));

                if (moduleCount < MaximumModules)
                {
                    Append(metadata, "module_snapshot_count=");
                    AppendDecimal(metadata, moduleCount);
                    Append(metadata, "\r\n");
                }
            }
            CloseHandle(snapshot);
        }

        if (metadata.length < metadata.capacity)
        {
            g_metadataLength = static_cast<DWORD>(metadata.length);
        }
        else
        {
            g_metadataLength = static_cast<DWORD>(metadata.capacity);
        }
    }

    LONG CALLBACK LogException(PEXCEPTION_POINTERS exceptionInfo)
    {
        if (exceptionInfo == nullptr || exceptionInfo->ExceptionRecord == nullptr
            || exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (InterlockedCompareExchange(&g_handlingException, 1, 0) != 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            if (InterlockedCompareExchange(&g_logWriteLock, 1, 0) == 0)
            {
                const bool liveSnapshotLocked =
                    InterlockedCompareExchange(&g_liveModuleLock, 1, 0) == 0;
                const CachedModule* liveModules =
                    liveSnapshotLocked ? g_liveModules : nullptr;
                const LONG liveModuleCount = liveSnapshotLocked
                    && InterlockedCompareExchange(&g_liveSnapshotReady, 0, 0) != 0
                    ? InterlockedCompareExchange(&g_liveModuleCount, 0, 0) : 0;
                const ULONGLONG snapshotAgeMilliseconds =
                    liveModuleCount != 0 && liveSnapshotLocked
                    ? GetTickCount64() - g_liveSnapshotTick : 0;
                const bool liveSnapshotTruncated =
                    InterlockedCompareExchange(&g_liveSnapshotTruncated, 0, 0) != 0;
                DWORD written = 0;
                if (g_metadataLength != 0)
                {
                    WriteFile(g_logFile, g_metadata, g_metadataLength, &written, nullptr);
                }

                char line[16384];
                Buffer record{line, sizeof(line)};
                AppendTimestamp(record);
                Append(record, "exception code=");
                AppendHex(record, exceptionInfo->ExceptionRecord->ExceptionCode, 8);
                Append(record, " flags=");
                AppendHex(record, exceptionInfo->ExceptionRecord->ExceptionFlags, 8);
                Append(record, " parameters=");
                AppendDecimal(record, exceptionInfo->ExceptionRecord->NumberParameters);
                Append(record, " pid=");
                AppendDecimal(record, GetCurrentProcessId());
                Append(record, " thread=");
                AppendDecimal(record, GetCurrentThreadId());
                Append(record, " live_module_snapshot=");
                if (!liveSnapshotLocked)
                {
                    Append(record, "busy");
                }
                else if (InterlockedCompareExchange(&g_liveSnapshotReady, 0, 0) == 0)
                {
                    Append(record, "unavailable error=");
                    AppendDecimal(record, static_cast<unsigned long>(
                        InterlockedCompareExchange(&g_liveSnapshotError, 0, 0)));
                }
                else
                {
                    Append(record, "available age_ms=");
                    AppendDecimal64(record, snapshotAgeMilliseconds);
                    Append(record, " count=");
                    AppendDecimal(record, static_cast<unsigned long>(liveModuleCount));
                    if (liveSnapshotTruncated)
                    {
                        Append(record, " truncated=1");
                    }
                }
                Append(record, " address=");
                AppendHex(record, reinterpret_cast<std::uintptr_t>(
                    exceptionInfo->ExceptionRecord->ExceptionAddress),
                    static_cast<unsigned>(sizeof(void*) * 2));
                if (exceptionInfo->ExceptionRecord->NumberParameters >= 2)
                {
                    Append(record, " access=");
                    const ULONG_PTR operation = exceptionInfo->ExceptionRecord->ExceptionInformation[0];
                    Append(record, operation == 0 ? "read" : operation == 1 ? "write"
                        : operation == 8 ? "execute" : "other");
                    Append(record, " target=");
                    AppendHex(record, exceptionInfo->ExceptionRecord->ExceptionInformation[1],
                        static_cast<unsigned>(sizeof(void*) * 2));
                }
                const std::uintptr_t faultAddress = reinterpret_cast<std::uintptr_t>(
                    exceptionInfo->ExceptionRecord->ExceptionAddress);
                AppendModuleAttribution(
                    record, " fault_module=", " fault_rva=", faultAddress,
                    liveModules, liveModuleCount);

                if (exceptionInfo->ContextRecord != nullptr)
                {
                    const CONTEXT& context = *exceptionInfo->ContextRecord;
                    Append(record, " context_rip=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.Rip),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    Append(record, " context_rsp=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.Rsp),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    Append(record, " context_rbp=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.Rbp),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    Append(record, " rax=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.Rax),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    Append(record, " rbx=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.Rbx),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    Append(record, " rcx=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.Rcx),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    Append(record, " rdx=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.Rdx),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    Append(record, " r8=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.R8),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    Append(record, " r9=");
                    AppendHex(record, static_cast<std::uintptr_t>(context.R9),
                        static_cast<unsigned>(sizeof(void*) * 2));
                    AppendModuleAttribution(
                        record, " context_module=", " context_rva=",
                        static_cast<std::uintptr_t>(context.Rip),
                        liveModules, liveModuleCount);
                    AppendExceptionStack(record, context, liveModules, liveModuleCount);
                }
                else
                {
                    Append(record, " context=<unavailable>");
                }
                if (g_lastFileIssueThreadId == GetCurrentThreadId()
                    && g_lastFileIssueLength != 0)
                {
                    Append(record, " preceding_file_issue_same_thread={");
                    const std::size_t available = record.capacity - record.length;
                    const std::size_t copiedLength =
                        g_lastFileIssueLength < available
                        ? g_lastFileIssueLength : available;
                    for (std::size_t index = 0; index < copiedLength; ++index)
                    {
                        record.bytes[record.length++] = g_lastFileIssue[index];
                    }
                    Append(record, "}");
                }
                if (InterlockedCompareExchange(
                        &g_activeModuleThreadId, 0, 0)
                    == static_cast<LONG>(GetCurrentThreadId())
                    && g_activeModuleOperationLength != 0)
                {
                    Append(record, " module_operation_in_progress={");
                    const std::size_t available = record.capacity - record.length;
                    const std::size_t copiedLength =
                        g_activeModuleOperationLength < available
                        ? g_activeModuleOperationLength : available;
                    for (std::size_t index = 0; index < copiedLength; ++index)
                    {
                        record.bytes[record.length++] = g_activeModuleOperation[index];
                    }
                    Append(record, "}");
                }
                Append(record, "\r\n");

                WriteFile(g_logFile, record.bytes, static_cast<DWORD>(record.length), &written, nullptr);
                if (liveSnapshotLocked
                    && InterlockedCompareExchange(&g_liveSnapshotReady, 0, 0) != 0)
                {
                    WriteLiveModuleInventory(
                        liveModules, liveModuleCount,
                        snapshotAgeMilliseconds, liveSnapshotTruncated);
                }
                if (liveSnapshotLocked)
                {
                    InterlockedExchange(&g_liveModuleLock, 0);
                }
                InterlockedExchange(&g_logWriteLock, 0);
            }
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool IsLocalFixedDrivePath(const wchar_t* path)
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
}

#ifndef SOULASH_DIAGNOSTICS_EMBEDDED
#define SOULASH_DIAGNOSTICS_EXPORT __declspec(dllexport)
#else
#define SOULASH_DIAGNOSTICS_EXPORT
#endif

extern "C" SOULASH_DIAGNOSTICS_EXPORT BOOL WINAPI SoulashDiagnosticsWritePluginMessage(
    const char* message)
{
    if (message == nullptr || g_logFile == INVALID_HANDLE_VALUE)
    {
        SetLastError(message == nullptr ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (InterlockedCompareExchange(&g_logWriteLock, 1, 0) != 0)
    {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }

    char line[544];
    Buffer record{line, sizeof(line)};
    AppendTimestamp(record);
    Append(record, "pid=");
    AppendDecimal(record, GetCurrentProcessId());
    Append(record, " thread=");
    AppendDecimal(record, GetCurrentThreadId());
    Append(record, " plugin message=");
    for (std::size_t index = 0; index < 512 && message[index] != '\0'; ++index)
    {
        const unsigned char value = static_cast<unsigned char>(message[index]);
        if (record.length == record.capacity)
        {
            break;
        }
        record.bytes[record.length++] = value < 0x20 || value == 0x7f ? ' ' : static_cast<char>(value);
    }
    Append(record, "\r\n");

    DWORD written = 0;
    const BOOL succeeded = WriteFile(
        g_logFile, record.bytes, static_cast<DWORD>(record.length), &written, nullptr);
    InterlockedExchange(&g_logWriteLock, 0);
    if (!succeeded || written != record.length)
    {
        SetLastError(succeeded ? ERROR_WRITE_FAULT : GetLastError());
        return FALSE;
    }
    return TRUE;
}

extern "C" SOULASH_DIAGNOSTICS_EXPORT BOOL WINAPI SoulashDiagnosticsReportFileIssue(
    const wchar_t* path,
    unsigned int operation,
    const char* reason)
{
    if (path == nullptr || reason == nullptr)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (g_logFile == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (operation < 1 || operation > 4)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (InterlockedCompareExchange(&g_logWriteLock, 1, 0) != 0)
    {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }

    wchar_t boundedPath[512]{};
    std::size_t pathLength = 0;
    while (pathLength < (sizeof(boundedPath) / sizeof(boundedPath[0])) - 1
        && path[pathLength] != L'\0')
    {
        boundedPath[pathLength] = path[pathLength];
        ++pathLength;
    }

    char utf8Path[1536]{};
    const int convertedLength = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        boundedPath,
        static_cast<int>(pathLength),
        utf8Path,
        static_cast<int>(sizeof(utf8Path)),
        nullptr,
        nullptr);
    char line[2304];
    Buffer record{line, sizeof(line)};
    AppendTimestamp(record);
    Append(record, "pid=");
    AppendDecimal(record, GetCurrentProcessId());
    Append(record, " thread=");
    AppendDecimal(record, GetCurrentThreadId());
    Append(record, " ");
    const std::size_t issueStart = record.length;
    Append(record, "file_issue operation=");
    switch (operation)
    {
    case SOULASH_EXTENDER_FILE_READ: Append(record, "read"); break;
    case SOULASH_EXTENDER_FILE_WRITE: Append(record, "write"); break;
    case SOULASH_EXTENDER_FILE_PARSE: Append(record, "parse"); break;
    case SOULASH_EXTENDER_FILE_VALIDATE: Append(record, "validate"); break;
    default: Append(record, "unknown"); break;
    }
    Append(record, " path=");
    if (convertedLength <= 0 && pathLength != 0)
    {
        Append(record, "<invalid-utf16>");
    }
    else
    {
        for (int index = 0; index < convertedLength; ++index)
        {
            const unsigned char value = static_cast<unsigned char>(utf8Path[index]);
            if (value == '\0')
            {
                break;
            }
            if (value < 0x20 || value == 0x7f)
            {
                if (record.length < record.capacity) record.bytes[record.length++] = ' ';
            }
            else if (record.length < record.capacity)
            {
                record.bytes[record.length++] = static_cast<char>(value);
            }
        }
    }

    Append(record, " reason=");
    for (std::size_t index = 0; index < 512 && reason[index] != '\0'; ++index)
    {
        const unsigned char value = static_cast<unsigned char>(reason[index]);
        if (record.length == record.capacity)
        {
            break;
        }
        record.bytes[record.length++] = value < 0x20 || value == 0x7f
            ? ' ' : static_cast<char>(value);
    }
    Append(record, "\r\n");

    DWORD written = 0;
    const BOOL succeeded = WriteFile(
        g_logFile, record.bytes, static_cast<DWORD>(record.length), &written, nullptr);
    if (succeeded && written == record.length)
    {
        std::size_t issueLength = record.length - issueStart;
        if (issueLength >= 2)
        {
            issueLength -= 2;
        }
        if (issueLength > sizeof(g_lastFileIssue))
        {
            issueLength = sizeof(g_lastFileIssue);
        }
        for (std::size_t index = 0; index < issueLength; ++index)
        {
            g_lastFileIssue[index] = record.bytes[issueStart + index];
        }
        g_lastFileIssueLength = issueLength;
        g_lastFileIssueThreadId = GetCurrentThreadId();
    }
    InterlockedExchange(&g_logWriteLock, 0);
    if (!succeeded || written != record.length)
    {
        SetLastError(succeeded ? ERROR_WRITE_FAULT : GetLastError());
        return FALSE;
    }
    return TRUE;
}

extern "C" SOULASH_DIAGNOSTICS_EXPORT BOOL WINAPI SoulashDiagnosticsRegisterModule(
    HMODULE module,
    const char* moduleId)
{
    if (module == nullptr || moduleId == nullptr)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    std::size_t moduleIdLength = 0;
    while (moduleIdLength < ModuleIdCapacity && moduleId[moduleIdLength] != '\0')
    {
        const unsigned char value = static_cast<unsigned char>(moduleId[moduleIdLength]);
        const bool alphaNumeric = (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9');
        if (!alphaNumeric && value != '.' && value != '_' && value != '-')
        {
            SetLastError(ERROR_INVALID_NAME);
            return FALSE;
        }
        ++moduleIdLength;
    }
    if (moduleIdLength == 0 || moduleIdLength >= ModuleIdCapacity
        || !((moduleId[0] >= 'a' && moduleId[0] <= 'z')
            || (moduleId[0] >= 'A' && moduleId[0] <= 'Z')
            || (moduleId[0] >= '0' && moduleId[0] <= '9')))
    {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }

    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0)
    {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const BYTE*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE
        || ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || ntHeaders->OptionalHeader.SizeOfImage == 0)
    {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }

    wchar_t modulePath[ModulePathCapacity]{};
    const DWORD pathLength = GetModuleFileNameW(
        module, modulePath, static_cast<DWORD>(sizeof(modulePath) / sizeof(modulePath[0])));
    if (pathLength == 0 || pathLength >= sizeof(modulePath) / sizeof(modulePath[0]))
    {
        SetLastError(pathLength == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }

    if (InterlockedCompareExchange(&g_moduleWriteLock, 1, 0) != 0)
    {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }

    const std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(module);
    const LONG moduleCount = InterlockedCompareExchange(&g_moduleCount, 0, 0);
    for (LONG index = 0; index < moduleCount; ++index)
    {
        if (g_modules[index].base == moduleBase)
        {
            if (g_modules[index].id[0] == '\0')
            {
                for (std::size_t character = 0; character <= moduleIdLength; ++character)
                {
                    g_modules[index].id[character] = moduleId[character];
                }
            }
            InterlockedExchange(&g_moduleWriteLock, 0);
            return TRUE;
        }
    }
    if (moduleCount >= static_cast<LONG>(MaximumModules))
    {
        InterlockedExchange(&g_moduleWriteLock, 0);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    CachedModule& cached = g_modules[moduleCount];
    cached.base = moduleBase;
    cached.size = ntHeaders->OptionalHeader.SizeOfImage;
    const int convertedLength = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        modulePath,
        -1,
        cached.path,
        static_cast<int>(sizeof(cached.path)),
        nullptr,
        nullptr);
    if (convertedLength <= 0)
    {
        InterlockedExchange(&g_moduleWriteLock, 0);
        SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return FALSE;
    }
    for (std::size_t index = 0; index <= moduleIdLength; ++index)
    {
        cached.id[index] = moduleId[index];
    }
    InterlockedExchange(&g_moduleCount, moduleCount + 1);
    InterlockedExchange(&g_moduleWriteLock, 0);

    if (g_logFile != INVALID_HANDLE_VALUE
        && InterlockedCompareExchange(&g_logWriteLock, 1, 0) == 0)
    {
        char line[ModulePathCapacity + 128];
        Buffer record{line, sizeof(line)};
        AppendTimestamp(record);
        Append(record, "module_loaded path=");
        Append(record, cached.path);
        Append(record, " plugin_id=");
        Append(record, cached.id);
        Append(record, " base=");
        AppendHex(record, cached.base, static_cast<unsigned>(sizeof(void*) * 2));
        Append(record, " size=");
        AppendHex(record, cached.size, static_cast<unsigned>(sizeof(void*) * 2));
        Append(record, "\r\n");
        DWORD written = 0;
        WriteFile(g_logFile, record.bytes, static_cast<DWORD>(record.length), &written, nullptr);
        InterlockedExchange(&g_logWriteLock, 0);
    }
    return TRUE;
}

extern "C" SOULASH_DIAGNOSTICS_EXPORT BOOL WINAPI SoulashDiagnosticsLogModuleEvent(
    const wchar_t* path,
    const char* moduleId,
    const char* stage,
    DWORD error)
{
    if (path == nullptr || moduleId == nullptr || stage == nullptr)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    enum class ModuleStage
    {
        Loading,
        Loaded,
        Initializing,
        Initialized,
        InitializationFailed,
        LoadFailed,
        LoadedWithoutInitializer
    };
    ModuleStage moduleStage;
    if (std::strcmp(stage, "loading") == 0)
    {
        moduleStage = ModuleStage::Loading;
    }
    else if (std::strcmp(stage, "loaded") == 0)
    {
        moduleStage = ModuleStage::Loaded;
    }
    else if (std::strcmp(stage, "initializing") == 0)
    {
        moduleStage = ModuleStage::Initializing;
    }
    else if (std::strcmp(stage, "initialized") == 0)
    {
        moduleStage = ModuleStage::Initialized;
    }
    else if (std::strcmp(stage, "initialization_failed") == 0)
    {
        moduleStage = ModuleStage::InitializationFailed;
    }
    else if (std::strcmp(stage, "load_failed") == 0)
    {
        moduleStage = ModuleStage::LoadFailed;
    }
    else if (std::strcmp(stage, "loaded_without_initializer") == 0)
    {
        moduleStage = ModuleStage::LoadedWithoutInitializer;
    }
    else
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    char boundedPath[1024]{};
    const int convertedLength = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
        boundedPath, static_cast<int>(sizeof(boundedPath)), nullptr, nullptr);
    if (convertedLength <= 0)
    {
        SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return FALSE;
    }
    for (int index = 0; index < convertedLength - 1; ++index)
    {
        const unsigned char value = static_cast<unsigned char>(boundedPath[index]);
        if (value < 0x20 || value == 0x7f)
        {
            boundedPath[index] = ' ';
        }
    }

    std::size_t moduleIdLength = 0;
    while (moduleIdLength < ModuleIdCapacity && moduleId[moduleIdLength] != '\0')
    {
        const unsigned char value = static_cast<unsigned char>(moduleId[moduleIdLength]);
        const bool alphaNumeric = (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9');
        if ((!alphaNumeric && value != '.' && value != '_' && value != '-')
            || (moduleIdLength == 0 && !alphaNumeric))
        {
            SetLastError(ERROR_INVALID_NAME);
            return FALSE;
        }
        ++moduleIdLength;
    }
    if (moduleIdLength == 0 || moduleIdLength >= ModuleIdCapacity)
    {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }

    InterlockedExchange(&g_activeModuleThreadId, 0);
    std::size_t operationLength = 0;
    if (moduleStage == ModuleStage::Loading
        || moduleStage == ModuleStage::Initializing)
    {
        const char* stageName = moduleStage == ModuleStage::Loading
            ? "loading" : "initializing";
        constexpr char Prefix[] = "stage=";
        for (const char value : Prefix)
        {
            if (value == '\0') break;
            g_activeModuleOperation[operationLength++] = value;
        }
        while (*stageName != '\0' && operationLength < sizeof(g_activeModuleOperation))
        {
            g_activeModuleOperation[operationLength++] = *stageName++;
        }
        constexpr char PathLabel[] = " path=";
        for (const char value : PathLabel)
        {
            if (value == '\0') break;
            if (operationLength == sizeof(g_activeModuleOperation)) break;
            g_activeModuleOperation[operationLength++] = value;
        }
        for (int index = 0;
             index < convertedLength - 1
                 && operationLength < sizeof(g_activeModuleOperation);
             ++index)
        {
            g_activeModuleOperation[operationLength++] = boundedPath[index];
        }
        constexpr char IdLabel[] = " plugin_id=";
        for (const char value : IdLabel)
        {
            if (value == '\0') break;
            if (operationLength == sizeof(g_activeModuleOperation)) break;
            g_activeModuleOperation[operationLength++] = value;
        }
        for (std::size_t index = 0;
             index < moduleIdLength
                 && operationLength < sizeof(g_activeModuleOperation);
             ++index)
        {
            g_activeModuleOperation[operationLength++] = moduleId[index];
        }
        g_activeModuleOperationLength = operationLength;
        InterlockedExchange(&g_activeModuleThreadId, static_cast<LONG>(GetCurrentThreadId()));
    }
    else
    {
        g_activeModuleOperationLength = 0;
    }

    if (g_logFile == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (InterlockedCompareExchange(&g_logWriteLock, 1, 0) != 0)
    {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }

    char line[1280];
    Buffer record{line, sizeof(line)};
    AppendTimestamp(record);
    Append(record, "module_event stage=");
    Append(record, stage);
    Append(record, " plugin_id=");
    for (std::size_t index = 0; index < moduleIdLength; ++index)
    {
        if (record.length == record.capacity) break;
        record.bytes[record.length++] = moduleId[index];
    }
    Append(record, " path=");
    Append(record, boundedPath);
    if (error != ERROR_SUCCESS)
    {
        Append(record, " error=");
        AppendDecimal(record, error);
    }
    Append(record, "\r\n");
    DWORD written = 0;
    const BOOL succeeded = WriteFile(
        g_logFile, record.bytes, static_cast<DWORD>(record.length), &written, nullptr);
    InterlockedExchange(&g_logWriteLock, 0);
    if (!succeeded || written != record.length)
    {
        SetLastError(succeeded ? ERROR_WRITE_FAULT : GetLastError());
        return FALSE;
    }
    return TRUE;
}

extern "C" SOULASH_DIAGNOSTICS_EXPORT BOOL WINAPI SoulashDiagnosticsStart(const wchar_t* logPath)
{
    if (InterlockedCompareExchange(&g_started, -1, 0) != 0)
    {
        SetLastError(ERROR_ALREADY_INITIALIZED);
        return FALSE;
    }

    if (!IsLocalFixedDrivePath(logPath))
    {
        InterlockedExchange(&g_started, 0);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    HANDLE file = CreateFileW(
        logPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        InterlockedExchange(&g_started, 0);
        SetLastError(error);
        return FALSE;
    }

    g_logFile = file;
    BuildStartupMetadata();
    DWORD written = 0;
    const BOOL metadataWriteSucceeded = g_metadataLength == 0
        || WriteFile(g_logFile, g_metadata, g_metadataLength, &written, nullptr);
    if (!metadataWriteSucceeded || written != g_metadataLength)
    {
        const DWORD error = metadataWriteSucceeded ? ERROR_WRITE_FAULT : GetLastError();
        CloseHandle(file);
        g_logFile = INVALID_HANDLE_VALUE;
        InterlockedExchange(&g_started, 0);
        SetLastError(error);
        return FALSE;
    }

    PVOID handler = AddVectoredExceptionHandler(1, LogException);
    if (handler == nullptr)
    {
        const DWORD error = GetLastError();
        CloseHandle(file);
        g_logFile = INVALID_HANDLE_VALUE;
        InterlockedExchange(&g_started, 0);
        SetLastError(error);
        return FALSE;
    }

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&SoulashDiagnosticsStart),
            &self))
    {
        const DWORD error = GetLastError();
        RemoveVectoredExceptionHandler(handler);
        CloseHandle(file);
        g_logFile = INVALID_HANDLE_VALUE;
        InterlockedExchange(&g_started, 0);
        SetLastError(error);
        return FALSE;
    }

    InterlockedExchange(&g_started, 1);
    return TRUE;
}
