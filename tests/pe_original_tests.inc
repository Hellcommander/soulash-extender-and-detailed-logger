// Included by tests.cpp inside its anonymous namespace (uses Check()).
// plugins/common/pe_original.hpp: the unpatched value of a vtable slot, read
// from the executable file. Reproduces the live failure (2026-11-05): a slot
// already hooked by an earlier plugin points outside the image.

    void TestOriginalImagePointer()
    {
        // Pure header parsing on a synthetic PE32+ header block.
        {
            std::vector<unsigned char> headers(0x400, 0);
            headers[0] = 'M';
            headers[1] = 'Z';
            const std::uint32_t pe = 0x80;
            std::memcpy(headers.data() + 0x3C, &pe, 4);
            std::memcpy(headers.data() + pe, "PE\0\0", 4);
            const std::uint16_t sections = 2, optionalSize = 0xF0, magic = 0x20B;
            std::memcpy(headers.data() + pe + 6, &sections, 2);
            std::memcpy(headers.data() + pe + 20, &optionalSize, 2);
            std::memcpy(headers.data() + pe + 24, &magic, 2);
            const std::uint64_t imageBase = 0x140000000ull;
            std::memcpy(headers.data() + pe + 24 + 24, &imageBase, 8);
            const std::size_t table = pe + 24 + optionalSize;
            const std::uint32_t text[] = {0x1000, 0x5000, 0x400};   // VA, raw size, raw pointer
            const std::uint32_t rdata[] = {0x8000, 0x2000, 0x5400};
            std::memcpy(headers.data() + table + 12, text, 12);
            std::memcpy(headers.data() + table + 40 + 12, rdata, 12);
            std::uint64_t offset = 0, base = 0;
            Check(soulash_plugins::PeRvaToFileOffset(headers.data(), headers.size(), 0x8D70, offset, base)
                    && offset == 0x5400 + 0xD70 && base == imageBase,
                "pe: RVA in .rdata maps to its file offset; preferred base read");
            Check(!soulash_plugins::PeRvaToFileOffset(headers.data(), headers.size(), 0x9FFC, offset, base),
                "pe: an 8-byte read crossing the end of raw data is refused");
            Check(!soulash_plugins::PeRvaToFileOffset(headers.data(), headers.size(), 0x7000, offset, base),
                "pe: an RVA in no section is refused");
            headers[pe + 25] = 0x01;   // magic 0x010B: PE32
            Check(!soulash_plugins::PeRvaToFileOffset(headers.data(), headers.size(), 0x8D70, offset, base),
                "pe: only PE32+ is accepted");
        }

        // Real: this test executable's own vtable, hooked in memory.
        SyntheticPolymorphicType* object = new SyntheticPolymorphicType();
        void** vtable = *reinterpret_cast<void***>(object);
        const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        const std::size_t imageSize = soulash_plugins::MappedImageSize(image);
        void* const original = vtable[1];
        void* const hook = reinterpret_cast<void*>(&TestOriginalImagePointer);   // anything outside .rdata
        static int outsideImage = 0;
        void* const foreign = &outsideImage;
        Check(soulash_plugins::OriginalCodePointer(image, imageSize, vtable + 1) == original,
            "pe: an unhooked slot returns its current value");
        DWORD oldProtect = 0;
        if (VirtualProtect(vtable + 1, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            // A pointer into the image that is not the original is returned as is (still in the image).
            vtable[1] = hook;
            Check(soulash_plugins::OriginalCodePointer(image, imageSize, vtable + 1) == hook,
                "pe: a slot pointing into the image is taken as current");
            // A heap pointer stands in for another plugin's DLL.
            int* heap = new int(0);
            vtable[1] = heap;
            const unsigned char* recovered = nullptr;
            Check(soulash_plugins::ReadOriginalImagePointer(image, imageSize, vtable + 1, recovered)
                    && recovered == original,
                "pe: the original slot value is recovered from the executable file");
            Check(soulash_plugins::OriginalCodePointer(image, imageSize, vtable + 1) == original,
                "pe: a hooked slot (pointing outside the image) yields the game's own function");
            delete heap;
            vtable[1] = original;
            VirtualProtect(vtable + 1, sizeof(void*), oldProtect, &oldProtect);
        }
        else
        {
            Check(false, "pe: cannot make the test vtable writable");
        }
        const unsigned char* none = nullptr;
        Check(!soulash_plugins::ReadOriginalImagePointer(image, imageSize, reinterpret_cast<void* const*>(&foreign), none),
            "pe: a slot outside the image is refused");
        delete object;
    }
