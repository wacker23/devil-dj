#pragma once

// Spotify sign-in (P4-20): OAuth 2 authorization code with PKCE (RFC 7636) - no client secret - for the playlist
// import (P4-21). Spotify audio is never streamed or touched: only the owner's playlists' track lists are read.
//
// 1. A random code verifier; its SHA-256 (base64url) is the code challenge.
// 2. The system browser opens Spotify's authorize page (client id, scopes, challenge, a random state, the redirect
//    http://127.0.0.1:<port>/callback - which must be registered in the owner's Spotify app, decision D8).
// 3. A listener on 127.0.0.1:<port> waits (up to 3 minutes) for the browser to come back with ?code=&state=, checks
//    the state, answers with a short page, and exchanges the code (+ verifier) for tokens at the token endpoint.
// 4. Tokens are kept encrypted for the Windows user (DPAPI) and refreshed shortly before they expire.
// Endpoints are configurable so the tests run against a local fake server. Tokens are never logged.

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace devildj::app
{

class SpotifyAuth
{
public:
    struct Config
    {
        juce::String clientId;
        int redirectPort = 43879;
        juce::String accountsBase = "https://accounts.spotify.com"; // authorize + token endpoints
        juce::String scopes = "playlist-read-private playlist-read-collaborative";
        juce::File tokenFile; // where the encrypted tokens live
        int listenSeconds = 180;
    };

    explicit SpotifyAuth(Config config);
    ~SpotifyAuth();
    SpotifyAuth(const SpotifyAuth&) = delete;
    SpotifyAuth& operator=(const SpotifyAuth&) = delete;

    // Opens the browser (openUrl) and waits on a thread of its own; done(ok, message) is called on that thread.
    void beginSignIn(std::function<void(const juce::URL&)> openUrl, std::function<void(bool ok, juce::String message)> done);
    void cancel();
    void signOut(); // forgets the tokens (the file goes)
    bool signedIn() const;
    bool waiting() const noexcept { return listening.load(); }
    juce::String redirectUri() const;

    // A valid access token - refreshed (blocking, network) if it expires within a minute; empty if signed out or
    // the refresh fails (`error` says why).
    juce::String accessToken(juce::String* error = nullptr);
    // P4-21: a new access token now (Spotify said the one we have is no good: a 401).
    juce::String refreshNow(juce::String* error = nullptr);

    // Pieces, public for tests.
    static juce::String makeVerifier(juce::Random& random); // 64 characters of [A-Za-z0-9-._~]
    static juce::String challengeFor(const juce::String& verifier);
    static juce::String base64Url(const void* data, size_t size);
    juce::URL authorizeUrl(const juce::String& challenge, const juce::String& state) const;
    struct Tokens
    {
        juce::String access, refresh, scope;
        juce::int64 expiresAtMs = 0; // wall clock
    };
    Tokens tokens() const;

    // DPAPI (Windows: for this user only); elsewhere the bytes as they are.
    static juce::MemoryBlock protect(const juce::MemoryBlock& plain);
    static bool unprotect(const juce::MemoryBlock& sealed, juce::MemoryBlock& plain);

private:
    bool exchange(const juce::String& body, juce::String& error); // POST to the token endpoint; tokens on success
    void save();
    void load();
    void listen(juce::String state, juce::String verifier, std::function<void(bool, juce::String)> done);

    Config config;
    mutable std::mutex mutex;
    Tokens current;
    std::atomic<bool> listening{false}, stop{false};
    std::thread worker;
};

} // namespace devildj::app
