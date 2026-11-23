#include "app/SpotifyAuth.h"

#include "app/SecretBox.h"

#include <juce_cryptography/juce_cryptography.h>

namespace devildj::app
{

namespace
{
juce::String formEncode(const juce::String& s)
{
    return juce::URL::addEscapeChars(s, true);
}

// The value of `key` in a query string (a=b&c=d), unescaped.
juce::String queryValue(const juce::String& query, const juce::String& key)
{
    for (const auto& part : juce::StringArray::fromTokens(query, "&", ""))
        if (part.upToFirstOccurrenceOf("=", false, false) == key)
            return juce::URL::removeEscapeChars(part.fromFirstOccurrenceOf("=", false, false).replaceCharacter('+', ' '));
    return {};
}

void reply(juce::StreamingSocket& client, int status, const juce::String& message)
{
    const auto body = "<!doctype html><html><head><meta charset=\"utf-8\"><title>Devil-DJ</title></head>"
                      "<body style=\"font-family:sans-serif;background:#16161b;color:#e8e8ee;padding:40px\"><h2>Devil-DJ</h2><p>" +
                      juce::String(message).replace("<", "&lt;") + "</p></body></html>";
    const auto bytes = static_cast<int>(body.getNumBytesAsUTF8());
    const auto head = "HTTP/1.1 " + juce::String(status) + (status == 200 ? " OK" : " Bad Request") +
                      "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + juce::String(bytes) +
                      "\r\nConnection: close\r\n\r\n";
    client.write(head.toRawUTF8(), static_cast<int>(head.getNumBytesAsUTF8()));
    client.write(body.toRawUTF8(), bytes);
}
} // namespace

SpotifyAuth::SpotifyAuth(Config c)
    : config(std::move(c))
{
    load();
}

SpotifyAuth::~SpotifyAuth()
{
    cancel();
}

juce::String SpotifyAuth::redirectUri() const
{
    return "http://127.0.0.1:" + juce::String(config.redirectPort) + "/callback";
}

juce::String SpotifyAuth::base64Url(const void* data, size_t size)
{
    return juce::Base64::toBase64(data, size).replaceCharacter('+', '-').replaceCharacter('/', '_').trimCharactersAtEnd("=");
}

juce::String SpotifyAuth::makeVerifier(juce::Random& random)
{
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    juce::String v;
    for (int i = 0; i < 64; ++i)
        v << chars[random.nextInt(66)];
    return v;
}

juce::String SpotifyAuth::challengeFor(const juce::String& verifier)
{
    const auto digest = juce::SHA256(verifier.toUTF8()).getRawData();
    return base64Url(digest.getData(), digest.getSize());
}

juce::URL SpotifyAuth::authorizeUrl(const juce::String& challenge, const juce::String& state) const
{
    return juce::URL(config.accountsBase + "/authorize")
        .withParameter("client_id", config.clientId)
        .withParameter("response_type", "code")
        .withParameter("redirect_uri", redirectUri())
        .withParameter("code_challenge_method", "S256")
        .withParameter("code_challenge", challenge)
        .withParameter("scope", config.scopes)
        .withParameter("state", state);
}

SpotifyAuth::Tokens SpotifyAuth::tokens() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}

bool SpotifyAuth::signedIn() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return current.refresh.isNotEmpty() || current.access.isNotEmpty();
}

void SpotifyAuth::beginSignIn(std::function<void(const juce::URL&)> openUrl, std::function<void(bool, juce::String)> done)
{
    cancel();
    if (config.clientId.trim().isEmpty())
    {
        if (done)
            done(false, "Set your Spotify app's Client ID first (Spotify > Client ID...).");
        return;
    }
    juce::Random random;
    const auto verifier = makeVerifier(random);
    const auto state = makeVerifier(random).substring(0, 24);
    stop = false;
    listening = true;
    worker = std::thread([this, state, verifier, done] { listen(state, verifier, done); });
    if (openUrl)
        openUrl(authorizeUrl(challengeFor(verifier), state));
}

void SpotifyAuth::cancel()
{
    stop = true;
    if (worker.joinable())
        worker.join();
    listening = false;
}

void SpotifyAuth::listen(juce::String state, juce::String verifier, std::function<void(bool, juce::String)> done)
{
    const auto finish = [&](bool ok, const juce::String& message)
    {
        listening = false;
        if (done)
            done(ok, message);
    };
    juce::StreamingSocket server;
    if (!server.createListener(config.redirectPort, "127.0.0.1"))
    {
        finish(false, "Can't listen on 127.0.0.1:" + juce::String(config.redirectPort) + " (is another program using it?)");
        return;
    }
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 1000.0 * config.listenSeconds;
    while (!stop && juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        if (server.waitUntilReady(true, 200) != 1)
            continue;
        std::unique_ptr<juce::StreamingSocket> client(server.waitForNextConnection());
        if (client == nullptr)
            continue;
        // The request line: GET /callback?code=...&state=... HTTP/1.1
        char buffer[8192] = {};
        int got = 0;
        while (got < static_cast<int>(sizeof(buffer)) - 1 && client->waitUntilReady(true, 2000) == 1)
        {
            const int n = client->read(buffer + got, static_cast<int>(sizeof(buffer)) - 1 - got, false);
            if (n <= 0)
                break;
            got += n;
            if (juce::String(buffer).contains("\r\n\r\n"))
                break;
        }
        const auto request = juce::String::fromUTF8(buffer, got);
        const auto target = request.upToFirstOccurrenceOf("\r\n", false, false).fromFirstOccurrenceOf(" ", false, false)
                                .upToFirstOccurrenceOf(" ", false, false);
        if (!target.startsWith("/callback"))
        {
            reply(*client, 400, "Not this page.");
            continue; // a favicon or the like: keep waiting
        }
        const auto query = target.fromFirstOccurrenceOf("?", false, false);
        if (queryValue(query, "state") != state)
        {
            reply(*client, 400, "This sign-in didn't come from Devil-DJ (the state doesn't match). Try again from the app.");
            finish(false, "The sign-in reply didn't match (state): try again.");
            return;
        }
        if (const auto err = queryValue(query, "error"); err.isNotEmpty())
        {
            reply(*client, 400, "Spotify said: " + err + ". You can close this tab.");
            finish(false, err == "access_denied" ? "Sign-in cancelled at Spotify." : "Spotify said: " + err);
            return;
        }
        const auto code = queryValue(query, "code");
        juce::String error;
        const bool ok = code.isNotEmpty() &&
                        exchange("grant_type=authorization_code&code=" + formEncode(code) + "&redirect_uri=" + formEncode(redirectUri()) +
                                     "&client_id=" + formEncode(config.clientId) + "&code_verifier=" + formEncode(verifier),
                                 error);
        reply(*client, ok ? 200 : 400, ok ? "Signed in. You can close this tab and go back to Devil-DJ."
                                          : "Sign-in failed: " + error);
        finish(ok, ok ? juce::String("Signed in to Spotify.") : "Sign-in failed: " + error);
        return;
    }
    finish(false, stop ? juce::String("Sign-in cancelled.") : juce::String("Sign-in timed out (no reply from the browser)."));
}

bool SpotifyAuth::exchange(const juce::String& body, juce::String& error)
{
    int status = 0;
    juce::StringPairArray headers;
    const auto url = juce::URL(config.accountsBase + "/api/token").withPOSTData(body);
    auto stream = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                                            .withExtraHeaders("Content-Type: application/x-www-form-urlencoded")
                                            .withConnectionTimeoutMs(15000)
                                            .withStatusCode(&status)
                                            .withResponseHeaders(&headers));
    if (stream == nullptr)
    {
        error = "no connection to Spotify";
        return false;
    }
    const auto text = stream->readEntireStreamAsString();
    juce::var json;
    if (juce::JSON::parse(text, json).failed() || status != 200)
    {
        const auto what = json.getProperty("error_description", json.getProperty("error", "")).toString();
        error = "HTTP " + juce::String(status) + (what.isNotEmpty() ? ": " + what : juce::String());
        return false;
    }
    Tokens t;
    {
        std::lock_guard<std::mutex> lock(mutex);
        t = current;
    }
    t.access = json.getProperty("access_token", "").toString();
    if (const auto r = json.getProperty("refresh_token", "").toString(); r.isNotEmpty())
        t.refresh = r; // a refresh may or may not send a new one
    t.scope = json.getProperty("scope", t.scope).toString();
    t.expiresAtMs = juce::Time::currentTimeMillis() + 1000 * static_cast<juce::int64>(static_cast<int>(json.getProperty("expires_in", 3600)));
    if (t.access.isEmpty())
    {
        error = "no access token in the reply";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        current = t;
    }
    save();
    return true;
}

juce::String SpotifyAuth::accessToken(juce::String* error)
{
    Tokens t = tokens();
    if (t.access.isNotEmpty() && t.expiresAtMs - 60000 > juce::Time::currentTimeMillis())
        return t.access;
    if (t.refresh.isEmpty())
    {
        if (error != nullptr)
            *error = "not signed in";
        return {};
    }
    juce::String e;
    if (!exchange("grant_type=refresh_token&refresh_token=" + formEncode(t.refresh) + "&client_id=" + formEncode(config.clientId), e))
    {
        if (error != nullptr)
            *error = "the Spotify sign-in has expired (" + e + "): sign in again";
        return {};
    }
    return tokens().access;
}

juce::String SpotifyAuth::refreshNow(juce::String* error)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        current.expiresAtMs = 0; // as if it had expired
    }
    return accessToken(error);
}

void SpotifyAuth::signOut()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        current = {};
    }
    config.tokenFile.deleteFile();
}

juce::MemoryBlock SpotifyAuth::protect(const juce::MemoryBlock& plain)
{
    const auto* p = static_cast<const std::uint8_t*>(plain.getData());
    const auto sealed = sealForUser(std::vector<std::uint8_t>(p, p + plain.getSize()));
    return juce::MemoryBlock(sealed.data(), sealed.size());
}

bool SpotifyAuth::unprotect(const juce::MemoryBlock& sealed, juce::MemoryBlock& plain)
{
    const auto* p = static_cast<const std::uint8_t*>(sealed.getData());
    std::vector<std::uint8_t> out;
    if (!openForUser(std::vector<std::uint8_t>(p, p + sealed.getSize()), out))
        return false;
    plain = juce::MemoryBlock(out.data(), out.size());
    return true;
}

void SpotifyAuth::save()
{
    if (config.tokenFile == juce::File())
        return;
    Tokens t = tokens();
    auto* o = new juce::DynamicObject();
    const juce::var keep(o);
    o->setProperty("access", t.access);
    o->setProperty("refresh", t.refresh);
    o->setProperty("scope", t.scope);
    o->setProperty("expiresAt", t.expiresAtMs);
    const auto json = juce::JSON::toString(keep, true);
    const auto sealed = protect(juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()));
    if (sealed.getSize() > 0)
    {
        config.tokenFile.getParentDirectory().createDirectory();
        config.tokenFile.replaceWithData(sealed.getData(), sealed.getSize());
    }
}

void SpotifyAuth::load()
{
    juce::MemoryBlock sealed, plain;
    if (config.tokenFile == juce::File() || !config.tokenFile.loadFileAsData(sealed) || !unprotect(sealed, plain))
        return;
    juce::var json;
    if (juce::JSON::parse(plain.toString(), json).failed())
        return;
    std::lock_guard<std::mutex> lock(mutex);
    current.access = json.getProperty("access", "").toString();
    current.refresh = json.getProperty("refresh", "").toString();
    current.scope = json.getProperty("scope", "").toString();
    current.expiresAtMs = static_cast<juce::int64>(json.getProperty("expiresAt", 0));
}

} // namespace devildj::app
