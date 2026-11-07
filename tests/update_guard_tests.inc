// Included by tests.cpp inside its anonymous namespace (uses Check()).
// Pure update-guard logic: state file round-trip and the per-session decision.

    void TestUpdateGuardLogic()
    {
        namespace ug = soulash::update_guard;
        const std::string buildA(64, 'a');
        const std::string buildB(64, 'b');
        const std::string vendor1(64, '1');
        const std::string vendor2(64, '2');

        // --- state file -------------------------------------------------------
        ug::State state;
        state.exeSha256 = buildA;
        state.gameVersion = "0.10.5.1";
        state.originalSha256 = vendor1;
        state.startedUtc = "2026-11-05T10:00:00Z";
        state.cleanExit = true;
        state.pluginsLoaded = true;
        state.safeMode = false;
        state.uncleanStreak = 3;
        state.knownBuilds.push_back({buildA, "0.10.5.1", "2026-11-01T09:00:00Z"});
        ug::State parsed;
        Check(ug::ParseState(ug::SerializeState(state), parsed), "update guard: serialized state parses");
        Check(parsed.exeSha256 == buildA && parsed.gameVersion == "0.10.5.1" && parsed.originalSha256 == vendor1
                && parsed.startedUtc == state.startedUtc && parsed.cleanExit && parsed.pluginsLoaded
                && !parsed.safeMode && parsed.uncleanStreak == 3 && parsed.knownBuilds.size() == 1
                && parsed.knownBuilds[0].version == "0.10.5.1",
            "update guard: state round-trips every field");

        Check(!ug::ParseState("exe_sha256=" + buildA + "\n", parsed), "update guard: text without format=1 is rejected");
        Check(!ug::ParseState("format=2\nclean_exit=1\n", parsed), "update guard: unknown format is rejected");
        Check(!ug::ParseState(std::string(ug::MaxStateBytes + 1, 'x'), parsed), "update guard: oversized state is rejected");
        Check(ug::ParseState("format=1\nexe_sha256=XYZ\ngame_version=bad|value\nunclean_streak=-4\n"
                             "known_build=nothex|1|t\nsomething=else\nclean_exit=1\n", parsed)
                && parsed.exeSha256.empty() && parsed.gameVersion.empty() && parsed.uncleanStreak == 0
                && parsed.knownBuilds.empty() && parsed.cleanExit,
            "update guard: malformed values are dropped, the rest of the state survives");

        const ug::Config defaults;
        const std::string now = "2026-11-05T12:00:00Z";

        // --- first start --------------------------------------------------------
        ug::Decision first = ug::Decide(nullptr, defaults, buildA, "0.10.5.1", vendor1, now);
        Check(first.firstStart && !first.gameUpdated && first.firstRunOnBuild && !first.safeMode
                && first.uncleanStreak == 0 && first.next.knownBuilds.size() == 1
                && !first.next.cleanExit && !first.next.pluginsLoaded,
            "update guard: first start records the build and is not safe mode");

        // --- clean previous session on the same build ---------------------------
        ug::State previous = first.next;
        previous.cleanExit = true;
        previous.pluginsLoaded = true;
        ug::Decision same = ug::Decide(&previous, defaults, buildA, "0.10.5.1", vendor1, now);
        Check(!same.firstStart && !same.gameUpdated && !same.firstRunOnBuild && !same.vendorDllChanged
                && same.previousClean && same.uncleanStreak == 0 && !same.safeMode
                && same.next.knownBuilds.size() == 1,
            "update guard: clean session on the same build changes nothing");

        // --- crash loop -> safe mode -------------------------------------------
        previous.cleanExit = false;
        previous.pluginsLoaded = true;
        previous.uncleanStreak = 0;
        ug::Decision once = ug::Decide(&previous, defaults, buildA, "0.10.5.1", vendor1, now);
        Check(once.uncleanStreak == 1 && !once.safeMode, "update guard: one unclean exit is not yet safe mode");
        ug::State afterOnce = once.next;
        afterOnce.pluginsLoaded = true;   // that session loaded plugins and died again
        ug::Decision twice = ug::Decide(&afterOnce, defaults, buildA, "0.10.5.1", vendor1, now);
        Check(twice.uncleanStreak == 2 && twice.safeMode && !twice.safeModeReason.empty()
                && twice.next.safeMode,
            "update guard: two unclean exits with plugins loaded enter safe mode");

        // Safe-mode session (no plugins) that also dies keeps the streak: still safe mode.
        ug::State safeSession = twice.next;   // pluginsLoaded=false, cleanExit=false
        ug::Decision stillSafe = ug::Decide(&safeSession, defaults, buildA, "0.10.5.1", vendor1, now);
        Check(stillSafe.uncleanStreak == 2 && stillSafe.safeMode,
            "update guard: an unclean exit without plugins neither counts nor clears the streak");

        // A clean safe-mode session clears it: plugins get another chance.
        safeSession.cleanExit = true;
        ug::Decision recovered = ug::Decide(&safeSession, defaults, buildA, "0.10.5.1", vendor1, now);
        Check(recovered.uncleanStreak == 0 && !recovered.safeMode,
            "update guard: a clean exit resets the streak and leaves safe mode");

        // --- game update resets the streak and is reported ----------------------
        ug::Decision updated = ug::Decide(&afterOnce, defaults, buildB, "0.10.6.0", vendor1, now);
        Check(updated.gameUpdated && updated.firstRunOnBuild && updated.previousSha256 == buildA
                && updated.previousVersion == "0.10.5.1" && updated.uncleanStreak == 0 && !updated.safeMode
                && updated.next.knownBuilds.size() == 2 && updated.next.gameVersion == "0.10.6.0",
            "update guard: a new game build is detected, recorded and gets a fresh streak");

        // Rolling back to a known build is an update but not a first run.
        ug::State onB = updated.next;
        onB.cleanExit = true;
        ug::Decision rollback = ug::Decide(&onB, defaults, buildA, "0.10.5.1", vendor1, now);
        Check(rollback.gameUpdated && !rollback.firstRunOnBuild && rollback.next.knownBuilds.size() == 2,
            "update guard: returning to a known build is not a first run");

        // --- vendor DLL replaced (Steam verify / update) ------------------------
        ug::Decision vendor = ug::Decide(&previous, defaults, buildA, "0.10.5.1", vendor2, now);
        Check(vendor.vendorDllChanged, "update guard: a replaced BugSplat64_original.dll is detected");

        // --- configuration ------------------------------------------------------
        ug::Config forced;
        forced.forceSafeMode = true;
        Check(ug::Decide(&previous, forced, buildA, "0.10.5.1", vendor1, now).safeMode,
            "update guard: ForceSafeMode=1 forces safe mode");
        ug::Config never;
        never.safeModeAfterUncleanExits = 0;
        Check(!ug::Decide(&afterOnce, never, buildA, "0.10.5.1", vendor1, now).safeMode,
            "update guard: SafeModeAfterUncleanExits=0 never enters safe mode");

        // --- known builds are capped -------------------------------------------
        ug::State many;
        for (unsigned i = 0; i < ug::MaxKnownBuilds; ++i)
        {
            char sha[65];
            sprintf_s(sha, "%064x", i + 1);
            many.knownBuilds.push_back({sha, "v", "t"});
        }
        many.exeSha256 = many.knownBuilds.back().sha256;
        many.cleanExit = true;
        ug::Decision capped = ug::Decide(&many, defaults, buildB, "", vendor1, now);
        Check(capped.next.knownBuilds.size() == ug::MaxKnownBuilds
                && capped.next.knownBuilds.back().sha256 == buildB
                && capped.next.knownBuilds.front().sha256 == many.knownBuilds[1].sha256,
            "update guard: known builds keep the newest 16");

        // Unknown exe hash (cannot hash the game): no update claimed, nothing recorded.
        ug::Decision unknown = ug::Decide(&previous, defaults, "", "", vendor1, now);
        Check(!unknown.gameUpdated && !unknown.firstRunOnBuild && unknown.next.knownBuilds.size() == 1,
            "update guard: an unavailable game hash claims no update");
    }
