#include "codebeat/CodeEvaluator.h"

#include <charconv>
#include <cstdio>

namespace devildj::codebeat
{

namespace
{
constexpr std::string_view kFileName = "<code>";

std::vector<std::string> splitList(std::string_view s)
{
    std::vector<std::string> out;
    while (!s.empty())
    {
        const auto comma = s.find(',');
        out.emplace_back(s.substr(0, comma));
        if (comma == std::string_view::npos)
            break;
        s.remove_prefix(comma + 1);
    }
    return out;
}

std::optional<double> number(std::string_view s)
{
    if (s.empty())
        return std::nullopt;
    double x = 0.0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), x);
    if (ec != std::errc{} || end != s.data() + s.size())
        return std::nullopt;
    return x;
}

// A string as a JS string literal.
std::string quoted(std::string_view s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
            out += buf;
        }
        else
            out += c;
    }
    return out + "\"";
}

std::string num(double x)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", x);
    return buf;
}

void takeError(const JsResult& r, EvalResult& out, bool inUserCode)
{
    out.ok = false;
    out.error = r.error;
    out.timedOut = r.timedOut;
    out.outOfMemory = r.outOfMemory;
    if (inUserCode && r.line > 0)
    {
        out.line = r.line;
        // The code runs wrapped as "{" + code + "\n}": the first line is one column further right.
        out.column = r.line == 1 && r.column > 1 ? r.column - 1 : r.column;
    }
}
} // namespace

CodeEvaluator::CodeEvaluator(JsLimits limits)
    : js(limits)
{
    // Loading the language takes longer than one evaluation's budget would allow on a slow machine: give it 2 s.
    const auto r = js.evalModule("import './prelude.js';", "<prelude>", 2000.0);
    if (!r.ok)
        startup = "the Code Beat language failed to load: " + r.error;
}

EvalResult CodeEvaluator::evaluate(std::string_view code)
{
    EvalResult out;
    if (!ready())
    {
        out.error = startup;
        return out;
    }
    js.callGlobal("__beginEval", {}, js.limits().evalMs);
    // Its own block: const / let can be declared again in the next run; the block's last value is the completion.
    std::string wrapped;
    wrapped.reserve(code.size() + 4);
    wrapped += '{';
    wrapped += code;
    wrapped += "\n}";
    const auto ran = js.evalScript(wrapped, kFileName, 0.0, "__completion");
    if (!ran.ok)
    {
        takeError(ran, out, true);
        js.callGlobal("__abortEval", {}, js.limits().evalMs);
        out.printed = js.takePrinted();
        return out;
    }
    // Work out the new pattern and test-query it (an error here comes from the user's pattern, so its place in the
    // user's code is reported when the stack has one).
    const auto ended = js.callGlobal("__endEval", {}, js.limits().evalMs);
    out.printed = js.takePrinted();
    if (!ended.ok)
    {
        takeError(ended, out, true);
        js.callGlobal("__abortEval", {}, js.limits().evalMs);
        return out;
    }
    out.ok = true;
    std::string_view reply = ended.value;
    while (!reply.empty())
    {
        const auto nl = reply.find('\n');
        const auto line = reply.substr(0, nl);
        reply = nl == std::string_view::npos ? std::string_view{} : reply.substr(nl + 1);
        const auto eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const auto key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "playing")
            out.playing = value == "1";
        else if (key == "changed")
            out.changed = value == "1";
        else if (key == "slots")
            out.slots = splitList(value);
        else if (key == "muted")
            out.muted = splitList(value);
        else if (key == "bpm")
            out.bpm = number(value);
        else if (key == "cpb")
            out.cpb = number(value);
        else if (key == "message")
            out.message = std::string(value);
    }
    return out;
}

void CodeEvaluator::setDj(const DjHooks& hooks)
{
    if (!ready())
        return;
    std::string s = "__setDj({decks:[";
    for (std::size_t i = 0; i < hooks.decks.size(); ++i)
    {
        const auto& d = hooks.decks[i];
        if (i > 0)
            s += ',';
        s += "{bpm:" + num(d.bpm) + ",key:" + std::to_string(d.key) + ",keyName:" + quoted(d.keyName) +
             ",beat:" + num(d.beat) + ",bar:" + std::to_string(d.bar) + ",phrase:" + std::to_string(d.phrase) +
             ",playing:" + (d.playing ? "true" : "false") + "}";
    }
    s += "],master:{bpm:" + num(hooks.masterBpm) + ",beat:" + num(hooks.masterBeat) + "}})";
    js.evalScript(s, "<dj>");
}

std::string CodeEvaluator::show(double begin, double end)
{
    const double range[] = {begin, end};
    const auto r = js.callGlobal("__show", range, js.limits().evalMs);
    return r.ok ? r.value : "error: " + r.error;
}

double CodeEvaluator::beatsPerCycle()
{
    const auto r = js.evalScript("__session.cpb", "<cpb>");
    return r.ok ? number(r.value).value_or(4.0) : 4.0;
}

} // namespace devildj::codebeat
