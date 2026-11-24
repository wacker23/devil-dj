// P5-08: evaluating user code - good code becomes the pattern, bad code is reported and never stops the music.

#include "codebeat/BigStack.h"
#include "codebeat/CodeEvaluator.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using devildj::codebeat::CodeEvaluator;
using devildj::codebeat::DjHooks;
using devildj::codebeat::EvalResult;
using devildj::testing::onBigStack;

namespace
{
struct Step
{
    EvalResult result;
    std::string playing; // the active pattern's events in cycle 0 after this step
};

// Evaluates each code in turn on one evaluator (on a big-stack thread) and records what happened.
std::vector<Step> runAll(const std::vector<std::string>& codes, const DjHooks* dj = nullptr)
{
    std::vector<Step> steps;
    std::string startup;
    onBigStack([&]
    {
        CodeEvaluator ev;
        startup = ev.startupError();
        if (dj != nullptr)
            ev.setDj(*dj);
        for (const auto& code : codes)
        {
            Step s;
            s.result = ev.evaluate(code);
            s.playing = ev.show(0.0, 1.0);
            steps.push_back(std::move(s));
        }
    });
    INFO(startup);
    REQUIRE(startup.empty());
    return steps;
}
} // namespace

TEST_CASE("Code evaluation: good code becomes the playing pattern", "[codebeat][eval]")
{
    const auto steps = runAll({
        R"(s("bd sd"))",
        "const hats = s(\"hh*4\").gain(0.5);\nhats",
        "const hats = s(\"cp\");\nhats", // const again: each run is its own block
        R"(n("0 2 4").scale("C:minor").s("saw"))",
        R"(play("drums", s("bd*2")); play("hats", s("hh*2")))",
    });
    REQUIRE(steps.size() == 5);
    for (const auto& s : steps)
    {
        INFO(s.result.error);
        CHECK(s.result.ok);
        CHECK(s.result.playing);
        CHECK(s.result.changed);
    }
    CHECK(steps[0].playing == "s:bd [0, 1/2)\ns:sd [1/2, 1)");
    CHECK(steps[1].playing.find("s:hh gain:0.5 [0, 1/4)") == 0);
    CHECK(steps[2].playing == "s:cp [0, 1)");
    CHECK(steps[3].playing == "note:48 s:saw [0, 1/3)\nnote:51 s:saw [1/3, 2/3)\nnote:55 s:saw [2/3, 1)");
    CHECK(steps[4].result.slots == std::vector<std::string>{"drums", "hats"});
    CHECK(steps[4].playing == "s:bd [0, 1/2)\ns:hh [0, 1/2)\ns:bd [1/2, 1)\ns:hh [1/2, 1)");
}

TEST_CASE("Code evaluation: bad code is reported with its place, the last good pattern keeps playing", "[codebeat][eval]")
{
    const auto steps = runAll({
        R"(s("bd sd"))",
        "const a = 1;\nlet b = ;\n",                // syntax error, line 2
        "s(\"bd\")\n  .gain(0.5)\n  .nope()",       // TypeError at line 3
        R"(s("bd [sd"))",                           // mini-notation error
        R"(s("bd").gain("loud"))",                  // only shows when queried: caught by the test query
        "s(\"bd\").every(2, (x) =>\n  x.oops())",   // a broken function inside the pattern
        "for (;;) {}",                              // too slow
        "throw 'plain'",
    });
    REQUIRE(steps.size() == 8);
    CHECK(steps[0].result.ok);
    for (std::size_t i = 1; i < steps.size(); ++i)
    {
        INFO("step " << i << ": " << steps[i].result.error);
        CHECK_FALSE(steps[i].result.ok);
        CHECK_FALSE(steps[i].result.error.empty());
        CHECK(steps[i].playing == "s:bd [0, 1/2)\ns:sd [1/2, 1)"); // unchanged
    }
    CHECK(steps[1].result.error.find("SyntaxError") == 0);
    CHECK(steps[1].result.line == 2);
    CHECK(steps[2].result.error.find("TypeError") == 0);
    CHECK(steps[2].result.line == 3);
    CHECK(steps[3].result.error.find("missing \"]\"") != std::string::npos);
    CHECK(steps[3].result.line == 1);
    CHECK(steps[4].result.error.find("gain needs a number, got \"loud\"") != std::string::npos);
    CHECK(steps[5].result.error.find("TypeError") == 0);
    CHECK(steps[5].result.line == 2);
    CHECK(steps[6].result.timedOut);
    CHECK(steps[7].result.error == "plain");
}

TEST_CASE("Code evaluation: columns on the first line are the user's own", "[codebeat][eval]")
{
    // The code is wrapped as "{" + code: a first-line error must not be one column off.
    const auto steps = runAll({"nope()", "  nope()"});
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].result.line == 1);
    CHECK(steps[0].result.column == 1);
    CHECK(steps[1].result.column == 3);
}

TEST_CASE("Code evaluation: hush, mutes, code without a pattern, setbpm / setcpb", "[codebeat][eval]")
{
    const auto steps = runAll({
        R"(play("drums", s("bd*2")); play("hats", s("hh*2")))",
        R"(mute("hats"))",
        "const x = 2 + 2;",
        "setbpm(128); setcpb(3);",
        "hush()",
        "setbpm(999)",
    });
    REQUIRE(steps.size() == 6);
    CHECK(steps[1].result.ok);
    CHECK(steps[1].result.muted == std::vector<std::string>{"hats"});
    CHECK(steps[1].playing == "s:bd [0, 1/2)\ns:bd [1/2, 1)");
    CHECK(steps[2].result.ok);
    CHECK_FALSE(steps[2].result.changed);
    CHECK(steps[2].result.message.find("the last one keeps playing") != std::string::npos);
    CHECK(steps[2].playing == steps[1].playing);
    CHECK(steps[3].result.ok);
    REQUIRE(steps[3].result.bpm.has_value());
    CHECK(*steps[3].result.bpm == 128.0);
    REQUIRE(steps[3].result.cpb.has_value());
    CHECK(*steps[3].result.cpb == 3.0);
    CHECK(steps[4].result.ok);
    CHECK_FALSE(steps[4].result.playing);
    CHECK(steps[4].playing.empty());
    CHECK_FALSE(steps[5].result.ok);
    CHECK(steps[5].result.error.find("20 to 300 BPM") != std::string::npos);
}

TEST_CASE("Code evaluation: DJ hooks and print()", "[codebeat][eval]")
{
    DjHooks dj;
    dj.decks[0].bpm = 126.0;
    dj.decks[0].keyName = "8A \"minor\""; // quotes survive the trip into JS
    dj.decks[0].playing = true;
    dj.decks[0].bar = 17;
    dj.masterBpm = 126.0;
    const auto steps = runAll({
        "print(deckA.bpm, deckA.keyName, deckA.bar, deckA.playing, deckB.bpm, master.bpm);\n"
        "s(\"bd\").fast(deckA.bpm > 120 ? 2 : 1)",
    }, &dj);
    REQUIRE(steps.size() == 1);
    INFO(steps[0].result.error);
    REQUIRE(steps[0].result.ok);
    CHECK(steps[0].result.printed == std::vector<std::string>{"126 8A \"minor\" 17 true 0 126"});
    CHECK(steps[0].playing == "s:bd [0, 1/2)\ns:bd [1/2, 1)");
}
