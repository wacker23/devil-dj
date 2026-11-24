#pragma once

// The Code Beat JavaScript sandbox (P5-07, CODEBEAT_SPEC §8.2): one quickjs-ng runtime and context.
//
// - Memory is capped (64 MB by default) and so is the JS stack; running out is a normal error, not a crash.
// - Every evaluation has a time budget (eval 200 ms, query 5 ms by default). An interrupt handler stops code that
//   runs over - an endless loop included, even inside try/catch - and the runtime stays usable.
// - No std / os modules, no require, no timers, no file or network access: `import` resolves only the Code Beat
//   library modules embedded in the program (js/codebeat/*.js), by name.
// - print(...) collects lines for the console panel.
//
// Thread-confined: create it and use it on one thread (the code deck's, P5-09). That thread needs a stack of at
// least stackBytes + 1 MB, since the limit counts from where each call starts. Never on the audio thread.
// Debug builds use much bigger C stack frames per JS call, so their default limit is 4 MB (1 MB in Release, §8.2).

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace devildj::codebeat
{

struct JsLimits
{
    std::size_t memoryBytes = std::size_t{64} << 20;
#ifdef NDEBUG
    std::size_t stackBytes = std::size_t{1} << 20;
#else
    std::size_t stackBytes = std::size_t{4} << 20;
#endif
    double evalMs = 200.0;
#ifdef NDEBUG
    double queryMs = 5.0;
#else
    double queryMs = 50.0; // unoptimised QuickJS is ~10x slower
#endif
};

struct JsResult
{
    bool ok = false;
    std::string value;        // String(result) when ok
    std::string error;        // e.g. "SyntaxError: unexpected token in expression: ';'" when not ok
    int line = 0, column = 0; // where the error is in the evaluated source, 1-based (0 = unknown)
    bool timedOut = false;    // stopped by the time budget
    bool outOfMemory = false; // stopped by the memory limit
    int sourceIndex = -1;     // the error's own character offset in the buffer, when it has one (a MiniError)
};

class JsRuntime
{
public:
    explicit JsRuntime(JsLimits limits = {});
    ~JsRuntime();
    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    // A script (global code): its completion value. Budget: limits().evalMs unless given. With `keepAs`, the value
    // itself is also stored as that global (for the next call to use).
    JsResult evalScript(std::string_view code, std::string_view fileName = "<code>", double budgetMs = 0.0,
                        std::string_view keepAs = {});
    // A module (may import the library: import { mini } from "mini.js"); its value is "undefined" when it ran.
    JsResult evalModule(std::string_view code, std::string_view fileName = "<module>", double budgetMs = 0.0);
    // Calls a global function with numbers. Budget: limits().queryMs unless given.
    JsResult callGlobal(std::string_view function, std::span<const double> args = {}, double budgetMs = 0.0);

    std::vector<std::string> takePrinted();  // print() lines since the last call (at most 1000 kept)
    std::size_t memoryUsed() const;          // bytes the JS heap holds now
    const JsLimits& limits() const noexcept;

    // The embedded library modules, by name ("fraction.js", "mini.js", ...).
    static std::vector<std::string> libraryModules();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace devildj::codebeat
