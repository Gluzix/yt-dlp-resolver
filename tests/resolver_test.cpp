#include "fake_http_client.h"
#include "innertube.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using ytres::Error;
using namespace std::chrono_literals;

namespace {

const char *const VISITOR_DATA = "CgtGSVhUVVJFAAAA%3D%3D";
const std::string WATCH_PAGE =
    R"(<script>ytcfg.set({"INNERTUBE_CONTEXT":{"client":{"visitorData":"CgtGSVhUVVJFAAAA%3D%3D"}}});</script>)";

// A Resolver on a FakeHttpClient that answers with the recorded
// dQw4w9WgXcQ response, keeping the levels of whatever it logs.
class TestResolver
{
public:
    explicit TestResolver(const std::string &watchPage = WATCH_PAGE, std::chrono::milliseconds requestTimeout = 10s)
        : http(std::make_shared<FakeHttpClient>(readFixture("player_dQw4w9WgXcQ.json"), watchPage))
        , resolver(options(requestTimeout))
    {
    }

    bool logged(ytres::LogLevel level) const
    {
        return std::find(levels.begin(), levels.end(), level) != levels.end();
    }

    std::shared_ptr<FakeHttpClient> http;
    std::vector<ytres::LogLevel> levels;
    ytres::Resolver resolver;

private:
    ytres::Resolver::Options options(std::chrono::milliseconds requestTimeout)
    {
        ytres::Resolver::Options made;
        made.http = http;
        made.log = [this](ytres::LogLevel level, std::string_view) { levels.push_back(level); };
        made.requestTimeout = requestTimeout;
        return made;
    }
};

}

TEST_CASE("a resolve fetches the watch page, then asks the player with its visitor data")
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
    CHECK(test.logged(ytres::LogLevel::Warning));
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

TEST_CASE("milliseconds::max() as a deadline means no deadline, not an instant timeout")
{
    TestResolver test;
    ytres::Request request;
    request.deadline = std::chrono::milliseconds::max();
    CHECK(test.resolver.resolve("dQw4w9WgXcQ", request));
    CHECK(test.http->requests.size() == 2);
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
