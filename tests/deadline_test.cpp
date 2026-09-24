#include "fake_http_client.h"
#include "test_resolver.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>

using ytres::ClientId;
using ytres::Error;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

// The gap between the resolver working out a timeout and the fake noting the
// request's arrival: microseconds, with room for a busy scheduler.
const auto SLACK = 5ms;
// How late a whole resolve may return: the fake's waits end on a timer tick,
// 15.6 ms on Windows by default.
const auto LATE = 50ms;

// Every request must be over by the deadline: one that arrived at
// arrivals[i] may run for requests[i].timeout and no longer.
void checkWithinDeadline(const FakeHttpClient &http, Clock::time_point start, std::chrono::milliseconds deadline)
{
    REQUIRE(http.arrivals.size() == http.requests.size());
    for (size_t i = 0; i < http.requests.size(); ++i) {
        CAPTURE(i);
        CHECK(http.requests[i].timeout > 0ms);
        CHECK(http.arrivals[i] + http.requests[i].timeout <= start + deadline + SLACK);
    }
}

}

TEST_CASE("with 50 ms to spend, each request gets what is left of them")
{
    TestResolver test;
    test.http->pageDelay = 10ms;
    ytres::Request request;
    request.deadline = 50ms;
    const Clock::time_point start = Clock::now();
    CHECK(test.resolver.resolve("dQw4w9WgXcQ", request));
    REQUIRE(test.http->requests.size() == 2);
    checkWithinDeadline(*test.http, start, 50ms);
    CHECK(test.http->requests[0].timeout <= 50ms);
    CHECK(test.http->requests[1].timeout <= 40ms); // the page took 10 of them
}

TEST_CASE("a page that hangs spends the deadline, and the player is never asked")
{
    TestResolver test;
    test.http->pageDelay = 10s;
    ytres::Request request;
    request.deadline = 50ms;
    const Clock::time_point start = Clock::now();
    const auto result = test.resolver.resolve("dQw4w9WgXcQ", request);
    const auto took = Clock::now() - start;
    CHECK(result.status.code == Error::Timeout);
    CHECK(test.http->requests.size() == 1);
    checkWithinDeadline(*test.http, start, 50ms);
    CHECK(took < 50ms + LATE);
}

TEST_CASE("the bot check's second try spends the same deadline")
{
    TestResolver test;
    test.http->playerBody = readFixture("player_bot_check.json");
    test.http->pageDelay = 10ms;
    test.http->playerDelay = 10ms;
    ytres::Request request;
    request.deadline = 50ms;
    const Clock::time_point start = Clock::now();
    const auto result = test.resolver.resolve("dQw4w9WgXcQ", request);
    const auto took = Clock::now() - start;
    // Four requests of 10 ms fit in 50 only just, so the last may be cut short.
    CHECK((result.status.code == Error::BotCheck || result.status.code == Error::Timeout));
    CHECK(test.http->requests.size() >= 3);
    checkWithinDeadline(*test.http, start, 50ms);
    CHECK(took < 50ms + LATE);
}

TEST_CASE("the ladder spends one deadline, not one per client")
{
    TestResolver test(WATCH_PAGE, 10s, {ClientId::VisionOS, ClientId::Web});
    // visionos answers after 30 ms with nothing playable, which sends the
    // video on to web, which gets the 20 ms left and needs 30.
    test.http->playerBodyByClient["101"] = R"({"playabilityStatus":{"status":"OK"},"videoDetails":{"videoId":"dQw4w9WgXcQ"},)"
                                           R"("streamingData":{"adaptiveFormats":[{"itag":251,"signatureCipher":"s=abc"}]}})";
    test.http->playerDelay = 30ms;
    ytres::Request request;
    request.deadline = 50ms;
    const Clock::time_point start = Clock::now();
    const auto result = test.resolver.resolve("dQw4w9WgXcQ", request);
    const auto took = Clock::now() - start;
    CHECK(result.status.code == Error::Timeout);
    checkWithinDeadline(*test.http, start, 50ms);
    CHECK(took < 50ms + LATE);
}
