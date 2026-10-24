// Included inside the anonymous namespace of tests.cpp (uses Check/failures).

int RunEffectsTests(const wchar_t* examplePath)
{
    using namespace soulash::effects;
    using soulash::conditions::Snapshot;

    auto load = [](const std::string& text, EffectSet& set) {
        std::string error;
        if (!set.Load(text, error))
        {
            std::cerr << "unexpected load failure: " << error << '\n';
            return false;
        }
        return true;
    };
    auto rejects = [](const std::string& document, const char* reasonContains) {
        EffectSet set;
        std::string error;
        const bool loaded = set.Load(document, error);
        if (loaded || error.find(reasonContains) == std::string::npos)
        {
            std::cerr << "expected rejection containing '" << reasonContains << "', got "
                      << (loaded ? "success" : error) << '\n';
            return false;
        }
        return true;
    };
    // A deterministic "random" source: cycles through the given values.
    struct Sequence
    {
        std::vector<double> values;
        std::size_t index = 0;
        double operator()() { const double v = values[index % values.size()]; ++index; return v; }
    };
    auto spell = [](const std::string& effects, const std::string& extra = "") {
        return "{\"spec\":1,\"mod\":\"m\",\"spells\":[{\"id\":\"S\",\"target\":\"aoe_tile\",\"radius\":2,"
            "\"effects\":" + effects + "}" + extra + "]}";
    };
    auto castsOf = [](const Resolution& result) {
        std::vector<std::pair<std::string, int>> out;
        for (const Cast& cast : result.casts) out.emplace_back(cast.abilityId, cast.delayTurns);
        return out;
    };
    const std::set<std::string> everything = {"choose", "delay", "repeat", "remove_stacks", "teleport", "grant_xp",
        "on_kill", "on_turn", "on_damaged", "on_dodge", "on_block", "on_crit"};

    // ---- Strict loader --------------------------------------------------------
    Check(rejects("{\"spec\":2,\"mod\":\"m\"}", "spec must be 1"), "spec is checked");
    Check(rejects("{\"spec\":1,\"mod\":\"bad id\"}", "mod must be"), "mod id is checked");
    Check(rejects("{\"spec\":1,\"mod\":\"m\",\"extra\":1}", "unknown top-level key"), "unknown top-level key");
    Check(rejects("{\"spec\":1,\"mod\":\"m\",\"requires\":[\"magic\"]}", "unknown capability"), "unknown requires entry");
    Check(rejects(spell("[{\"damage\":{\"amount\":1,\"type\":\"fire\"},\"heal\":{\"amount\":1}}]"), "exactly one effect key"),
        "two effect heads");
    Check(rejects(spell("[{\"explode\":1}]"), "exactly one effect key"), "unknown leaf");
    Check(rejects(spell("[{\"chance\":1.5,\"do\":{\"heal\":{\"amount\":1}}}]"), "chance must be"), "chance range");
    Check(rejects(spell("[{\"chance\":0.5}]"), "needs 'do'"), "chance without do");
    Check(rejects(spell("[{\"delay\":0,\"do\":{\"heal\":{\"amount\":1}}}]"), "delay must be"), "delay range");
    Check(rejects(spell("[{\"repeat\":2,\"do\":{\"heal\":{\"amount\":1}},\"percent\":5}]"), "unknown key 'percent'"),
        "unknown key next to a combinator");
    Check(rejects(spell("[{\"choose\":[{\"weight\":0,\"do\":{\"heal\":{\"amount\":1}}}]}]"), "weight must be"), "weight range");
    Check(rejects(spell("[{\"cast\":{\"spell\":\"X\",\"chance\":2}}]"), "chance must be"), "cast chance range");
    Check(rejects(spell("[{\"when\":{\"a\":1,\"b\":2},\"do\":{\"heal\":{\"amount\":1}}}]"), "exactly one key"),
        "malformed condition");
    Check(rejects("{\"spec\":1,\"mod\":\"m\",\"spells\":[{\"id\":\"S\",\"ability\":\"x\",\"effects\":[{\"heal\":{\"amount\":1}}]}]}",
              "exactly one of"), "id and ability together");
    Check(rejects("{\"spec\":1,\"mod\":\"m\",\"spells\":[{\"id\":\"S\",\"effects\":[{\"heal\":{\"amount\":1}}]},"
              "{\"id\":\"S\",\"effects\":[{\"heal\":{\"amount\":1}}]}]}", "duplicate id"), "duplicate spell id");
    Check(rejects("{\"spec\":1,\"mod\":\"m\",\"triggers\":[{\"id\":\"t\",\"on\":{\"sneeze\":true},\"do\":{\"heal\":{\"amount\":1}}}]}",
              "exactly one event"), "unknown event");
    Check(rejects("{\"spec\":1,\"mod\":\"m\",\"triggers\":[{\"id\":\"t\",\"on\":{\"kill\":1},\"do\":{\"heal\":{\"amount\":1}}}]}",
              "must be true"), "event flag must be true");
    Check(rejects("{\"spec\":1,\"mod\":\"m\",\"triggers\":[{\"id\":\"t\",\"on\":{\"attack\":true}}]}", "missing 'do'"),
        "trigger needs do");
    {
        std::string deep = "{\"heal\":{\"amount\":1}}";
        for (int level = 0; level < 9; ++level) deep = "{\"chance\":0.5,\"do\":" + deep + "}";
        Check(rejects(spell("[" + deep + "]"), "deeper than"), "depth limit");
    }

    // ---- Classification mirrors the compiler ----------------------------------
    {
        EffectSet set;
        if (load(spell("[{\"damage\":{\"amount\":[3,6],\"type\":\"fire\"}},"
                 "{\"choose\":[{\"weight\":3,\"do\":{\"cast\":{\"spell\":\"X\"}}},{\"weight\":1,\"do\":{\"status\":{\"stun\":1}}}]},"
                 "{\"delay\":2,\"do\":{\"all\":[{\"buff\":{\"duration\":3,\"stats\":{\"strength\":1}}},{\"stamina\":10}]}},"
                 "{\"when\":{\"health_percent\":{\"max\":50}},\"do\":{\"cast\":{\"spell\":\"Y\"}}},"
                 "{\"when\":{\"all\":[{\"weapon_class\":\"Sword\"},{\"party\":\"alone\"}]},\"do\":{\"cast\":{\"spell\":\"Z\"}}},"
                 "{\"remove_stacks\":{\"stacker\":\"flag\",\"count\":0}},"
                 "{\"repeat\":2,\"every\":1,\"do\":{\"cast\":{\"spell\":\"W\"}}},"
                 "{\"choose\":[{\"do\":{\"cast\":{\"spell\":\"A\"}}},{\"do\":{\"cast\":{\"spell\":\"B\"}}}],\"approximate\":true},"
                 "{\"chance\":0.5,\"do\":{\"delay\":1,\"do\":{\"heal\":{\"amount\":2}}}}]"), set))
        {
            const Node& root = set.Spells()[0].root;
            Check(root.native && root.children[0].native, "root and a native leaf are native");
            Check(!root.children[1].native && !root.children[1].children[1].native, "exact choose and its children are evaluator-owned");
            Check(!root.children[2].native, "delay is evaluator-owned");
            Check(!root.children[3].native, "a when on health is evaluator-owned");
            Check(root.children[4].native && root.children[4].children[0].native, "a fully native when stays native");
            Check(!root.children[5].native, "remove_stacks is evaluator-owned");
            Check(!root.children[6].native, "repeat over turns is evaluator-owned");
            Check(root.children[7].native, "approximate choose is native");
            Check(root.children[8].native && !root.children[8].children[0].native, "chance stays native above an evaluator delay");
            Check(root.children[1].children[0].chance == 0.75 && root.children[1].children[1].chance == 0.25, "choose weights become shares");
            Check(root.children[2].PathText() == "2" && root.children[2].children[0].children[1].PathText() == "2_0_1", "paths");

            // ---- Resolution ------------------------------------------------------
            Snapshot state;
            state.healthPercent = 30.0;
            Sequence rng{{0.9, 0.9, 0.9, 0.9}};
            const Resolution result = Resolve(set, set.Spells()[0], state, std::ref(rng), everything);
            const auto casts = castsOf(result);
            auto has = [&](const char* id, int delay) {
                return std::find(casts.begin(), casts.end(), std::make_pair(std::string(id), delay)) != casts.end();
            };
            Check(has("m_S__1_1", 0), "choose with roll 0.9 picks the second option's group ability");
            Check(!has("X", 0), "the first option did not fire");
            Check(has("m_S__2_0", 2), "delayed group fires after 2 turns under its own path");
            Check(has("Y", 0), "health gate true casts Y");
            Check(!has("Z", 0), "fully native when emits nothing (the engine's passive does it)");
            Check(has("W", 0) && has("W", 1), "repeat every 1 schedules two casts");
            Check(!has("m_S__0", 0), "native leaves emit nothing");
            bool removeSkipped = false;
            for (const Unresolved& item : result.unresolved) removeSkipped = removeSkipped || (item.path == "5");
            Check(removeSkipped, "remove_stacks is reported unresolved (no cast realisation)");
            Check(!has("m_S__8_0_0", 1) && !has("m_S__8_0", 1), "chance 0.5 with roll 0.9 blocks the evaluator delay below it");

            Sequence low{{0.1, 0.1, 0.1, 0.1}};
            const auto lucky = castsOf(Resolve(set, set.Spells()[0], state, std::ref(low), everything));
            Check(std::find(lucky.begin(), lucky.end(), std::make_pair(std::string("X"), 0)) != lucky.end(),
                "choose with roll 0.1 picks the first option");
            Check(std::find(lucky.begin(), lucky.end(), std::make_pair(std::string("m_S__8_0_0"), 1)) != lucky.end(),
                "chance passes and the delayed heal fires at +1 under the leaf's own path");
            Check(std::find(lucky.begin(), lucky.end(), std::make_pair(std::string("A"), 0)) == lucky.end()
                    && std::find(lucky.begin(), lucky.end(), std::make_pair(std::string("B"), 0)) == lucky.end(),
                "approximate choose is native: the engine's rows fire it, the evaluator emits nothing");

            // Fail closed: unknown condition, missing capabilities, requires.
            const auto blind = castsOf(Resolve(set, set.Spells()[0], Snapshot{}, std::ref(low), everything));
            Check(std::find(blind.begin(), blind.end(), std::make_pair(std::string("Y"), 0)) == blind.end(),
                "an unknown health condition fails closed");
            const Resolution limited = Resolve(set, set.Spells()[0], state, std::ref(low), {"delay"});
            bool chooseSkipped = false;
            for (const Unresolved& item : limited.unresolved) chooseSkipped = chooseSkipped || item.path == "1";
            Check(chooseSkipped, "a missing choose capability skips the node");
        }
    }
    {
        EffectSet set;
        if (load("{\"spec\":1,\"mod\":\"m\",\"requires\":[\"teleport\"],\"spells\":[{\"id\":\"S\",\"effects\":[{\"delay\":1,\"do\":{\"heal\":{\"amount\":1}}}]}]}", set))
        {
            Sequence rng{{0.5}};
            const Resolution result = Resolve(set, set.Spells()[0], Snapshot{}, std::ref(rng));
            Check(result.casts.empty() && !result.unresolved.empty() && result.unresolved[0].path == "requires",
                "an unsupported requires entry makes the whole file unavailable");
        }
    }

    // ---- Group splitting and the active policy ----------------------------------
    {
        EffectSet set;
        if (load(spell("[{\"delay\":1,\"do\":{\"all\":[{\"damage\":{\"amount\":1,\"type\":\"fire\"}},{\"status\":{\"burn\":1}},"
                 "{\"damage\":{\"amount\":2,\"type\":\"frost\"}},{\"buff\":{\"duration\":2,\"stats\":{\"strength\":1}}},"
                 "{\"buff\":{\"duration\":5,\"stats\":{\"dexterity\":1}}}]}},"
                 "{\"when\":{\"in_combat\":true},\"on_unavailable\":\"active\",\"do\":{\"cast\":{\"spell\":\"Q\"}}}]"), set))
        {
            const std::vector<std::string> ids = GroupAbilityIds("m_S", set.Spells()[0].root.children[0].children[0]);
            // First fit: frost damage clashes with fire damage -> x1; the 2-turn buff joins
            // the first group; the 5-turn buff clashes on duration there and joins x1.
            Check(ids == std::vector<std::string>{"m_S__0_0", "m_S__0_0_x1"},
                "conflicting keys and durations split into _x groups like the compiler");
            Sequence rng{{0.5}};
            const auto casts = castsOf(Resolve(set, set.Spells()[0], Snapshot{}, std::ref(rng), everything));
            Check(std::find(casts.begin(), casts.end(), std::make_pair(std::string("m_S__0_0"), 1)) != casts.end()
                    && std::find(casts.begin(), casts.end(), std::make_pair(std::string("m_S__0_0_x1"), 1)) != casts.end(),
                "every split group is cast with the delay");
            Check(std::find(casts.begin(), casts.end(), std::make_pair(std::string("Q"), 0)) != casts.end(),
                "on_unavailable active lets an unknown condition through");
        }
    }

    // ---- Cast targets resolve local spell ids; attached spells ----------------------
    {
        EffectSet set;
        if (load("{\"spec\":1,\"mod\":\"m\",\"spells\":[{\"id\":\"A\",\"effects\":[{\"delay\":1,\"do\":{\"cast\":{\"spell\":\"B\"}}}]},"
                 "{\"id\":\"B\",\"effects\":[{\"heal\":{\"amount\":1}}]},"
                 "{\"ability\":\"m_Old\",\"effects\":[{\"status\":{\"burn\":1}},{\"delay\":2,\"do\":{\"stamina\":5}}]}]}", set))
        {
            Check(set.CastTarget("B") == "m_B" && set.CastTarget("other_x") == "other_x", "local ids resolve to <mod>_<id>");
            Check(set.FindSpell("m_Old") != nullptr && set.FindSpell("m_Old")->attached, "attached spells are found by ability id");
            Sequence rng{{0.5}};
            const auto casts = castsOf(Resolve(set, set.Spells()[0], Snapshot{}, std::ref(rng), everything));
            Check(casts == std::vector<std::pair<std::string, int>>{{"m_B", 1}}, "delayed cast of a local spell");
            const auto attached = castsOf(Resolve(set, *set.FindSpell("m_Old"), Snapshot{}, std::ref(rng), everything));
            Check(attached == std::vector<std::pair<std::string, int>>{{"m_Old__1_0", 2}},
                "an attached spell's evaluator leaf is named under the existing ability");
        }
    }

    // ---- Triggers ---------------------------------------------------------------------
    {
        EffectSet set;
        if (load("{\"spec\":1,\"mod\":\"m\",\"triggers\":["
                 "{\"id\":\"t1\",\"on\":{\"attack\":true},\"chance\":0.2,\"do\":{\"cast\":{\"spell\":\"P\"}}},"
                 "{\"id\":\"t2\",\"on\":{\"turn\":true},\"chance\":0.5,\"when\":{\"in_combat\":true},\"do\":{\"all\":[{\"heal\":{\"amount\":1}},{\"stamina\":2}]}},"
                 "{\"id\":\"t3\",\"on\":{\"kill\":true},\"do\":{\"create_item\":{\"item\":\"Charge\"}}},"
                 "{\"id\":\"t4\",\"on\":{\"cast\":\"m_P\"},\"do\":{\"delay\":3,\"do\":{\"cast\":{\"spell\":\"R\"}}}}]}", set))
        {
            Check(set.Triggers().size() == 4 && set.Triggers()[0].nativeEvent && !set.Triggers()[1].nativeEvent
                    && set.Triggers()[2].nativeEvent && set.Triggers()[3].nativeEvent,
                "cast, attack and kill+create_item are native events");
            Sequence rng{{0.1}};
            Check(ResolveEvent(set, set.Triggers()[0], Snapshot{}, std::ref(rng), everything).casts.empty(),
                "a fully native attack trigger emits nothing");
            Check(ResolveEvent(set, set.Triggers()[2], Snapshot{}, std::ref(rng), everything).casts.empty(),
                "on_kill_create_item emits nothing");
            Snapshot fighting;
            fighting.inCombat = true;
            const auto turn = castsOf(ResolveEvent(set, set.Triggers()[1], fighting, std::ref(rng), everything));
            Check(turn == std::vector<std::pair<std::string, int>>{{"m_ext_trigger_t2__r", 0}},
                "a per-turn trigger casts its merged group under the trigger's ability id");
            Check(ResolveEvent(set, set.Triggers()[1], Snapshot{}, std::ref(rng), everything).casts.empty(),
                "an unknown condition on an event fails closed");
            Check(ResolveEvent(set, set.Triggers()[1], fighting, std::ref(rng), {"delay"}).casts.empty(),
                "a missing on_turn capability skips the trigger");
            Sequence unlucky{{0.9}};
            Check(ResolveEvent(set, set.Triggers()[1], fighting, std::ref(unlucky), everything).casts.empty(),
                "the trigger chance is rolled by the evaluator for evaluator-owned actions");
            const auto delayed = castsOf(ResolveEvent(set, set.Triggers()[3], Snapshot{}, std::ref(rng), everything));
            Check(delayed == std::vector<std::pair<std::string, int>>{{"R", 3}},
                "an evaluator-owned part of a native cast trigger is resolved");
        }
    }

    // ---- The example document shipped in docs/ -------------------------------------
    if (examplePath != nullptr)
    {
        std::ifstream file(examplePath, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        EffectSet set;
        std::string error;
        Check(set.Load(text, error) && set.Spells().size() == 7 && set.Triggers().size() == 3,
            "the example document loads with 7 spells and 3 triggers");
        if (set.Spells().size() == 7)
        {
            Sequence rng{{0.5}};
            const Spell* entropy = set.FindSpell("arendeth_runic_magic_Entropy_Surge");
            Check(entropy != nullptr && castsOf(Resolve(set, *entropy, Snapshot{}, std::ref(rng), everything))
                    == std::vector<std::pair<std::string, int>>{{"arendeth_runic_magic_Entropy_Surge__1_1", 0}},
                "Entropy Surge with roll 0.5 casts the frost option (weights 3:2:1)");
            const Spell* prophecy = set.FindSpell("arendeth_runic_magic_Prophecy_Sequence");
            Check(prophecy != nullptr && castsOf(Resolve(set, *prophecy, Snapshot{}, std::ref(rng), everything))
                    == std::vector<std::pair<std::string, int>>{{"arendeth_runic_magic_Prophecy_Beacon_Ring", 2},
                        {"arendeth_runic_magic_Prophecy_Beacon_Ring", 4}, {"arendeth_runic_magic_Prophecy_Beacon_Ring", 6}},
                "Prophecy schedules three delayed beacon rings");
            const Spell* cascade = set.FindSpell("arendeth_runic_magic_Cascade_Discharge");
            Snapshot healthy;
            healthy.healthPercent = 90.0;
            Snapshot hurt;
            hurt.healthPercent = 50.0;
            Check(cascade != nullptr && cascade->attached
                    && castsOf(Resolve(set, *cascade, healthy, std::ref(rng), everything))
                        == std::vector<std::pair<std::string, int>>{{"arendeth_runic_magic_Rune_Spark", 0}}
                    && Resolve(set, *cascade, hurt, std::ref(rng), everything).casts.empty(),
                "the attached Cascade gate casts Rune Spark only above 75% health");
            const Spell* cinder = set.FindSpell("arendeth_runic_magic_Throw_Runeword_Cinder");
            Check(cinder != nullptr && Resolve(set, *cinder, Snapshot{}, std::ref(rng), everything).casts.empty()
                    && Resolve(set, *cinder, Snapshot{}, std::ref(rng), everything).unresolved.empty(),
                "a fully native spell needs nothing from the evaluator");
        }
    }

    if (failures == 0)
    {
        std::cout << "PASS: effects loader and resolver (strict loader, ownership, choose/delay/repeat/when, fail-closed)\n";
    }
    return failures;
}
