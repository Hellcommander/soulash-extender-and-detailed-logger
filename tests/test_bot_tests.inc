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
