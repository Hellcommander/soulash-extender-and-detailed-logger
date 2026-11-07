// Included inside the anonymous namespace of tests.cpp (uses Check/failures).
// Synthetic image for plugins/common/ability_bar_layout.hpp: the same byte
// patterns the game's DrawActionBar code uses (924e1ef3 / 60b72c10), laid out
// in a fake PE image so every check can be broken one at a time.

struct LayoutImage
{
    static constexpr std::size_t Size = 0x4000;
    static constexpr std::size_t Update = 0x1000;
    static constexpr std::size_t HandleInput = 0x1400;
    static constexpr std::size_t UiGetter = 0x2000;
    static constexpr std::size_t AbilityGetter = 0x2200;
    static constexpr std::size_t PlayerId = 0x3000;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(Size, 0xCC);

    std::size_t Put(std::size_t at, std::initializer_list<std::uint8_t> code)
    {
        std::copy(code.begin(), code.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        return at + code.size();
    }

    // rel32 at `at`, the instruction ending at `at + 4`.
    std::size_t Rel(std::size_t at, std::size_t target)
    {
        const auto value = static_cast<std::int32_t>(
            static_cast<std::int64_t>(target) - static_cast<std::int64_t>(at + 4));
        std::memcpy(bytes.data() + at, &value, sizeof(value));
        return at + 4;
    }

    void Getter(std::size_t at, std::uint32_t stride)
    {
        at = Put(at, {0x49, 0xC1, 0xE8, 0x0D, 0x48, 0x8B, 0x53, 0x08, 0x48, 0x8B, 0x43, 0x10,
            0x81, 0xE1, 0xFF, 0x1F, 0x00, 0x00, 0x48, 0x8B, 0x53, 0x38, 0x48, 0x8B, 0x4B, 0x40,
            0x48, 0x69, 0xC0});
        std::memcpy(bytes.data() + at, &stride, sizeof(stride));
        Put(at + 4, {0xC3});
    }

    LayoutImage()
    {
        Put(0, {'M', 'Z'});
        const std::int32_t peOffset = 0x80;
        std::memcpy(bytes.data() + 0x3C, &peOffset, sizeof(peOffset));
        Put(0x80, {'P', 'E', 0, 0});
        const std::uint16_t magic = 0x20B;
        std::memcpy(bytes.data() + 0x80 + 24, &magic, sizeof(magic));
        const std::uint32_t sizeOfImage = static_cast<std::uint32_t>(Size);
        std::memcpy(bytes.data() + 0x80 + 24 + 56, &sizeOfImage, sizeof(sizeOfImage));

        std::size_t at = Update;
        at = Rel(Put(at, {0x8B, 0x15}), PlayerId);
        at = Rel(Put(at, {0x48, 0x8B, 0x88, 0x00, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x89, 0xA0, 0x01, 0x00, 0x00, 0xE8}), UiGetter);
        at = Put(at, {0x83, 0x78, 0x60, 0x29});
        at = Rel(Put(at, {0x8B, 0x15}), PlayerId);
        at = Rel(Put(at, {0x48, 0x8B, 0x89, 0x00, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x08, 0xE8}), AbilityGetter);
        Put(at, {0xF3, 0x0F, 0x58, 0xB0, 0xB0, 0x00, 0x00, 0x00, 0xC3});

        at = HandleInput;
        at = Put(at, {0x0F, 0x2F, 0x57, 0x20, 0xF3, 0x0F, 0x10, 0x47, 0x28, 0xF3, 0x0F, 0x10, 0x47, 0x14, 0x0F, 0x2F, 0x4F, 0x24});
        at = Put(at, {0x48, 0xBA, 0x00, 0x04, 0x06, 0x00, 0x32, 0x01, 0x00, 0x00});
        at = Rel(Put(at, {0x8B, 0x15}), PlayerId);
        at = Rel(Put(at, {0x48, 0x8B, 0x49, 0x08, 0xE8}), AbilityGetter);
        Put(at, {0x33, 0xC9, 0x39, 0x48, 0x68, 0x0F, 0x94, 0xC1, 0x89, 0x48, 0x68, 0xC3});

        Getter(UiGetter, 0x130);
        Getter(AbilityGetter, 0x260);
    }

    soulash_plugins::AbilityBarLayout Verify() const
    {
        const unsigned char* image = bytes.data();
        return soulash_plugins::VerifyAbilityBarLayout(image,
            soulash_plugins::MappedImageSize(image), image + Update, image + HandleInput);
    }

    // Overwrites the first occurrence of `from` (searched from `start`) with `to`.
    bool Patch(std::size_t start, std::initializer_list<std::uint8_t> from, std::initializer_list<std::uint8_t> to)
    {
        const auto it = std::search(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end(), from.begin(), from.end());
        if (it == bytes.end())
        {
            return false;
        }
        std::copy(to.begin(), to.end(), it);
        return true;
    }
};

void TestAbilityBarLayout()
{
    {
        const LayoutImage image;
        Check(soulash_plugins::MappedImageSize(image.bytes.data()) == LayoutImage::Size, "layout: SizeOfImage read from the PE header");
        const auto layout = image.Verify();
        Check(layout.verified, "layout: synthetic DrawActionBar code verifies");
        Check(layout.playerEntityIdRva == LayoutImage::PlayerId, "layout: player-id global taken from the mov edx,[rip] loads");
        Check(layout.playerEntityId == reinterpret_cast<const std::uint32_t*>(image.bytes.data() + LayoutImage::PlayerId),
            "layout: player-id pointer is inside the image");
        Check(layout.barRectVerified, "layout: bar rectangle offsets verified");
    }

    struct Breakage
    {
        const char* name;
        std::size_t start;
        std::initializer_list<std::uint8_t> from;
        std::initializer_list<std::uint8_t> to;
        bool stillVerified;   // only the optional bar-rect check may fail and still verify
    };
    const Breakage breakages[] = {
        {"registry offset +0x1A0 changed", LayoutImage::Update, {0xA0, 0x01, 0x00, 0x00, 0xE8}, {0xA8, 0x01, 0x00, 0x00, 0xE8}, false},
        {"UI mode offset changed", LayoutImage::Update, {0x83, 0x78, 0x60, 0x29}, {0x83, 0x78, 0x64, 0x29}, false},
        {"AbilityUser storage offset changed", LayoutImage::Update, {0x48, 0x8B, 0x49, 0x08, 0xE8}, {0x48, 0x8B, 0x49, 0x10, 0xE8}, false},
        {"timer offset changed", LayoutImage::Update, {0xB0, 0xB0, 0x00, 0x00, 0x00}, {0xB0, 0xB8, 0x00, 0x00, 0x00}, false},
        {"AbilityUser stride changed", LayoutImage::AbilityGetter, {0x60, 0x02, 0x00, 0x00}, {0x70, 0x02, 0x00, 0x00}, false},
        {"UI-state stride changed", LayoutImage::UiGetter, {0x30, 0x01, 0x00, 0x00}, {0x38, 0x01, 0x00, 0x00}, false},
        {"page mask changed", LayoutImage::AbilityGetter, {0xFF, 0x1F, 0x00, 0x00}, {0xFF, 0x0F, 0x00, 0x00}, false},
        {"blocked-mode mask changed", LayoutImage::HandleInput, {0x32, 0x01, 0x00, 0x00}, {0x33, 0x01, 0x00, 0x00}, false},
        {"current-set offset changed", LayoutImage::HandleInput, {0x39, 0x48, 0x68}, {0x39, 0x48, 0x6C}, false},
        {"bar rectangle changed", LayoutImage::HandleInput, {0x0F, 0x2F, 0x4F, 0x24}, {0x0F, 0x2F, 0x4F, 0x2C}, true},
    };
    for (const Breakage& breakage : breakages)
    {
        LayoutImage image;
        const bool patched = image.Patch(breakage.start, breakage.from, breakage.to);
        Check(patched, (std::string("layout: fixture patch applies: ") + breakage.name).c_str());
        const auto layout = image.Verify();
        Check(layout.verified == breakage.stillVerified,
            (std::string("layout: ") + breakage.name + (breakage.stillVerified ? " still verifies" : " is refused")).c_str());
        if (breakage.stillVerified)
        {
            Check(!layout.barRectVerified, "layout: broken bar rectangle is reported");
        }
        else
        {
            Check(layout.failure != nullptr && layout.failure[0] != '\0' && layout.playerEntityId == nullptr,
                (std::string("layout: refusal names a reason: ") + breakage.name).c_str());
        }
    }

    {
        // Two player-id loads that disagree.
        LayoutImage image;
        image.Rel(LayoutImage::Update + 2, LayoutImage::PlayerId + 8);
        Check(!image.Verify().verified, "layout: disagreeing player-id loads are refused");
    }
    {
        // The toggle reading a different global than update.
        LayoutImage image;
        const std::uint8_t load[] = {0x8B, 0x15};
        const auto it = std::search(image.bytes.begin() + LayoutImage::HandleInput, image.bytes.end(),
            std::begin(load), std::end(load));
        image.Rel(static_cast<std::size_t>(it - image.bytes.begin()) + 2, LayoutImage::PlayerId + 4);
        Check(!image.Verify().verified, "layout: toggle on another global is refused");
    }
    {
        // A getter call that points outside the image must not be followed.
        LayoutImage image;
        image.Rel(LayoutImage::Update + 6 + 15, LayoutImage::Size + 0x100000);
        Check(!image.Verify().verified, "layout: getter outside the image is refused");
    }
    {
        Check(!soulash_plugins::VerifyAbilityBarLayout(nullptr, 0, nullptr, nullptr).verified, "layout: null image is refused");
        std::vector<std::uint8_t> junk(0x100, 0);
        Check(soulash_plugins::MappedImageSize(junk.data()) == 0, "layout: non-PE buffer has no image size");
    }

    // MSVC std::string reader: SSO, heap, and rejects.
    {
        unsigned char sso[32]{};
        std::memcpy(sso, "core_2_fireball", 15);
        const std::uint64_t ssoSize = 15, ssoCapacity = 15;
        std::memcpy(sso + 0x10, &ssoSize, 8);
        std::memcpy(sso + 0x18, &ssoCapacity, 8);
        char out[64];
        Check(soulash_plugins::ReadMsvcString(sso, out, sizeof(out)) && std::string(out) == "core_2_fireball",
            "msvc string: 15-char SSO string read");
        const char heap[] = "arendeth_runic_magic_rune_of_binding";
        unsigned char large[32]{};
        const char* heapPointer = heap;
        std::memcpy(large, &heapPointer, sizeof(heapPointer));
        const std::uint64_t heapSize = sizeof(heap) - 1, heapCapacity = 47;
        std::memcpy(large + 0x10, &heapSize, 8);
        std::memcpy(large + 0x18, &heapCapacity, 8);
        Check(soulash_plugins::ReadMsvcString(large, out, sizeof(out)) && std::string(out) == heap,
            "msvc string: heap string read");
        Check(!soulash_plugins::ReadMsvcString(large, out, 8), "msvc string: too long for the buffer is refused");
        unsigned char zeroed[32]{};
        Check(!soulash_plugins::ReadMsvcString(zeroed, out, sizeof(out)), "msvc string: unconstructed (zeroed) string is refused");
        const std::uint64_t badSize = 40;
        std::memcpy(large + 0x10, &badSize, 8);
        const std::uint64_t badCapacity = 20;
        std::memcpy(large + 0x18, &badCapacity, 8);
        Check(!soulash_plugins::ReadMsvcString(large, out, sizeof(out)), "msvc string: size above capacity is refused");
    }

    // Save-name locator.
    {
        LayoutImage image;
        constexpr std::size_t Saver = 0x1800, Getter = 0x1900, Object = 0x3100;
        std::size_t at = image.Rel(image.Put(Saver, {0xE8}), Getter);
        at = image.Put(at, {0x48, 0x8D, 0x90, 0x08, 0x0C, 0x00, 0x00, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8});
        image.Rel(at, Getter);
        at = image.Put(Getter, {0x33, 0xD2, 0x41, 0xB8, 0x30, 0x0C, 0x00, 0x00, 0x48, 0x8D, 0x1D});
        image.Put(image.Rel(at, Object), {0xC3});
        const unsigned char* base = image.bytes.data();
        auto location = soulash_plugins::LocateSaveName(base, LayoutImage::Size, base + Saver);
        Check(location.found && location.singletonRva == Object && location.nameOffset == 0xC08
                && location.nameString == base + Object + 0xC08,
            "save name: singleton and name offset derived from the save writer");
        image.Put(Getter + 4, {0x00, 0x0C, 0x00, 0x00});   // object smaller than the name offset
        location = soulash_plugins::LocateSaveName(base, LayoutImage::Size, base + Saver);
        Check(!location.found, "save name: offset outside the singleton is refused");
        Check(!soulash_plugins::LocateSaveName(base, LayoutImage::Size, base + LayoutImage::Update).found,
            "save name: a function without the access pattern is refused");
    }
    if (failures == 0)
    {
        std::cout << "PASS: ability-bar layout verifier (synthetic code, " << std::size(breakages) + 3
                  << " breakages), MSVC string reader, save-name locator\n";
    }
}

// Manual oracle: run the verifier against a real game executable mapped as an
// image (no code runs). RVAs of DrawActionBar::update / handleInput come from
// `python -I tools/pe_rtti.py <exe> DrawActionBar` (vtable slots 1 and 4); the
// save writer from `python -I tools/pe_xrefs.py <exe> str "Saving player"`.
int RunAbilityBarLayoutOracle(const wchar_t* exePath, const wchar_t* updateRva, const wchar_t* handleInputRva,
    const wchar_t* saverRva)
{
    HMODULE mapped = LoadLibraryExW(exePath, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (mapped == nullptr)
    {
        std::cout << "FAIL: cannot map " << GetLastError() << "\n";
        return 1;
    }
    const auto* base = reinterpret_cast<const unsigned char*>(reinterpret_cast<std::uintptr_t>(mapped) & ~std::uintptr_t{3});
    const std::size_t size = soulash_plugins::MappedImageSize(base);
    const auto update = static_cast<std::size_t>(std::wcstoull(updateRva, nullptr, 16));
    const auto handleInput = static_cast<std::size_t>(std::wcstoull(handleInputRva, nullptr, 16));
    const auto layout = soulash_plugins::VerifyAbilityBarLayout(base, size, base + update, base + handleInput);
    std::cout << "image size 0x" << std::hex << size << ", verified=" << layout.verified
              << ", player id rva 0x" << layout.playerEntityIdRva << std::dec
              << ", bar rect=" << layout.barRectVerified << ", failure='" << layout.failure << "'\n";
    const auto saver = static_cast<std::size_t>(std::wcstoull(saverRva, nullptr, 16));
    const auto save = soulash_plugins::LocateSaveName(base, size, base + saver);
    std::cout << "save name found=" << save.found << ", singleton rva 0x" << std::hex << save.singletonRva
              << " + 0x" << save.nameOffset << std::dec << ", failure='" << save.failure << "'\n";
    FreeLibrary(mapped);
    return layout.verified && save.found ? 0 : 1;
}
