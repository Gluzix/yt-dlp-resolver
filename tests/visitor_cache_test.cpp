#include "fake_http_client.h"
#include "test_resolver.h"
#include "visitor_cache.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

using ytres::Error;
using ytres::VisitorCache;
using namespace std::chrono_literals;

namespace {

const char *const FRESH_VISITOR_DATA = "CgtGUkVTSEZJWFRVUkU%3D";

// Any fixed moment will do: the cache only compares.
const VisitorCache::Clock::time_point T0 = VisitorCache::Clock::time_point{} + 1000h;

// The visitor data a player request carried, header and body alike.
std::string sentVisitorData(const ytres::HttpRequest &request)
{
    const std::string header = headerValue(request, "X-Goog-Visitor-Id");
    const auto client = nlohmann::json::parse(request.body)["context"]["client"];
    const std::string body = client.contains("visitorData") ? client["visitorData"].get<std::string>() : "";
    return header == body ? header : "header " + header + " but body " + body;
}

}

TEST_CASE("an empty cache has nothing, fresh or stale")
{
    const VisitorCache cache;
    const VisitorCache::Entry entry = cache.get(T0);
    CHECK(entry.value.empty());
    CHECK_FALSE(entry.fresh);
}

TEST_CASE("a value is fresh for six hours, then stale but still there")
{
    VisitorCache cache;
    cache.put("v1", T0);
    CHECK(cache.get(T0).fresh);
    CHECK(cache.get(T0 + 6h - 1ms).fresh);

    const VisitorCache::Entry stale = cache.get(T0 + 6h);
    CHECK_FALSE(stale.fresh);
    CHECK(stale.value == "v1");
}

TEST_CASE("a new value replaces the old and starts its own six hours; an empty one changes nothing")
{
    VisitorCache cache;
    cache.put("v1", T0);
    cache.put("", T0 + 7h);
    CHECK(cache.get(T0 + 7h).value == "v1");
    CHECK_FALSE(cache.get(T0 + 7h).fresh);

    cache.put("v2", T0 + 7h);
    CHECK(cache.get(T0 + 7h).value == "v2");
    CHECK(cache.get(T0 + 13h - 1ms).fresh);
}

TEST_CASE("a warm Resolver asks the player alone, with the cached visitor data")
{
    TestResolver test;
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.resolver.resolve("https://youtu.be/dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 3);
    CHECK_FALSE(isPlayerRequest(test.http->requests[0]));
    CHECK(isPlayerRequest(test.http->requests[1]));
    CHECK(isPlayerRequest(test.http->requests[2]));
    CHECK(sentVisitorData(test.http->requests[2]) == VISITOR_DATA);
}

TEST_CASE("a page without visitor data caches nothing, so the next resolve asks the page again")
{
    TestResolver test("<html>Before you continue to YouTube</html>");
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    test.http->watchPage = WATCH_PAGE;
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 4); // page, player, page, player
    CHECK_FALSE(isPlayerRequest(test.http->requests[2]));
    CHECK(sentVisitorData(test.http->requests[3]) == VISITOR_DATA);
}

TEST_CASE("a bot check fetches fresh visitor data, caches it and asks once more")
{
    TestResolver test;
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ")); // caches VISITOR_DATA
    test.http->watchPage = watchPageWith(FRESH_VISITOR_DATA);
    test.http->playerBodyQueue.push_back(readFixture("player_bot_check.json"));

    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 5); // page, player; player (bot check), page, player
    CHECK(sentVisitorData(test.http->requests[2]) == VISITOR_DATA);
    CHECK_FALSE(isPlayerRequest(test.http->requests[3]));
    CHECK(sentVisitorData(test.http->requests[4]) == FRESH_VISITOR_DATA);
    CHECK(test.logLine(ytres::LogLevel::Warning, "Bot check for dQw4w9WgXcQ") != nullptr);

    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 6);
    CHECK(sentVisitorData(test.http->requests[5]) == FRESH_VISITOR_DATA);
}

TEST_CASE("a second bot check is the answer: one retry, no more")
{
    TestResolver test;
    test.http->playerBody = readFixture("player_bot_check.json");
    const auto result = test.resolver.resolve("jNQXAC9IVRw");
    CHECK(result.status.code == Error::LoginRequired);
    CHECK(result.status.message.find("not a bot") != std::string::npos);
    CHECK(test.http->requests.size() == 4); // page, player, page, player
}

TEST_CASE("a page that fails after a bot check keeps the cached value and asks no more")
{
    TestResolver test;
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    test.http->pageFailure = {Error::Network, "Couldn't connect to server"};
    test.http->playerBodyQueue.push_back(readFixture("player_bot_check.json"));

    CHECK(test.resolver.resolve("dQw4w9WgXcQ").status.code == Error::LoginRequired);
    REQUIRE(test.http->requests.size() == 4); // page, player; player (bot check), page
    CHECK(test.logLine(ytres::LogLevel::Warning, "No watch page (Couldn't connect to server)") != nullptr);

    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    REQUIRE(test.http->requests.size() == 5);
    CHECK(sentVisitorData(test.http->requests[4]) == VISITOR_DATA);
}

TEST_CASE("a cancel while fetching fresh visitor data ends the resolve")
{
    TestResolver test;
    REQUIRE(test.resolver.resolve("dQw4w9WgXcQ"));
    test.http->pageFailure = {Error::Cancelled, "Cancelled"};
    test.http->playerBodyQueue.push_back(readFixture("player_bot_check.json"));
    CHECK(test.resolver.resolve("dQw4w9WgXcQ").status.code == Error::Cancelled);
    CHECK(test.http->requests.size() == 4); // page, player; player (bot check), page
}
