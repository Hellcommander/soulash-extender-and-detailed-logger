// Included inside the anonymous namespace of tests.cpp (uses Check/failures).

struct DecoderCase
{
    const char* name;
    std::vector<std::uint8_t> bytes;
    unsigned length;  // 0 means the decoder must reject the input
    soulash::x64::Flow flow = soulash::x64::Flow::Sequential;
    bool relative = false;
    bool ripRelative = false;
};

void TestX64Decoder()
{
    using soulash::x64::Flow;
    const std::vector<DecoderCase> cases = {
        {"push rbx", {0x53}, 1},
        {"sub rsp,28h", {0x48, 0x83, 0xEC, 0x28}, 4},
        {"mov [rsp+8],rbx", {0x48, 0x89, 0x5C, 0x24, 0x08}, 5},
        {"lea rax,[rip+10h]", {0x48, 0x8D, 0x05, 0x10, 0, 0, 0}, 7, Flow::Sequential, false, true},
        {"call rel32", {0xE8, 0, 0, 0, 0}, 5, Flow::Call, true},
        {"jmp rel8", {0xEB, 0x05}, 2, Flow::Jump, true},
        {"jz rel8", {0x74, 0x05}, 2, Flow::ConditionalJump, true},
        {"jz rel32", {0x0F, 0x84, 0x10, 0, 0, 0}, 6, Flow::ConditionalJump, true},
        {"loop rel8", {0xE2, 0xFE}, 2, Flow::Other, true},
        {"movabs rax,imm64", {0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}, 10},
        {"mov ax,imm16", {0x66, 0xB8, 0x34, 0x12}, 4},
        {"mov eax,imm32", {0xB8, 0x78, 0x56, 0x34, 0x12}, 5},
        {"test ecx,imm32", {0xF7, 0xC1, 1, 0, 0, 0}, 6},
        {"neg ecx (no imm)", {0xF7, 0xD9}, 2},
        {"test cl,imm8", {0xF6, 0xC1, 0x01}, 3},
        {"nop dword [rax+rax]", {0x0F, 0x1F, 0x44, 0x00, 0x00}, 5},
        {"66 nop word [rax+rax+0]", {0x66, 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0}, 9},
        {"endbr64", {0xF3, 0x0F, 0x1E, 0xFA}, 4},
        {"vmovaps xmm0,xmm1", {0xC5, 0xF8, 0x28, 0xC1}, 4},
        {"vbroadcastss ymm0,xmm1", {0xC4, 0xE2, 0x7D, 0x18, 0xC1}, 5},
        {"evex vmovaps zmm0,zmm1", {0x62, 0xF1, 0x7C, 0x48, 0x28, 0xC1}, 6},
        {"mov eax,[abs32] via SIB", {0x8B, 0x04, 0x25, 0x78, 0x56, 0x34, 0x12}, 7},
        {"jmp [rip+0]", {0xFF, 0x25, 0, 0, 0, 0}, 6, Flow::Jump, false, true},
        {"call [rax]", {0xFF, 0x10}, 2, Flow::Call},
        {"ret", {0xC3}, 1, Flow::Return},
        {"ret 8", {0xC2, 0x08, 0x00}, 3, Flow::Return},
        {"int3", {0xCC}, 1, Flow::Trap},
        {"mov rax,-1", {0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF}, 7},
        {"mov word [rax],imm16", {0x66, 0xC7, 0x00, 0x34, 0x12}, 5},
        {"sub rsp,100h", {0x48, 0x81, 0xEC, 0x00, 0x01, 0, 0}, 7},
        {"push imm8", {0x6A, 0x00}, 2},
        {"push imm32", {0x68, 0, 0, 0, 0}, 5},
        {"palignr xmm0,xmm1,8", {0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x08}, 6},
        {"mov eax,[moffs64]", {0xA1, 1, 2, 3, 4, 5, 6, 7, 8}, 9},
        {"enter", {0xC8, 0x10, 0x00, 0x00}, 4},
        {"syscall", {0x0F, 0x05}, 2},
        {"invalid in 64-bit: 06", {0x06}, 0},
        {"invalid in 64-bit: 60", {0x60}, 0},
        {"invalid 0F 04", {0x0F, 0x04}, 0},
        {"FF /7", {0xFF, 0xF8}, 0},
        {"truncated lea", {0x48, 0x8D, 0x05, 0x10}, 0},
        {"truncated call", {0xE8, 0x00}, 0},
        {"only prefixes", {0x66, 0x66}, 0},
    };

    for (const DecoderCase& test : cases)
    {
        soulash::x64::Decoded decoded;
        const bool ok = soulash::x64::Decode(test.bytes.data(), test.bytes.size(), decoded);
        if (test.length == 0)
        {
            Check(!ok, (std::string("decoder rejects: ") + test.name).c_str());
            continue;
        }
        Check(ok && decoded.length == test.length,
            (std::string("decoder length: ") + test.name).c_str());
        if (ok)
        {
            Check(decoded.flow == test.flow,
                (std::string("decoder flow: ") + test.name).c_str());
            Check(decoded.relative == test.relative,
                (std::string("decoder relative: ") + test.name).c_str());
            Check(decoded.ripRelative == test.ripRelative,
                (std::string("decoder rip-relative: ") + test.name).c_str());
        }
        // Extra trailing bytes must not change the result; fewer must not read past the end.
        std::vector<std::uint8_t> padded = test.bytes;
        padded.insert(padded.end(), 8, 0x90);
        soulash::x64::Decoded paddedDecoded;
        Check(soulash::x64::Decode(padded.data(), padded.size(), paddedDecoded)
                && paddedDecoded.length == test.length,
            (std::string("decoder ignores following bytes: ") + test.name).c_str());
        if (test.bytes.size() > 1)
        {
            soulash::x64::Decoded truncated;
            Check(!soulash::x64::Decode(test.bytes.data(), test.bytes.size() - 1, truncated),
                (std::string("decoder rejects truncated input: ") + test.name).c_str());
        }
    }
}

// Compares decoder lengths with dumpbin's disassembly of the same image over
// every executable section. Bare data bytes that dumpbin lists as one-byte
// lines, and the APX REX2 prefix (D5, which appears as jump-table data in
// compiled code), are not instructions this decoder targets.
int RunDecoderOracle(const wchar_t* imagePath, const wchar_t* disassemblyPath)
{
    std::ifstream imageFile(imagePath, std::ios::binary);
    std::vector<unsigned char> image(
        (std::istreambuf_iterator<char>(imageFile)), std::istreambuf_iterator<char>());
    Check(image.size() > 0x400, "oracle image is readable");
    if (image.size() <= 0x400)
    {
        return failures;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    const unsigned long long base = nt->OptionalHeader.ImageBase;

    std::ifstream listing(disassemblyPath);
    std::vector<unsigned long long> addresses;
    std::vector<std::string> texts;
    std::string line;
    while (std::getline(listing, line))
    {
        if (line.size() > 20 && line[0] == ' ' && line[18] == ':')
        {
            char* end = nullptr;
            const unsigned long long address = std::strtoull(line.c_str() + 2, &end, 16);
            if (end == line.c_str() + 18)
            {
                addresses.push_back(address);
                texts.push_back(line.substr(20));
            }
        }
    }
    Check(addresses.size() > 1000, "oracle disassembly has instructions");

    unsigned long long compared = 0;
    unsigned long long mismatches = 0;
    unsigned long long rejected = 0;
    for (std::size_t i = 0; i + 1 < addresses.size(); ++i)
    {
        const unsigned long long expected = addresses[i + 1] - addresses[i];
        if (expected == 0 || expected > 15)
        {
            continue;
        }
        const std::string& text = texts[i];
        const bool bareByte = text.size() >= 2 && std::isxdigit(static_cast<unsigned char>(text[0]))
            && std::isxdigit(static_cast<unsigned char>(text[1]))
            && (text.size() == 2 || text[2] == ' ');
        if (bareByte && expected == 1)
        {
            continue;
        }
        const unsigned long long rva = addresses[i] - base;
        std::size_t offset = 0;
        bool found = false;
        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections && !found; ++s)
        {
            if ((sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0
                && rva >= sections[s].VirtualAddress
                && rva < sections[s].VirtualAddress + sections[s].Misc.VirtualSize)
            {
                offset = sections[s].PointerToRawData + (rva - sections[s].VirtualAddress);
                found = true;
            }
        }
        if (!found || offset + 15 > image.size())
        {
            continue;
        }
        ++compared;
        soulash::x64::Decoded decoded;
        if (!soulash::x64::Decode(&image[offset], 15, decoded))
        {
            if (image[offset] != 0xD5)
            {
                ++rejected;
                if (rejected <= 5)
                {
                    std::cerr << "oracle: decoder rejected '" << text << "' at "
                        << std::hex << addresses[i] << std::dec << '\n';
                }
            }
            continue;
        }
        if (decoded.length != expected)
        {
            ++mismatches;
            if (mismatches <= 5)
            {
                std::cerr << "oracle: length " << decoded.length << " != " << expected
                    << " for '" << text << "' at " << std::hex << addresses[i] << std::dec << '\n';
            }
        }
    }
    Check(compared > 1000, "oracle compared a meaningful number of instructions");
    Check(mismatches == 0, "decoder lengths match dumpbin for every compared instruction");
    Check(rejected == 0, "decoder accepts every compared instruction");
    if (failures == 0)
    {
        std::cout << "PASS: x64 length decoder matches dumpbin on " << compared
            << " instructions\n";
    }
    return failures;
}
