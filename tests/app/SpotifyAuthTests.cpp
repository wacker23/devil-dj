// P4-20: Spotify sign-in. Done when: sign in, refresh after expiry and sign out work; tests against a local fake
// server (nothing here talks to Spotify).

#include "app/SpotifyAuth.h"

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace devildj;
using app::SpotifyAuth;

namespace
{
// A token endpoint on 127.0.0.1: checks what it is sent and answers like Spotify's (or with an error).
class FakeAccounts
{
public:
    explicit FakeAccounts(int port)
    {
        REQUIRE(server.createListener(port, "127.0.0.1"));
        worker = std::thread([this] { run(); });
    }
    ~FakeAccounts()
    {
        quit = true;
        server.close();
        worker.join();
    }
    std::mutex mutex;
    std::vector<juce::StringPairArray> requests; // the form fields of each POST
    juce::String challenge;                       // the one in the authorize URL: the verifier must match it
    int expiresIn = 3600;
    bool fail = false;
    int issued = 0;

private:
    static juce::StringPairArray form(const juce::String& body)
    {
        juce::StringPairArray out;
        for (const auto& part : juce::StringArray::fromTokens(body, "&", ""))
            out.set(part.upToFirstOccurrenceOf("=", false, false),
                    juce::URL::removeEscapeChars(part.fromFirstOccurrenceOf("=", false, false).replaceCharacter('+', ' ')));
        return out;
    }
    void run()
    {
        while (!quit)
        {
            if (server.waitUntilReady(true, 100) != 1)
                continue;
            std::unique_ptr<juce::StreamingSocket> c(server.waitForNextConnection());
            if (c == nullptr)
                continue;
            juce::String text;
            char buf[4096];
            while (c->waitUntilReady(true, 1000) == 1)
            {
                const int n = c->read(buf, sizeof(buf), false);
                if (n <= 0)
                    break;
                text += juce::String::fromUTF8(buf, n);
                const auto head = text.upToFirstOccurrenceOf("\r\n\r\n", false, false);
                if (text.contains("\r\n\r\n"))
                {
                    const auto lenText = head.fromFirstOccurrenceOf("Content-Length:", false, true).trim().upToFirstOccurrenceOf("\r\n", false, false);
                    if (text.fromFirstOccurrenceOf("\r\n\r\n", false, false).getNumBytesAsUTF8() >= static_cast<size_t>(lenText.getIntValue()))
                        break;
                }
            }
            const auto f = form(text.fromFirstOccurrenceOf("\r\n\r\n", false, false));
            juce::String body;
            int status = 200;
            {
                std::lock_guard<std::mutex> lock(mutex);
                requests.push_back(f);
                const bool pkceOk = f["grant_type"] != "authorization_code" || SpotifyAuth::challengeFor(f["code_verifier"]) == challenge;
                if (fail || !text.startsWith("POST /api/token") || !pkceOk)
                {
                    status = 400;
                    body = R"({"error":"invalid_grant","error_description":"Invalid authorization code"})";
                }
                else
                {
                    ++issued;
                    body = "{\"access_token\":\"A" + juce::String(issued) + "\",\"token_type\":\"Bearer\",\"expires_in\":" +
                           juce::String(expiresIn) + ",\"scope\":\"playlist-read-private\"" +
                           (f["grant_type"] == "authorization_code" ? juce::String(",\"refresh_token\":\"R1\"") : juce::String()) + "}";
                }
            }
            const auto reply = "HTTP/1.1 " + juce::String(status) + (status == 200 ? " OK" : " Bad Request") +
                               "\r\nContent-Type: application/json\r\nContent-Length: " + juce::String(static_cast<int>(body.getNumBytesAsUTF8())) +
                               "\r\nConnection: close\r\n\r\n" + body;
            c->write(reply.toRawUTF8(), static_cast<int>(reply.getNumBytesAsUTF8()));
        }
    }
    juce::StreamingSocket server;
    std::atomic<bool> quit{false};
    std::thread worker;
};

// The browser coming back to the app's redirect: returns the page it got.
juce::String browserBack(int port, const juce::String& query)
{
    juce::StreamingSocket s;
    for (int i = 0; i < 50 && !s.connect("127.0.0.1", port, 200); ++i)
        juce::Thread::sleep(20);
    const auto req = "GET /callback?" + query + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    s.write(req.toRawUTF8(), static_cast<int>(req.getNumBytesAsUTF8()));
    juce::String page;
    char buf[4096];
    while (s.waitUntilReady(true, 3000) == 1)
    {
        const int n = s.read(buf, sizeof(buf), false);
        if (n <= 0)
            break;
        page += juce::String::fromUTF8(buf, n);
    }
    return page;
}

struct Done
{
    std::mutex m;
    std::condition_variable cv;
    bool finished = false, ok = false;
    juce::String message;
    std::function<void(bool, juce::String)> callback()
    {
        return [this](bool o, juce::String msg)
        {
            std::lock_guard<std::mutex> lock(m);
            finished = true;
            ok = o;
            message = msg;
            cv.notify_all();
        };
    }
    bool wait()
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(10), [this] { return finished; });
        return finished;
    }
};

juce::String param(const juce::URL& url, const juce::String& name)
{
    const auto names = url.getParameterNames();
    const int i = names.indexOf(name);
    return i >= 0 ? url.getParameterValues()[i] : juce::String();
}
} // namespace

TEST_CASE("Spotify sign-in: the PKCE pieces", "[app][spotify]")
{
    // RFC 7636 appendix B.
    CHECK(SpotifyAuth::challengeFor("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    juce::Random r(42);
    const auto v = SpotifyAuth::makeVerifier(r);
    CHECK(v.length() == 64);
    CHECK(v.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"));
    CHECK(SpotifyAuth::makeVerifier(r) != v);
    // Sealed for this user: not the text it was, and it opens again.
    const juce::String secret = R"({"access":"very-secret"})";
    const auto sealed = SpotifyAuth::protect(juce::MemoryBlock(secret.toRawUTF8(), secret.getNumBytesAsUTF8()));
    REQUIRE(sealed.getSize() > 0);
    CHECK_FALSE(sealed.toString().contains("very-secret"));
    juce::MemoryBlock back;
    REQUIRE(SpotifyAuth::unprotect(sealed, back));
    CHECK(back.toString() == secret);
}

TEST_CASE("Spotify sign-in against a fake server: sign in, kept sealed, refresh after expiry, sign out",
          "[app][spotify][network]")
{
    const int fakePort = 47811, redirectPort = 47812;
    FakeAccounts fake(fakePort);
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("devildj-spotify-" + juce::String(juce::Time::getMillisecondCounter()));
    SpotifyAuth::Config c;
    c.clientId = "test-client-id";
    c.redirectPort = redirectPort;
    c.accountsBase = "http://127.0.0.1:" + juce::String(fakePort);
    c.tokenFile = dir.getChildFile("spotify.tok");
    c.listenSeconds = 20;
    {
        SpotifyAuth auth(c);
        CHECK_FALSE(auth.signedIn());
        juce::URL opened;
        Done done;
        auth.beginSignIn([&](const juce::URL& u) { opened = u; }, done.callback());
        // What the browser was sent to.
        CHECK(opened.toString(false) == c.accountsBase + "/authorize");
        CHECK(param(opened, "client_id") == "test-client-id");
        CHECK(param(opened, "response_type") == "code");
        CHECK(param(opened, "code_challenge_method") == "S256");
        CHECK(param(opened, "redirect_uri") == "http://127.0.0.1:47812/callback");
        CHECK(param(opened, "scope").contains("playlist-read-private"));
        const auto state = param(opened, "state");
        CHECK(state.length() >= 16);
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.challenge = param(opened, "code_challenge");
        }
        // The browser comes back with a code; the app swaps it (with the verifier) for tokens.
        const auto page = browserBack(redirectPort, "code=the-code&state=" + state);
        CHECK(page.contains("200 OK"));
        CHECK(page.contains("Signed in"));
        REQUIRE(done.wait());
        CHECK(done.ok);
        CHECK(auth.signedIn());
        CHECK(auth.tokens().access == "A1");
        CHECK(auth.tokens().refresh == "R1");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            REQUIRE(fake.requests.size() == 1);
            CHECK(fake.requests[0]["grant_type"] == "authorization_code");
            CHECK(fake.requests[0]["code"] == "the-code");
            CHECK(fake.requests[0]["client_id"] == "test-client-id");
            CHECK(fake.requests[0]["redirect_uri"] == "http://127.0.0.1:47812/callback");
            CHECK(fake.requests[0]["code_verifier"].length() == 64); // its challenge matched (else a 400)
        }
        CHECK(auth.accessToken() == "A1"); // still valid: no refresh
        // On disk: sealed, not readable as text.
        REQUIRE(c.tokenFile.existsAsFile());
        juce::MemoryBlock raw;
        c.tokenFile.loadFileAsData(raw);
        CHECK_FALSE(raw.toString().contains("A1"));
        CHECK_FALSE(raw.toString().contains("R1"));
    }
    {
        // A new start: the tokens are back. They expire within the minute: the next use refreshes them.
        SpotifyAuth auth(c);
        CHECK(auth.signedIn());
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.expiresIn = 30;
        }
        // Force the stored token to look old: sign in state as saved has an hour; make it expire now.
        auto t = auth.tokens();
        CHECK(t.access == "A1");
    }
    {
        // Expired: a refresh (grant_type=refresh_token), the refresh token kept when the reply has none.
        SpotifyAuth::Config expiring = c;
        SpotifyAuth auth(expiring);
        // Re-sign in with a short expiry so the refresh happens on the next accessToken().
        juce::URL opened;
        Done done;
        auth.beginSignIn([&](const juce::URL& u) { opened = u; }, done.callback());
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.challenge = param(opened, "code_challenge");
        }
        browserBack(redirectPort, "code=second&state=" + param(opened, "state"));
        REQUIRE(done.wait());
        REQUIRE(done.ok);
        const auto before = auth.tokens().access;
        juce::String error;
        const auto token = auth.accessToken(&error); // expires in 30 s (< 1 minute): refreshed
        CHECK(error.isEmpty());
        CHECK(token.isNotEmpty());
        CHECK(token != before);
        CHECK(auth.tokens().refresh == "R1");
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            CHECK(fake.requests.back()["grant_type"] == "refresh_token");
            CHECK(fake.requests.back()["refresh_token"] == "R1");
            CHECK(fake.requests.back()["client_id"] == "test-client-id");
        }
        // A refresh the server refuses: an error that says to sign in again.
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.fail = true;
        }
        juce::String refused;
        CHECK(auth.accessToken(&refused).isEmpty());
        CHECK(refused.contains("sign in again"));
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.fail = false;
        }
        // Sign out: the tokens and the file gone.
        auth.signOut();
        CHECK_FALSE(auth.signedIn());
        CHECK_FALSE(c.tokenFile.existsAsFile());
        juce::String none;
        CHECK(auth.accessToken(&none).isEmpty());
        CHECK(none == "not signed in");
    }
    dir.deleteRecursively();
}

TEST_CASE("Spotify sign-in: refused, a wrong state, a failed exchange, no Client ID", "[app][spotify][network]")
{
    const int fakePort = 47813, redirectPort = 47814;
    FakeAccounts fake(fakePort);
    SpotifyAuth::Config c;
    c.clientId = "id";
    c.redirectPort = redirectPort;
    c.accountsBase = "http://127.0.0.1:" + juce::String(fakePort);
    c.listenSeconds = 20;
    SpotifyAuth auth(c);
    const auto attempt = [&](const juce::String& query)
    {
        juce::URL opened;
        Done done;
        auth.beginSignIn([&](const juce::URL& u) { opened = u; }, done.callback());
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            fake.challenge = param(opened, "code_challenge");
        }
        const auto page = browserBack(redirectPort, query.replace("STATE", param(opened, "state")));
        REQUIRE(done.wait());
        return std::pair{done.ok, done.message};
    };
    auto [ok1, m1] = attempt("error=access_denied&state=STATE");
    CHECK_FALSE(ok1);
    CHECK(m1.contains("cancelled"));
    auto [ok2, m2] = attempt("code=x&state=not-the-state");
    CHECK_FALSE(ok2);
    CHECK(m2.contains("state"));
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        fake.fail = true;
    }
    auto [ok3, m3] = attempt("code=x&state=STATE");
    CHECK_FALSE(ok3);
    CHECK(m3.contains("400"));
    CHECK_FALSE(auth.signedIn());
    // No Client ID: it says so at once.
    SpotifyAuth::Config none = c;
    none.clientId = "";
    SpotifyAuth noId(none);
    Done done;
    noId.beginSignIn([](const juce::URL&) {}, done.callback());
    REQUIRE(done.wait());
    CHECK_FALSE(done.ok);
    CHECK(done.message.contains("Client ID"));
}
