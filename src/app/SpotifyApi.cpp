#include "app/SpotifyApi.h"

#include <thread>

namespace devildj::app
{

namespace
{
SpotifyApi::Response httpGet(const juce::URL& url, const juce::String& bearer)
{
    SpotifyApi::Response r;
    juce::StringPairArray headers;
    auto stream = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                            .withExtraHeaders("Authorization: Bearer " + bearer + "\r\nAccept: application/json")
                                            .withConnectionTimeoutMs(15000)
                                            .withStatusCode(&r.status)
                                            .withResponseHeaders(&headers));
    if (stream == nullptr)
        return r;
    r.body = stream->readEntireStreamAsString();
    r.retryAfterSeconds = headers.getValue("Retry-After", "0").getIntValue();
    return r;
}
} // namespace

SpotifyApi::SpotifyApi(TokenSource tokens, juce::String apiBase)
    : get(httpGet)
    , sleepFor([](int s) { std::this_thread::sleep_for(std::chrono::seconds(s)); })
    , token(std::move(tokens))
    , base(std::move(apiBase))
{
}

util::Result<juce::var> SpotifyApi::getJson(const juce::String& url)
{
    using R = util::Result<juce::var>;
    juce::String error;
    auto bearer = token ? token(false, &error) : juce::String();
    if (bearer.isEmpty())
        return R::fail(error.isEmpty() ? std::string("not signed in to Spotify") : error.toStdString());
    bool refreshed = false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto r = get(juce::URL(url), bearer);
        if (r.status == 200)
        {
            juce::var json;
            if (juce::JSON::parse(r.body, json).failed())
                return R::fail("Spotify sent something that isn't JSON");
            return R::ok(json);
        }
        if (r.status == 429) // too many requests: wait as asked, then again
        {
            sleepFor(std::clamp(r.retryAfterSeconds, 1, 30));
            continue;
        }
        if (r.status == 401 && !refreshed) // the token went stale: one refresh
        {
            refreshed = true;
            bearer = token(true, &error);
            if (bearer.isEmpty())
                return R::fail(error.toStdString());
            continue;
        }
        if (r.status == 0)
            return R::fail("no connection to Spotify");
        juce::var json;
        juce::JSON::parse(r.body, json);
        const auto message = json.getProperty("error", {}).getProperty("message", "").toString();
        if (r.status == 403)
            return R::fail("Spotify refused it (403): an app in development mode may only read the tracks of playlists you "
                           "made or collaborate on - for others, copy the tracks into a playlist of your own in Spotify");
        return R::fail("Spotify: HTTP " + juce::String(r.status).toStdString() +
                       (message.isNotEmpty() ? " (" + message.toStdString() + ")" : std::string()));
    }
    return R::fail("Spotify kept asking to wait (too many requests): try again in a minute");
}

RemotePlaylist SpotifyApi::parsePlaylist(const juce::var& item)
{
    RemotePlaylist p;
    p.id = item.getProperty("id", "").toString().toStdString();
    p.name = item.getProperty("name", "").toString().toStdString();
    p.owner = item.getProperty("owner", {}).getProperty("display_name", "").toString().toStdString();
    // The count: "tracks": {"total": n} (or "items" in newer replies).
    const auto counts = item.getProperty("tracks", item.getProperty("items", {}));
    p.tracks = static_cast<int>(counts.getProperty("total", 0));
    return p;
}

bool SpotifyApi::parseTrack(const juce::var& entry, RemoteTrack& out)
{
    // A playlist entry holds its track as "track" (or "item" in newer replies); episodes and removed tracks skipped.
    const auto t = entry.getProperty("track", entry.getProperty("item", {}));
    if (!t.isObject() || t.getProperty("type", "track").toString() != "track")
        return false;
    out = {};
    out.title = t.getProperty("name", "").toString().toStdString();
    if (out.title.empty())
        return false;
    if (const auto* artists = t.getProperty("artists", {}).getArray())
        for (const auto& a : *artists)
            if (const auto n = a.getProperty("name", "").toString(); n.isNotEmpty())
                out.artists.push_back(n.toStdString());
    out.album = t.getProperty("album", {}).getProperty("name", "").toString().toStdString();
    out.seconds = static_cast<double>(t.getProperty("duration_ms", 0)) / 1000.0;
    out.isrc = t.getProperty("external_ids", {}).getProperty("isrc", "").toString().toStdString();
    return true;
}

util::Result<std::vector<RemotePlaylist>> SpotifyApi::playlists()
{
    using R = util::Result<std::vector<RemotePlaylist>>;
    std::vector<RemotePlaylist> out;
    juce::String next = base + "/v1/me/playlists?limit=50";
    for (int page = 0; next.isNotEmpty() && page < 100; ++page)
    {
        auto json = getJson(next);
        if (!json)
            return R::fail(json.error());
        if (const auto* items = json.value().getProperty("items", {}).getArray())
            for (const auto& item : *items)
                if (item.isObject())
                    out.push_back(parsePlaylist(item));
        next = json.value().getProperty("next", juce::var()).toString();
    }
    return R::ok(std::move(out));
}

util::Result<std::vector<RemoteTrack>> SpotifyApi::tracks(const std::string& playlistId)
{
    using R = util::Result<std::vector<RemoteTrack>>;
    std::vector<RemoteTrack> out;
    juce::String next = base + "/v1/playlists/" + juce::URL::addEscapeChars(juce::String(playlistId), false) + "/items?limit=100"; // "/tracks" before Spotify's 2026 change: refused (403) now
    for (int page = 0; next.isNotEmpty() && page < 200; ++page)
    {
        auto json = getJson(next);
        if (!json)
            return R::fail(json.error());
        if (const auto* items = json.value().getProperty("items", {}).getArray())
            for (const auto& item : *items)
            {
                RemoteTrack t;
                if (parseTrack(item, t))
                    out.push_back(std::move(t));
            }
        next = json.value().getProperty("next", juce::var()).toString();
    }
    return R::ok(std::move(out));
}

} // namespace devildj::app
