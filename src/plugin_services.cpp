#include "plugin_services.hpp"
#include "sha256.hpp"
#include "x64_length.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

extern "C" BOOL WINAPI SoulashDiagnosticsWritePluginMessage(const char* message);
extern "C" BOOL WINAPI SoulashDiagnosticsReportFileIssue(
    const wchar_t* path, unsigned int operation, const char* reason);

namespace
{
    constexpr unsigned MaximumPlugins = 64;
    constexpr unsigned MaximumListeners = 128;
    constexpr std::size_t PluginIdCapacity = 129;
    constexpr std::size_t MaximumPatchBytes = 4096;
    constexpr std::size_t MinimumDetourBytes = 14;
    constexpr std::size_t MaximumDetourBytes = 64;
    constexpr std::size_t MaximumAnchorTextLength = 256;
    constexpr unsigned MaximumStringHits = 16;
    constexpr unsigned MaximumFunctionHits = 256;

    struct PluginRecord
    {
        char id[PluginIdCapacity];
        wchar_t directory[MAX_PATH];
        SoulashExtenderInterface api;
    };

    struct Listener
    {
        unsigned int ownerHandle;
        bool anySender;
        char sender[PluginIdCapacity];
        SoulashExtenderMessageListenerFn callback;
        void* context;
    };

    INIT_ONCE g_gameInfoOnce = INIT_ONCE_STATIC_INIT;
    wchar_t g_gameDirectory[MAX_PATH]{};
    char g_gameHash[65]{};
    void* g_gameBase = nullptr;

    SRWLOCK g_lock = SRWLOCK_INIT;
    PluginRecord g_plugins[MaximumPlugins]{};
    unsigned g_pluginCount = 0;
    Listener g_listeners[MaximumListeners]{};
    unsigned g_listenerCount = 0;

    void Log(const char* format, ...);

    BOOL CALLBACK GatherGameInfo(PINIT_ONCE, PVOID, PVOID*)
    {
        g_gameBase = GetModuleHandleW(nullptr);
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length != 0 && length < MAX_PATH)
        {
            HANDLE file = CreateFileW(
                path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                if (!soulash::plugin_services::Sha256OfHandle(file, g_gameHash))
                {
                    g_gameHash[0] = '\0';
                }
                CloseHandle(file);
            }
            wchar_t* separator = std::wcsrchr(path, L'\\');
            if (separator != nullptr)
            {
                separator[1] = L'\0';
                std::wmemcpy(g_gameDirectory, path, std::wcslen(path) + 1);
            }
        }
        return TRUE;
    }

    void Log(const char* format, ...)
    {
        char message[480];
        va_list arguments;
        va_start(arguments, format);
        _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
        va_end(arguments);
        SoulashDiagnosticsWritePluginMessage(message);
        OutputDebugStringA("Soulash extender: ");
        OutputDebugStringA(message);
        OutputDebugStringA("\r\n");
    }

    const char* IdForHandle(unsigned int handle)
    {
        // Handles are 1-based indexes into the immutable-after-creation table.
        if (handle == 0 || handle > MaximumPlugins)
        {
            return nullptr;
        }
        AcquireSRWLockShared(&g_lock);
        const char* id = handle <= g_pluginCount ? g_plugins[handle - 1].id : nullptr;
        ReleaseSRWLockShared(&g_lock);
        return id;
    }

    void SendMessageToListeners(
        const char* sender, unsigned int type, const void* data,
        unsigned int dataSize, const char* receiver)
    {
        Listener matches[MaximumListeners];
        unsigned matchCount = 0;
        AcquireSRWLockShared(&g_lock);
        for (unsigned i = 0; i < g_listenerCount; ++i)
        {
            const Listener& listener = g_listeners[i];
            if (!listener.anySender && std::strcmp(listener.sender, sender) != 0)
            {
                continue;
            }
            if (receiver != nullptr
                && std::strcmp(g_plugins[listener.ownerHandle - 1].id, receiver) != 0)
            {
                continue;
            }
            matches[matchCount++] = listener;
        }
        ReleaseSRWLockShared(&g_lock);

        const SoulashExtenderMessage message{sender, type, dataSize, data};
        for (unsigned i = 0; i < matchCount; ++i)
        {
            matches[i].callback(&message, matches[i].context);
        }
    }

    BOOL WINAPI RegisterListener(
        unsigned int handle, const char* sender,
        SoulashExtenderMessageListenerFn callback, void* context)
    {
        if (IdForHandle(handle) == nullptr || callback == nullptr
            || (sender != nullptr && std::strlen(sender) >= PluginIdCapacity))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        AcquireSRWLockExclusive(&g_lock);
        BOOL registered = FALSE;
        if (g_listenerCount < MaximumListeners)
        {
            Listener& listener = g_listeners[g_listenerCount];
            listener.ownerHandle = handle;
            listener.anySender = sender == nullptr;
            listener.sender[0] = '\0';
            if (sender != nullptr)
            {
                strcpy_s(listener.sender, sizeof(listener.sender), sender);
            }
            listener.callback = callback;
            listener.context = context;
            ++g_listenerCount;
            registered = TRUE;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (!registered)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
        }
        return registered;
    }

    BOOL WINAPI DispatchMessageFromPlugin(
        unsigned int handle, unsigned int type, const void* data,
        unsigned int dataSize, const char* receiver)
    {
        const char* sender = IdForHandle(handle);
        if (sender == nullptr || type < SOULASH_EXTENDER_MSG_USER_BASE
            || (data == nullptr && dataSize != 0)
            || (receiver != nullptr && std::strlen(receiver) >= PluginIdCapacity))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        SendMessageToListeners(sender, type, data, dataSize, receiver);
        return TRUE;
    }

    bool IsCommittedRange(const void* address, std::size_t size, bool requireExecutable)
    {
        const auto* cursor = static_cast<const BYTE*>(address);
        std::size_t remaining = size;
        while (remaining != 0)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(cursor, &info, sizeof(info)) == 0
                || info.State != MEM_COMMIT
                || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            {
                return false;
            }
            if (requireExecutable
                && (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                    | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
            {
                return false;
            }
            const auto regionEnd = static_cast<const BYTE*>(info.BaseAddress) + info.RegionSize;
            const std::size_t available = static_cast<std::size_t>(regionEnd - cursor);
            const std::size_t step = available < remaining ? available : remaining;
            cursor += step;
            remaining -= step;
        }
        return true;
    }

    bool WriteProtected(void* address, const void* bytes, std::size_t size)
    {
        DWORD previous = 0;
        if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous))
        {
            return false;
        }
        std::memcpy(address, bytes, size);
        DWORD ignored = 0;
        VirtualProtect(address, size, previous, &ignored);
        FlushInstructionCache(GetCurrentProcess(), address, size);
        return true;
    }

    void WriteAbsoluteJump(BYTE* destination, const void* target)
    {
        // jmp qword ptr [rip+0] ; dq target
        static constexpr BYTE Prefix[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        std::memcpy(destination, Prefix, sizeof(Prefix));
        std::memcpy(destination + sizeof(Prefix), &target, sizeof(target));
    }

    BOOL WINAPI PatchMemory(unsigned int handle, void* address, const void* bytes, std::size_t size)
    {
        const char* id = IdForHandle(handle);
        if (id == nullptr || address == nullptr || bytes == nullptr
            || size == 0 || size > MaximumPatchBytes
            || !IsCommittedRange(address, size, false))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (!WriteProtected(address, bytes, size))
        {
            return FALSE;
        }
        Log("plugin_id=%s patch address=%p size=%zu", id, address, size);
        return TRUE;
    }

    BOOL WINAPI InstallDetour(
        unsigned int handle, void* target, void* detour,
        std::size_t stolenBytes, void** trampolineOut)
    {
        const char* id = IdForHandle(handle);
        if (id == nullptr || target == nullptr || detour == nullptr || trampolineOut == nullptr
            || stolenBytes < MinimumDetourBytes || stolenBytes > MaximumDetourBytes
            || !IsCommittedRange(target, stolenBytes, true))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        auto* trampoline = static_cast<BYTE*>(VirtualAlloc(
            nullptr, stolenBytes + MinimumDetourBytes,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (trampoline == nullptr)
        {
            return FALSE;
        }
        std::memcpy(trampoline, target, stolenBytes);
        WriteAbsoluteJump(trampoline + stolenBytes, static_cast<BYTE*>(target) + stolenBytes);
        FlushInstructionCache(GetCurrentProcess(), trampoline, stolenBytes + MinimumDetourBytes);

        BYTE patch[MaximumDetourBytes];
        std::memset(patch, 0x90, stolenBytes);
        WriteAbsoluteJump(patch, detour);
        if (!WriteProtected(target, patch, stolenBytes))
        {
            const DWORD error = GetLastError();
            VirtualFree(trampoline, 0, MEM_RELEASE);
            SetLastError(error);
            return FALSE;
        }

        *trampolineOut = trampoline;
        Log("plugin_id=%s detour target=%p detour=%p stolen=%zu trampoline=%p",
            id, target, detour, stolenBytes, static_cast<void*>(trampoline));
        return TRUE;
    }

    // ---- Automatic detours ----------------------------------------------
    // Decodes whole instructions from the target until at least 14 bytes are
    // covered, rebuilds them in a trampoline (relative branches become
    // absolute, RIP-relative operands are re-based), and only then patches.

    constexpr std::size_t DetourPatchSize = 14;
    constexpr std::size_t TrampolineCapacity = 512;

    BYTE* AllocateNear(const void* target, std::size_t size)
    {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const std::uintptr_t granularity = info.dwAllocationGranularity;
        const std::uintptr_t center = reinterpret_cast<std::uintptr_t>(target);
        const std::uintptr_t reach = 0x7FFF0000u;
        const std::uintptr_t minimum = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
        const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
        const std::uintptr_t low = center > reach + minimum ? center - reach : minimum;
        const std::uintptr_t high = center + reach < maximum ? center + reach : maximum;

        // Upwards from the target, then downwards.
        for (std::uintptr_t cursor = center; cursor < high;)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) == 0)
            {
                break;
            }
            const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uintptr_t regionEnd = regionStart + mbi.RegionSize;
            if (mbi.State == MEM_FREE)
            {
                std::uintptr_t candidate = (regionStart + granularity - 1) / granularity * granularity;
                if (candidate + size <= regionEnd && candidate + size < high)
                {
                    void* block = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                    if (block != nullptr)
                    {
                        return static_cast<BYTE*>(block);
                    }
                }
            }
            cursor = regionEnd;
        }
        for (std::uintptr_t cursor = center; cursor > low;)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor - 1), &mbi, sizeof(mbi)) == 0)
            {
                break;
            }
            const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uintptr_t regionEnd = regionStart + mbi.RegionSize;
            if (mbi.State == MEM_FREE && regionEnd >= size)
            {
                std::uintptr_t candidate = (regionEnd - size) / granularity * granularity;
                if (candidate >= regionStart && candidate > low)
                {
                    void* block = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                    if (block != nullptr)
                    {
                        return static_cast<BYTE*>(block);
                    }
                }
            }
            if (regionStart == 0)
            {
                break;
            }
            cursor = regionStart;
        }
        return nullptr;
    }

    void WriteAbsoluteCall(BYTE* destination, const void* target)
    {
        // call qword ptr [rip+2] ; jmp +8 ; dq target
        static constexpr BYTE Code[8] = {0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08};
        std::memcpy(destination, Code, sizeof(Code));
        std::memcpy(destination + sizeof(Code), &target, sizeof(target));
    }

    // Fills `trampoline` and returns the number of stolen bytes (>= 14), or 0
    // with `reason` set. Reads of the target are SEH-guarded.
    std::size_t BuildTrampoline(
        const BYTE* target, BYTE* trampoline, const char*& reason)
    {
        std::size_t consumed = 0;
        std::size_t written = 0;
        const BYTE* branchTargets[DetourPatchSize + 16];
        unsigned branchTargetCount = 0;
        reason = nullptr;

        __try
        {
            while (consumed < DetourPatchSize)
            {
                const BYTE* source = target + consumed;
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(source, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT
                    || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
                {
                    reason = "target is not readable";
                    return 0;
                }
                const std::size_t readable = static_cast<std::size_t>(
                    static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize - source);

                soulash::x64::Decoded decoded;
                if (!soulash::x64::Decode(source, readable < 15 ? readable : 15, decoded))
                {
                    reason = "undecodable or unsupported instruction in the stolen region";
                    return 0;
                }
                const std::size_t end = consumed + decoded.length;
                const bool endsFlow = decoded.flow == soulash::x64::Flow::Return
                    || decoded.flow == soulash::x64::Flow::Trap
                    || (decoded.flow == soulash::x64::Flow::Jump && !decoded.relative);
                if (endsFlow && end < DetourPatchSize)
                {
                    reason = "function ends before 14 bytes can be replaced";
                    return 0;
                }

                BYTE* destination = trampoline + written;
                if (decoded.relative)
                {
                    if (decoded.flow == soulash::x64::Flow::Other)
                    {
                        reason = "loop/jrcxz/xbegin cannot be relocated";
                        return 0;
                    }
                    std::int64_t displacement = 0;
                    if (decoded.relativeSize == 1)
                    {
                        displacement = static_cast<std::int8_t>(source[decoded.relativeOffset]);
                    }
                    else
                    {
                        std::int32_t value;
                        std::memcpy(&value, source + decoded.relativeOffset, sizeof(value));
                        displacement = value;
                    }
                    const BYTE* destinationAddress = source + decoded.length + displacement;
                    if (branchTargetCount < sizeof(branchTargets) / sizeof(branchTargets[0]))
                    {
                        branchTargets[branchTargetCount++] = destinationAddress;
                    }

                    if (decoded.flow == soulash::x64::Flow::Call)
                    {
                        WriteAbsoluteCall(destination, destinationAddress);
                        written += 16;
                    }
                    else if (decoded.flow == soulash::x64::Flow::Jump)
                    {
                        WriteAbsoluteJump(destination, destinationAddress);
                        written += 14;
                    }
                    else
                    {
                        // Inverted short jcc hops over an absolute jump.
                        destination[0] = static_cast<BYTE>(0x70 | ((decoded.opcode & 0x0F) ^ 1));
                        destination[1] = 14;
                        WriteAbsoluteJump(destination + 2, destinationAddress);
                        written += 16;
                    }
                }
                else
                {
                    std::memcpy(destination, source, decoded.length);
                    if (decoded.ripRelative)
                    {
                        std::int32_t displacement;
                        std::memcpy(&displacement, source + decoded.ripDisplacementOffset,
                            sizeof(displacement));
                        const std::int64_t absolute =
                            reinterpret_cast<std::int64_t>(source + decoded.length) + displacement;
                        const std::int64_t rebased =
                            absolute - reinterpret_cast<std::int64_t>(destination + decoded.length);
                        if (rebased < INT32_MIN || rebased > INT32_MAX)
                        {
                            reason = "RIP-relative operand out of reach of the trampoline";
                            return 0;
                        }
                        const std::int32_t value = static_cast<std::int32_t>(rebased);
                        std::memcpy(destination + decoded.ripDisplacementOffset, &value, sizeof(value));
                    }
                    written += decoded.length;
                }
                consumed = end;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            reason = "fault while reading the target";
            return 0;
        }

        for (unsigned i = 0; i < branchTargetCount; ++i)
        {
            if (branchTargets[i] >= target && branchTargets[i] < target + consumed)
            {
                reason = "a stolen instruction branches into the stolen region";
                return 0;
            }
        }
        WriteAbsoluteJump(trampoline + written, target + consumed);
        return consumed;
    }

    BOOL WINAPI InstallDetourAuto(
        unsigned int handle, void* target, void* detour, void** trampolineOut)
    {
        const char* id = IdForHandle(handle);
        if (id == nullptr || target == nullptr || detour == nullptr || trampolineOut == nullptr
            || !IsCommittedRange(target, 1, true))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        BYTE* trampoline = AllocateNear(target, TrampolineCapacity);
        if (trampoline == nullptr)
        {
            Log("plugin_id=%s detour refused: no memory near target %p", id, target);
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }

        const char* reason = nullptr;
        const std::size_t stolen = BuildTrampoline(static_cast<const BYTE*>(target), trampoline, reason);
        if (stolen == 0)
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            Log("plugin_id=%s detour refused at %p: %s", id, target,
                reason != nullptr ? reason : "unknown");
            SetLastError(ERROR_NOT_SUPPORTED);
            return FALSE;
        }
        FlushInstructionCache(GetCurrentProcess(), trampoline, TrampolineCapacity);

        BYTE patch[DetourPatchSize + 15];
        std::memset(patch, 0x90, sizeof(patch));
        WriteAbsoluteJump(patch, detour);
        if (!WriteProtected(target, patch, stolen))
        {
            const DWORD error = GetLastError();
            VirtualFree(trampoline, 0, MEM_RELEASE);
            SetLastError(error);
            return FALSE;
        }

        *trampolineOut = trampoline;
        Log("plugin_id=%s auto detour target=%p detour=%p stolen=%zu trampoline=%p",
            id, target, detour, stolen, static_cast<void*>(trampoline));
        return TRUE;
    }

    // ---- Build-independent anchors -------------------------------------
    // These scan the mapped game image, so they keep working across game
    // updates as long as the string / class name still exists. Scans use only
    // plain data and are guarded by SEH in case a page is unreadable.

    struct SectionView
    {
        const BYTE* start;
        std::size_t size;
        bool executable;
    };

    std::size_t CollectSections(SectionView (&sections)[24])
    {
        const auto* base = static_cast<const BYTE*>(g_gameBase);
        if (base == nullptr)
        {
            return 0;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return 0;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            return 0;
        }
        const IMAGE_SECTION_HEADER* header = IMAGE_FIRST_SECTION(nt);
        std::size_t count = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections && count < 24; ++i)
        {
            const DWORD characteristics = header[i].Characteristics;
            if ((characteristics & IMAGE_SCN_MEM_READ) == 0
                || (characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0)
            {
                continue;
            }
            sections[count].start = base + header[i].VirtualAddress;
            sections[count].size = header[i].Misc.VirtualSize;
            sections[count].executable = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            ++count;
        }
        return count;
    }

    // Finds `text` (plus NUL) in non-executable sections where it begins a
    // string (preceded by NUL or the section start).
    unsigned FindExactStrings(
        const SectionView* sections, std::size_t sectionCount,
        const char* text, std::size_t length, const BYTE* (&hits)[MaximumStringHits])
    {
        unsigned count = 0;
        for (std::size_t s = 0; s < sectionCount && count < MaximumStringHits; ++s)
        {
            if (sections[s].executable || sections[s].size < length + 1)
            {
                continue;
            }
            const BYTE* const begin = sections[s].start;
            const BYTE* const last = begin + sections[s].size - (length + 1);
            for (const BYTE* p = begin; p <= last && count < MaximumStringHits; ++p)
            {
                p = static_cast<const BYTE*>(std::memchr(p, text[0], static_cast<std::size_t>(last - p) + 1));
                if (p == nullptr)
                {
                    break;
                }
                if ((p == begin || p[-1] == 0)
                    && std::memcmp(p, text, length + 1) == 0)
                {
                    hits[count++] = p;
                }
            }
        }
        return count;
    }

    // Returns false on a fault while reading the image.
    bool ScanFunctionsForStrings(
        const SectionView* sections, std::size_t sectionCount,
        const BYTE* const* strings, unsigned stringCount,
        std::uintptr_t* functions, unsigned& functionCount)
    {
        __try
        {
            for (std::size_t s = 0; s < sectionCount; ++s)
            {
                if (!sections[s].executable || sections[s].size < 8)
                {
                    continue;
                }
                const BYTE* const code = sections[s].start;
                const std::size_t limit = sections[s].size - 7;
                for (std::size_t i = 0; i < limit; ++i)
                {
                    // lea r64, [rip+disp32]: REX.W(+R) 8D /r with mod=00 rm=101
                    if ((code[i] != 0x48 && code[i] != 0x4C) || code[i + 1] != 0x8D
                        || (code[i + 2] & 0xC7) != 0x05)
                    {
                        continue;
                    }
                    std::int32_t displacement;
                    std::memcpy(&displacement, code + i + 3, sizeof(displacement));
                    const BYTE* const target = code + i + 7 + displacement;
                    bool matches = false;
                    for (unsigned k = 0; k < stringCount && !matches; ++k)
                    {
                        matches = strings[k] == target;
                    }
                    if (!matches)
                    {
                        continue;
                    }

                    DWORD64 imageBase = 0;
                    const RUNTIME_FUNCTION* entry = RtlLookupFunctionEntry(
                        reinterpret_cast<DWORD64>(code + i), &imageBase, nullptr);
                    if (entry == nullptr)
                    {
                        continue; // leaf function without unwind data
                    }
                    const std::uintptr_t start =
                        static_cast<std::uintptr_t>(imageBase) + entry->BeginAddress;
                    bool known = false;
                    for (unsigned k = 0; k < functionCount && !known; ++k)
                    {
                        known = functions[k] == start;
                    }
                    if (!known && functionCount < MaximumFunctionHits)
                    {
                        functions[functionCount++] = start;
                    }
                }
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    BOOL WINAPI FindFunctionsReferencingString(
        unsigned int handle, const char* text, void** results,
        unsigned int capacity, unsigned int* count)
    {
        const std::size_t length = text == nullptr ? 0 : strnlen_s(text, MaximumAnchorTextLength + 1);
        if (IdForHandle(handle) == nullptr || length == 0 || length > MaximumAnchorTextLength
            || count == nullptr || (results == nullptr && capacity != 0))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        SectionView sections[24];
        const std::size_t sectionCount = CollectSections(sections);
        const BYTE* strings[MaximumStringHits];
        unsigned stringCount = 0;
        static std::uintptr_t functions[MaximumFunctionHits];
        unsigned functionCount = 0;

        // The shared scratch array is guarded by the plugin table lock.
        AcquireSRWLockExclusive(&g_lock);
        BOOL succeeded = sectionCount != 0;
        if (succeeded)
        {
            stringCount = FindExactStrings(sections, sectionCount, text, length, strings);
            succeeded = ScanFunctionsForStrings(
                sections, sectionCount, strings, stringCount, functions, functionCount);
        }
        if (succeeded)
        {
            *count = functionCount;
            for (unsigned i = 0; i < functionCount && i < capacity; ++i)
            {
                results[i] = reinterpret_cast<void*>(functions[i]);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (!succeeded)
        {
            SetLastError(ERROR_INVALID_DATA);
        }
        return succeeded;
    }

    // Returns false on a fault while reading the image.
    bool ScanRttiVtable(
        const SectionView* sections, std::size_t sectionCount,
        const BYTE* typeDescriptor, void*& vtable)
    {
        const auto* base = static_cast<const BYTE*>(g_gameBase);
        const DWORD descriptorRva = static_cast<DWORD>(typeDescriptor - base);
        void* fallback = nullptr;
        __try
        {
            for (std::size_t s = 0; s < sectionCount; ++s)
            {
                if (sections[s].executable || sections[s].size < 24)
                {
                    continue;
                }
                const BYTE* const data = sections[s].start;
                for (std::size_t i = 12; i + 12 <= sections[s].size; i += 4)
                {
                    DWORD field;
                    std::memcpy(&field, data + i, sizeof(field));
                    if (field != descriptorRva)
                    {
                        continue;
                    }
                    // x64 RTTICompleteObjectLocator: signature, offset,
                    // cdOffset, pTypeDescriptor, pClassDescriptor, pSelf.
                    const BYTE* const locator = data + i - 12;
                    if (i < 12 || i + 12 > sections[s].size)
                    {
                        continue;
                    }
                    DWORD signature, offset, self;
                    std::memcpy(&signature, locator, sizeof(signature));
                    std::memcpy(&offset, locator + 4, sizeof(offset));
                    std::memcpy(&self, locator + 20, sizeof(self));
                    if (signature != 1 || self != static_cast<DWORD>(locator - base))
                    {
                        continue;
                    }

                    // The vtable stores the locator's address one slot before
                    // the first virtual function pointer.
                    const std::uintptr_t locatorAddress = reinterpret_cast<std::uintptr_t>(locator);
                    for (std::size_t t = 0; t < sectionCount; ++t)
                    {
                        if (sections[t].executable || sections[t].size < 16)
                        {
                            continue;
                        }
                        const BYTE* const table = sections[t].start;
                        for (std::size_t j = 0; j + 16 <= sections[t].size; j += 8)
                        {
                            std::uintptr_t value;
                            std::memcpy(&value, table + j, sizeof(value));
                            if (value == locatorAddress)
                            {
                                void* candidate = const_cast<BYTE*>(table + j + 8);
                                if (offset == 0)
                                {
                                    vtable = candidate;
                                    return true;
                                }
                                if (fallback == nullptr)
                                {
                                    fallback = candidate;
                                }
                            }
                        }
                    }
                }
            }
            vtable = fallback;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    BOOL WINAPI FindRttiVtable(unsigned int handle, const char* className, void** vtable)
    {
        const std::size_t length =
            className == nullptr ? 0 : strnlen_s(className, MaximumAnchorTextLength + 1);
        if (IdForHandle(handle) == nullptr || length == 0 || length > 200 || vtable == nullptr)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        *vtable = nullptr;

        SectionView sections[24];
        const std::size_t sectionCount = CollectSections(sections);
        if (sectionCount == 0)
        {
            SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }

        char candidates[2][MaximumAnchorTextLength + 8]{};
        unsigned candidateCount = 0;
        if (std::strncmp(className, ".?A", 3) == 0)
        {
            strcpy_s(candidates[candidateCount++], sizeof(candidates[0]), className);
        }
        else
        {
            sprintf_s(candidates[candidateCount++], sizeof(candidates[0]), ".?AV%s@@", className);
            sprintf_s(candidates[candidateCount++], sizeof(candidates[0]), ".?AU%s@@", className);
        }

        for (unsigned c = 0; c < candidateCount && *vtable == nullptr; ++c)
        {
            const BYTE* names[MaximumStringHits];
            const unsigned nameCount = FindExactStrings(
                sections, sectionCount, candidates[c], std::strlen(candidates[c]), names);
            for (unsigned n = 0; n < nameCount && *vtable == nullptr; ++n)
            {
                // type_info layout: vftable pointer, spare pointer, name.
                const BYTE* descriptor = names[n] - 16;
                if (descriptor < static_cast<const BYTE*>(g_gameBase))
                {
                    continue;
                }
                if (!ScanRttiVtable(sections, sectionCount, descriptor, *vtable))
                {
                    SetLastError(ERROR_INVALID_DATA);
                    return FALSE;
                }
            }
        }
        return TRUE;
    }

    void WINAPI LogForPlugins(const char* message)
    {
        if (message != nullptr)
        {
            SoulashDiagnosticsWritePluginMessage(message);
            OutputDebugStringA("Soulash extender: ");
            OutputDebugStringA(message);
            OutputDebugStringA("\r\n");
        }
    }
}

namespace soulash::plugin_services
{
    bool Sha256OfHandle(HANDLE file, char (&hex)[65])
    {
        return soulash::Sha256OfHandle(file, hex);
    }

    void Initialize()
    {
        InitOnceExecuteOnce(&g_gameInfoOnce, GatherGameInfo, nullptr, nullptr);
    }

    const SoulashExtenderInterface* CreateInterface(
        const char* pluginId, const wchar_t* pluginDirectory)
    {
        Initialize();
        if (pluginId == nullptr || pluginDirectory == nullptr
            || std::strlen(pluginId) >= PluginIdCapacity
            || std::wcslen(pluginDirectory) >= MAX_PATH)
        {
            return nullptr;
        }

        AcquireSRWLockExclusive(&g_lock);
        const SoulashExtenderInterface* result = nullptr;
        bool duplicate = false;
        for (unsigned i = 0; i < g_pluginCount; ++i)
        {
            if (_stricmp(g_plugins[i].id, pluginId) == 0)
            {
                duplicate = true;
            }
        }
        if (!duplicate && g_pluginCount < MaximumPlugins)
        {
            PluginRecord& record = g_plugins[g_pluginCount];
            strcpy_s(record.id, sizeof(record.id), pluginId);
            wcscpy_s(record.directory, MAX_PATH, pluginDirectory);
            SoulashExtenderInterface& api = record.api;
            api = {};
            api.structureSize = sizeof(api);
            api.interfaceVersion = SOULASH_EXTENDER_INTERFACE_VERSION;
            api.log = LogForPlugins;
            api.reportFileIssue = SoulashDiagnosticsReportFileIssue;
            api.pluginHandle = g_pluginCount + 1;
            api.pluginId = record.id;
            api.gameDirectory = g_gameDirectory;
            api.pluginDirectory = record.directory;
            api.gameExecutableSha256 = g_gameHash;
            api.gameBase = g_gameBase;
            api.registerListener = RegisterListener;
            api.dispatchMessage = DispatchMessageFromPlugin;
            api.patchMemory = PatchMemory;
            api.installDetour = InstallDetour;
            api.installDetourAuto = InstallDetourAuto;
            api.findFunctionsReferencingString = FindFunctionsReferencingString;
            api.findRttiVtable = FindRttiVtable;
            ++g_pluginCount;
            result = &api;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return result;
    }

    void DispatchCoreMessage(unsigned int type)
    {
        SendMessageToListeners(SOULASH_EXTENDER_CORE_SENDER, type, nullptr, 0, nullptr);
    }
}
