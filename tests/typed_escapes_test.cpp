// MacroQuest's typed escapes (mq/typed_escapes.h) and /echo, which writes with them: the player's "\ay" as a colour
// code, the BEL a ${...} may have brought into a typed line taken out first.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "mq/commands.h"
#include "mq/tlo.h"
#include "mq/typed_escapes.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"

using hailstone::mq::typed_escapes;
using hailstone::mq::typed_line;

TEST_CASE("typed escapes: \\a is a colour code, \\n a line break, \\\\ a backslash") {
    CHECK(typed_escapes("\\ayHello\\ax there") == "\ayHello\ax there");
    CHECK(typed_escapes("one\\ntwo") == "one\ntwo");
    CHECK(typed_escapes("C:\\\\path") == "C:\\path");
    CHECK(typed_escapes("ends with \\") == "ends with "); // MQ drops a lone backslash at the end
    // The other letters (MQ's control characters) and the upper case stay as typed.
    CHECK(typed_escapes("\\t\\b\\A") == "\\t\\b\\A");
    CHECK(typed_escapes("no escapes") == "no escapes");
}

TEST_CASE("typed escapes: a line no script sent loses its own BEL before the player's escapes become codes") {
    // "\a" + "rServerName": what a ${...} could have brought in; "\\ag": what the player typed.
    CHECK(typed_line("\arServerName \\agok", false) == "rServerName \agok");
    // A script's line keeps its codes (C#'s and C's "\a" is the BEL) and takes the escapes too.
    CHECK(typed_line("\arScript \\agok", true) == "\arScript \agok");
}

TEST_CASE("/echo: the player's escapes as colour codes, through the coded echo") {
    hailstone::plugin::Hooks hooks;
    hailstone::mq::Registry reg;
    hailstone::mq::Context ctx;
    ctx.registry = &reg;
    std::vector<std::string> plain, coded;
    hailstone::mq::CommandEnv ce;
    ce.hooks = &hooks;
    ce.registry = &reg;
    ce.context = [&ctx]() -> const hailstone::mq::Context& { return ctx; };
    ce.echo = [&plain](const std::string& s) { plain.push_back(s); };
    ce.echo_coded = [&coded](const std::string& s) { coded.push_back(s); };
    hailstone::mq::Commands commands(ce);
    auto run = [&](std::string line) { hooks.command(line); };
    run("/echo \\ayHello\\ax world");
    REQUIRE(coded.size() == 1);
    CHECK(coded.back() == "\ayHello\ax world");
    CHECK(plain.empty());
    // A BEL already in the player's line (a server's text through ${...}) does not colour.
    run("/echo \arname \\agok");
    CHECK(coded.back() == "rname \agok");
    // A script's line keeps its own codes.
    {
        hailstone::plugin::CallerScope scope(hailstone::plugin::kCapAll, "echo-test");
        run("/echo \arscript");
    }
    CHECK(coded.back() == "\arscript");
}
