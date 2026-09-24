#include "fake_http_client.h"
#include "innertube.h"
#include "test_resolver.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

using ytres::Error;
using namespace std::chrono_literals;

TEST_CASE("a cold resolve fetches the watch page, then asks the player with its visitor data")
{
    TestResolver test;
    const auto result = test.resolver.resolve("https://youtu.be/dQw4w9WgXcQ");
    REQUIRE(result);
    REQUIRE(test.http->requests.size() == 2);

    const ytres::HttpRequest &page = test.http->requests[0];
    CHECK(page.method == "GET");
    CHECK(page.url == "https://www.youtube.com/watch?v=dQw4w9WgXcQ");

    // What the glue sends, not only what the builder builds.
    const ytres::HttpRequest &player = test.http->requests[1];
    const ytres::HttpRequest built = ytres::innertube::playerRequest(
        *ytres::innertube::findClient(ytres::ClientId::VisionOS), "dQw4w9WgXcQ", "en", VISITOR_DATA);
    CHECK(player.method == "POST");
    CHECK(player.url == "https://www.youtube.com/youtubei/v1/player?prettyPrint=false");
    CHECK(player.headers == built.headers);
    CHECK(player.body == built.body);
    CHECK(headerValue(player, "X-Goog-Visitor-Id") == VISITOR_DATA);
    CHECK(nlohmann::json::parse(player.body)["context"]["client"]["visitorData"] == VISITOR_DATA);
    CHECK_FALSE(test.logged(ytres::LogLevel::Warning));
}

TEST_CASE("without visitor data the player is still asked, and a warning says so")
{
    TestResolver test("<html>Before you continue to YouTube</html>");
    CHECK(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 2);
    CHECK(headerValue(test.http->requests[1], "X-Goog-Visitor-Id").empty());
    CHECK(nlohmann::json::parse(test.http->requests[1].body)["context"]["client"].count("visitorData") == 0);
    CHECK(test.logLine(ytres::LogLevel::Warning, "has no visitor data") != nullptr);
}

TEST_CASE("visitor data the page has but cannot be sent gets a warning of its own")
{
    // What a grown token would look like: past the 4096-character limit.
    TestResolver test(R"(ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":")" + std::string(5000, 'A') + R"("}}});)");
    CHECK(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 2);
    CHECK(headerValue(test.http->requests[1], "X-Goog-Visitor-Id").empty());
    CHECK(test.logLine(ytres::LogLevel::Warning, "Refused the watch page's visitor data (5000 characters") != nullptr);
    CHECK(test.logLine(ytres::LogLevel::Warning, "has no visitor data") == nullptr);
}

TEST_CASE("a watch page that fails costs a warning, and the player is asked without visitor data")
{
    TestResolver refused;
    refused.http->pageStatus = 429;
    TestResolver unreachable;
    unreachable.http->pageFailure = {Error::Network, "Couldn't connect to server"};

    for (TestResolver *test : {&refused, &unreachable}) {
        CHECK(test->resolver.resolve("dQw4w9WgXcQ"));
        REQUIRE(test->http->requests.size() == 2);
        CHECK(headerValue(test->http->requests[1], "X-Goog-Visitor-Id").empty());
        CHECK(nlohmann::json::parse(test->http->requests[1].body)["context"]["client"].count("visitorData") == 0);
        CHECK(test->logged(ytres::LogLevel::Warning));
    }
}

TEST_CASE("bad input never reaches the network")
{
    TestResolver test;
    const auto result = test.resolver.resolve("https://vimeo.com/76979871");
    CHECK(result.status.code == Error::BadInput);
    CHECK(test.http->requests.empty());
}

TEST_CASE("a resolve cancelled up front sends nothing")
{
    TestResolver test;
    ytres::Request request;
    request.cancelled = [] { return true; };
    CHECK(test.resolver.resolve("dQw4w9WgXcQ", request).status.code == Error::Cancelled);
    CHECK(test.http->requests.empty());
}

TEST_CASE("the cancel check travels with every request")
{
    TestResolver test;
    ytres::Request request;
    int polls = 0;
    request.cancelled = [&polls] {
        ++polls;
        return false;
    };
    CHECK(test.resolver.resolve("dQw4w9WgXcQ", request));
    for (const ytres::HttpRequest &sent : test.http->requests) {
        REQUIRE(sent.cancelled);
        sent.cancelled();
    }
    CHECK(polls >= 2);
}

TEST_CASE("a spent deadline is a Timeout and sends nothing")
{
    TestResolver test;
    ytres::Request request;
    request.deadline = 0ms;
    CHECK(test.resolver.resolve("dQw4w9WgXcQ", request).status.code == Error::Timeout);
    CHECK(test.http->requests.empty());
}

TEST_CASE("a deadline at either extreme does not wrap around")
{
    // max() as "no deadline" resolves rather than timing out at once...
    TestResolver endless;
    ytres::Request request;
    request.deadline = std::chrono::milliseconds::max();
    CHECK(endless.resolver.resolve("dQw4w9WgXcQ", request));
    CHECK(endless.http->requests.size() == 2);

    // ...and one far in the past is spent, not wrapped into no deadline.
    TestResolver spent;
    request.deadline = std::chrono::milliseconds(-10'000'000'000'000);
    CHECK(spent.resolver.resolve("dQw4w9WgXcQ", request).status.code == Error::Timeout);
    CHECK(spent.http->requests.empty());
}

TEST_CASE("each request gets the request timeout, never more than the deadline leaves")
{
    TestResolver test(WATCH_PAGE, 5s);
    CHECK(test.resolver.resolve("dQw4w9WgXcQ"));
    for (const ytres::HttpRequest &sent : test.http->requests) {
        CHECK(sent.timeout == 5000ms);
    }

    TestResolver tight(WATCH_PAGE, 5s);
    ytres::Request request;
    request.deadline = 2s;
    CHECK(tight.resolver.resolve("dQw4w9WgXcQ", request));
    for (const ytres::HttpRequest &sent : tight.http->requests) {
        CHECK(sent.timeout > 0ms);
        CHECK(sent.timeout <= 2000ms);
    }
}

TEST_CASE("an HTTP error from the player is Http")
{
    TestResolver test;
    test.http->playerStatus = 403;
    const auto result = test.resolver.resolve("dQw4w9WgXcQ");
    CHECK(result.status.code == Error::Http);
    CHECK(result.status.message.find("403") != std::string::npos);
}

TEST_CASE("the start of an error page goes to the Debug log, not into the message")
{
    TestResolver test;
    test.http->playerStatus = 429;
    test.http->playerBody = "<html>\nQuota exceeded" + std::string(500, 'x');
    const auto result = test.resolver.resolve("dQw4w9WgXcQ");
    CHECK(result.status.code == Error::Http);
    CHECK(result.status.message.find("Quota") == std::string::npos);

    const std::string *line = test.logLine(ytres::LogLevel::Debug, "HTTP 429 from https://www.youtube.com/youtubei/v1/player");
    REQUIRE(line != nullptr);
    CHECK(line->find("<html> Quota exceeded") != std::string::npos); // one line
    CHECK(line->find(std::string(200, 'x')) == std::string::npos);    // cut at 200 bytes of body

    // The cut never lands inside a UTF-8 sequence: "\xC5\xBC" (z with dot) straddles byte 200.
    TestResolver polish;
    polish.http->playerStatus = 429;
    polish.http->playerBody = std::string(199, 'a') + "\xC5\xBC" + "b";
    CHECK_FALSE(polish.resolver.resolve("dQw4w9WgXcQ"));
    const std::string *cut = polish.logLine(ytres::LogLevel::Debug, "HTTP 429 from ");
    REQUIRE(cut != nullptr);
    CHECK(cut->size() >= 199);
    CHECK(cut->substr(cut->size() - 199) == std::string(199, 'a'));
}

TEST_CASE("a player status outside 2xx that the client let through is Http too")
{
    TestResolver test;
    test.http->playerStatus = 302; // a redirect CurlHttpClient does not follow
    const auto result = test.resolver.resolve("dQw4w9WgXcQ");
    CHECK(result.status.code == Error::Http);
    CHECK(result.status.message.find("302") != std::string::npos);
}

TEST_CASE("a failed player request passes its status on")
{
    TestResolver test;
    test.http->playerFailure = {Error::Timeout, "Timeout was reached"};
    CHECK(test.resolver.resolve("dQw4w9WgXcQ").status.code == Error::Timeout);
}

TEST_CASE("an exception that reaches resolve() is Internal, never a client's failure")
{
    // Breaks the HttpClient contract on purpose: send() must not throw.
    class ThrowingHttpClient : public ytres::HttpClient
    {
    public:
        explicit ThrowingHttpClient(bool standard_)
            : standard(standard_)
        {
        }

        ytres::Result<ytres::HttpResponse> send(const ytres::HttpRequest &) override
        {
            if (standard) {
                throw std::bad_alloc();
            }
            throw 42;
        }

    private:
        bool standard;
    };

    for (const bool standard : {true, false}) {
        CAPTURE(standard);
        ytres::Resolver::Options options;
        options.http = std::make_shared<ThrowingHttpClient>(standard);
        ytres::Resolver resolver(options);
        const auto result = resolver.resolve("dQw4w9WgXcQ");
        CHECK(result.status.code == Error::Internal);
        CHECK(result.status.message.find("Unexpected failure") == 0);
    }
}

static_assert(std::is_nothrow_move_constructible_v<ytres::Resolver>);
static_assert(std::is_nothrow_move_assignable_v<ytres::Resolver>);
static_assert(!std::is_copy_constructible_v<ytres::Resolver>);

TEST_CASE("a Resolver can come out of a factory and move, and a moved-from one refuses")
{
    const auto http = std::make_shared<FakeHttpClient>(readFixture("player_dQw4w9WgXcQ.json"), WATCH_PAGE);
    const auto make = [&http] {
        ytres::Resolver::Options options;
        options.http = http;
        ytres::Resolver made(options);
        return made; // a named local: this needs the move constructor
    };
    ytres::Resolver first = make();
    CHECK(first.resolve("dQw4w9WgXcQ"));

    ytres::Resolver second; // its own libcurl client, which never gets to send
    second = std::move(first);
    CHECK(second.resolve("dQw4w9WgXcQ"));
    CHECK(http->requests.size() == 3); // the cache moved along: the player alone

    const auto moved = first.resolve("dQw4w9WgXcQ"); // NOLINT(bugprone-use-after-move): that is the point
    CHECK(moved.status.code == Error::BadInput);
    CHECK(http->requests.size() == 3);
}
