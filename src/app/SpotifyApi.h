#pragma once

// Reading the owner's Spotify playlists (P4-21): their names, then a playlist's track list (title, artists, album,
// duration, ISRC). Nothing else - no audio, no previews, no caching of anything but the import's result.
// Blocking calls (a worker thread); paging followed ("next"), a 429 waited out (Retry-After, up to 3 times), a
// 401 answered with one token refresh. The API base is configurable for the tests' fake server.

#include "app/PlaylistMatch.h"
#include "util/Result.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <string>
#include <vector>

namespace devildj::app
{

struct RemotePlaylist
{
    std::string id, name, owner;
    int tracks = 0;
};

class SpotifyApi
{
public:
    // The access token: force = true after a 401 (refresh now); empty with `error` when there is none.
    using TokenSource = std::function<juce::String(bool force, juce::String* error)>;
    explicit SpotifyApi(TokenSource tokens, juce::String apiBase = "https://api.spotify.com");

    util::Result<std::vector<RemotePlaylist>> playlists();
    util::Result<std::vector<RemoteTrack>> tracks(const std::string& playlistId);

    // How a GET is made (tests can count / slow it); the default is juce::URL.
    struct Response
    {
        int status = 0;
        juce::String body;
        int retryAfterSeconds = 0;
    };
    std::function<Response(const juce::URL& url, const juce::String& bearer)> get;
    std::function<void(int seconds)> sleepFor; // a 429's wait (tests: none)

    static RemotePlaylist parsePlaylist(const juce::var& item);
    static bool parseTrack(const juce::var& item, RemoteTrack& out); // false: not a track (an episode, a gap)

private:
    util::Result<juce::var> getJson(const juce::String& url);

    TokenSource token;
    juce::String base;
};

} // namespace devildj::app
