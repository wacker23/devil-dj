// matcher_eval (P6-13; MATCHER_SPEC §10): the Track Matcher evaluated on the owner's own history.
//
//   matcher_eval [--library <library.db>] [--out <report.md>] [--no-tune] [--write-presets]
//
//   --library        default %APPDATA%\Devil-DJ\library.db (the app's)
//   --out            default docs\reports\matcher-<date>.md (under the current folder)
//   --no-tune        skip the tuned-weights suggestion
//   --write-presets  add (or replace) "Tuned" in Documents\Devil-DJ\Matcher\presets.json; the app reads it at start
//
// Reads the library only (opening it brings an older schema up to date, as the app would). Prints a summary.

#include "library/Connection.h"
#include "library/Database.h"
#include "matcher/Evaluation.h"
#include "matcher/Ranker.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

using namespace devildj;
namespace fs = std::filesystem;

namespace
{
fs::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

std::string today()
{
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    return buffer;
}

int usage()
{
    std::cerr << "usage: matcher_eval [--library <library.db>] [--out <report.md>] [--no-tune] [--write-presets]\n";
    return 2;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    fs::path library = knownFolder(FOLDERID_RoamingAppData) / "Devil-DJ" / "library.db";
    fs::path out = fs::path("docs") / "reports" / ("matcher-" + today() + ".md");
    bool tune = true, writePresets = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        if (a == L"--library" && i + 1 < argc)
            library = argv[++i];
        else if (a == L"--out" && i + 1 < argc)
            out = argv[++i];
        else if (a == L"--no-tune")
            tune = false;
        else if (a == L"--write-presets")
            writePresets = true;
        else
            return usage();
    }
    if (!fs::exists(library))
    {
        std::wcerr << L"No library at " << library.wstring() << L"\n";
        return 1;
    }
    auto db = library::Database::open(library);
    if (!db)
    {
        std::cerr << "Can't open the library: " << db.error() << "\n";
        return 1;
    }
    auto reader = std::move(db.value()->openReader()).value();
    const auto t0 = std::chrono::steady_clock::now();

    const auto data = matcher::loadEvalData(*reader);
    std::vector<matcher::ReplayCase> cases;
    std::set<std::int64_t> sessions;
    for (const auto& t : data.transitions)
    {
        cases.push_back(matcher::replayCase(data, t));
        sessions.insert(t.session);
    }
    matcher::EvalReport report;
    report.date = today();
    report.tracks = static_cast<int>(data.tracks.size());
    report.transitions = static_cast<int>(data.transitions.size());
    report.sessions = static_cast<int>(sessions.size());
    report.personal = matcher::loadPersonalModel(*reader);
    report.choices = matcher::choiceStats(*reader);
    const auto presets = matcher::defaultPresets();
    for (const auto& set : matcher::weightSets(presets, report.personal))
        report.results.push_back({set.name, matcher::evaluate(cases, set.weights)});
    if (tune && !cases.empty())
    {
        report.tuned = matcher::tuneWeights(cases, presets.front().weights);
        report.tunedMetrics = matcher::evaluate(cases, *report.tuned);
    }

    fs::create_directories(out.parent_path().empty() ? fs::path(".") : out.parent_path());
    std::ofstream(out, std::ios::binary) << matcher::reportMarkdown(report);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d tracks, %d transitions in %d sessions, %d logged choices (%.1f s)\n", report.tracks,
                report.transitions, report.sessions, report.choices.impressions, seconds);
    for (const auto& [name, m] : report.results)
        if (name.find(" - ") == std::string::npos) // the presets and the personal ranker (ablations: the report)
            std::printf("  %-40s HitRate@10 %.3f  MRR %.3f\n", name.c_str(), m.hitRate(10), m.mrr());
    if (report.tuned)
        std::printf("  %-40s HitRate@10 %.3f  MRR %.3f\n", "Tuned (in-sample)", report.tunedMetrics.hitRate(10),
                    report.tunedMetrics.mrr());
    std::wcout << L"Report: " << fs::absolute(out).wstring() << L"\n";

    if (writePresets)
    {
        if (!report.tuned)
        {
            std::cerr << "Nothing tuned (no transitions yet): presets.json left alone.\n";
            return 1;
        }
        const auto file = knownFolder(FOLDERID_Documents) / "Devil-DJ" / "Matcher" / "presets.json";
        fs::create_directories(file.parent_path());
        std::vector<matcher::Preset> list = presets;
        if (fs::exists(file))
        {
            std::stringstream text;
            text << std::ifstream(file).rdbuf();
            list = matcher::presetsFromJson(text.str());
        }
        list.erase(std::remove_if(list.begin(), list.end(), [](const auto& p) { return p.name == "Tuned"; }), list.end());
        list.push_back({"Tuned", *report.tuned});
        std::ofstream(file, std::ios::binary) << matcher::presetsToJson(list);
        std::wcout << L"Presets: " << file.wstring() << L" (\"Tuned\" added; restart Devil-DJ)\n";
    }
    return 0;
}
