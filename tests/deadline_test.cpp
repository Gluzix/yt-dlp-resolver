#include "fake_http_client.h"
#include "test_resolver.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>

using ytres::ClientId;
using ytres::Error;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// The fake's delays are real sleeps, so the budgets here are ten times what
// the checks need: at 50 ms a busy machine running suites in parallel made
// them fail now and then. They stay far below the 10 s request timeout, so a
// request that gets the request timeout instead of what is left still fails.

namespace {

// The gap between the resolver working out a timeout and the fake noting the
// request's arrival: microseconds, with room for a busy scheduler.
const auto SLACK = 50ms;
// How late a whole resolve may return: the fake's waits end on a timer tick,
// 15.6 ms on Windows by default, and a loaded machine adds its own.
const auto LATE = 500ms;

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

TEST_CASE("with 500 ms to spend, each request gets what is left of them")
{
    TestResolver test;
    test.http->pageDelay = 100ms;
    ytres::Request request;
    request.deadline = 500ms;
    const Clock::time_point start = Clock::now();
    CHECK(test.resolver.resolve("dQw4w9WgXcQ", request));
    REQUIRE(test.http->requests.size() == 2);
    checkWithinDeadline(*test.http, start, 500ms);
    CHECK(test.http->requests[0].timeout <= 500ms);
    CHECK(test.http->requests[1].timeout <= 400ms); // the page took 100 of them
}

TEST_CASE("a page that hangs spends the deadline, and the player is never asked")
{
    TestResolver test;
    test.http->pageDelay = 10s;
    ytres::Request request;
    request.deadline = 500ms;
    const Clock::time_point start = Clock::now();
    const auto result = test.resolver.resolve("dQw4w9WgXcQ", request);
    const auto took = Clock::now() - start;
    CHECK(result.status.code == Error::Timeout);
    CHECK(test.http->requests.size() == 1);
    checkWithinDeadline(*test.http, start, 500ms);
    CHECK(took < 500ms + LATE);
}

TEST_CASE("the bot check's second try spends the same deadline")
{
    TestResolver test;
    test.http->playerBody = readFixture("player_bot_check.json");
    test.http->pageDelay = 100ms;
    test.http->playerDelay = 100ms;
    ytres::Request request;
    request.deadline = 500ms;
    const Clock::time_point start = Clock::now();
    const auto result = test.resolver.resolve("dQw4w9WgXcQ", request);
    const auto took = Clock::now() - start;
    // Four requests of 100 ms fit in 500 only just, so the last may be cut short.
    CHECK((result.status.code == Error::BotCheck || result.status.code == Error::Timeout));
    CHECK(test.http->requests.size() >= 3);
    checkWithinDeadline(*test.http, start, 500ms);
    CHECK(took < 500ms + LATE);
}

TEST_CASE("the ladder spends one deadline, not one per client")
{
    TestResolver test(WATCH_PAGE, 10s, {ClientId::VisionOS, ClientId::Web});
    // visionos answers after 300 ms with nothing playable, which sends the
    // video on to web, which gets the 200 ms left and needs 300.
    test.http->playerBodyByClient["101"] = R"({"playabilityStatus":{"status":"OK"},"videoDetails":{"videoId":"dQw4w9WgXcQ"},)"
                                           R"("streamingData":{"adaptiveFormats":[{"itag":251,"signatureCipher":"s=abc"}]}})";
    test.http->playerDelay = 300ms;
    ytres::Request request;
    request.deadline = 500ms;
    const Clock::time_point start = Clock::now();
    const auto result = test.resolver.resolve("dQw4w9WgXcQ", request);
    const auto took = Clock::now() - start;
    CHECK(result.status.code == Error::Timeout);
    checkWithinDeadline(*test.http, start, 500ms);
    CHECK(took < 500ms + LATE);
}
