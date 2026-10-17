// Included inside the anonymous namespace of tests.cpp (uses Check/failures).

int RunConditionsTests(const wchar_t* examplePath)
{
    using namespace soulash::conditions;

    // Wraps an expression in a one-condition document and evaluates it.
    auto evaluate = [](const std::string& expression, const Snapshot& state,
                        const std::string& policy = "") {
        const std::string document = "{\"spec\":1,\"conditions\":[{\"id\":\"c\","
            "\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":" + expression
            + (policy.empty() ? "" : ",\"on_unavailable\":\"" + policy + "\"") + "}]}";
        ConditionSet set;
        std::string error;
        if (!set.Load(document, error))
        {
            std::cerr << "unexpected load failure: " << error << " for " << expression << '\n';
            return Tri::Unknown;
        }
        return Evaluate(set.Items()[0].when, state);
    };
    auto active = [](const std::string& expression, const Snapshot& state, const std::string& policy,
                      const std::set<std::string>& supported = SupportedPredicates(),
                      const std::string& requires = "") {
        const std::string document = "{\"spec\":1," + (requires.empty() ? "" : "\"requires\":" + requires + ",")
            + "\"conditions\":[{\"id\":\"c\",\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":"
            + expression + ",\"on_unavailable\":\"" + policy + "\"}]}";
        ConditionSet set;
        std::string error;
        if (!set.Load(document, error)) return false;
        return IsActive(set, set.Items()[0], state, supported);
    };
    auto rejects = [](const std::string& document, const char* reasonContains) {
        ConditionSet set;
        std::string error;
        const bool loaded = set.Load(document, error);
        return !loaded && error.find(reasonContains) != std::string::npos;
    };

    Snapshot full;
    full.weaponClass = std::string("Sword");
    full.shield = true;
    full.holderTags = std::set<std::string>{"Human", "Alive"};
    full.targetTags = std::set<std::string>{"Undead"};
    full.party = std::string("in_party");
    full.healthPercent = 40.0;
    full.staminaPercent = 90.0;
    full.targetHealthPercent = 10.0;
    full.stackers = std::map<std::string, double>{{"burn", 3}};
    full.inventory = std::map<std::string, double>{{"rune_charge", 2}};
    full.skills = std::map<std::string, double>{{"runic", 12}};
    EquippedItem sword;
    sword.item = "iron_sword";
    sword.itemType = "Weapon";
    sword.itemTags = {"blade"};
    full.equipped = std::map<std::string, EquippedItem>{{"right_hand", sword}};
    full.dayPart = std::string("Night");
    full.season = std::string("Winter");
    full.biome = std::string("Forest");
    full.inCombat = true;
    full.inSettlement = false;
    full.companions = 2.0;
    const Snapshot empty;

    // ---- Every predicate: true, false, unknown --------------------------
    struct Row { const char* expression; Tri expected; };
    const Row rows[] = {
        {"{\"weapon_class\":\"sword\"}", Tri::True},
        {"{\"weapon_class\":[\"Axe\",\"SWORD\"]}", Tri::True},
        {"{\"weapon_class\":\"Bow\"}", Tri::False},
        {"{\"shield\":true}", Tri::True}, {"{\"shield\":false}", Tri::False},
        {"{\"holder_tag\":\"human\"}", Tri::True}, {"{\"holder_tag\":[\"Orc\",\"Elf\"]}", Tri::False},
        {"{\"target_tag\":[\"Undead\",\"Demon\"]}", Tri::True}, {"{\"target_tag\":\"Human\"}", Tri::False},
        {"{\"party\":\"in_party\"}", Tri::True}, {"{\"party\":\"alone\"}", Tri::False},
        {"{\"health_percent\":{\"max\":50}}", Tri::True}, {"{\"health_percent\":{\"min\":41}}", Tri::False},
        {"{\"health_percent\":{\"min\":40,\"max\":40}}", Tri::True},
        {"{\"stamina_percent\":{\"min\":80}}", Tri::True},
        {"{\"target_health_percent\":{\"max\":5}}", Tri::False},
        {"{\"stacker\":{\"id\":\"burn\"}}", Tri::True}, {"{\"stacker\":{\"id\":\"burn\",\"min\":4}}", Tri::False},
        {"{\"stacker\":{\"id\":\"frost\"}}", Tri::False}, {"{\"stacker\":{\"id\":\"frost\",\"max\":0}}", Tri::True},
        {"{\"inventory_count\":{\"item\":\"rune_charge\",\"min\":2}}", Tri::True},
        {"{\"inventory_count\":{\"item\":\"rune_charge\",\"min\":3}}", Tri::False},
        {"{\"inventory_count\":{\"item\":\"missing\",\"max\":0}}", Tri::True},
        {"{\"skill_level\":{\"skill\":\"runic\",\"min\":10,\"max\":20}}", Tri::True},
        {"{\"skill_level\":{\"skill\":\"other\",\"min\":1}}", Tri::False},
        {"{\"equipped\":{\"slot\":\"right_hand\",\"empty\":false}}", Tri::True},
        {"{\"equipped\":{\"slot\":\"left_hand\",\"empty\":true}}", Tri::True},
        {"{\"equipped\":{\"slot\":\"right_hand\",\"item\":\"iron_sword\"}}", Tri::True},
        {"{\"equipped\":{\"slot\":\"right_hand\",\"item_type\":\"weapon\"}}", Tri::True},
        {"{\"equipped\":{\"slot\":\"right_hand\",\"item_tag\":\"blade\"}}", Tri::True},
        {"{\"equipped\":{\"slot\":\"right_hand\",\"item_tag\":\"axe\"}}", Tri::False},
        {"{\"equipped\":{\"slot\":\"left_hand\",\"item\":\"x\"}}", Tri::False},
        {"{\"time_of_day\":[\"Night\",\"Late Night\"]}", Tri::True}, {"{\"time_of_day\":\"Morning\"}", Tri::False},
        {"{\"season\":\"winter\"}", Tri::True}, {"{\"biome\":[\"Desert\"]}", Tri::False},
        {"{\"in_combat\":true}", Tri::True}, {"{\"in_settlement\":true}", Tri::False},
        {"{\"companions\":{\"min\":2,\"max\":3}}", Tri::True},
    };
    for (const Row& row : rows)
    {
        Check(evaluate(row.expression, full) == row.expected,
            (std::string("predicate on a full snapshot: ") + row.expression).c_str());
        Check(evaluate(row.expression, empty) == Tri::Unknown,
            (std::string("predicate is unknown without its input: ") + row.expression).c_str());
    }

    // ---- Malformed arguments and unknown predicates are unknown ----------
    Check(evaluate("{\"moon_phase\":\"full\"}", full) == Tri::Unknown, "unknown predicate is unknown");
    Check(evaluate("{\"health_percent\":{}}", full) == Tri::Unknown, "empty range is unknown");
    Check(evaluate("{\"weapon_class\":5}", full) == Tri::Unknown, "wrong argument type is unknown");
    Check(evaluate("{\"equipped\":{\"slot\":\"right_hand\"}}", full) == Tri::Unknown,
        "equipped without a selector is unknown");
    Check(evaluate("{\"equipped\":{\"slot\":\"right_hand\",\"empty\":true,\"item\":\"x\"}}", full) == Tri::Unknown,
        "equipped with two selectors is unknown");

    // ---- Three-valued logic ----------------------------------------------
    Snapshot partial;
    partial.inCombat = true;     // known; everything else unknown
    const std::string T = "{\"in_combat\":true}";
    const std::string F = "{\"in_combat\":false}";
    const std::string U = "{\"shield\":true}";
    auto all = [](const std::string& a, const std::string& b) { return "{\"all\":[" + a + "," + b + "]}"; };
    auto any = [](const std::string& a, const std::string& b) { return "{\"any\":[" + a + "," + b + "]}"; };
    Check(evaluate(all(T, T), partial) == Tri::True, "true and true");
    Check(evaluate(all(T, F), partial) == Tri::False, "true and false");
    Check(evaluate(all(F, U), partial) == Tri::False, "false and unknown is false");
    Check(evaluate(all(T, U), partial) == Tri::Unknown, "true and unknown is unknown");
    Check(evaluate(any(F, F), partial) == Tri::False, "false or false");
    Check(evaluate(any(T, U), partial) == Tri::True, "true or unknown is true");
    Check(evaluate(any(F, U), partial) == Tri::Unknown, "false or unknown is unknown");
    Check(evaluate("{\"not\":" + T + "}", partial) == Tri::False, "not true");
    Check(evaluate("{\"not\":" + F + "}", partial) == Tri::True, "not false");
    Check(evaluate("{\"not\":" + U + "}", partial) == Tri::Unknown, "not unknown stays unknown");

    // ---- Activation: fail closed unless the author opts out ---------------
    Check(active(T, partial, "inactive"), "true activates");
    Check(!active(F, partial, "active"), "false deactivates even when unavailable means active");
    Check(!active(U, partial, "inactive"), "unknown is inactive by default");
    Check(active(U, partial, "active"), "unknown is active when the author opted in");
    const std::set<std::string> fewer = {"in_combat"};
    Check(!active(T, partial, "inactive", fewer, "[\"health_percent\"]"),
        "a required predicate the evaluator lacks makes the target unavailable");
    Check(active(T, partial, "active", fewer, "[\"health_percent\"]"),
        "unavailable follows on_unavailable when a required predicate is missing");
    Check(active(T, partial, "inactive", fewer, "[\"in_combat\"]"), "a supported requirement is fine");

    // ---- The loader is strict ---------------------------------------------
    auto wrap = [](const std::string& condition) {
        return "{\"spec\":1,\"conditions\":[" + condition + "]}";
    };
    const std::string good = "{\"id\":\"a\",\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":{\"in_combat\":true}}";
    {
        ConditionSet set;
        std::string error;
        Check(set.Load(wrap(good), error) && set.Items().size() == 1 && set.Find("passive", "p") != nullptr
                && set.Find("ability", "p") == nullptr,
            "a valid document loads and targets can be found");
    }
    Check(rejects("{\"spec\":2,\"conditions\":[]}", "spec"), "wrong spec version");
    Check(rejects("{\"spec\":1,\"conditions\":[],\"extra\":1}", "unknown top-level"), "unknown top-level key");
    Check(rejects("{\"spec\":1,\"spec\":1,\"conditions\":[]}", "duplicate"), "duplicate JSON key");
    Check(rejects("{\"spec\":1,\"conditions\":[]} x", "after the document"), "trailing text");
    Check(rejects("{\"spec\":1,\"conditions\":[", "unexpected end"), "truncated document");
    Check(rejects("{\"spec\":1,\"conditions\":\"no\"}", "conditions"), "conditions must be a list");
    Check(rejects(wrap(good + "," + good), "duplicate condition id"), "duplicate condition id");
    Check(rejects(wrap(good + "," + std::string("{\"id\":\"b\",\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":{\"in_combat\":false}}")),
        "same target"), "two conditions for one target");
    Check(rejects(wrap("{\"id\":\"a\",\"target\":{\"kind\":\"skill\",\"id\":\"p\"},\"when\":{\"in_combat\":true}}"),
        "target"), "reserved target kind");
    Check(rejects(wrap("{\"id\":\"a\",\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":{\"in_combat\":true},"
        "\"on_unavailable\":\"maybe\"}"), "on_unavailable"), "bad on_unavailable");
    Check(rejects(wrap("{\"id\":\"a\",\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":{\"in_combat\":true,\"shield\":true}}"),
        "exactly one key"), "two keys in an expression");
    Check(rejects(wrap("{\"id\":\"a\",\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":{\"all\":[]}}"),
        "1 to 32"), "empty all");
    {
        std::string deep = "{\"in_combat\":true}";
        for (int level = 0; level < 9; ++level) deep = "{\"not\":" + deep + "}";
        Check(rejects(wrap("{\"id\":\"a\",\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":" + deep + "}"),
            "deeper than 8"), "expression depth limit");
        std::string leaves = "{\"all\":[";
        for (int index = 0; index < 32; ++index) leaves += (index ? "," : "") + std::string("{\"in_combat\":true}");
        leaves += "]}";
        const std::string wide = "{\"all\":[" + leaves + "," + leaves + "]}";
        Check(rejects(wrap("{\"id\":\"a\",\"target\":{\"kind\":\"passive\",\"id\":\"p\"},\"when\":" + wide + "}"),
            "64 nodes"), "expression node limit");
        std::string nested = "[";
        for (int level = 0; level < 40; ++level) nested += "[";
        Check(rejects(nested, ""), "JSON nesting limit");
    }
    Check(rejects(std::string(70 * 1024, ' '), "64 KiB"), "document size limit");
    Check(rejects("{\"spec\":1,\"conditions\":[{\"id\":\"\\u00zz\"}]}", ""), "bad unicode escape");
    Check(rejects("{\"spec\":1,\"conditions\":[{\"id\":\"a\\q\"}]}", ""), "bad escape");

    // ---- The example document shipped in docs/ -----------------------------
    if (examplePath != nullptr)
    {
        std::ifstream file(examplePath, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        ConditionSet set;
        std::string error;
        Check(set.Load(text, error) && set.Items().size() == 3, "the example document loads");
        const Condition* harvest = set.Find("passive", "arendeth_runic_magic_charge_harvest");
        Check(harvest != nullptr, "the example's first target is found");
        if (harvest != nullptr)
        {
            Snapshot wounded;
            wounded.healthPercent = 30.0;
            wounded.weaponClass = std::string("Unarmed");
            Check(IsActive(set, *harvest, wounded), "wounded and unarmed activates Charge Harvest");
            wounded.healthPercent = 80.0;
            Check(!IsActive(set, *harvest, wounded), "healthy does not");
            wounded.healthPercent = 30.0;
            wounded.weaponClass = std::string("Sword");
            Check(!IsActive(set, *harvest, wounded), "armed does not");
            Check(!IsActive(set, *harvest, Snapshot{}), "an empty snapshot fails closed");
        }
    }

    if (failures == 0)
    {
        std::cout << "PASS: skill-conditions evaluator (strict loader, 18 predicates, three-valued logic, fail-closed)\n";
    }
    return failures;
}
