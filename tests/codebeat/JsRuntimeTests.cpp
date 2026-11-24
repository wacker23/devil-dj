// P5-07: the quickjs-ng sandbox - limits, no file access, the embedded library.

#include "codebeat/JsRuntime.h"
#include "codebeat/BigStack.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <set>
#include <string>

using devildj::codebeat::JsLimits;
using devildj::codebeat::JsResult;
using devildj::codebeat::JsRuntime;
using devildj::testing::onBigStack;

namespace
{
double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The tests run on Catch2's main thread (1 MB stack on Windows): a smaller JS stack keeps them inside it.
JsLimits testLimits()
{
    JsLimits l;
    l.stackBytes = 512 * 1024; // Debug builds use more C stack per JS call
    return l;
}
} // namespace

TEST_CASE("JS runtime: scripts give their completion value; errors their place", "[codebeat][js]")
{
    JsRuntime js(testLimits());
    auto r = js.evalScript("const a = 20; a + 22");
    REQUIRE(r.ok);
    CHECK(r.value == "42");
    CHECK(js.evalScript("[1, 2].map(x => x * 2).join(' ')").value == "2 4");
    // Globals persist between evaluations.
    REQUIRE(js.evalScript("globalThis.kept = 'yes'").ok);
    CHECK(js.evalScript("kept").value == "yes");

    // A syntax error: its message and line / column.
    r = js.evalScript("let x = 1;\nlet y = ;\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("SyntaxError") == 0);
    CHECK(r.line == 2);
    CHECK(r.column > 0);
    // A runtime error inside a function.
    r = js.evalScript("function f() {\n  return nope.x;\n}\nf()", "buffer.js");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("ReferenceError") == 0);
    CHECK(r.line == 2);
    // A thrown value that isn't an Error.
    r = js.evalScript("throw 'plain'");
    CHECK_FALSE(r.ok);
    CHECK(r.error == "plain");
}

TEST_CASE("JS runtime: no std / os / timers / require - nothing outside the sandbox", "[codebeat][js]")
{
    JsRuntime js(testLimits());
    for (const char* name : {"std", "os", "require", "process", "fetch", "setTimeout", "setInterval", "XMLHttpRequest",
                             "WebSocket", "Deno", "Bun", "scriptArgs"})
        CHECK(js.evalScript(std::string("typeof ") + name).value == "undefined");

    // Modules: only the embedded library resolves - no system modules, no paths, no files.
    for (const char* spec : {"os", "std", "qjs:os", "C:/Windows/win.ini", "../../../../Windows/win.ini", "./../mini.js",
                             "/etc/passwd", "tests/mini.test.js", "fraction"})
    {
        const auto r = js.evalModule(std::string("import * as m from '") + spec + "';");
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("no module named") != std::string::npos);
    }
    // Dynamic import is refused as well.
    const auto r = js.evalModule("await import('os');");
    CHECK_FALSE(r.ok);

    // A file next to the program can't be read either: there is no API for it.
    CHECK(js.evalScript("typeof readFile === 'undefined' && typeof open === 'undefined' && typeof loadFile === 'undefined'").value == "true");
}

TEST_CASE("JS runtime: an endless loop is stopped by the budget, the runtime goes on", "[codebeat][js]")
{
    JsRuntime js(testLimits());
    auto t0 = std::chrono::steady_clock::now();
    auto r = js.evalScript("for (;;) {}");
    const double ms = msSince(t0);
    CHECK_FALSE(r.ok);
    CHECK(r.timedOut);
    CHECK(r.error == "too slow: stopped after 200 ms");
    CHECK(ms >= 190.0);
    CHECK(ms < 1000.0);

    // try / catch can't hold on to it.
    r = js.evalScript("let caught = false; try { while (true) {} } catch (e) { caught = true; } caught");
    CHECK(r.timedOut);

    // Still usable afterwards.
    r = js.evalScript("1 + 1");
    REQUIRE(r.ok);
    CHECK(r.value == "2");

    // A query (a function call) has the 5 ms budget.
    REQUIRE(js.evalScript("globalThis.spin = () => { for (;;) {} }; globalThis.twice = (x) => x * 2;").ok);
    t0 = std::chrono::steady_clock::now();
    r = js.callGlobal("spin");
    CHECK(r.timedOut);
    CHECK(msSince(t0) < 200.0);
    const double three[] = {3.0};
    r = js.callGlobal("twice", three);
    REQUIRE(r.ok);
    CHECK(r.value == "6");
    CHECK_FALSE(js.callGlobal("missing").ok);

    // An endless loop inside a module's evaluation too.
    r = js.evalModule("while (true) {}");
    CHECK(r.timedOut);
    CHECK(js.evalScript("'alive'").value == "alive");
}

TEST_CASE("JS runtime: the memory limit and the stack limit are errors, not crashes", "[codebeat][js]")
{
    JsLimits limits = testLimits();
    limits.memoryBytes = std::size_t{16} << 20;
    JsRuntime js(limits);
    auto r = js.evalScript("const keep = []; for (;;) keep.push(new Array(100000).fill(1.5));", "<code>", 10000.0);
    CHECK_FALSE(r.ok);
    CHECK(r.outOfMemory);
    CHECK_FALSE(r.timedOut);
    CHECK(js.memoryUsed() < limits.memoryBytes);
    // The heap is back to normal once the hog is gone.
    r = js.evalScript("[1, 2, 3].length");
    REQUIRE(r.ok);
    CHECK(r.value == "3");

    // Endless recursion: a RangeError.
    r = js.evalScript("function down(n) { return down(n + 1) + 1; } down(0)");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("RangeError") == 0);
    CHECK(js.evalScript("'still here'").value == "still here");
}

TEST_CASE("JS runtime: print() lines are collected for the console", "[codebeat][js]")
{
    JsRuntime js(testLimits());
    REQUIRE(js.evalScript("print('hello', 1, [2, 3]); print({}); for (let i = 0; i < 1500; ++i) print(i)").ok);
    const auto lines = js.takePrinted();
    REQUIRE(lines.size() == 1000); // the oldest dropped
    CHECK(lines.front() == "500"); // 1502 lines printed: the first 502 dropped
    CHECK(lines.back() == "1499");
    CHECK(js.takePrinted().empty());
    REQUIRE(js.evalScript("print('hello', 1, [2, 3])").ok);
    CHECK(js.takePrinted() == std::vector<std::string>{"hello 1 2,3"});
}

TEST_CASE("JS runtime: the pattern language is embedded and runs inside QuickJS", "[codebeat][js]")
{
    // Every library file is embedded (and nothing from the tests folder).
    std::set<std::string> expected;
    for (const auto& f : std::filesystem::directory_iterator(std::filesystem::path(DEVILDJ_SOURCE_DIR) / "js" / "codebeat"))
        if (f.path().extension() == ".js")
            expected.insert(f.path().filename().string());
    const auto names = JsRuntime::libraryModules();
    CHECK(std::set<std::string>(names.begin(), names.end()) == expected);
    CHECK(expected.count("mini.js") == 1);

    // Results are gathered on the worker thread and checked here (Catch2 assertions stay on the main thread).
    JsResult r, parseError;
    std::string out;
    onBigStack([&]
    {
        JsRuntime js; // the default limits
        r = js.evalModule(R"js(
            import { F } from "fraction.js";
            import { mini } from "./mini.js";
            import { s, note } from "controls.js";
            import { sine } from "signals.js";
            globalThis.out = [
                F(1, 3).add(F(1, 6)).toString(),
                mini("kick [~ snare] hat(3,8)").show().join("; "),
                s("hat*4").gain(sine.range(0.5, 1)).onsets(0, 1).length,
                note("c3 e3").add(12).show().join("; "),
                mini("a b").every(2, (x) => x.rev()).show(0).join("; "),
            ].join(" | ");
        )js");
        out = js.evalScript("out").value;
        parseError = js.evalModule("import { mini } from 'mini.js'; mini('a b)');");
    });
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(out.find("1/2 | ") == 0);
    CHECK(out.find("s:kick [0, 1/3)") == std::string::npos); // mini() alone gives plain values
    CHECK(out.find("kick [0, 1/3)") != std::string::npos);
    CHECK(out.find("snare [1/2, 2/3)") != std::string::npos);
    CHECK(out.find("| 4 |") != std::string::npos);
    CHECK(out.find("note:60 [0, 1/2); note:64 [1/2, 1)") != std::string::npos);
    CHECK(out.find("b [0, 1/2); a [1/2, 1)") != std::string::npos);

    // A parse error from the library reaches C++ with its message.
    CHECK_FALSE(parseError.ok);
    CHECK(parseError.error.find("did you forget") != std::string::npos);
}
