#pragma once

// Evaluating the user's code (P5-08, CODEBEAT_SPEC §5.7): the prelude (js/codebeat/prelude.js) puts the language on
// globalThis; each evaluation runs the whole buffer as a script whose completion value - or its play(...) calls - is
// the new pattern. The new pattern is test-queried before it replaces the playing one, so code with an error (a
// syntax error, a thrown exception, a bad control value) never stops the music.
//
//   s("bd sd").gain(0.8)                       the last value is the pattern
//   play("drums", s("bd*2")); play("hats", ...) named slots, all stacked; mute("hats") / unmute / hush()
//   setbpm(128); setcpb(3)                     requests the caller applies (setbpm only when the deck leads)
//   deckA.bpm, deckA.keyName, master.beat      DJ hooks, refreshed through setDj() before evaluating
//
// Each run is its own block, so `const x = ...` can be written again next time. Thread-confined like JsRuntime.

#include "codebeat/JsRuntime.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devildj::codebeat
{

struct DeckHook
{
    double bpm = 0.0;
    int key = -1;        // the app's key index, -1 = unknown
    std::string keyName; // as shown in the app ("8A", "Am")
    double beat = 0.0;
    int bar = 0, phrase = 0;
    bool playing = false;
};

struct DjHooks
{
    std::array<DeckHook, 4> decks;
    double masterBpm = 120.0;
    double masterBeat = 0.0;
};

struct EvalResult
{
    bool ok = false;
    std::string error;        // what went wrong, e.g. "SyntaxError: ..." or "column 4: unexpected ..."
    int line = 0, column = 0; // in the user's code, 1-based (0 = unknown)
    bool timedOut = false, outOfMemory = false;

    bool changed = false;              // a new pattern (or mutes) took effect
    bool playing = false;              // something is playing now
    std::vector<std::string> slots;    // the play() slots ("$0" ... for unnamed ones)
    std::vector<std::string> muted;
    std::optional<double> bpm, cpb;    // setbpm() / setcpb() in this code
    std::string message;               // a note ("no pattern in this code: the last one keeps playing")
    std::vector<std::string> printed;  // print() lines
};

class CodeEvaluator
{
public:
    explicit CodeEvaluator(JsLimits limits = {});

    bool ready() const noexcept { return startup.empty(); }
    const std::string& startupError() const noexcept { return startup; } // the prelude failed to load (a bug)

    EvalResult evaluate(std::string_view code);
    void setDj(const DjHooks& hooks);

    // The playing pattern's events over [begin, end) cycles, one per line ("s:bd [0, 1/2)"): tests and the console.
    std::string show(double begin, double end);
    double beatsPerCycle(); // setcpb(), 4 by default

    JsRuntime& runtime() noexcept { return js; }

private:
    JsRuntime js;
    std::string startup;
};

} // namespace devildj::codebeat
