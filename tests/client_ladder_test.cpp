#include "fake_http_client.h"
#include "test_resolver.h"

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

using ytres::ClientId;
using ytres::Error;
using namespace std::chrono_literals;

namespace {

// X-YouTube-Client-Name, which the fake answers by.
const std::string VISIONOS = "101";
const std::string WEB = "1";

// The X-YouTube-Client-Name of every player request, in the order asked.
std::vector<std::string> clientsAsked(const FakeHttpClient &http)
{
    std::vector<std::string> asked;
    for (const ytres::HttpRequest &request : http.requests) {
        if (isPlayerRequest(request)) {
            asked.push_back(headerValue(request, "X-YouTube-Client-Name"));
        }
    }
    return asked;
}

// A player response whose playability fails as given.
std::string failing(const std::string &playability)
{
    return R"({"playabilityStatus":)" + playability + "}";
}

using SetUp = std::function<void(FakeHttpClient &)>;

// A set-up in which visionos answers with body.
SetUp visionosSays(const std::string &body)
{
    return [body](FakeHttpClient &http) { http.playerBodyByClient[VISIONOS] = body; };
}

// Playable, but every format ciphered: NoFormats.
const std::string CIPHERED = R"({"playabilityStatus":{"status":"OK"},"videoDetails":{"videoId":"dQw4w9WgXcQ"},)"
                             R"("streamingData":{"adaptiveFormats":[{"itag":251,"signatureCipher":"s=abc"}]}})";

const std::string REFUSED_WEB = "YouTube refused the client: Video unavailable. The page needs to be reloaded.";
const std::string BOT_CHECK_ANSWER = "visionos: Sign in to confirm you\xE2\x80\x99re not a bot";
const std::string CIPHERED_ANSWER = "visionos: Every playable format needs the player JavaScript (1 ciphered)";

}

TEST_CASE("the ladder falls through the recorded web answer to visionos")
{
    TestResolver test(WATCH_PAGE, 10s, {ClientId::Web, ClientId::VisionOS});
    test.http->playerBodyByClient[WEB] = readFixture("player_web_dQw4w9WgXcQ.json");
    const auto result = test.resolver.resolve("dQw4w9WgXcQ");
    REQUIRE(result);
    CHECK(result.value.bestAudio()->itag == 251);
    CHECK(clientsAsked(*test.http) == std::vector<std::string>{WEB, VISIONOS});
    CHECK(test.http->requests.size() == 3); // one watch page serves both
    CHECK(test.logLine(ytres::LogLevel::Warning, "The web client failed for dQw4w9WgXcQ (" + REFUSED_WEB
                                                     + "); asking the visionos client next") != nullptr);
}

TEST_CASE("web alone ends in NoFormats, and the message names it")
{
    TestResolver test(WATCH_PAGE, 10s, {ClientId::Web});
    test.http->playerBodyByClient[WEB] = readFixture("player_web_dQw4w9WgXcQ.json");
    const auto result = test.resolver.resolve("dQw4w9WgXcQ");
    CHECK(result.status.code == Error::NoFormats);
    CHECK(result.status.message == "web: " + REFUSED_WEB);
}

TEST_CASE("visionos met by the bot check, then web refused: BotCheck, and the message names both")
{
    TestResolver test(WATCH_PAGE, 10s, {ClientId::VisionOS, ClientId::Web});
    test.http->playerBodyByClient[VISIONOS] = readFixture("player_bot_check.json");
    test.http->playerBodyByClient[WEB] = readFixture("player_web_dQw4w9WgXcQ.json");
    const auto result = test.resolver.resolve("dQw4w9WgXcQ");
    CHECK(result.status.code == Error::BotCheck);
    CHECK(result.status.message == BOT_CHECK_ANSWER + "; web: " + REFUSED_WEB);
    // The bot check's one fresh page serves the rest of the ladder: page,
    // visionos, page, visionos again, web.
    CHECK(clientsAsked(*test.http) == std::vector<std::string>{VISIONOS, VISIONOS, WEB});
    CHECK(test.http->requests.size() == 5);
}

TEST_CASE("a ladder that runs out reports the most telling code, whichever client gave it")
{
    struct Case
    {
        const char *name;
        SetUp visionosFails; // web is refused, as recorded
        Error expected;
    };
    const Case cases[] = {
        {"BotCheck", visionosSays(readFixture("player_bot_check.json")), Error::BotCheck},
        {"Http", [](FakeHttpClient &http) { http.playerFailureByClient[VISIONOS] = {Error::Http, "HTTP 429"}; }, Error::Http},
        {"Parse", visionosSays("not json"), Error::Parse},
        {"NoFormats", visionosSays(CIPHERED), Error::NoFormats},
    };
    const std::vector<ClientId> orders[] = {{ClientId::VisionOS, ClientId::Web}, {ClientId::Web, ClientId::VisionOS}};
    for (const Case &c : cases) {
        for (const std::vector<ClientId> &order : orders) {
            const std::string name = std::string(c.name) + (order.front() == ClientId::Web ? ", web first" : ", visionos first");
            CAPTURE(name);
            TestResolver test(WATCH_PAGE, 10s, order);
            test.http->playerBodyByClient[WEB] = readFixture("player_web_dQw4w9WgXcQ.json");
            c.visionosFails(*test.http);
            const auto result = test.resolver.resolve("dQw4w9WgXcQ");
            CHECK(result.status.code == c.expected);
            CHECK(result.status.message.find("web: " + REFUSED_WEB) != std::string::npos);
            CHECK(result.status.message.find("visionos: ") != std::string::npos);
        }
    }
}

TEST_CASE("the network, the deadline or the library ending the ladder late keeps the earlier answers")
{
    struct Case
    {
        const char *name;
        SetUp visionosFails;
        ytres::Status webFails;
        Error expected;
        std::string message;
    };
    const SetUp botCheck = visionosSays(readFixture("player_bot_check.json"));
    const Case cases[] = {
        // the bot check outranks the network and the deadline...
        {"BotCheck, then Network", botCheck, {Error::Network, "Couldn't connect to server"}, Error::BotCheck,
         BOT_CHECK_ANSWER + "; web: Couldn't connect to server"},
        {"BotCheck, then Timeout", botCheck, {Error::Timeout, "Timeout was reached"}, Error::BotCheck,
         BOT_CHECK_ANSWER + "; web: Timeout was reached"},
        // ...but not a bug of the library's
        {"BotCheck, then Internal", botCheck, {Error::Internal, "Out of memory"}, Error::Internal,
         BOT_CHECK_ANSWER + "; web: Out of memory"},
        {"NoFormats, then Timeout", visionosSays(CIPHERED), {Error::Timeout, "Timeout was reached"}, Error::Timeout,
         CIPHERED_ANSWER + "; web: Timeout was reached"},
        {"NoFormats, then Network", visionosSays(CIPHERED), {Error::Network, "Couldn't connect to server"}, Error::Network,
         CIPHERED_ANSWER + "; web: Couldn't connect to server"},
        // the call's own cancel keeps its own words
        {"BotCheck, then Cancelled", botCheck, {Error::Cancelled, "Cancelled"}, Error::Cancelled, "Cancelled"},
    };
    for (const Case &c : cases) {
        const std::string name = c.name; // a std::string, so a failure prints the text, not the pointer
        CAPTURE(name);
        TestResolver test(WATCH_PAGE, 10s, {ClientId::VisionOS, ClientId::Web});
        c.visionosFails(*test.http);
        test.http->playerFailureByClient[WEB] = c.webFails;
        const auto result = test.resolver.resolve("dQw4w9WgXcQ");
        CHECK(result.status.code == c.expected);
        CHECK(result.status.message == c.message);
    }
}

TEST_CASE("the video's own failure after a bot check keeps its code and YouTube's words")
{
    TestResolver test(WATCH_PAGE, 10s, {ClientId::VisionOS, ClientId::Web});
    test.http->playerBodyByClient[VISIONOS] = readFixture("player_bot_check.json");
    test.http->playerBodyByClient[WEB] = readFixture("player_unavailable.json");
    const auto result = test.resolver.resolve("00000000000");
    CHECK(result.status.code == Error::Unavailable);
    CHECK(result.status.message == "This video is unavailable");
}

TEST_CASE("a failure about the client passes the video on; any other ends the resolve")
{
    struct Case
    {
        const char *name;
        SetUp setUp; // web keeps answering with the dQw4w9WgXcQ fixture
        Error expected;
        std::vector<std::string> asked;
    };
    const auto everySendFails = [](Error code, const char *message) {
        return [code, message](FakeHttpClient &http) { http.playerFailure = {code, message}; };
    };
    const Case cases[] = {
        {"BotCheck", visionosSays(readFixture("player_bot_check.json")), Error::Ok, {VISIONOS, VISIONOS, WEB}},
        {"NoFormats", visionosSays(CIPHERED), Error::Ok, {VISIONOS, WEB}},
        {"Parse", visionosSays("not json"), Error::Ok, {VISIONOS, WEB}},
        {"Http", [](FakeHttpClient &http) { http.playerStatus = 500; }, Error::Http, {VISIONOS, WEB}},
        {"Unavailable", visionosSays(readFixture("player_unavailable.json")), Error::Unavailable, {VISIONOS}},
        {"AgeRestricted", visionosSays(failing(R"({"status":"LOGIN_REQUIRED","reason":"Sign in to confirm your age"})")),
         Error::AgeRestricted, {VISIONOS}},
        {"GeoBlocked", visionosSays(failing(R"({"status":"UNPLAYABLE","reason":"This video is not available in your country"})")),
         Error::GeoBlocked, {VISIONOS}},
        {"LoginRequired", visionosSays(failing(R"({"status":"LOGIN_REQUIRED","reason":"Sign in to view this video"})")),
         Error::LoginRequired, {VISIONOS}},
        {"Network", everySendFails(Error::Network, "Couldn't connect to server"), Error::Network, {VISIONOS}},
        {"Timeout", everySendFails(Error::Timeout, "Timeout was reached"), Error::Timeout, {VISIONOS}},
        {"Cancelled", everySendFails(Error::Cancelled, "Cancelled"), Error::Cancelled, {VISIONOS}},
        {"Internal", everySendFails(Error::Internal, "Out of memory"), Error::Internal, {VISIONOS}},
    };
    for (const Case &c : cases) {
        const std::string name = c.name; // a std::string, so a failure prints the text, not the pointer
        CAPTURE(name);
        TestResolver test(WATCH_PAGE, 10s, {ClientId::VisionOS, ClientId::Web});
        c.setUp(*test.http);
        const auto result = test.resolver.resolve("dQw4w9WgXcQ");
        CHECK(result.status.code == c.expected);
        CHECK(clientsAsked(*test.http) == c.asked);
    }
}

TEST_CASE("a failure that ends the resolve keeps YouTube's words, with no client named")
{
    TestResolver test(WATCH_PAGE, 10s, {ClientId::VisionOS, ClientId::Web});
    test.http->playerBodyByClient[VISIONOS] = readFixture("player_unavailable.json");
    const auto result = test.resolver.resolve("00000000000");
    CHECK(result.status.code == Error::Unavailable);
    CHECK(result.status.message == "This video is unavailable");
}

TEST_CASE("an empty ladder, or a client the library does not know, is BadInput and sends nothing")
{
    TestResolver empty(WATCH_PAGE, 10s, {});
    CHECK(empty.resolver.resolve("dQw4w9WgXcQ").status.code == Error::BadInput);
    CHECK(empty.http->requests.empty());

    TestResolver unknown(WATCH_PAGE, 10s, {ClientId::VisionOS, static_cast<ClientId>(99)});
    const auto result = unknown.resolver.resolve("dQw4w9WgXcQ");
    CHECK(result.status.code == Error::BadInput);
    CHECK(result.status.message == "Unknown InnerTube client 99");
    CHECK(unknown.http->requests.empty());
}
