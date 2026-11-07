// Included inside the anonymous namespace of tests.cpp (uses Check/failures).
// Tests for the Variants plugin's pure parts (plugins/common/variant_index.hpp):
// path canonicalisation, wildcard listing filters, write classification, the
// variant index and the read / write redirect decision.

void TestVariantPaths()
{
    namespace vx = soulash_plugins::variants;
    Check(vx::CanonicalPath(L"D:/Games/Soulash 2/data/mods/ModA/abilities\\Fire.json")
            == L"D:\\Games\\Soulash 2\\data\\mods\\ModA\\abilities\\Fire.json",
        "variants: forward slashes become backslashes, case is kept");
    Check(vx::CanonicalPath(L"D:\\a\\\\b\\.\\c\\..\\d\\") == L"D:\\a\\b\\d",
        "variants: repeated separators, '.', '..' and a trailing separator are resolved");
    Check(vx::CanonicalPath(L"\\\\?\\D:\\a\\b") == L"D:\\a\\b", "variants: a \\\\?\\ prefix before a drive is dropped");
    Check(vx::CanonicalPath(L"D:\\") == L"D:\\", "variants: a drive root keeps its separator");
    Check(vx::CanonicalPath(L"D:\\..\\..\\a") == L"D:\\a", "variants: '..' cannot climb above an absolute root");
    Check(vx::CanonicalPath(L"\\\\server\\share\\x") == L"\\\\server\\share\\x", "variants: UNC prefix kept");
    Check(vx::CanonicalPath(L"a/../../b") == L"..\\b", "variants: relative '..' is kept when it climbs out");
    Check(vx::PathKey(L"D:/Games/MODS/X.JSON") == L"d:\\games\\mods\\x.json", "variants: keys are lower-case");
    Check(vx::ParentOf(L"d:\\a\\b.json") == L"d:\\a" && vx::LeafOf(L"d:\\a\\b.json") == L"b.json",
        "variants: parent and leaf");

    Check(vx::WildcardMatch(L"*", L"Fire.json") && vx::WildcardMatch(L"*.*", L"noext"),
        "variants: '*' and '*.*' match everything");
    Check(vx::WildcardMatch(L"*.JSON", L"fire.json") && !vx::WildcardMatch(L"*.json", L"fire.png"),
        "variants: extension patterns, case-insensitive");
    Check(vx::WildcardMatch(L"f?re*", L"Fire_ext.json") && !vx::WildcardMatch(L"f?re", L"Fire_ext"),
        "variants: '?' is one character, the whole name must match");
    Check(vx::WildcardMatch(L"*a*b*c", L"xxaYYbZZc") && !vx::WildcardMatch(L"*a*b*c", L"xxaYYcZZb"),
        "variants: several stars backtrack correctly");

    Check(!vx::FopenWrites(L"rb") && !vx::FopenWrites(L"r, ccs=UTF-8") && !vx::FopenWrites(L"rt"),
        "variants: read modes do not write");
    Check(vx::FopenWrites(L"wb") && vx::FopenWrites(L"a") && vx::FopenWrites(L"r+b"),
        "variants: w, a and + write");
    Check(!vx::CreateFileWrites(GENERIC_READ, OPEN_EXISTING) && !vx::CreateFileWrites(FILE_READ_ATTRIBUTES, OPEN_EXISTING)
            && !vx::CreateFileWrites(0, OPEN_EXISTING),
        "variants: read / attribute / existence opens do not write");
    Check(vx::CreateFileWrites(GENERIC_WRITE, CREATE_ALWAYS) && vx::CreateFileWrites(DELETE | FILE_READ_ATTRIBUTES, OPEN_EXISTING)
            && vx::CreateFileWrites(GENERIC_READ, OPEN_ALWAYS) && vx::CreateFileWrites(FILE_APPEND_DATA, OPEN_EXISTING),
        "variants: write, delete and creating dispositions write");
}

void TestVariantIndex()
{
    namespace vx = soulash_plugins::variants;
    vx::Index index;
    const std::size_t a = index.AddMod(L"ModA", L"D:/Game/data/mods/ModA/");
    const std::size_t b = index.AddMod(L"ModB", L"D:\\Game\\data\\mods\\ModB");
    std::wstring reason;
    Check(index.Mods()[a].root == L"D:\\Game\\data\\mods\\ModA", "variants: mod root canonicalised");
    Check(index.AddEntry(a, L"abilities", true, true, true, reason) && index.Size() == 0,
        "variants: a folder on both sides adds nothing");
    Check(index.AddEntry(a, L"abilities\\Fireball.json", false, true, false, reason), "variants: override accepted");
    Check(index.AddEntry(a, L"abilities/Fireball_Chain.json", false, false, false, reason), "variants: added file accepted");
    Check(index.AddEntry(a, L"skills.json", false, true, false, reason), "variants: root file override accepted");
    Check(index.AddEntry(a, L"milestones", true, false, false, reason), "variants: added folder accepted");
    Check(index.AddEntry(a, L"milestones\\Deep.json", false, false, false, reason), "variants: file in added folder accepted");
    Check(!index.AddEntry(a, L"mod.json", false, true, false, reason) && !reason.empty(), "variants: mod.json refused");
    Check(!index.AddEntry(a, L"extender\\effects.json", false, true, false, reason), "variants: extender\\ refused");
    Check(!index.AddEntry(a, L"..\\ModB\\skills.json", false, true, false, reason), "variants: escaping paths refused");
    Check(!index.AddEntry(a, L"passives.json", false, true, true, reason), "variants: file over a base folder refused");
    Check(!index.AddEntry(a, L"entities", true, true, false, reason), "variants: folder over a base file refused");
    Check(!index.AddEntry(a, L"ABILITIES\\fireball.json", false, true, false, reason), "variants: duplicate (any case) refused");
    Check(!index.AddEntry(a, L"1\\2\\3\\4\\5\\6\\7\\8\\9.json", false, false, false, reason), "variants: depth limit");
    index.Finish();

    const vx::Mod& modA = index.Mods()[a];
    Check(modA.overrides == 2 && modA.addedFiles == 2 && modA.addedDirectories == 1 && index.Size() == 5,
        "variants: entry counts per kind");
    Check(index.Mods()[b].entries() == 0, "variants: a mod without entries stays empty");

    const vx::Entry* fireball = index.Find(vx::PathKey(L"d:\\game\\DATA\\mods\\moda\\abilities\\FIREBALL.json"));
    Check(fireball != nullptr && fireball->kind == vx::EntryKind::Override
            && fireball->variantPath == L"D:\\Game\\data\\mods\\ModA\\extender\\variants\\abilities\\Fireball.json",
        "variants: override found case-insensitively, variant path keeps the case on disk");
    const std::vector<std::size_t>* abilities = index.AdditionsIn(vx::PathKey(L"D:\\Game\\data\\mods\\ModA\\abilities"));
    Check(abilities != nullptr && abilities->size() == 1
            && index.At(abilities->front()).relative == L"abilities\\Fireball_Chain.json",
        "variants: added file listed in its base folder");
    const std::vector<std::size_t>* root = index.AdditionsIn(vx::PathKey(L"D:\\Game\\data\\mods\\ModA"));
    Check(root != nullptr && root->size() == 1 && index.At(root->front()).kind == vx::EntryKind::AddedDirectory,
        "variants: added folder listed in the mod root");
    Check(index.AdditionsIn(vx::PathKey(L"D:\\Game\\data\\mods\\ModA\\milestones")) == nullptr,
        "variants: an added folder is listed from the variant folder itself, not by injection");

    // Reads.
    vx::Decision read = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\abilities\\Fireball.json", false, true);
    Check(read.action == vx::Action::Redirect && read.path == fireball->variantPath, "variants: active read redirected");
    read = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\abilities\\Fireball.json", false, false);
    Check(read.action == vx::Action::PassThrough && read.entry == fireball, "variants: log-mode read reports the entry only");
    read = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\milestones\\Deep.json", false, true);
    Check(read.action == vx::Action::Redirect
            && read.path == L"D:\\Game\\data\\mods\\ModA\\extender\\variants\\milestones\\Deep.json",
        "variants: file inside an added folder redirected");
    read = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\abilities\\Other.json", false, true);
    Check(read.action == vx::Action::PassThrough && read.entry == nullptr, "variants: unmapped read untouched");

    // Writes.
    vx::Decision write = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\abilities\\moda_Fireball.json", true, true);
    Check(write.action == vx::Action::Quarantine
            && write.path == L"D:\\Game\\data\\mods\\ModA\\extender\\editor_saves\\abilities\\moda_Fireball.json",
        "variants: editor write inside a variant mod goes to editor_saves");
    write = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\skills.json", true, true);
    Check(write.action == vx::Action::Quarantine, "variants: overwrite of an overridden base file is quarantined");
    write = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\mod.json", true, true);
    Check(write.action == vx::Action::PassThrough && write.mod == nullptr, "variants: mod.json writes allowed");
    write = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\extender\\editor_saves\\x.json", true, true);
    Check(write.action == vx::Action::PassThrough, "variants: writes into extender\\ allowed");
    write = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA", true, true);
    Check(write.action == vx::Action::Deny, "variants: changing the mod folder itself refused");
    write = vx::Decide(index, L"D:\\Game\\data\\mods\\ModB\\skills.json", true, true);
    Check(write.action == vx::Action::PassThrough && write.mod == nullptr, "variants: mods without variants untouched");
    write = vx::Decide(index, L"D:\\Game\\data\\mods\\ModAX\\skills.json", true, true);
    Check(write.action == vx::Action::PassThrough && write.mod == nullptr, "variants: a mod name prefix is not the mod");
    write = vx::Decide(index, L"D:\\Game\\data\\mods\\ModA\\abilities\\x.json", true, false);
    Check(write.action == vx::Action::PassThrough && write.mod != nullptr && write.relative == L"abilities\\x.json",
        "variants: log-mode write passes through but names the mod");
}
