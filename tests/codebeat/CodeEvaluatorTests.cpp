// P5-08: evaluating user code - good code becomes the pattern, bad code is reported and never stops the music.

#include "codebeat/BigStack.h"
#include "codebeat/CodeEvaluator.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
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

TEST_CASE("Code evaluation: exact places for errors in and after rewritten strings (P5-15)", "[codebeat][eval]")
{
    const auto steps = runAll({
        "s(\"bd\")\n  .nope()",          // a TypeError at line 2: no string there
        "1;\ns(\"bd\").gain(\"0.5\").nope()", // line 2, after two rewritten strings: column of ".nope" as typed
        "stack(\n  s(\"bd [sd\")\n)",      // a mini-notation error: its own character -> line 2, column 12
        "s(\"bd sd\").show",              // fine: a string still works as a string elsewhere
        "\"bd ~ sd\".split(\" \").length > 0 ? s(\"bd\") : s(\"sd\")",
    });
    REQUIRE(steps.size() == 5);
    CHECK(steps[0].result.line == 2);
    CHECK(steps[1].result.line == 2);
    // Line 2 as typed is s("bd").gain("0.5").nope(): ".nope()" is columns 20-26. Without mapping the pre-tokenizer's
    // insertions back out it would be more than 20 columns further right.
    CHECK(steps[1].result.column >= 20);
    CHECK(steps[1].result.column <= 26);
    CHECK(steps[2].result.error.find("missing \"]\"") != std::string::npos);
    CHECK(steps[2].result.line == 2);
    CHECK(steps[2].result.column == 9); // the "[" of `  s("bd [sd")`: its own character, exactly
    CHECK(steps[4].result.ok);
}

// Query cost regression (performance pass after P6): the producer gives every ~25 ms window 5 ms (Release) and
// drops the window's sound past it. A 7-part pattern queried window by window over 32 bars, in QuickJS: the mean
// was ~2.4 ms on the dev machine before the pass (whole-number and same-denominator Fraction fast paths, no
// re-checks of exact results, timecat asking only the steps near the query), ~1.1 ms after. Three passes, the
// fastest judged: a busy machine only makes passes slower, so the best one is the code's own cost.
TEST_CASE("Code evaluator: a 7-part pattern's 25 ms queries stay well inside the query budget", "[codebeat][perf]")
{
#ifdef NDEBUG
    constexpr bool kDebugBuild = false;
#else
    constexpr bool kDebugBuild = true;
#endif
    if (kDebugBuild)
        SKIP("timing: Release only (unoptimised QuickJS is ~10x slower and has its own budget)");
    const std::string code = R"(setbpm(144)
play("kick", s("bd ~ ~ bd ~ ~ bd ~ bd ~ bd ~ ~ bd ~ ~").gain(1.05))
play("clap", s("~ cp ~ cp").room(0.25))
play("hats", s("hh*8").gain("0.5 0.3 0.6 0.3").off(1/16, x => x.gain(0.2)))
play("perc", s("rim*16").euclid(5, 16, 2).speed(1.3).pan(sine.range(0.2, 0.8)).gain(0.45))
play("stab", note("<[a3,c4,e4] [f3,a3,c4] [g3,b3,d4] [e3,g3,b3]>").s("supersaw").struct("~ x ~ ~ x ~ x ~")
  .decay(0.12).sustain(0).lpf(3200).gain(0.45))
play("bass", n("0 ~ 0 [3 ~] 0 ~ 5 ~").scale("A1:minor").s("subbass").legato(0.8).gain(0.8))
play("lead", n("<[7 9 10 9] [7 ~ 5 4]>").scale("A4:minor").s("fm").every(4, x => x.ply(2)).delay(0.3).gain(0.3))
)";
    double mean = 1.0e9, worst = 0.0;
    int slow = 0, windows = 0, events = 0;
    onBigStack([&]
    {
        devildj::codebeat::JsLimits limits;
        limits.queryMs = 1000.0; // measure every window instead of stopping the slow ones
        CodeEvaluator ev(limits);
        REQUIRE(ev.evaluate(code, true).ok);
        constexpr int kPerBar = 67; // 1.667 s bars at 144 BPM in ~25 ms windows
        for (int pass = 0; pass < 3; ++pass)
        {
            double total = 0.0, passWorst = 0.0;
            int passSlow = 0, passWindows = 0, passEvents = 0;
            for (int w = kPerBar; w < kPerBar * 33; ++w)
            {
                const double range[] = {double(w), double(kPerBar), double(w + 1), double(kPerBar)};
                const auto t0 = std::chrono::steady_clock::now();
                const auto r = ev.runtime().callGlobal("__query", range);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                REQUIRE(r.ok);
                for (const char c : r.value)
                    passEvents += c == '\n' ? 1 : 0;
                passEvents += r.value.empty() ? 0 : 1;
                total += ms;
                passWorst = std::max(passWorst, ms);
                passSlow += ms > 5.0 ? 1 : 0;
                ++passWindows;
            }
            events = passEvents; // the same every pass
            if (total / passWindows < mean)
            {
                mean = total / passWindows;
                worst = passWorst;
                slow = passSlow;
                windows = passWindows;
            }
        }
    });
    CAPTURE(windows, worst, slow);
    CHECK(events == 1520); // bars 1-32 queried whole give 1520 onsets: each window has its share, none twice
    CHECK(mean < 1.8); // the old engine: 2.03 at this machine's full speed; this one 1.1-1.3 by power state
    CHECK(slow * 100 <= windows); // GC pauses aside, no window near the 5 ms budget
}
