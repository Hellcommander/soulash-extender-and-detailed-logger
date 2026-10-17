// Purpose-built ABI v3 test plugin. Built once and copied under several file
// names so the plugin directory loader gets distinct plugin ids.
#include "plugin_api.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr unsigned int HelloMessage = SOULASH_EXTENDER_MSG_USER_BASE + 1;

    const SoulashExtenderInterface* g_extender = nullptr;

    void WriteMarker(const char* format, const char* a = "", const char* b = "")
    {
        wchar_t markerPath[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(
            L"SOULASH2_TEST_MODULE_MARKER", markerPath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            return;
        }

        char line[512];
        _snprintf_s(line, sizeof(line), _TRUNCATE, format, a, b);
        HANDLE file = CreateFileW(
            markerPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        WriteFile(file, "\r\n", 2, &written, nullptr);
        CloseHandle(file);
    }

    void WINAPI OnMessage(const SoulashExtenderMessage* message, void*)
    {
        const char* self = g_extender->pluginId;
        if (std::strcmp(message->sender, SOULASH_EXTENDER_CORE_SENDER) == 0)
        {
            if (message->type == SOULASH_EXTENDER_MSG_POSTLOAD)
            {
                WriteMarker("postload:%s", self);
                // Broadcast to every other plugin's listener.
                const char hello[] = "hello";
                g_extender->dispatchMessage(
                    g_extender->pluginHandle, HelloMessage, hello, sizeof(hello), nullptr);
            }
            else if (message->type == SOULASH_EXTENDER_MSG_POSTPOSTLOAD)
            {
                WriteMarker("postpostload:%s", self);
            }
            return;
        }
        if (message->type == HelloMessage)
        {
            WriteMarker("message:%s<-%s", self, message->sender);
        }
    }

    // Executable test target: 16 NOPs, then `mov eax, 41 ; ret`.
    using TargetFn = int (*)();
    TargetFn g_originalTarget = nullptr;

    int DetourTarget()
    {
        return g_originalTarget() + 1;
    }

    void RunDetourAndPatchChecks()
    {
        auto* code = static_cast<BYTE*>(VirtualAlloc(
            nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (code == nullptr)
        {
            WriteMarker("detour:alloc-failed");
            return;
        }
        std::memset(code, 0x90, 16);
        static const BYTE tail[] = {0xB8, 0x29, 0x00, 0x00, 0x00, 0xC3};
        std::memcpy(code + 16, tail, sizeof(tail));
        const auto target = reinterpret_cast<TargetFn>(code);

        void* trampoline = nullptr;
        const BOOL tooSmallRejected = !g_extender->installDetour(
            g_extender->pluginHandle, code, reinterpret_cast<void*>(&DetourTarget), 8, &trampoline);
        const BOOL badHandleRejected = !g_extender->installDetour(
            0, code, reinterpret_cast<void*>(&DetourTarget), 14, &trampoline);
        const BOOL before = target() == 41;
        const BOOL installed = g_extender->installDetour(
            g_extender->pluginHandle, code, reinterpret_cast<void*>(&DetourTarget), 16, &trampoline);
        g_originalTarget = reinterpret_cast<TargetFn>(trampoline);
        const BOOL detoured = installed && target() == 42;

        const BYTE patch[] = {0xB8, 0x07, 0x00, 0x00, 0x00};
        const BOOL patched = g_extender->patchMemory(
            g_extender->pluginHandle, code + 16, patch, sizeof(patch));
        // Target is now detoured; the trampoline sees the patched tail.
        const BOOL patchSeen = patched && g_originalTarget() == 7;

        WriteMarker(
            tooSmallRejected && badHandleRejected && before && detoured && patchSeen
                ? "detour:ok" : "detour:FAILED");
    }

    using IntFn = int (*)();
    using IntArgFn = int (*)(int);

    IntFn g_trampolineA = nullptr;
    IntArgFn g_trampolineB = nullptr;

    int DetourA() { return g_trampolineA() + 100; }
    int DetourB(int value) { return g_trampolineB(value) + 1000; }

    BYTE* NewCodeBuffer()
    {
        auto* code = static_cast<BYTE*>(VirtualAlloc(
            nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (code != nullptr)
        {
            std::memset(code, 0xCC, 4096);
        }
        return code;
    }

    // installDetourAuto must refuse, leaving the target bytes untouched.
    bool RefusalLeavesTargetUntouched(
        const SoulashExtenderInterface* extender, const BYTE* bytes, std::size_t size)
    {
        BYTE* code = NewCodeBuffer();
        if (code == nullptr)
        {
            return false;
        }
        std::memcpy(code, bytes, size);
        BYTE before[64];
        std::memcpy(before, code, sizeof(before));
        void* trampoline = nullptr;
        const BOOL installed = extender->installDetourAuto(
            extender->pluginHandle, code, reinterpret_cast<void*>(&DetourA), &trampoline);
        return !installed && trampoline == nullptr && std::memcmp(before, code, sizeof(before)) == 0;
    }

    void RunAutoDetourChecks()
    {
        // Function A: RIP-relative lea and a relative call inside the stolen bytes.
        BYTE* a = NewCodeBuffer();
        // Function B: a short conditional jump inside the stolen bytes.
        BYTE* b = NewCodeBuffer();
        if (a == nullptr || b == nullptr)
        {
            WriteMarker("autodetour:FAILED alloc");
            return;
        }

        const BYTE functionA[] = {
            0x48, 0x83, 0xEC, 0x28,                    // sub rsp,28h
            0x48, 0x8D, 0x0D, 0x75, 0x00, 0x00, 0x00,  // lea rcx,[rip+0x75] -> a+0x80
            0x8B, 0x01,                                // mov eax,[rcx]
            0xE8, 0x8E, 0x00, 0x00, 0x00,              // call a+0xA0
            0x48, 0x83, 0xC4, 0x28,                    // add rsp,28h
            0xC3                                       // ret
        };
        std::memcpy(a, functionA, sizeof(functionA));
        const std::int32_t forty = 40;
        std::memcpy(a + 0x80, &forty, sizeof(forty));
        const BYTE helper[] = {0x83, 0xC0, 0x02, 0xC3}; // add eax,2 ; ret
        std::memcpy(a + 0xA0, helper, sizeof(helper));

        const BYTE functionB[] = {
            0x85, 0xC9,                                // test ecx,ecx
            0x74, 0x1C,                                // jz +0x1C -> b+0x20
            0xB8, 0x01, 0x00, 0x00, 0x00,              // mov eax,1
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90,        // nop x6
            0xC3                                       // ret
        };
        std::memcpy(b, functionB, sizeof(functionB));
        const BYTE taken[] = {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3}; // mov eax,2 ; ret
        std::memcpy(b + 0x20, taken, sizeof(taken));

        const IntFn callA = reinterpret_cast<IntFn>(a);
        const IntArgFn callB = reinterpret_cast<IntArgFn>(b);
        bool ok = callA() == 42 && callB(0) == 2 && callB(1) == 1;

        void* trampoline = nullptr;
        ok = ok && g_extender->installDetourAuto(
            g_extender->pluginHandle, a, reinterpret_cast<void*>(&DetourA), &trampoline);
        g_trampolineA = reinterpret_cast<IntFn>(trampoline);
        ok = ok && callA() == 142 && g_trampolineA() == 42;

        trampoline = nullptr;
        ok = ok && g_extender->installDetourAuto(
            g_extender->pluginHandle, b, reinterpret_cast<void*>(&DetourB), &trampoline);
        g_trampolineB = reinterpret_cast<IntArgFn>(trampoline);
        ok = ok && callB(0) == 1002 && callB(1) == 1001
            && g_trampolineB(0) == 2 && g_trampolineB(1) == 1;

        // Refusals.
        const BYTE internalBranch[] = {0x85, 0xC9, 0x75, 0x02, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
        const BYTE tooShort[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        const BYTE loopInstruction[] = {0xE2, 0xFE, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        const BYTE invalidOpcode[] = {0x06, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        const BOOL refused =
            RefusalLeavesTargetUntouched(g_extender, internalBranch, sizeof(internalBranch))
            && RefusalLeavesTargetUntouched(g_extender, tooShort, sizeof(tooShort))
            && RefusalLeavesTargetUntouched(g_extender, loopInstruction, sizeof(loopInstruction))
            && RefusalLeavesTargetUntouched(g_extender, invalidOpcode, sizeof(invalidOpcode));
        // Bad handle / null arguments.
        void* ignored = nullptr;
        const BOOL badArguments = !g_extender->installDetourAuto(0, a, a, &ignored)
            && !g_extender->installDetourAuto(g_extender->pluginHandle, nullptr, a, &ignored)
            && !g_extender->installDetourAuto(g_extender->pluginHandle, a, nullptr, &ignored)
            && !g_extender->installDetourAuto(g_extender->pluginHandle, a, a, nullptr);

        WriteMarker(ok && refused && badArguments ? "autodetour:ok" : "autodetour:FAILED");
        if (!ok) WriteMarker("autodetour:FAILED hooks");
        if (!refused) WriteMarker("autodetour:FAILED refusals");
        if (!badArguments) WriteMarker("autodetour:FAILED arguments");
    }

    // The "game" in tests is the host test executable, which contains a
    // function using a known string and a class with a known RTTI name.
    void RunAnchorChecks()
    {
        const unsigned int handle = g_extender->pluginHandle;
        void* functions[4]{};
        unsigned int count = 99;
        const BOOL found = g_extender->findFunctionsReferencingString(
            handle, "synthetic-anchor-string-7f3a", functions, 4, &count);
        char text[96];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%llx:%u",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(functions[0])), count);
        WriteMarker(found ? "anchor:%s" : "anchor:FAILED %s", text);

        count = 99;
        const BOOL none = g_extender->findFunctionsReferencingString(
            handle, "no-such-string-in-the-host-7f3a-xyz", functions, 4, &count);
        WriteMarker(none && count == 0 ? "anchor-none:ok" : "anchor-none:FAILED");

        const BOOL rejected = !g_extender->findFunctionsReferencingString(
            0, "synthetic-anchor-string-7f3a", functions, 4, &count)
            && !g_extender->findFunctionsReferencingString(handle, "", functions, 4, &count);
        WriteMarker(rejected ? "anchor-invalid:ok" : "anchor-invalid:FAILED");

        void* vtable = nullptr;
        const BOOL rtti = g_extender->findRttiVtable(handle, "SyntheticPolymorphicType", &vtable);
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%llx",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(vtable)));
        WriteMarker(rtti ? "rtti:%s" : "rtti:FAILED %s", text);

        void* missing = &vtable;
        const BOOL absent = g_extender->findRttiVtable(handle, "NoSuchClassInHost7f3a", &missing);
        WriteMarker(absent && missing == nullptr ? "rtti-none:ok" : "rtti-none:FAILED");
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SoulashExtenderPluginLoad(
    const SoulashExtenderInterface* extender)
{
    if (extender == nullptr
        || extender->structureSize != sizeof(SoulashExtenderInterface)
        || extender->interfaceVersion != SOULASH_EXTENDER_INTERFACE_VERSION
        || extender->pluginId == nullptr
        || extender->gameDirectory == nullptr
        || extender->pluginDirectory == nullptr
        || extender->gameExecutableSha256 == nullptr
        || extender->gameBase == nullptr)
    {
        return FALSE;
    }

    g_extender = extender;
    extender->log("v3 plugin loaded");
    WriteMarker("load:%s", extender->pluginId);
    WriteMarker(std::strlen(extender->gameExecutableSha256) == 64
        ? "hash:ok" : "hash:unavailable");

    if (!extender->registerListener(extender->pluginHandle, nullptr, OnMessage, nullptr)
        || extender->registerListener(extender->pluginHandle, nullptr, nullptr, nullptr))
    {
        return FALSE;
    }
    // Only the first plugin (by handle) runs the detour and anchor checks.
    if (extender->pluginHandle == 1)
    {
        RunDetourAndPatchChecks();
        RunAutoDetourChecks();
        RunAnchorChecks();
    }
    // User-range types only: core types must be rejected.
    if (extender->dispatchMessage(
            extender->pluginHandle, SOULASH_EXTENDER_MSG_POSTLOAD, nullptr, 0, nullptr))
    {
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
