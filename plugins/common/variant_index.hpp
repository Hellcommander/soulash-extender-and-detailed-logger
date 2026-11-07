// Pure parts of the Variants plugin (docs/variants.md): path canonicalisation,
// the index of a mod's extender versions (<mod>\extender\variants\...) and the
// redirect decision for one file-system call. No file-system access here, so
// everything is unit-tested with synthetic paths (tests/variant_tests.inc).
//
// A mod ships the plain-game version of its data in the usual places and the
// extender version of any file in the same relative place below
// <mod>\extender\variants\. With the plugin active the game reads the variant
// instead of the base file (an *override*), sees variant files and folders that
// have no base counterpart as if they were in the mod (*additions*), and every
// write the game makes inside such a mod lands in <mod>\extender\editor_saves\
// instead, so the plain-game files are never overwritten with extender data.
#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace soulash_plugins::variants
{
    constexpr std::size_t MaximumEntriesPerMod = 4096;
    constexpr std::size_t MaximumDepth = 8;
    constexpr wchar_t VariantsFolder[] = L"extender\\variants";
    constexpr wchar_t QuarantineFolder[] = L"extender\\editor_saves";

    inline wchar_t LowerAscii(wchar_t c)
    {
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }

    inline std::wstring LowerAscii(std::wstring_view text)
    {
        std::wstring out(text);
        for (wchar_t& c : out) c = LowerAscii(c);
        return out;
    }

    inline bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
        }
        return true;
    }

    // Case-preserving canonical form: '/' -> '\', a "\\?\" prefix in front of a
    // drive letter dropped, repeated separators collapsed (a leading "\\" of a
    // UNC path kept), "." and ".." segments resolved, no trailing separator
    // (except a drive root, "C:\"). Comparison keys are LowerAscii() of this.
    inline std::wstring CanonicalPath(std::wstring_view path)
    {
        std::wstring s(path);
        for (wchar_t& c : s) if (c == L'/') c = L'\\';
        if (s.size() >= 6 && s.compare(0, 4, L"\\\\?\\") == 0 && s[5] == L':')
        {
            s.erase(0, 4);
        }
        std::wstring prefix;
        std::size_t position = 0;
        bool absolute = false;
        if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\')
        {
            prefix = L"\\\\";
            position = 2;
            absolute = true;
        }
        else if (s.size() >= 2 && s[1] == L':')
        {
            prefix = s.substr(0, 2);
            position = 2;
            absolute = s.size() > 2 && s[2] == L'\\';
        }
        else if (!s.empty() && s[0] == L'\\')
        {
            absolute = true;
        }
        std::vector<std::wstring> segments;
        while (position <= s.size())
        {
            std::size_t next = s.find(L'\\', position);
            if (next == std::wstring::npos) next = s.size();
            std::wstring segment = s.substr(position, next - position);
            position = next + 1;
            if (segment.empty() || segment == L".") continue;
            if (segment == L"..")
            {
                if (!segments.empty() && segments.back() != L"..")
                {
                    segments.pop_back();
                }
                else if (!absolute)
                {
                    segments.push_back(segment);
                }
                continue;
            }
            segments.push_back(std::move(segment));
        }
        std::wstring out = prefix;
        if (absolute && prefix != L"\\\\") out += L'\\';
        for (std::size_t i = 0; i < segments.size(); ++i)
        {
            if (i != 0) out += L'\\';
            out += segments[i];
        }
        return out;
    }

    inline std::wstring PathKey(std::wstring_view path)
    {
        return LowerAscii(CanonicalPath(path));
    }

    // Parent / leaf of a canonical path ("" parent for a bare name).
    inline std::wstring_view ParentOf(std::wstring_view canonical)
    {
        const std::size_t slash = canonical.rfind(L'\\');
        return slash == std::wstring_view::npos ? std::wstring_view() : canonical.substr(0, slash);
    }

    inline std::wstring_view LeafOf(std::wstring_view canonical)
    {
        const std::size_t slash = canonical.rfind(L'\\');
        return slash == std::wstring_view::npos ? canonical : canonical.substr(slash + 1);
    }

    // FindFirstFile-style match for long names: '*' any run, '?' one character,
    // case-insensitive; "*.*" matches everything, like the Win32 API.
    inline bool WildcardMatch(std::wstring_view pattern, std::wstring_view name)
    {
        if (pattern == L"*.*") pattern = L"*";
        std::size_t p = 0;
        std::size_t n = 0;
        std::size_t star = std::wstring_view::npos;
        std::size_t resume = 0;
        while (n < name.size())
        {
            if (p < pattern.size() && (pattern[p] == L'?' || LowerAscii(pattern[p]) == LowerAscii(name[n])))
            {
                ++p;
                ++n;
            }
            else if (p < pattern.size() && pattern[p] == L'*')
            {
                star = p++;
                resume = n;
            }
            else if (star != std::wstring_view::npos)
            {
                p = star + 1;
                n = ++resume;
            }
            else
            {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == L'*') ++p;
        return p == pattern.size();
    }

    // _wfsopen / _fsopen mode strings: any of w, a, + creates or changes the file.
    inline bool FopenWrites(std::wstring_view mode)
    {
        return mode.find_first_of(L"wa+") != std::wstring_view::npos;
    }

    // CreateFile2: any write / delete / security access, or a disposition that
    // can create or truncate.
    inline bool CreateFileWrites(DWORD access, DWORD disposition)
    {
        constexpr DWORD WriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA
            | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
        return (access & WriteAccess) != 0 || disposition == CREATE_NEW || disposition == CREATE_ALWAYS
            || disposition == OPEN_ALWAYS || disposition == TRUNCATE_EXISTING;
    }

    enum class EntryKind
    {
        Override,        // the base file exists; the game reads the variant instead
        AddedFile,       // only in variants; shown in the base folder's listing
        AddedDirectory,  // only in variants; listings of it come from the variant folder
    };

    struct Entry
    {
        std::wstring baseKey;      // PathKey of the base path
        std::wstring variantPath;  // absolute variant path, original case
        std::wstring relative;     // below the mod root, original case
        std::size_t mod = 0;
        EntryKind kind = EntryKind::Override;
    };

    struct Mod
    {
        std::wstring name;
        std::wstring root;      // canonical, absolute, original case
        std::wstring rootKey;   // PathKey(root)
        std::size_t overrides = 0;
        std::size_t addedFiles = 0;
        std::size_t addedDirectories = 0;
        std::size_t entries() const { return overrides + addedFiles + addedDirectories; }
    };

    class Index
    {
    public:
        std::size_t AddMod(std::wstring_view name, std::wstring_view root)
        {
            Mod mod;
            mod.name = std::wstring(name);
            mod.root = CanonicalPath(root);
            mod.rootKey = LowerAscii(mod.root);
            m_mods.push_back(std::move(mod));
            return m_mods.size() - 1;
        }

        // `relative` is below <root>\extender\variants. Folders that exist on
        // both sides are accepted and add nothing (their contents are added one
        // by one). Returns false with a reason for a refused entry.
        bool AddEntry(std::size_t modIndex, std::wstring_view relative, bool variantIsDirectory,
            bool baseExists, bool baseIsDirectory, std::wstring& reason)
        {
            reason.clear();
            if (modIndex >= m_mods.size())
            {
                reason = L"unknown mod";
                return false;
            }
            Mod& mod = m_mods[modIndex];
            const std::wstring rel = CanonicalPath(relative);
            const std::wstring relKey = LowerAscii(rel);
            if (rel.empty() || rel[0] == L'\\' || (rel.size() >= 2 && rel[1] == L':')
                || relKey == L".." || relKey.compare(0, 3, L"..\\") == 0)
            {
                reason = L"not a path inside the mod";
                return false;
            }
            std::size_t depth = 1;
            for (wchar_t c : rel) if (c == L'\\') ++depth;
            if (depth > MaximumDepth)
            {
                reason = L"nested deeper than 8 folders";
                return false;
            }
            if (relKey == L"mod.json")
            {
                reason = L"mod.json cannot have an extender version";
                return false;
            }
            if (relKey == L"extender" || relKey.compare(0, 9, L"extender\\") == 0)
            {
                reason = L"extender\\ files are the extender's own; they have no base version";
                return false;
            }
            if (baseExists && variantIsDirectory != baseIsDirectory)
            {
                reason = variantIsDirectory ? L"a folder in variants, but a file in the mod"
                                            : L"a file in variants, but a folder in the mod";
                return false;
            }
            if (baseExists && variantIsDirectory)
            {
                return true;   // both sides have the folder: nothing to redirect
            }
            if (mod.entries() >= MaximumEntriesPerMod)
            {
                reason = L"more than 4096 variant entries in one mod";
                return false;
            }
            Entry entry;
            entry.relative = rel;
            entry.baseKey = mod.rootKey + L'\\' + relKey;
            entry.variantPath = mod.root + L'\\' + VariantsFolder + L'\\' + rel;
            entry.mod = modIndex;
            entry.kind = baseExists ? EntryKind::Override
                : (variantIsDirectory ? EntryKind::AddedDirectory : EntryKind::AddedFile);
            if (m_byKey.find(entry.baseKey) != m_byKey.end())
            {
                reason = L"listed twice";
                return false;
            }
            switch (entry.kind)
            {
            case EntryKind::Override: ++mod.overrides; break;
            case EntryKind::AddedFile: ++mod.addedFiles; break;
            case EntryKind::AddedDirectory: ++mod.addedDirectories; break;
            }
            m_byKey.emplace(entry.baseKey, m_entries.size());
            m_entries.push_back(std::move(entry));
            return true;
        }

        // Builds the per-folder addition lists; call once after the last AddEntry.
        void Finish()
        {
            m_additions.clear();
            for (std::size_t i = 0; i < m_entries.size(); ++i)
            {
                const Entry& entry = m_entries[i];
                if (entry.kind == EntryKind::Override) continue;
                const std::wstring parent(ParentOf(entry.baseKey));
                const Entry* parentEntry = Find(parent);
                if (parentEntry != nullptr && parentEntry->kind == EntryKind::AddedDirectory)
                {
                    continue;   // listings of an added folder come from the variant folder itself
                }
                m_additions[parent].push_back(i);
            }
        }

        const Entry* Find(std::wstring_view key) const
        {
            const auto found = m_byKey.find(std::wstring(key));
            return found == m_byKey.end() ? nullptr : &m_entries[found->second];
        }

        // Additions directly inside the base folder `directoryKey` (a PathKey).
        const std::vector<std::size_t>* AdditionsIn(std::wstring_view directoryKey) const
        {
            const auto found = m_additions.find(std::wstring(directoryKey));
            return found == m_additions.end() ? nullptr : &found->second;
        }

        const Entry& At(std::size_t i) const { return m_entries[i]; }
        std::size_t Size() const { return m_entries.size(); }
        const std::vector<Mod>& Mods() const { return m_mods; }

        // The mod with at least one entry whose root is `key` or contains it.
        const Mod* ModFor(std::wstring_view key) const
        {
            for (const Mod& mod : m_mods)
            {
                if (mod.entries() == 0) continue;
                if (key.size() >= mod.rootKey.size() && key.compare(0, mod.rootKey.size(), mod.rootKey) == 0
                    && (key.size() == mod.rootKey.size() || key[mod.rootKey.size()] == L'\\'))
                {
                    return &mod;
                }
            }
            return nullptr;
        }

    private:
        std::vector<Mod> m_mods;
        std::vector<Entry> m_entries;
        std::unordered_map<std::wstring, std::size_t> m_byKey;
        std::unordered_map<std::wstring, std::vector<std::size_t>> m_additions;
    };

    enum class Action
    {
        PassThrough,   // call the game's function unchanged
        Redirect,      // read: use `path` (the variant)
        Quarantine,    // write: use `path` (below <mod>\extender\editor_saves)
        Deny,          // write to the mod root folder itself: refuse
    };

    struct Decision
    {
        Action action = Action::PassThrough;
        std::wstring path;
        const Entry* entry = nullptr;   // reads: the variant the path maps to (also in log mode)
        const Mod* mod = nullptr;       // writes: the variant mod the path is in (also in log mode)
        std::wstring relative;          // writes: path below the mod root, original case
    };

    // One file-system call on `fullPath` (absolute, as GetFullPathNameW returns
    // it). Reads of a mapped path go to the variant; writes anywhere inside a
    // mod that has variants go to <mod>\extender\editor_saves\<same relative
    // path>, except mod.json at the mod root and the extender\ folder (never
    // variant data). With active == false nothing changes, but entry / mod are
    // filled so the caller can log what would have happened.
    inline Decision Decide(const Index& index, std::wstring_view fullPath, bool writes, bool active)
    {
        Decision decision;
        const std::wstring canonical = CanonicalPath(fullPath);
        const std::wstring key = LowerAscii(canonical);
        if (!writes)
        {
            decision.entry = index.Find(key);
            if (decision.entry != nullptr && active)
            {
                decision.action = Action::Redirect;
                decision.path = decision.entry->variantPath;
            }
            return decision;
        }
        const Mod* mod = index.ModFor(key);
        if (mod == nullptr)
        {
            return decision;
        }
        const std::wstring relativeKey = key.size() > mod->rootKey.size() ? key.substr(mod->rootKey.size() + 1) : L"";
        if (relativeKey == L"mod.json" || relativeKey == L"extender" || relativeKey.compare(0, 9, L"extender\\") == 0)
        {
            return decision;
        }
        decision.mod = mod;
        decision.relative = canonical.size() > mod->root.size() ? canonical.substr(mod->root.size() + 1) : L"";
        if (!active)
        {
            return decision;
        }
        if (decision.relative.empty())
        {
            decision.action = Action::Deny;
            return decision;
        }
        decision.action = Action::Quarantine;
        decision.path = mod->root + L'\\' + QuarantineFolder + L'\\' + decision.relative;
        return decision;
    }
}
