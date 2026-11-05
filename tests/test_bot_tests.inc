// Included inside the anonymous namespace of tests.cpp (uses Check/failures).
// TestBot pure parts: plugins/common/test_bot_script.hpp.

void TestTestBotScript()
{
    namespace tb = soulash_plugins::testbot;

    // Key names -> SDL keycodes.
    {
        std::uint32_t key = 0;
        Check(tb::ParseKeyName("a", key) && key == 'a', "testbot: letter key");
        Check(tb::ParseKeyName("Z", key) && key == 'z', "testbot: key names are case-insensitive");
        Check(tb::ParseKeyName("7", key) && key == '7', "testbot: digit key");
        Check(tb::ParseKeyName("f1", key) && key == 0x4000003Au, "testbot: F1 = SDLK_F1");
        Check(tb::ParseKeyName("F12", key) && key == 0x40000045u, "testbot: F12 = SDLK_F12");
        Check(tb::ParseKeyName("lctrl", key) && key == 0x400000E0u, "testbot: lctrl = SDLK_LCTRL (the game's bar switch key)");
        Check(tb::ParseKeyName("up", key) && key == 0x40000052u, "testbot: arrow up");
        Check(tb::ParseKeyName("escape", key) && key == 27 && tb::ParseKeyName("esc", key) && key == 27, "testbot: escape aliases");
        Check(tb::ParseKeyName("1073742048", key) && key == 0x400000E0u, "testbot: decimal keycode (keymap.json ase_key)");
        Check(tb::ParseKeyName("0x400000E1", key) && key == 0x400000E1u, "testbot: hex keycode");
        Check(!tb::ParseKeyName("f13", key) && !tb::ParseKeyName("ctrlx", key) && !tb::ParseKeyName("", key)
                && !tb::ParseKeyName("-5", key) && !tb::ParseKeyName("0x", key),
            "testbot: unknown keys are refused");
    }

    // Modifier masks.
    Check(tb::ParseModMask("none") == 0 && tb::ParseModMask("ctrl") == 0x40 && tb::ParseModMask("Shift") == 0x1
            && tb::ParseModMask("alt") == 0x100 && tb::ParseModMask("ctrl+shift") == 0x41,
        "testbot: modifier masks");
    Check(tb::ParseModMask("ctrl+") == -1 && tb::ParseModMask("meta") == -1 && tb::ParseModMask("") == -1,
        "testbot: bad modifier masks are refused");

    // A full script.
    {
        const char* script =
            "# comment line\r\n"
            "\r\n"
            "log  start of run  # trailing comment\r\n"
            "wait_world\n"
            "WAIT 250\n"
            "key lctrl 30\n"
            "key_down a\n"
            "key_up a\n"
            "mod ctrl+alt\n"
            "wheel -3\n"
            "click 100 200 right\n"
            "click 5 6\n"
            "expect_bar_set 1\n"
            "wait_world 5000";
        const tb::ParseResult parsed = tb::ParseScript(script);
        Check(parsed.ok && parsed.steps.size() == 12, "testbot: full example script parses into 12 steps");
        if (parsed.ok && parsed.steps.size() == 12)
        {
            const auto& s = parsed.steps;
            Check(s[0].op == tb::Op::Log && s[0].text == "start of run" && s[0].line == 3, "testbot: log text trimmed, comment removed, line number kept");
            Check(s[1].op == tb::Op::WaitWorld && s[1].a == 120000, "testbot: wait_world default timeout");
            Check(s[2].op == tb::Op::Wait && s[2].a == 250, "testbot: commands are case-insensitive");
            Check(s[3].op == tb::Op::Key && s[3].key == 0x400000E0u && s[3].b == 30, "testbot: key with hold");
            Check(s[4].op == tb::Op::KeyDown && s[5].op == tb::Op::KeyUp && s[4].key == 'a', "testbot: key_down/key_up");
            Check(s[6].op == tb::Op::Mod && s[6].a == 0x140, "testbot: mod ctrl+alt");
            Check(s[7].op == tb::Op::Wheel && s[7].a == -3, "testbot: wheel down");
            Check(s[8].op == tb::Op::Click && s[8].a == 100 && s[8].b == 200 && s[8].c == 3, "testbot: right click");
            Check(s[9].c == 1, "testbot: click defaults to the left button");
            Check(s[10].op == tb::Op::ExpectBarSet && s[10].a == 1, "testbot: expect_bar_set");
            Check(s[11].a == 5000 && s[11].line == 14, "testbot: last line without newline is parsed");
        }
    }

    // Errors name the line and leave no steps behind.
    struct Bad
    {
        const char* script;
        int line;
    };
    const Bad bad[] = {
        {"wait 10\njump 3\n", 2},
        {"wait\n", 1},
        {"wait -1\n", 1},
        {"wait 600001\n", 1},
        {"wait 10 20\n", 1},
        {"key\n", 1},
        {"key nosuchkey\n", 1},
        {"key_down a 30\n", 1},
        {"key a 60001\n", 1},
        {"mod hyper\n", 1},
        {"wheel 0\n", 1},
        {"wheel 101\n", 1},
        {"click 1\n", 1},
        {"click 1 2 side\n", 1},
        {"expect_bar_set x\n", 1},
        {"log\n", 1},
        {"log   # only a comment\n", 1},
        {"log caf\xC3\xA9\n", 1},
        {"wait_world 0\n", 1},
    };
    for (const Bad& entry : bad)
    {
        const tb::ParseResult parsed = tb::ParseScript(entry.script);
        Check(!parsed.ok && parsed.errorLine == entry.line && !parsed.error.empty() && parsed.steps.empty(),
            (std::string("testbot: refused with its line: ") + entry.script).c_str());
    }
    Check(tb::ParseScript(std::string(300, 'w')).errorLine == 1, "testbot: over-long line refused");
    Check(!tb::ParseScript(std::string(tb::MaxScriptBytes + 1, '\n')).ok, "testbot: over-size script refused");
    {
        std::string many;
        for (int i = 0; i < 2001; ++i)
        {
            many += "wait 1\n";
        }
        Check(!tb::ParseScript(many).ok, "testbot: more than 2000 steps refused");
    }
    Check(tb::ParseScript("# nothing\n\n").ok && tb::ParseScript("# nothing\n\n").steps.empty(),
        "testbot: comment-only script parses to no steps");

    // steam_api64.dll flat wrapper decoder (bytes from the shipped DLL).
    {
        const unsigned char setAchievement[] = {0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x30, 0xCC};
        const unsigned char resetAll[] = {0x48, 0x8B, 0x01, 0x48, 0xFF, 0xA0, 0xA0, 0x00, 0x00, 0x00};
        const unsigned char slotZero[] = {0x48, 0x8B, 0x01, 0x48, 0xFF, 0x20, 0xCC, 0xCC};
        const unsigned char other[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00};
        const unsigned char unaligned[] = {0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x31, 0xCC};
        const unsigned char negative[] = {0x48, 0x8B, 0x01, 0x48, 0xFF, 0xA0, 0xF8, 0xFF, 0xFF, 0xFF};
        Check(tb::DecodeVtableJump(setAchievement) == 0x30, "testbot: decode jmp [rax+disp8]");
        Check(tb::DecodeVtableJump(resetAll) == 0xA0, "testbot: decode jmp [rax+disp32]");
        Check(tb::DecodeVtableJump(slotZero) == 0, "testbot: decode jmp [rax]");
        Check(tb::DecodeVtableJump(other) == -1 && tb::DecodeVtableJump(unaligned) == -1
                && tb::DecodeVtableJump(negative) == -1 && tb::DecodeVtableJump(nullptr) == -1,
            "testbot: other shapes are refused");
    }

    if (failures == 0)
    {
        std::cout << "PASS: test bot script parser, key names, modifier masks, Steam wrapper decoder\n";
    }
}

// Manual oracle: decode the ISteamUserStats write methods from a real
// steam_api64.dll (mapped without running its DllMain).
int RunSteamStatsSlotsOracle(const wchar_t* dllPath)
{
    HMODULE module = LoadLibraryExW(dllPath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (module == nullptr)
    {
        std::cout << "FAIL: cannot map " << GetLastError() << "\n";
        return 1;
    }
    const char* names[] = {"SetStatFloat", "SetStatInt32", "UpdateAvgRateStat", "SetAchievement", "ClearAchievement",
        "StoreStats", "IndicateAchievementProgress", "ResetAllStats"};
    int bad = 0;
    for (const char* name : names)
    {
        const std::string exportName = std::string("SteamAPI_ISteamUserStats_") + name;
        const auto* code = reinterpret_cast<const unsigned char*>(GetProcAddress(module, exportName.c_str()));
        const int offset = code == nullptr ? -1 : soulash_plugins::testbot::DecodeVtableJump(code);
        std::cout << exportName << " -> " << (offset < 0 ? std::string("UNDECODABLE") : "slot " + std::to_string(offset / 8)
            + " (+0x" + [&] { char b[16]; std::snprintf(b, sizeof(b), "%X", offset); return std::string(b); }() + ")") << "\n";
        bad += offset < 0 ? 1 : 0;
    }
    FreeLibrary(module);
    return bad == 0 ? 0 : 1;
}

// TestBot snapshot / mods_errors commands and the PNG writer.
void TestTestBotCapture()
{
    namespace tb = soulash_plugins::testbot;
    namespace png = soulash_plugins::png;

    {
        const tb::ParseResult parsed = tb::ParseScript(
            "snapshot storm_start\n"
            "snapshot Storm-02 5 250\n"
            "mods_errors\n"
            "mods_errors 0\n");
        Check(parsed.ok && parsed.steps.size() == 4, "testbot: snapshot and mods_errors parse");
        if (parsed.ok && parsed.steps.size() == 4)
        {
            Check(parsed.steps[0].op == tb::Op::Snapshot && parsed.steps[0].text == "storm_start"
                    && parsed.steps[0].a == 1 && parsed.steps[0].b == 500,
                "testbot: snapshot defaults to 1 frame, 500 ms");
            Check(parsed.steps[1].text == "Storm-02" && parsed.steps[1].a == 5 && parsed.steps[1].b == 250,
                "testbot: snapshot count and interval");
            Check(parsed.steps[2].op == tb::Op::ModsErrors && parsed.steps[2].a == -1 && parsed.steps[3].a == 0,
                "testbot: mods_errors without and with a maximum");
        }
        const char* bad[] = {"snapshot", "snapshot ../x", "snapshot a.png", "snapshot x 0", "snapshot x 21",
            "snapshot x 1 60001", "snapshot x 1 2 3", "snapshot abcdefghijklmnopqrstuvwxyz0123456",
            "mods_errors -1", "mods_errors 1 2", "mods_errors x"};
        bool allRefused = true;
        for (const char* line : bad)
        {
            allRefused = allRefused && !tb::ParseScript(line).ok;
        }
        Check(allRefused, "testbot: bad snapshot / mods_errors arguments are refused (names can't escape the run folder)");
    }

    // Editor commands: screens, text input, mouse move / drag / double click.
    {
        const tb::ParseResult parsed = tb::ParseScript(
            "wait_screen MainMenuScreen\n"
            "wait_screen AnimatorScreen 5000\n"
            "expect_screen EntityMakerScreen\n"
            "log_screen\n"
            "type  Fire bolt (copy)  \n"
            "move 10 20\n"
            "dclick 30 40\n"
            "dclick 30 40 right\n"
            "drag 1 2 300 400\n"
            "drag 1 2 3 4 Middle\n"
            "click 5 6 right\n");
        Check(parsed.ok && parsed.steps.size() == 11, "testbot: editor commands parse");
        if (parsed.ok && parsed.steps.size() == 11)
        {
            const std::vector<tb::Step>& s = parsed.steps;
            Check(s[0].op == tb::Op::WaitScreen && s[0].text == "MainMenuScreen" && s[0].a == 30000,
                "testbot: wait_screen defaults to 30 s");
            Check(s[1].text == "AnimatorScreen" && s[1].a == 5000, "testbot: wait_screen timeout");
            Check(s[2].op == tb::Op::ExpectScreen && s[2].text == "EntityMakerScreen", "testbot: expect_screen");
            Check(s[3].op == tb::Op::LogScreen, "testbot: log_screen");
            Check(s[4].op == tb::Op::Type && s[4].text == "Fire bolt (copy)", "testbot: type keeps inner blanks, trims the ends");
            Check(s[5].op == tb::Op::Move && s[5].a == 10 && s[5].b == 20, "testbot: move");
            Check(s[6].op == tb::Op::DoubleClick && s[6].a == 30 && s[6].b == 40 && s[6].c == 1 && s[7].c == 3,
                "testbot: dclick defaults to the left button");
            Check(s[8].op == tb::Op::Drag && s[8].a == 1 && s[8].b == 2 && s[8].c == 300 && s[8].d == 400 && s[8].e == 1,
                "testbot: drag coordinates, left button by default");
            Check(s[9].e == 2, "testbot: drag button names are case-insensitive");
            Check(s[10].op == tb::Op::Click && s[10].c == 3, "testbot: click still takes a button");
        }
        const std::string tooLong = "type " + std::string(tb::MaxTypeLength + 1, 'x');
        const char* bad[] = {"wait_screen", "wait_screen Main-Menu", "wait_screen MainMenuScreen 0",
            "wait_screen MainMenuScreen 600001", "wait_screen A B C", "expect_screen", "expect_screen A 5",
            "expect_screen ../x", "log_screen now", "type", "type caf\xc3\xa9", tooLong.c_str(), "move 1",
            "move -1 5", "move 1 16385", "dclick 1 2 side", "dclick 1", "drag 1 2 3", "drag 1 2 3 4 up",
            "drag 1 2 3 4 left 6", "click 1 2 up"};
        bool allRefused = true;
        for (const char* line : bad)
        {
            allRefused = allRefused && !tb::ParseScript(line).ok;
        }
        Check(allRefused, "testbot: bad editor command arguments are refused");
        Check(tb::ParseScript("type a # b").ok && tb::ParseScript("type a # b").steps[0].text == "a",
            "testbot: '#' starts a comment after type");
    }

    // Checksums against published values.
    const unsigned char iend[] = {'I', 'E', 'N', 'D'};
    Check(png::Crc32(iend, 4) == 0xAE426082u, "png: CRC-32 of IEND");
    const unsigned char wiki[] = {'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a'};
    Check(png::Adler32(wiki, sizeof(wiki)) == 0x11E60398u, "png: Adler-32 of 'Wikipedia'");

    // Stored zlib stream spanning several 65535-byte blocks.
    {
        std::vector<unsigned char> data(150000);
        for (std::size_t i = 0; i < data.size(); ++i)
        {
            data[i] = static_cast<unsigned char>(i * 7);
        }
        const std::vector<unsigned char> z = png::StoredZlib(data.data(), data.size());
        Check(z.size() == 2 + 3 * 5 + data.size() + 4 && z[0] == 0x78 && z[2] == 0 && z[2 + 5 + 65535] == 0
                && z[2 + 2 * (5 + 65535)] == 1,
            "png: stored deflate blocks are 64 KiB max and only the last is final");
    }

    // A 3x2 image read bottom-up (as glReadPixels returns it).
    {
        const unsigned char pixels[] = {
            0, 0, 255,   255, 255, 255,   0, 0, 0,     // bottom row: blue, white, black
            255, 0, 0,   0, 255, 0,       10, 20, 30,  // top row: red, green, (10,20,30)
        };
        const std::vector<unsigned char> file = png::EncodeRgb(pixels, 3, 2, true);
        const unsigned char signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        Check(file.size() > 8 + 25 + 12 && std::memcmp(file.data(), signature, 8) == 0
                && std::memcmp(file.data() + 12, "IHDR", 4) == 0 && file[19] == 3 && file[23] == 2 && file[24] == 8
                && file[25] == 2,
            "png: signature and IHDR (3x2, 8-bit RGB)");
        // Every chunk's CRC is right and the file ends with IEND.
        bool crcOk = true;
        std::size_t at = 8;
        std::string last;
        while (at + 12 <= file.size())
        {
            const std::uint32_t length = (std::uint32_t(file[at]) << 24) | (std::uint32_t(file[at + 1]) << 16)
                | (std::uint32_t(file[at + 2]) << 8) | file[at + 3];
            if (at + 12 + length > file.size())
            {
                crcOk = false;
                break;
            }
            const std::uint32_t stored = (std::uint32_t(file[at + 8 + length]) << 24)
                | (std::uint32_t(file[at + 9 + length]) << 16) | (std::uint32_t(file[at + 10 + length]) << 8)
                | file[at + 11 + length];
            crcOk = crcOk && stored == png::Crc32(file.data() + at + 4, length + 4);
            last.assign(reinterpret_cast<const char*>(file.data() + at + 4), 4);
            at += 12 + length;
        }
        Check(crcOk && at == file.size() && last == "IEND", "png: chunk CRCs and IEND");
        Check(png::EncodeRgb(pixels, 0, 2, true).empty() && png::EncodeRgb(nullptr, 1, 1, false).empty()
                && png::EncodeRgb(pixels, png::MaxDimension + 1, 1, false).empty(),
            "png: invalid sizes are refused");
    }

    // A failing compressor falls back to stored blocks.
    {
        const unsigned char pixel[] = {1, 2, 3};
        const auto failing = [](unsigned char*, unsigned long*, const unsigned char*, unsigned long, int) { return -2; };
        Check(png::EncodeRgb(pixel, 1, 1, false, failing) == png::EncodeRgb(pixel, 1, 1, false),
            "png: a failing compress2 falls back to stored deflate");
    }
}

// --png <path>: writes the 3x2 bottom-up test image; scripts/test.ps1 decodes it with .NET.
int WriteTestPng(const wchar_t* path)
{
    const unsigned char pixels[] = {
        0, 0, 255,   255, 255, 255,   0, 0, 0,
        255, 0, 0,   0, 255, 0,       10, 20, 30,
    };
    const std::vector<unsigned char> file = soulash_plugins::png::EncodeRgb(pixels, 3, 2, true);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return out.good() && !file.empty() ? 0 : 1;
}
