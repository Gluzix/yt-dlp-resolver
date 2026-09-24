#include "fake_http_client.h"
#include "test_resolver.h"

#include "ytres/http.h"
#include "ytres/ytres.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string replaced(std::string text, const std::string &from, const std::string &to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// Answers any number of threads at once, each player request about the video
// it asked for: the recorded dQw4w9WgXcQ response with that id put in. A
// Resolver that mixed up two threads' requests would read an answer about
// another video, which it refuses. Counts what it is asked.
class ConcurrentHttpClient : public ytres::HttpClient
{
public:
    ytres::Result<ytres::HttpResponse> send(const ytres::HttpRequest &request) override
    {
        if (request.url.find("/youtubei/v1/player") == std::string::npos) {
            ++pages;
            // Slow enough for the threads to pile up on the cold cache.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return {{}, {200, WATCH_PAGE}};
        }
        ++players;
        if (headerValue(request, "X-Goog-Visitor-Id") != VISITOR_DATA) {
            ++withoutVisitorData;
        }
        const std::string videoId = nlohmann::json::parse(request.body)["videoId"].get<std::string>();
        return {{}, {200, replaced(fixture, "dQw4w9WgXcQ", videoId)}};
    }

    std::atomic<int> pages{0};
    std::atomic<int> players{0};
    std::atomic<int> withoutVisitorData{0};

private:
    const std::string fixture = readFixture("player_dQw4w9WgXcQ.json");
};

}

TEST_CASE("eight threads resolving through one Resolver each get their own videos, and share the cache")
{
    const int THREADS = 8;
    const int RESOLVES = 20;

    const auto http = std::make_shared<ConcurrentHttpClient>();
    ytres::Resolver::Options options;
    options.http = http;
    ytres::Resolver resolver(options);

    // Tallied here and checked after the join: nothing below asserts off the
    // test's own thread.
    std::atomic<int> ready{0};
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            const std::string videoId = "concurrent" + std::to_string(t); // 11 characters, a valid id
            ++ready;
            while (ready < THREADS) {
                std::this_thread::yield(); // all start together, on a cold cache
            }
            for (int i = 0; i < RESOLVES; ++i) {
                const ytres::Result<ytres::VideoInfo> result = resolver.resolve(videoId);
                const std::optional<ytres::Format> best = result ? result.value.bestAudio() : std::nullopt;
                if (!result || result.value.videoId != videoId || !best || best->itag != 251) {
                    ++wrong;
                }
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }

    CHECK(wrong == 0);
    CHECK(http->players == THREADS * RESOLVES);
    CHECK(http->withoutVisitorData == 0);
    // Once per thread at worst: the mutex is never held across the page
    // request, so threads that meet the cold cache together each fetch it.
    // Here all eight do, and every later resolve finds the cache warm.
    INFO("the watch page was fetched " << http->pages.load() << " times");
    CHECK(http->pages >= 1);
    CHECK(http->pages <= THREADS);
}
