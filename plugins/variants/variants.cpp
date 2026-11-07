// Variants: one version of a mod's data for the plain game, another for the
// game with the extender (docs/variants.md).
//
// A mod keeps its plain-game files where they always are and puts the extender
// version of any of them, in the same relative place, below
// <mod>\extender\variants\ (abilities\Fireball.json, passives.json,
// skills.json, milestones\..., ...). Without the extender the game never looks
// there. With this plugin in Mode=active the game reads the extender version
// instead, also sees files and folders that exist only in variants\ (new
// sub-abilities, milestones, ...), and so the deeper version of a skill -
// conditional triggers, effects.json chains, no compile-down passives - is what
// gets loaded. Without the plugin, or with Enabled=0 / Mode=log, the plain
// version loads, unchanged.
//
// How: pass-through hooks on the game's own import slots for the file calls
// its data loaders use. Reading goes through the dynamic UCRT (_wfsopen /
// _fsopen / _stat64i32), listing folders and every other file operation
// through the statically linked STL filesystem (KERNEL32 CreateFile2,
// FindFirstFileExW, FindNextFileW, FindClose, GetFileAttributesExW,
// MoveFileExW, CopyFile2, CreateDirectoryW) plus _mkdir. Nothing inside the
// game's code is patched, nothing is keyed by an exe hash, and a game build
// that stops importing one of them only loses that one hook.
//
// The mod editor saves abilities from the in-memory ability registry
// (build 526b6ab4: 0x5473D0 reads the registry, then writes
// data/mods/{}/abilities/{}.json), i.e. it would write the EXTENDER version
// over the plain-game file. So while a mod has variants and Mode=active, every
// write the game makes inside that mod (except mod.json and extender\) is
// redirected to <mod>\extender\editor_saves\<same path>; the mod's own files
// are never changed. Edit plain-game files with Mode=log or Enabled=0.
#include "plugin_api.h"
#include "../common/plugin_status.hpp"
#include "../common/variant_index.hpp"

#include <windows.h>

#include <sys/stat.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    namespace vx = soulash_plugins::variants;

    constexpr std::size_t MaximumMods = 512;

    using WfsopenFn = FILE* (__cdecl*)(const wchar_t*, const wchar_t*, int);
    using FsopenFn = FILE* (__cdecl*)(const char*, const char*, int);
    using StatFn = int (__cdecl*)(const char*, struct _stat64i32*);
    using WStatFn = int (__cdecl*)(const wchar_t*, struct _stat64i32*);
    using MkdirFn = int (__cdecl*)(const char*);
    using WMkdirFn = int (__cdecl*)(const wchar_t*);
    using CreateFile2Fn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
    using FindFirstFileExWFn = HANDLE (WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    using FindNextFileWFn = BOOL (WINAPI*)(HANDLE, LPWIN32_FIND_DATAW);
    using FindCloseFn = BOOL (WINAPI*)(HANDLE);
    using GetFileAttributesExWFn = BOOL (WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    using MoveFileExWFn = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
    using CopyFile2Fn = HRESULT (WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);
    using CreateDirectoryWFn = BOOL (WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);

    const SoulashExtenderInterface* g_extender = nullptr;
    soulash_plugins::StartupReason g_startupReason;

    // ---- configuration --------------------------------------------------------
    bool g_enabled = true;
    bool g_active = false;      // Mode=active
    bool g_logFiles = true;     // one line per variant file the first time it is used
    LONG g_maxLogLines = 300;

    // ---- index (immutable once the hooks are in) -------------------------------
    vx::Index* g_index = nullptr;          // heap: never destroyed at process exit
    volatile LONG* g_entryUsed = nullptr;  // per entry: logged once
    bool g_filterByName = true;            // fast path: only paths containing "mods"

    // ---- runtime --------------------------------------------------------------
    volatile LONG g_logLines = 0;
    volatile LONG g_redirectedReads = 0;
    volatile LONG g_addedListings = 0;
    volatile LONG g_quarantinedWrites = 0;
    volatile LONG g_deniedWrites = 0;
    volatile LONG g_loggedWrites = 0;
    volatile LONG g_quarantineHintShown = 0;
    volatile LONG g_firstModReadLogged = 0;

    // Listings that get variant-only entries appended after the real ones.
    struct Listing
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        std::vector<std::size_t> pending;
        std::size_t next = 0;
    };
    SRWLOCK g_listingLock = SRWLOCK_INIT;
    std::vector<Listing>* g_listings = nullptr;
    volatile LONG g_openListings = 0;

    // ---- the game's functions (previous import slot values) --------------------
    WfsopenFn g_wfsopen = nullptr;          // also used to open redirected narrow paths
    FsopenFn g_fsopen = nullptr;
    StatFn g_stat = nullptr;
    WStatFn g_wstat = nullptr;              // UCRT export, not hooked
    MkdirFn g_mkdir = nullptr;
    WMkdirFn g_wmkdir = nullptr;            // UCRT export, not hooked
    CreateFile2Fn g_createFile2 = nullptr;
    FindFirstFileExWFn g_findFirstFileExW = nullptr;
    FindNextFileWFn g_findNextFileW = nullptr;
    FindCloseFn g_findClose = nullptr;
    GetFileAttributesExWFn g_getFileAttributesExW = nullptr;
    MoveFileExWFn g_moveFileExW = nullptr;
    CopyFile2Fn g_copyFile2 = nullptr;
    CreateDirectoryWFn g_createDirectoryW = nullptr;

    void Log(const char* format, ...)
    {
        char line[700];
        va_list args;
        va_start(args, format);
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
        va_end(args);
        g_startupReason.Capture(line);
        char prefixed[740];
        _snprintf_s(prefixed, sizeof(prefixed), _TRUNCATE, "Variants: %s", line);
        g_extender->log(prefixed);
    }

    // File lines are capped per session; problems and summaries always log.
    bool LogBudget()
    {
        const LONG line = InterlockedIncrement(&g_logLines);
        if (line == g_maxLogLines + 1)
        {
            Log("file log cap (%ld lines) reached; only problems from here on", g_maxLogLines);
        }
        return line <= g_maxLogLines;
    }

    std::string Narrow(std::wstring_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (wchar_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        return out;
    }

    // ---- configuration --------------------------------------------------------
    void ReadConfiguration()
    {
        wchar_t iniPath[MAX_PATH * 2];
        _snwprintf_s(iniPath, _TRUNCATE, L"%sVariants.ini", g_extender->pluginDirectory);
        g_enabled = GetPrivateProfileIntW(L"Variants", L"Enabled", 1, iniPath) != 0;
        wchar_t mode[32];
        GetPrivateProfileStringW(L"Variants", L"Mode", L"log", mode, 32, iniPath);
        g_active = _wcsicmp(mode, L"active") == 0;
        g_logFiles = GetPrivateProfileIntW(L"Variants", L"LogFiles", 1, iniPath) != 0;
        g_maxLogLines = static_cast<LONG>(GetPrivateProfileIntW(L"Variants", L"MaxLogLines", 300, iniPath));
        if (g_maxLogLines < 0) g_maxLogLines = 0;
    }

    // ---- scanning <mods>\<mod>\extender\variants -------------------------------
    bool IsDirectory(DWORD attributes)
    {
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    void ScanFolder(std::size_t mod, const std::wstring& modRoot, const std::wstring& variantFolder,
        const std::wstring& relative, std::size_t depth, unsigned int& refused)
    {
        WIN32_FIND_DATAW item{};
        HANDLE find = FindFirstFileW((variantFolder + L"\\*").c_str(), &item);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            const std::wstring name = item.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const bool isDirectory = (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const std::wstring rel = relative.empty() ? name : relative + L"\\" + name;
            const DWORD baseAttributes = GetFileAttributesW((modRoot + L"\\" + rel).c_str());
            std::wstring reason;
            if (!g_index->AddEntry(mod, rel, isDirectory, baseAttributes != INVALID_FILE_ATTRIBUTES,
                    IsDirectory(baseAttributes), reason))
            {
                ++refused;
                Log("%s: extender\\variants\\%s ignored: %s", Narrow(g_index->Mods()[mod].name).c_str(),
                    Narrow(rel).c_str(), Narrow(reason).c_str());
                continue;
            }
            if (isDirectory && depth < vx::MaximumDepth)
            {
                ScanFolder(mod, modRoot, variantFolder + L"\\" + name, rel, depth + 1, refused);
            }
        } while (FindNextFileW(find, &item) != FALSE);
        FindClose(find);
    }

    void ScanMods()
    {
        g_index = new vx::Index();
        const std::wstring root = std::wstring(g_extender->gameDirectory) + L"data\\mods";
        WIN32_FIND_DATAW item{};
        HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &item);
        if (find == INVALID_HANDLE_VALUE)
        {
            Log("no mods folder at %S", root.c_str());
            return;
        }
        std::size_t mods = 0;
        do
        {
            if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || item.cFileName[0] == L'.')
            {
                continue;
            }
            if (++mods > MaximumMods)
            {
                Log("more than %zu mod folders; the rest are not scanned", MaximumMods);
                break;
            }
            const std::wstring modRoot = root + L"\\" + item.cFileName;
            const std::wstring variantFolder = modRoot + L"\\" + vx::VariantsFolder;
            if (!IsDirectory(GetFileAttributesW(variantFolder.c_str())))
            {
                continue;
            }
            const std::size_t mod = g_index->AddMod(item.cFileName, modRoot);
            unsigned int refused = 0;
            ScanFolder(mod, modRoot, variantFolder, L"", 1, refused);
            const vx::Mod& info = g_index->Mods()[mod];
            Log("%S: %zu extender version(s): %zu override(s), %zu added file(s), %zu added folder(s)%s",
                item.cFileName, info.entries(), info.overrides, info.addedFiles, info.addedDirectories,
                refused != 0 ? " (some entries ignored, see above)" : "");
        } while (FindNextFileW(find, &item) != FALSE);
        FindClose(find);
        g_index->Finish();
        g_entryUsed = new LONG[g_index->Size() + 1]();
    }

    // ---- path handling inside hooks ---------------------------------------------
    bool MentionsMods(const wchar_t* path)
    {
        for (const wchar_t* p = path; p[0] != L'\0'; ++p)
        {
            if ((p[0] | 0x20) == L'm' && (p[1] | 0x20) == L'o' && (p[2] | 0x20) == L'd' && (p[3] | 0x20) == L's')
            {
                return true;
            }
            if (p[1] == L'\0' || p[2] == L'\0' || p[3] == L'\0') break;
        }
        return false;
    }

    bool FullPath(const wchar_t* path, std::wstring& out)
    {
        if (path == nullptr || path[0] == L'\0' || (g_filterByName && !MentionsMods(path)))
        {
            return false;
        }
        wchar_t buffer[MAX_PATH * 2];
        const DWORD length = GetFullPathNameW(path, static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
        if (length == 0)
        {
            return false;
        }
        if (length < std::size(buffer))
        {
            out.assign(buffer, length);
            return true;
        }
        std::wstring large(length, L'\0');
        const DWORD again = GetFullPathNameW(path, length, large.data(), nullptr);
        if (again == 0 || again >= length)
        {
            return false;
        }
        large.resize(again);
        out = std::move(large);
        return true;
    }

    bool Widen(const char* text, std::wstring& out)
    {
        if (text == nullptr)
        {
            return false;
        }
        const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
        if (length <= 0)
        {
            return false;
        }
        out.assign(static_cast<std::size_t>(length), L'\0');
        if (MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), length) != length)
        {
            return false;
        }
        out.resize(static_cast<std::size_t>(length - 1));
        return true;
    }

    const char* ModName(std::size_t mod)
    {
        static thread_local std::string name;
        name = Narrow(g_index->Mods()[mod].name);
        return name.c_str();
    }

    void NoteEntry(const vx::Entry& entry, const char* how)
    {
        const std::size_t index = static_cast<std::size_t>(&entry - &g_index->At(0));
        if (!g_logFiles || InterlockedExchange(&g_entryUsed[index], 1) != 0 || !LogBudget())
        {
            return;
        }
        const char* kind = entry.kind == vx::EntryKind::Override ? "extender version of"
            : (entry.kind == vx::EntryKind::AddedFile ? "extender-only file" : "extender-only folder");
        Log("%s%s %s %s\\%s", g_active ? "" : "would use (Mode=log) ", how, kind, ModName(entry.mod),
            Narrow(entry.relative).c_str());
    }

    void NoteFirstModRead(const std::wstring& fullPath)
    {
        if (InterlockedExchange(&g_firstModReadLogged, 1) == 0)
        {
            Log("first read inside a mod seen: %s (the hooks were in place before the game read it)",
                Narrow(fullPath).c_str());
        }
    }

    // Read of `path`: the variant to use instead, or "" for the game's own path.
    std::wstring ReadTarget(const wchar_t* path, const char* how)
    {
        std::wstring full;
        if (!FullPath(path, full))
        {
            return std::wstring();
        }
        const vx::Decision decision = vx::Decide(*g_index, full, false, g_active);
        if (decision.entry == nullptr)
        {
            if (g_firstModReadLogged == 0 && g_index->ModFor(vx::PathKey(full)) != nullptr)
            {
                NoteFirstModRead(full);
            }
            return std::wstring();
        }
        NoteFirstModRead(full);
        NoteEntry(*decision.entry, how);
        if (decision.action != vx::Action::Redirect)
        {
            return std::wstring();
        }
        InterlockedIncrement(&g_redirectedReads);
        return decision.path;
    }

    // Creates every missing folder above `path` (only ever below editor_saves).
    void EnsureParentFolders(const std::wstring& path)
    {
        const std::wstring parent(vx::ParentOf(path));
        if (parent.empty() || IsDirectory(GetFileAttributesW(parent.c_str())))
        {
            return;
        }
        EnsureParentFolders(parent);
        CreateDirectoryW(parent.c_str(), nullptr);
    }

    // Write to `path`: Quarantine / Deny with the path to use, or PassThrough.
    vx::Decision WriteTarget(const wchar_t* path, const char* how)
    {
        std::wstring full;
        if (!FullPath(path, full))
        {
            return vx::Decision();
        }
        vx::Decision decision = vx::Decide(*g_index, full, true, g_active);
        if (decision.mod == nullptr)
        {
            return decision;
        }
        const std::string mod = Narrow(decision.mod->name);
        const std::string relative = Narrow(decision.relative);
        if (decision.action == vx::Action::PassThrough)
        {
            InterlockedIncrement(&g_loggedWrites);
            if (LogBudget())
            {
                Log("%s %s\\%s (Mode=log: written to the mod itself)", how, mod.c_str(), relative.c_str());
            }
            return decision;
        }
        if (InterlockedExchange(&g_quarantineHintShown, 1) == 0)
        {
            Log("the game is writing into %s, which has extender versions loaded (the mod editor?). Its writes go to "
                "%s\\extender\\editor_saves\\ so the plain-game files stay untouched; to edit the plain-game files "
                "set Mode=log or Enabled=0 in Variants.ini and restart", mod.c_str(), mod.c_str());
        }
        if (decision.action == vx::Action::Deny)
        {
            InterlockedIncrement(&g_deniedWrites);
            Log("%s the mod folder %s itself refused (it has extender versions loaded)", how, mod.c_str());
            return decision;
        }
        InterlockedIncrement(&g_quarantinedWrites);
        if (LogBudget())
        {
            Log("%s %s\\%s -> %s\\extender\\editor_saves\\%s", how, mod.c_str(), relative.c_str(), mod.c_str(),
                relative.c_str());
        }
        return decision;
    }

    bool IsNotFound(DWORD error)
    {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    // ---- hooks: UCRT ------------------------------------------------------------
    FILE* __cdecl HookWfsopen(const wchar_t* path, const wchar_t* mode, int share)
    {
        const bool writes = mode != nullptr && vx::FopenWrites(mode);
        if (!writes)
        {
            const std::wstring target = ReadTarget(path, "loading");
            if (!target.empty())
            {
                if (FILE* file = g_wfsopen(target.c_str(), mode, share))
                {
                    return file;
                }
                Log("cannot open %s; the game gets its own file", Narrow(target).c_str());
            }
            return g_wfsopen(path, mode, share);
        }
        const vx::Decision decision = WriteTarget(path, "write");
        if (decision.action == vx::Action::Deny)
        {
            errno = EACCES;
            return nullptr;
        }
        if (decision.action == vx::Action::Quarantine)
        {
            EnsureParentFolders(decision.path);
            return g_wfsopen(decision.path.c_str(), mode, share);
        }
        return g_wfsopen(path, mode, share);
    }

    FILE* __cdecl HookFsopen(const char* path, const char* mode, int share)
    {
        std::wstring widePath;
        std::wstring wideMode;
        if (g_wfsopen == nullptr || !Widen(path, widePath) || !Widen(mode, wideMode))
        {
            return g_fsopen(path, mode, share);
        }
        if (!vx::FopenWrites(wideMode))
        {
            const std::wstring target = ReadTarget(widePath.c_str(), "loading");
            if (!target.empty())
            {
                if (FILE* file = g_wfsopen(target.c_str(), wideMode.c_str(), share))
                {
                    return file;
                }
                Log("cannot open %s; the game gets its own file", Narrow(target).c_str());
            }
            return g_fsopen(path, mode, share);
        }
        const vx::Decision decision = WriteTarget(widePath.c_str(), "write");
        if (decision.action == vx::Action::Deny)
        {
            errno = EACCES;
            return nullptr;
        }
        if (decision.action == vx::Action::Quarantine)
        {
            EnsureParentFolders(decision.path);
            return g_wfsopen(decision.path.c_str(), wideMode.c_str(), share);
        }
        return g_fsopen(path, mode, share);
    }

    int __cdecl HookStat(const char* path, struct _stat64i32* buffer)
    {
        std::wstring widePath;
        if (g_wstat != nullptr && Widen(path, widePath))
        {
            const std::wstring target = ReadTarget(widePath.c_str(), "checking");
            if (!target.empty() && g_wstat(target.c_str(), buffer) == 0)
            {
                return 0;
            }
        }
        return g_stat(path, buffer);
    }

    int __cdecl HookMkdir(const char* path)
    {
        std::wstring widePath;
        if (g_wmkdir != nullptr && Widen(path, widePath))
        {
            const vx::Decision decision = WriteTarget(widePath.c_str(), "create folder");
            if (decision.action == vx::Action::Deny)
            {
                errno = EACCES;
                return -1;
            }
            if (decision.action == vx::Action::Quarantine)
            {
                EnsureParentFolders(decision.path);
                return g_wmkdir(decision.path.c_str());
            }
        }
        return g_mkdir(path);
    }

    // ---- hooks: KERNEL32 (the STL filesystem) -----------------------------------
    HANDLE WINAPI HookCreateFile2(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
        LPCREATEFILE2_EXTENDED_PARAMETERS parameters)
    {
        if (!vx::CreateFileWrites(access, disposition))
        {
            const std::wstring target = ReadTarget(path, "opening");
            if (!target.empty())
            {
                const HANDLE file = g_createFile2(target.c_str(), access, share, disposition, parameters);
                if (file != INVALID_HANDLE_VALUE || !IsNotFound(GetLastError()))
                {
                    return file;
                }
            }
            return g_createFile2(path, access, share, disposition, parameters);
        }
        const vx::Decision decision = WriteTarget(path, (access & DELETE) != 0 ? "delete/rename" : "write");
        if (decision.action == vx::Action::Deny)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (decision.action == vx::Action::Quarantine)
        {
            if (disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
            {
                EnsureParentFolders(decision.path);
            }
            return g_createFile2(decision.path.c_str(), access, share, disposition, parameters);
        }
        return g_createFile2(path, access, share, disposition, parameters);
    }

    BOOL WINAPI HookGetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID information)
    {
        const std::wstring target = ReadTarget(path, "checking");
        if (!target.empty())
        {
            if (g_getFileAttributesExW(target.c_str(), level, information) != FALSE)
            {
                return TRUE;
            }
        }
        return g_getFileAttributesExW(path, level, information);
    }

    BOOL WINAPI HookMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
    {
        const vx::Decision source = WriteTarget(from, "move from");
        const vx::Decision destination = to != nullptr ? WriteTarget(to, "move to") : vx::Decision();
        if (source.action == vx::Action::Deny || destination.action == vx::Action::Deny)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (destination.action == vx::Action::Quarantine)
        {
            EnsureParentFolders(destination.path);
        }
        return g_moveFileExW(source.action == vx::Action::Quarantine ? source.path.c_str() : from,
            destination.action == vx::Action::Quarantine ? destination.path.c_str() : to, flags);
    }

    HRESULT WINAPI HookCopyFile2(PCWSTR from, PCWSTR to, COPYFILE2_EXTENDED_PARAMETERS* parameters)
    {
        const std::wstring source = ReadTarget(from, "copying");
        const vx::Decision destination = WriteTarget(to, "copy to");
        if (destination.action == vx::Action::Deny)
        {
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        }
        if (destination.action == vx::Action::Quarantine)
        {
            EnsureParentFolders(destination.path);
        }
        return g_copyFile2(source.empty() ? from : source.c_str(),
            destination.action == vx::Action::Quarantine ? destination.path.c_str() : to, parameters);
    }

    BOOL WINAPI HookCreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security)
    {
        const vx::Decision decision = WriteTarget(path, "create folder");
        if (decision.action == vx::Action::Deny)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (decision.action == vx::Action::Quarantine)
        {
            EnsureParentFolders(decision.path);
            return g_createDirectoryW(decision.path.c_str(), security);
        }
        return g_createDirectoryW(path, security);
    }

    // Folder listings. A folder that exists only in variants is listed from the
    // variant folder; a base folder with variant-only entries gets them
    // appended once the real listing ends.
    HANDLE WINAPI HookFindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data,
        FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags)
    {
        std::wstring full;
        if (!FullPath(pattern, full))
        {
            return g_findFirstFileExW(pattern, level, data, search, filter, flags);
        }
        const std::wstring canonical = vx::CanonicalPath(full);
        const std::wstring folderKey = vx::LowerAscii(vx::ParentOf(canonical));
        const std::wstring leaf(vx::LeafOf(canonical));
        const vx::Entry* folder = g_index->Find(folderKey);
        if (folder != nullptr && folder->kind == vx::EntryKind::AddedDirectory)
        {
            NoteEntry(*folder, "listing");
            if (g_active)
            {
                const std::wstring redirected = folder->variantPath + L"\\" + leaf;
                return g_findFirstFileExW(redirected.c_str(), level, data, search, filter, flags);
            }
            return g_findFirstFileExW(pattern, level, data, search, filter, flags);
        }
        const std::vector<std::size_t>* additions = g_index->AdditionsIn(folderKey);
        if (additions == nullptr)
        {
            return g_findFirstFileExW(pattern, level, data, search, filter, flags);
        }
        std::vector<std::size_t> matching;
        for (std::size_t i : *additions)
        {
            const vx::Entry& entry = g_index->At(i);
            if (vx::WildcardMatch(leaf, vx::LeafOf(entry.relative))
                && (search != FindExSearchLimitToDirectories || entry.kind == vx::EntryKind::AddedDirectory))
            {
                matching.push_back(i);
            }
        }
        if (matching.empty())
        {
            return g_findFirstFileExW(pattern, level, data, search, filter, flags);
        }
        if (!g_active)
        {
            for (std::size_t i : matching) NoteEntry(g_index->At(i), "listing");
            return g_findFirstFileExW(pattern, level, data, search, filter, flags);
        }
        const HANDLE handle = g_findFirstFileExW(pattern, level, data, search, filter, flags);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND)
            {
                // Nothing in the base folder matches, so every match in the
                // variant folder is an addition (an override has a base twin).
                const std::wstring variantFolder(vx::ParentOf(g_index->At(matching.front()).variantPath));
                for (std::size_t i : matching) NoteEntry(g_index->At(i), "listing");
                InterlockedExchangeAdd(&g_addedListings, static_cast<LONG>(matching.size()));
                return g_findFirstFileExW((variantFolder + L"\\" + leaf).c_str(), level, data, search, filter, flags);
            }
            SetLastError(error);
            return handle;
        }
        Listing listing;
        listing.handle = handle;
        listing.pending = std::move(matching);
        AcquireSRWLockExclusive(&g_listingLock);
        g_listings->push_back(std::move(listing));
        InterlockedIncrement(&g_openListings);
        ReleaseSRWLockExclusive(&g_listingLock);
        return handle;
    }

    bool NextAddition(HANDLE handle, std::size_t& entry)
    {
        bool found = false;
        AcquireSRWLockExclusive(&g_listingLock);
        for (Listing& listing : *g_listings)
        {
            if (listing.handle == handle)
            {
                if (listing.next < listing.pending.size())
                {
                    entry = listing.pending[listing.next++];
                    found = true;
                }
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_listingLock);
        return found;
    }

    BOOL WINAPI HookFindNextFileW(HANDLE handle, LPWIN32_FIND_DATAW data)
    {
        const BOOL result = g_findNextFileW(handle, data);
        if (result != FALSE || g_openListings == 0)
        {
            return result;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_FILES && data != nullptr)
        {
            std::size_t index = 0;
            while (NextAddition(handle, index))
            {
                const vx::Entry& entry = g_index->At(index);
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                if (GetFileAttributesExW(entry.variantPath.c_str(), GetFileExInfoStandard, &attributes) == FALSE)
                {
                    continue;   // removed since the scan
                }
                ZeroMemory(data, sizeof(*data));
                data->dwFileAttributes = attributes.dwFileAttributes;
                data->ftCreationTime = attributes.ftCreationTime;
                data->ftLastAccessTime = attributes.ftLastAccessTime;
                data->ftLastWriteTime = attributes.ftLastWriteTime;
                data->nFileSizeHigh = attributes.nFileSizeHigh;
                data->nFileSizeLow = attributes.nFileSizeLow;
                wcsncpy_s(data->cFileName, std::wstring(vx::LeafOf(entry.relative)).c_str(), _TRUNCATE);
                NoteEntry(entry, "listing");
                InterlockedIncrement(&g_addedListings);
                return TRUE;
            }
        }
        SetLastError(error);
        return FALSE;
    }

    BOOL WINAPI HookFindClose(HANDLE handle)
    {
        if (g_openListings != 0)
        {
            AcquireSRWLockExclusive(&g_listingLock);
            for (auto it = g_listings->begin(); it != g_listings->end(); ++it)
            {
                if (it->handle == handle)
                {
                    g_listings->erase(it);
                    InterlockedDecrement(&g_openListings);
                    break;
                }
            }
            ReleaseSRWLockExclusive(&g_listingLock);
        }
        return g_findClose(handle);
    }

    // ---- import slots -----------------------------------------------------------
    // The game's import slot for `function` (any DLL: the UCRT functions come
    // from api-ms-win-crt-* forwarders, which may change). Plain C body for __try.
    void** FindImportSlot(unsigned char* base, const char* function)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return nullptr;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return nullptr;
        }
        const IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imports.VirtualAddress == 0)
        {
            return nullptr;
        }
        for (const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
            descriptor->Name != 0; ++descriptor)
        {
            if (descriptor->OriginalFirstThunk == 0)
            {
                continue;
            }
            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
            auto* functions = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
            for (std::size_t i = 0; names[i].u1.AddressOfData != 0; ++i)
            {
                if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal))
                {
                    continue;
                }
                const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                if (std::strcmp(byName->Name, function) == 0)
                {
                    return reinterpret_cast<void**>(&functions[i].u1.Function);
                }
            }
        }
        return nullptr;
    }

    void** GuardedFindImportSlot(unsigned char* base, const char* function)
    {
        __try
        {
            return FindImportSlot(base, function);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    struct HookSpec
    {
        const char* function;
        void* hook;
        void** original;
        void** slot = nullptr;
        bool installed = false;
    };

    HookSpec g_hooks[] = {
        {"_wfsopen", reinterpret_cast<void*>(&HookWfsopen), reinterpret_cast<void**>(&g_wfsopen)},
        {"_fsopen", reinterpret_cast<void*>(&HookFsopen), reinterpret_cast<void**>(&g_fsopen)},
        {"_stat64i32", reinterpret_cast<void*>(&HookStat), reinterpret_cast<void**>(&g_stat)},
        {"_mkdir", reinterpret_cast<void*>(&HookMkdir), reinterpret_cast<void**>(&g_mkdir)},
        {"CreateFile2", reinterpret_cast<void*>(&HookCreateFile2), reinterpret_cast<void**>(&g_createFile2)},
        {"FindFirstFileExW", reinterpret_cast<void*>(&HookFindFirstFileExW), reinterpret_cast<void**>(&g_findFirstFileExW)},
        {"FindNextFileW", reinterpret_cast<void*>(&HookFindNextFileW), reinterpret_cast<void**>(&g_findNextFileW)},
        {"FindClose", reinterpret_cast<void*>(&HookFindClose), reinterpret_cast<void**>(&g_findClose)},
        {"GetFileAttributesExW", reinterpret_cast<void*>(&HookGetFileAttributesExW), reinterpret_cast<void**>(&g_getFileAttributesExW)},
        {"MoveFileExW", reinterpret_cast<void*>(&HookMoveFileExW), reinterpret_cast<void**>(&g_moveFileExW)},
        {"CopyFile2", reinterpret_cast<void*>(&HookCopyFile2), reinterpret_cast<void**>(&g_copyFile2)},
        {"CreateDirectoryW", reinterpret_cast<void*>(&HookCreateDirectoryW), reinterpret_cast<void**>(&g_createDirectoryW)},
    };

    HookSpec& Hook(const char* function)
    {
        for (HookSpec& spec : g_hooks)
        {
            if (std::strcmp(spec.function, function) == 0) return spec;
        }
        return g_hooks[0];
    }

    // The UCRT's wide twin of a hooked narrow function, from the same module.
    void* WideTwin(void* narrow, const char* name)
    {
        HMODULE module = nullptr;
        if (narrow == nullptr || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(narrow), &module) == FALSE)
        {
            module = GetModuleHandleW(L"ucrtbase.dll");
        }
        return module != nullptr ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    }

    std::uint64_t MillisecondsSinceProcessStart()
    {
        FILETIME created{}, exited{}, kernel{}, user{}, now{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE)
        {
            return 0;
        }
        GetSystemTimeAsFileTime(&now);
        const auto value = [](const FILETIME& time) {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        return value(now) > value(created) ? (value(now) - value(created)) / 10000 : 0;
    }

    // Returns false when the game cannot be read through the hooks at all.
    bool Install(char* summary, std::size_t size)
    {
        unsigned char* base = static_cast<unsigned char*>(g_extender->gameBase);
        for (HookSpec& spec : g_hooks)
        {
            spec.slot = GuardedFindImportSlot(base, spec.function);
            if (spec.slot != nullptr && *spec.slot != nullptr)
            {
                *spec.original = *spec.slot;
            }
        }
        // The read hooks are the feature; without one of them nothing is installed.
        if (Hook("_wfsopen").slot == nullptr && Hook("_fsopen").slot == nullptr)
        {
            Log("the game imports neither _wfsopen nor _fsopen; inactive (nothing hooked)");
            return false;
        }
        g_wstat = reinterpret_cast<WStatFn>(WideTwin(*Hook("_stat64i32").original, "_wstat64i32"));
        g_wmkdir = reinterpret_cast<WMkdirFn>(WideTwin(*Hook("_mkdir").original, "_wmkdir"));
        if (g_wfsopen == nullptr)
        {
            g_wfsopen = reinterpret_cast<WfsopenFn>(WideTwin(reinterpret_cast<void*>(g_fsopen), "_wfsopen"));
        }
        // Listing hooks only make sense together.
        const bool listings = Hook("FindFirstFileExW").slot != nullptr && Hook("FindNextFileW").slot != nullptr
            && Hook("FindClose").slot != nullptr;
        summary[0] = '\0';
        char missing[300] = "";
        for (HookSpec& spec : g_hooks)
        {
            const bool isListing = std::strcmp(spec.function, "FindFirstFileExW") == 0
                || std::strcmp(spec.function, "FindNextFileW") == 0 || std::strcmp(spec.function, "FindClose") == 0;
            if (spec.slot == nullptr || *spec.original == nullptr || (isListing && !listings))
            {
                strncat_s(missing, " ", _TRUNCATE);
                strncat_s(missing, spec.function, _TRUNCATE);
                continue;
            }
            void* hook = spec.hook;
            if (!g_extender->patchMemory(g_extender->pluginHandle, spec.slot, &hook, sizeof(hook)))
            {
                strncat_s(missing, " ", _TRUNCATE);
                strncat_s(missing, spec.function, _TRUNCATE);
                strncat_s(missing, "(patch failed)", _TRUNCATE);
                continue;
            }
            spec.installed = true;
            strncat_s(summary, size, " ", _TRUNCATE);
            strncat_s(summary, size, spec.function, _TRUNCATE);
        }
        if (missing[0] != '\0')
        {
            Log("not hooked on this build:%s", missing);
        }
        return Hook("_wfsopen").installed || Hook("_fsopen").installed;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI SoulashExtenderPluginLoad(const SoulashExtenderInterface* extender)
{
    if (extender == nullptr
        || extender->structureSize < SOULASH_EXTENDER_INTERFACE_V3_BASE_SIZE
        || extender->interfaceVersion != SOULASH_EXTENDER_INTERFACE_VERSION
        || extender->gameBase == nullptr || extender->gameDirectory == nullptr || extender->pluginDirectory == nullptr)
    {
        return FALSE;
    }
    g_extender = extender;
    g_startupReason.Begin();
    ReadConfiguration();
    if (!g_enabled)
    {
        Log("disabled: [Variants] Enabled=0 in %SVariants.ini; every mod loads its plain-game files", extender->pluginDirectory);
        g_startupReason.End();
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_DISABLED, "Enabled=0 in Variants.ini");
        return TRUE;
    }
    ScanMods();
    std::size_t mods = 0;
    std::size_t overrides = 0;
    std::size_t addedFiles = 0;
    std::size_t addedFolders = 0;
    for (const vx::Mod& mod : g_index->Mods())
    {
        if (mod.entries() == 0) continue;
        ++mods;
        overrides += mod.overrides;
        addedFiles += mod.addedFiles;
        addedFolders += mod.addedDirectories;
    }
    if (g_index->Size() == 0)
    {
        Log("no mod has files in extender\\variants; nothing hooked");
        g_startupReason.End();
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_ACTIVE, "no mod ships extender versions; idle");
        return TRUE;
    }
    // Relative paths are resolved against the working folder; if that is itself
    // inside something called "mods", the cheap name filter would be wrong.
    wchar_t workingFolder[MAX_PATH * 2] = L"";
    const DWORD workingLength = GetCurrentDirectoryW(static_cast<DWORD>(std::size(workingFolder)), workingFolder);
    if (workingLength == 0 || workingLength >= std::size(workingFolder) || MentionsMods(workingFolder))
    {
        g_filterByName = false;
    }
    if (workingLength != 0 && workingLength < std::size(workingFolder)
        && vx::PathKey(workingFolder) != vx::PathKey(extender->gameDirectory))
    {
        Log("note: the game's working folder is %S, not the game folder; relative data/mods paths resolve there",
            workingFolder);
    }
    g_listings = new std::vector<Listing>();
    char hooked[400];
    const bool installed = Install(hooked, sizeof(hooked));
    g_startupReason.End();
    if (!installed)
    {
        soulash_plugins::ReportStatus(extender, SOULASH_PLUGIN_STATUS_INACTIVE, g_startupReason.Last());
        return TRUE;
    }
    Log("%s: %zu extender version(s) in %zu mod(s) (%zu override(s), %zu added file(s), %zu added folder(s)); "
        "hooked%s; %llu ms after process start%s",
        g_active ? "active" : "Mode=log (plain-game files load; would-use lines show what Mode=active changes)",
        g_index->Size(), mods, overrides, addedFiles, addedFolders, hooked,
        static_cast<unsigned long long>(MillisecondsSinceProcessStart()),
        g_active ? "; writes into these mods go to <mod>\\extender\\editor_saves" : "");
    char detail[300];
    _snprintf_s(detail, _TRUNCATE, "mode=%s; %zu extender version(s) in %zu mod(s)", g_active ? "active" : "log",
        g_index->Size(), mods);
    const bool complete = Hook("FindFirstFileExW").installed && Hook("CreateFile2").installed
        && Hook("GetFileAttributesExW").installed;
    soulash_plugins::ReportStatus(extender,
        complete ? SOULASH_PLUGIN_STATUS_ACTIVE : SOULASH_PLUGIN_STATUS_DEGRADED, detail);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
